#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define MCTRACKERBRIDGE_EXPORTS

#include "MCTrackerBridge.h"
#include "ExtractorSeh.h"
#include "../MCST.Shared/TrackerRecoveryPolicy.h"
#include "../MCST.Shared/MCBridgeProtocol.h"

#include <windows.h>
#include <tlhelp32.h>
#include <unknwn.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <cwctype>
#include <cwchar>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\MCTrackerBridge";
    constexpr wchar_t kSingletonMutexName[] = L"Local\\MCTrackerBridgeSingleton_V150";
    constexpr wchar_t kOutputDirectory[] = L"C:\\Temp";
    constexpr int kBridgeVersion = 178;
    constexpr DWORD kPipeBufferBytes = 1024u * 1024u;

    enum class RuntimeState : LONG
    {
        Stopped = 0,
        Starting = 1,
        Running = 2,
        Stopping = 3,
        Failed = 4
    };

    struct WindowRecord
    {
        HWND hwnd = nullptr;
        HWND parent = nullptr;
        DWORD threadId = 0;
        DWORD processId = 0;
        std::wstring className;
        std::wstring text;
        RECT rect{};
        bool visible = false;
        bool enabled = false;
        LONG_PTR wndProc = 0;
        LONG_PTR userData = 0;
        LONG_PTR instance = 0;
    };

    struct ModuleRecord
    {
        std::wstring name;
        std::wstring path;
        std::uintptr_t base = 0;
        DWORD size = 0;
    };

    struct Snapshot
    {
        std::uint64_t generation = 0;
        SYSTEMTIME capturedUtc{};
        DWORD processId = 0;
        DWORD initializeThreadId = 0;
        DWORD workerThreadId = 0;
        DWORD trackerThreadId = 0;
        std::wstring processPath;
        std::uint64_t processCreationTime = 0;
        HWND topWindow = nullptr;
        HWND trackerWindow = nullptr;
        bool trackerFound = false;
        bool trackerInSameProcess = false;
        HMODULE atonpTrackerModule = nullptr;
        std::uintptr_t atonpTrackerBase = 0;
        DWORD atonpTrackerSize = 0;
        DWORD atonpTrackerPeTimestamp = 0;
        std::size_t pageCount = 0;
        std::size_t flexGridCount = 0;
        std::vector<WindowRecord> windows;
        std::vector<ModuleRecord> modules;
        std::vector<DWORD> processThreads;
    };

    HMODULE g_module = nullptr;
    HMODULE g_selfReference = nullptr;
    std::atomic<RuntimeState> g_state{ RuntimeState::Stopped };
    std::atomic<DWORD> g_lastError{ ERROR_SUCCESS };
    std::atomic<ULONGLONG> g_lastHeartbeatTick{ 0 };
    std::atomic<std::uint64_t> g_generation{ 0 };
    DWORD g_initializeThreadId = 0;
    DWORD g_workerThreadId = 0;
    HANDLE g_workerThread = nullptr;
    HANDLE g_stopEvent = nullptr;
    HANDLE g_singletonMutex = nullptr;
    SRWLOCK g_snapshotLock = SRWLOCK_INIT;
    Snapshot g_lastSnapshot;
    SRWLOCK g_executionTraceLock = SRWLOCK_INIT;
    SRWLOCK g_gridReadLock = SRWLOCK_INIT;

    std::wstring ExecutionTracePath()
    {
        std::wostringstream out;
        out << kOutputDirectory << L"\\MC_V147_Extractor_Execution_"
            << GetCurrentProcessId() << L".txt";
        return out.str();
    }

    void AppendExecutionTrace(const char* stage, const std::string& detail = {})
    {
        AcquireSRWLockExclusive(&g_executionTraceLock);
        CreateDirectoryW(kOutputDirectory, nullptr);
        std::ofstream file(ExecutionTracePath(), std::ios::binary | std::ios::app);
        if (file)
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            file << std::setfill('0')
                 << std::setw(4) << now.wYear << '-'
                 << std::setw(2) << now.wMonth << '-'
                 << std::setw(2) << now.wDay << ' '
                 << std::setw(2) << now.wHour << ':'
                 << std::setw(2) << now.wMinute << ':'
                 << std::setw(2) << now.wSecond << '.'
                 << std::setw(3) << now.wMilliseconds
                 << " pid=" << GetCurrentProcessId()
                 << " tid=" << GetCurrentThreadId()
                 << " stage=" << (stage ? stage : "")
                 << (detail.empty() ? "" : " detail=") << detail << "\r\n";
            file.flush();
        }
        ReleaseSRWLockExclusive(&g_executionTraceLock);
    }

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(towlower(ch));
        });
        return value;
    }

    std::wstring ClassName(HWND hwnd)
    {
        wchar_t buffer[512]{};
        if (!hwnd || GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer))) <= 0)
            return L"";
        return buffer;
    }

    std::wstring WindowTextWithTimeout(HWND hwnd)
    {
        if (!hwnd)
            return L"";

        DWORD_PTR lengthResult = 0;
        if (!SendMessageTimeoutW(
                hwnd,
                WM_GETTEXTLENGTH,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                100,
                &lengthResult))
        {
            return L"";
        }

        std::size_t length = static_cast<std::size_t>(lengthResult);
        if (length > 8192)
            length = 8192;

        std::vector<wchar_t> buffer(length + 1, L'\0');
        DWORD_PTR copied = 0;
        if (!SendMessageTimeoutW(
                hwnd,
                WM_GETTEXT,
                static_cast<WPARAM>(buffer.size()),
                reinterpret_cast<LPARAM>(buffer.data()),
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                100,
                &copied))
        {
            return L"";
        }
        return std::wstring(buffer.data());
    }

    std::wstring ProcessPath()
    {
        std::vector<wchar_t> buffer(32768, L'\0');
        DWORD length = static_cast<DWORD>(buffer.size());
        if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, buffer.data(), &length))
            return L"";
        return std::wstring(buffer.data(), length);
    }

    std::wstring BaseName(const std::wstring& path)
    {
        const std::size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    std::uint64_t ProcessCreationFileTime()
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            return 0;
        ULARGE_INTEGER value{};
        value.LowPart = created.dwLowDateTime;
        value.HighPart = created.dwHighDateTime;
        return value.QuadPart;
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};
        const int required = WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0)
            return {};
        std::string result(static_cast<std::size_t>(required), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
        return result;
    }

    std::string JsonEscape(const std::string& value)
    {
        std::ostringstream out;
        for (unsigned char ch : value)
        {
            switch (ch)
            {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20)
                {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(ch) << std::dec;
                }
                else
                {
                    out << static_cast<char>(ch);
                }
                break;
            }
        }
        return out.str();
    }

    std::string JsonString(const std::wstring& value)
    {
        return std::string("\"") + JsonEscape(WideToUtf8(value)) + "\"";
    }

    std::string JsonString(const std::string& value)
    {
        return std::string("\"") + JsonEscape(value) + "\"";
    }

    std::string HexValue(std::uintptr_t value)
    {
        std::ostringstream out;
        out << "0x" << std::hex << std::uppercase << value;
        return out.str();
    }


    std::uintptr_t ParseAtlClassAddress(const std::wstring& className)
    {
        constexpr wchar_t prefix[] = L"ATL:";
        if (className.size() <= 4 || className.compare(0, 4, prefix) != 0)
            return 0;
        std::uintptr_t value = 0;
        for (std::size_t i = 4; i < className.size(); ++i)
        {
            const wchar_t ch = className[i];
            unsigned int digit = 0;
            if (ch >= L'0' && ch <= L'9') digit = static_cast<unsigned int>(ch - L'0');
            else if (ch >= L'a' && ch <= L'f') digit = 10u + static_cast<unsigned int>(ch - L'a');
            else if (ch >= L'A' && ch <= L'F') digit = 10u + static_cast<unsigned int>(ch - L'A');
            else return 0;
            if (value > (UINTPTR_MAX >> 4))
                return 0;
            value = (value << 4) | digit;
        }
        return value;
    }

    bool IsCurrentProcessWindow(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return pid == GetCurrentProcessId();
    }

    WindowRecord MakeWindowRecord(HWND hwnd)
    {
        WindowRecord record;
        record.hwnd = hwnd;
        record.parent = GetParent(hwnd);
        record.threadId = GetWindowThreadProcessId(hwnd, &record.processId);
        record.className = ClassName(hwnd);
        const std::wstring lowerClass = Lower(record.className);
        const bool textIsUseful =
            lowerClass.find(L"atl_mcmdichildframe") != std::wstring::npos ||
            lowerClass.find(L"flexgrid") != std::wstring::npos ||
            lowerClass.find(L"tab") != std::wstring::npos ||
            lowerClass.find(L"header") != std::wstring::npos ||
            lowerClass == L"#32770";
        if (textIsUseful)
            record.text = WindowTextWithTimeout(hwnd);
        GetWindowRect(hwnd, &record.rect);
        record.visible = IsWindowVisible(hwnd) != FALSE;
        record.enabled = IsWindowEnabled(hwnd) != FALSE;
        if (record.processId == GetCurrentProcessId())
        {
            SetLastError(ERROR_SUCCESS);
            record.wndProc = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
            record.userData = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            record.instance = GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        }
        return record;
    }

    struct WindowEnumContext
    {
        DWORD pid = 0;
        std::vector<HWND>* windows = nullptr;
    };

    BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM parameter)
    {
        auto* context = reinterpret_cast<WindowEnumContext*>(parameter);
        if (!context || !context->windows)
            return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == context->pid)
            context->windows->push_back(hwnd);
        return TRUE;
    }

    std::vector<HWND> TopWindowsForCurrentProcess()
    {
        std::vector<HWND> result;
        WindowEnumContext context{ GetCurrentProcessId(), &result };
        EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&context));
        return result;
    }

    std::vector<HWND> Descendants(HWND root)
    {
        std::vector<HWND> result;
        if (!root)
            return result;
        WindowEnumContext context{ GetCurrentProcessId(), &result };
        EnumChildWindows(root, CollectWindow, reinterpret_cast<LPARAM>(&context));
        return result;
    }

    bool IsFlexGridClass(const std::wstring& className)
    {
        return Lower(className).find(L"cls_flexgridwnd") != std::wstring::npos;
    }

    int TrackerCandidateScore(HWND hwnd)
    {
        if (!IsCurrentProcessWindow(hwnd))
            return -1;
        const std::wstring cls = Lower(ClassName(hwnd));
        if (cls.find(L"atl_mcmdichildframe") == std::wstring::npos)
            return -1;
        const std::wstring text = Lower(WindowTextWithTimeout(hwnd));
        int score = 80;
        if (text.find(L"order and position tracker") != std::wstring::npos)
            score += 1000;

        std::size_t grids = 0;
        for (HWND child : Descendants(hwnd))
        {
            if (IsFlexGridClass(ClassName(child)))
                ++grids;
        }
        if (grids > 0)
            score += 100 + static_cast<int>(std::min<std::size_t>(grids, 20));
        return score;
    }

    HWND FindTrackerWindow(HWND& topWindow)
    {
        HWND best = nullptr;
        int bestScore = 0;
        topWindow = nullptr;

        for (HWND top : TopWindowsForCurrentProcess())
        {
            std::vector<HWND> candidates;
            candidates.push_back(top);
            const auto children = Descendants(top);
            candidates.insert(candidates.end(), children.begin(), children.end());

            for (HWND hwnd : candidates)
            {
                const int score = TrackerCandidateScore(hwnd);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = hwnd;
                    topWindow = GetAncestor(hwnd, GA_ROOT);
                }
            }
        }
        return bestScore >= 1000 ? best : nullptr;
    }

    std::vector<ModuleRecord> EnumerateModules()
    {
        std::vector<ModuleRecord> result;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE)
            return result;

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                ModuleRecord record;
                record.name = entry.szModule;
                record.path = entry.szExePath;
                record.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                record.size = entry.modBaseSize;
                result.push_back(std::move(record));
                entry.dwSize = sizeof(entry);
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    bool ReadPeTimestampFromFile(const std::wstring& path, DWORD& timestamp)
    {
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        if (!input)
            return false;

        IMAGE_DOS_HEADER dos{};
        input.read(reinterpret_cast<char*>(&dos), sizeof(dos));
        if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
            return false;

        input.seekg(dos.e_lfanew, std::ios::beg);
        DWORD signature = 0;
        input.read(reinterpret_cast<char*>(&signature), sizeof(signature));
        if (!input || signature != IMAGE_NT_SIGNATURE)
            return false;

        IMAGE_FILE_HEADER fileHeader{};
        input.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
        if (!input)
            return false;

        timestamp = fileHeader.TimeDateStamp;
        return true;
    }

    std::vector<DWORD> EnumerateThreads()
    {
        std::vector<DWORD> result;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return result;

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID == GetCurrentProcessId())
                    result.push_back(entry.th32ThreadID);
                entry.dwSize = sizeof(entry);
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        std::sort(result.begin(), result.end());
        return result;
    }

    Snapshot CaptureSnapshot()
    {
        Snapshot snapshot;
        snapshot.generation = ++g_generation;
        GetSystemTime(&snapshot.capturedUtc);
        snapshot.processId = GetCurrentProcessId();
        snapshot.initializeThreadId = g_initializeThreadId;
        snapshot.workerThreadId = g_workerThreadId;
        snapshot.processPath = ProcessPath();
        snapshot.processCreationTime = ProcessCreationFileTime();
        snapshot.modules = EnumerateModules();
        snapshot.processThreads = EnumerateThreads();

        snapshot.trackerWindow = FindTrackerWindow(snapshot.topWindow);
        snapshot.trackerFound = snapshot.trackerWindow != nullptr;
        if (snapshot.trackerFound)
        {
            DWORD trackerPid = 0;
            snapshot.trackerThreadId = GetWindowThreadProcessId(snapshot.trackerWindow, &trackerPid);
            snapshot.trackerInSameProcess = trackerPid == snapshot.processId;

            if (snapshot.topWindow && snapshot.topWindow != snapshot.trackerWindow)
                snapshot.windows.push_back(MakeWindowRecord(snapshot.topWindow));
            snapshot.windows.push_back(MakeWindowRecord(snapshot.trackerWindow));
            const auto descendants = Descendants(snapshot.trackerWindow);
            for (HWND hwnd : descendants)
            {
                WindowRecord record = MakeWindowRecord(hwnd);
                const std::wstring lowerClass = Lower(record.className);
                if (lowerClass == L"#32770")
                    ++snapshot.pageCount;
                if (IsFlexGridClass(record.className))
                    ++snapshot.flexGridCount;
                snapshot.windows.push_back(std::move(record));
            }
        }

        snapshot.atonpTrackerModule = GetModuleHandleW(L"ATOnPTracker.dll");
        snapshot.atonpTrackerBase = reinterpret_cast<std::uintptr_t>(snapshot.atonpTrackerModule);
        for (const ModuleRecord& module : snapshot.modules)
        {
            if (Lower(module.name) == L"atonptracker.dll")
            {
                snapshot.atonpTrackerBase = module.base;
                snapshot.atonpTrackerSize = module.size;
                DWORD timestamp = 0;
                if (ReadPeTimestampFromFile(module.path, timestamp))
                    snapshot.atonpTrackerPeTimestamp = timestamp;
                break;
            }
        }

        AcquireSRWLockExclusive(&g_snapshotLock);
        g_lastSnapshot = snapshot;
        ReleaseSRWLockExclusive(&g_snapshotLock);
        return snapshot;
    }

    bool LastSnapshotHasTracker()
    {
        AcquireSRWLockShared(&g_snapshotLock);
        const bool found = g_lastSnapshot.trackerFound && g_lastSnapshot.trackerInSameProcess;
        ReleaseSRWLockShared(&g_snapshotLock);
        return found;
    }

    std::string TimestampJson(const SYSTEMTIME& time)
    {
        std::ostringstream out;
        out << '"' << std::setfill('0')
            << std::setw(4) << time.wYear << '-'
            << std::setw(2) << time.wMonth << '-'
            << std::setw(2) << time.wDay << 'T'
            << std::setw(2) << time.wHour << ':'
            << std::setw(2) << time.wMinute << ':'
            << std::setw(2) << time.wSecond << '.'
            << std::setw(3) << time.wMilliseconds << "Z\"";
        return out.str();
    }

    void AppendStatusJson(std::ostringstream& out, const Snapshot& snapshot)
    {
        out << "\"bridge_version\":" << kBridgeVersion << ',';
        out << "\"protocol_version\":" << mcbridge::kProtocolVersion << ',';
        out << "\"state\":" << static_cast<LONG>(g_state.load()) << ',';
        out << "\"last_error\":" << g_lastError.load() << ',';
        out << "\"last_heartbeat_tick\":" << g_lastHeartbeatTick.load() << ',';
        out << "\"process_id\":" << snapshot.processId << ',';
        out << "\"process_path\":" << JsonString(snapshot.processPath) << ',';
        out << "\"process_creation_time\":" << snapshot.processCreationTime << ',';
        out << "\"initialize_thread_id\":" << snapshot.initializeThreadId << ',';
        out << "\"worker_thread_id\":" << snapshot.workerThreadId << ',';
        out << "\"tracker_found\":" << (snapshot.trackerFound ? "true" : "false") << ',';
        out << "\"tracker_same_process\":" << (snapshot.trackerInSameProcess ? "true" : "false") << ',';
        out << "\"top_hwnd\":\"" << HexValue(reinterpret_cast<std::uintptr_t>(snapshot.topWindow)) << "\",";
        out << "\"tracker_hwnd\":\"" << HexValue(reinterpret_cast<std::uintptr_t>(snapshot.trackerWindow)) << "\",";
        out << "\"tracker_thread_id\":" << snapshot.trackerThreadId << ',';
        out << "\"atonptracker_loaded\":" << (snapshot.atonpTrackerBase != 0 ? "true" : "false") << ',';
        out << "\"atonptracker_base\":\"" << HexValue(snapshot.atonpTrackerBase) << "\",";
        out << "\"atonptracker_size\":" << snapshot.atonpTrackerSize << ',';
        out << "\"atonptracker_pe_timestamp\":" << snapshot.atonpTrackerPeTimestamp << ',';
        out << "\"page_count\":" << snapshot.pageCount << ',';
        out << "\"flexgrid_count\":" << snapshot.flexGridCount << ',';
        out << "\"generation\":" << snapshot.generation << ',';
        out << "\"captured_utc\":" << TimestampJson(snapshot.capturedUtc) << ',';
        out << "\"internal_function_calls_enabled\":false,";
        out << "\"experimental_internal_function_calls_available\":true,";
        out << "\"controlled_gettext_single_cell_probe_enabled\":true,";
        out << "\"status_report_snapshot_enabled\":true,";
        out << "\"state_changing_messages_enabled\":false";
    }

    std::string StatusJson(const Snapshot& snapshot)
    {
        std::ostringstream out;
        out << '{';
        AppendStatusJson(out, snapshot);
        out << '}';
        return out.str();
    }

    std::string TrackerMapJson(const Snapshot& snapshot)
    {
        std::ostringstream out;
        out << '{';
        AppendStatusJson(out, snapshot);
        out << ",\"windows\":[";
        bool first = true;
        for (const WindowRecord& window : snapshot.windows)
        {
            if (!first) out << ',';
            first = false;
            out << '{'
                << "\"hwnd\":\"" << HexValue(reinterpret_cast<std::uintptr_t>(window.hwnd)) << "\"," 
                << "\"parent\":\"" << HexValue(reinterpret_cast<std::uintptr_t>(window.parent)) << "\"," 
                << "\"thread_id\":" << window.threadId << ','
                << "\"process_id\":" << window.processId << ','
                << "\"class\":" << JsonString(window.className) << ','
                << "\"text\":" << JsonString(window.text) << ','
                << "\"visible\":" << (window.visible ? "true" : "false") << ','
                << "\"enabled\":" << (window.enabled ? "true" : "false") << ','
                << "\"rect\":[" << window.rect.left << ',' << window.rect.top << ','
                << window.rect.right << ',' << window.rect.bottom << "],"
                << "\"wndproc\":\"" << HexValue(static_cast<std::uintptr_t>(window.wndProc)) << "\"," 
                << "\"userdata\":\"" << HexValue(static_cast<std::uintptr_t>(window.userData)) << "\"," 
                << "\"hinstance\":\"" << HexValue(static_cast<std::uintptr_t>(window.instance)) << "\""
                << '}';
        }
        out << "]}";
        return out.str();
    }

    bool EnsureOutputDirectory()
    {
        if (CreateDirectoryW(kOutputDirectory, nullptr))
            return true;
        const DWORD error = GetLastError();
        return error == ERROR_ALREADY_EXISTS;
    }

    std::wstring ReportPath(const wchar_t* stem, DWORD pid)
    {
        std::wostringstream out;
        out << kOutputDirectory << L"\\" << stem << L"_" << pid << L".txt";
        return out.str();
    }

    bool WriteUtf8File(const std::wstring& path, const std::string& text)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        bool ok = WriteFile(file, bom, static_cast<DWORD>(sizeof(bom)), &written, nullptr) != FALSE;
        if (ok && !text.empty())
            ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE;
        FlushFileBuffers(file);
        CloseHandle(file);
        return ok;
    }

    bool AppendUtf8Line(const std::wstring& path, const std::string& line)
    {
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size{};
        const bool empty = GetFileSizeEx(file, &size) && size.QuadPart == 0;
        DWORD written = 0;
        bool ok = true;
        if (empty)
        {
            const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            ok = WriteFile(file, bom, static_cast<DWORD>(sizeof(bom)), &written, nullptr) != FALSE;
        }
        std::string text = line;
        if (text.size() < 2 || text.substr(text.size() - 2) != "\r\n")
            text += "\r\n";
        if (ok)
            ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE;
        FlushFileBuffers(file);
        CloseHandle(file);
        return ok;
    }

    bool FileExists(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::string HumanSnapshotReport(const Snapshot& snapshot)
    {
        std::ostringstream out;
        out << "MC V147 In-Process Bridge Local Snapshot\r\n";
        out << "=========================================\r\n";
        out << "bridge_version=" << kBridgeVersion << "\r\n";
        out << "last_error=" << g_lastError.load() << "\r\n";
        out << "last_heartbeat_tick=" << g_lastHeartbeatTick.load() << "\r\n";
        out << "process_id=" << snapshot.processId << "\r\n";
        out << "process_path=" << WideToUtf8(snapshot.processPath) << "\r\n";
        out << "process_creation_time=" << snapshot.processCreationTime << "\r\n";
        out << "initialize_thread_id=" << snapshot.initializeThreadId << "\r\n";
        out << "worker_thread_id=" << snapshot.workerThreadId << "\r\n";
        out << "tracker_found=" << (snapshot.trackerFound ? "yes" : "no") << "\r\n";
        out << "tracker_same_process=" << (snapshot.trackerInSameProcess ? "yes" : "no") << "\r\n";
        out << "top_hwnd=" << HexValue(reinterpret_cast<std::uintptr_t>(snapshot.topWindow)) << "\r\n";
        out << "tracker_hwnd=" << HexValue(reinterpret_cast<std::uintptr_t>(snapshot.trackerWindow)) << "\r\n";
        out << "tracker_thread_id=" << snapshot.trackerThreadId << "\r\n";
        out << "atonptracker_loaded=" << (snapshot.atonpTrackerBase != 0 ? "yes" : "no") << "\r\n";
        out << "atonptracker_base=" << HexValue(snapshot.atonpTrackerBase) << "\r\n";
        out << "atonptracker_size=" << snapshot.atonpTrackerSize << "\r\n";
        out << "page_count=" << snapshot.pageCount << "\r\n";
        out << "flexgrid_count=" << snapshot.flexGridCount << "\r\n";
        out << "internal_function_calls_attempted=no\r\n";
        out << "state_changing_messages_sent=no\r\n";
        out << "synthetic_input_used=no\r\n";
        out << "clipboard_used=no\r\n\r\n";

        out << "WINDOWS\r\n-------\r\n";
        for (const WindowRecord& window : snapshot.windows)
        {
            out << "hwnd=" << HexValue(reinterpret_cast<std::uintptr_t>(window.hwnd))
                << " parent=" << HexValue(reinterpret_cast<std::uintptr_t>(window.parent))
                << " tid=" << window.threadId
                << " class=[" << WideToUtf8(window.className) << "]"
                << " text=[" << WideToUtf8(window.text) << "]"
                << " visible=" << (window.visible ? "yes" : "no")
                << " enabled=" << (window.enabled ? "yes" : "no")
                << " wndproc=" << HexValue(static_cast<std::uintptr_t>(window.wndProc))
                << " userdata=" << HexValue(static_cast<std::uintptr_t>(window.userData))
                << "\r\n";
        }

        out << "\r\nMODULES\r\n-------\r\n";
        for (const ModuleRecord& module : snapshot.modules)
        {
            out << "base=" << HexValue(module.base)
                << " size=" << module.size
                << " name=[" << WideToUtf8(module.name) << "]"
                << " path=[" << WideToUtf8(module.path) << "]\r\n";
        }

        out << "\r\nTHREADS\r\n-------\r\n";
        for (DWORD threadId : snapshot.processThreads)
            out << "thread_id=" << threadId << "\r\n";
        return out.str();
    }

    bool WriteSnapshotReports(const Snapshot& snapshot, std::vector<std::wstring>& writtenPaths)
    {
        EnsureOutputDirectory();
        const std::wstring localPath = ReportPath(L"MC_V147_Bridge_Local_Snapshot", snapshot.processId);
        const std::wstring statusPath = ReportPath(L"MC_V147_Bridge_Status", snapshot.processId);
        const std::wstring trackerPath = ReportPath(L"MC_V147_Bridge_Tracker_Window_Map", snapshot.processId);

        bool ok = true;
        if (WriteUtf8File(localPath, HumanSnapshotReport(snapshot))) writtenPaths.push_back(localPath); else ok = false;
        if (WriteUtf8File(statusPath, StatusJson(snapshot))) writtenPaths.push_back(statusPath); else ok = false;
        if (WriteUtf8File(trackerPath, TrackerMapJson(snapshot))) writtenPaths.push_back(trackerPath); else ok = false;
        return ok;
    }
    constexpr std::uintptr_t kExtractAccountsRva = 0x10FAE6;
    constexpr std::uintptr_t kExtractOpenPositionsRva = 0x10FF56;
    constexpr DWORD kKnownAtonpSize = 3534848;
    constexpr DWORD kPositionHistoryAtonpTimestamp = 0x6A5694FBu;
    constexpr std::uintptr_t kKnownTabViewPrimaryVtableRva = 0x1D78C8u;

    struct TrackerCompatibilityProfile
    {
        bool matched = false;
        bool externalVerified = false;
        std::wstring name;
        std::wstring section;
        std::wstring source;
        std::wstring mode;
        std::wstring diagnostic;
        DWORD atonpTrackerPeTimestamp = 0;
        std::uint64_t atonpTrackerImageSize = 0;
        std::uintptr_t tabViewVtableRva = 0;
        std::uintptr_t accountsExtractorRva = 0;
        std::uintptr_t openPositionsExtractorRva = 0;
        std::size_t accountsPageOffset = 0;
        std::size_t openPositionsPageOffset = 0;
        std::size_t positionHistoryPageOffset = 0;
        std::size_t logsPageOffset = 0;
        std::size_t gridMemberOffset = 0;
        std::size_t rowsOffset1 = 0;
        std::size_t rowsOffset2 = 0;
        std::size_t getTextSlot = 0;
        std::uintptr_t flexGridVtableRva = 0;
        std::uintptr_t getTextRva = 0;
        bool allowAdaptiveFlexGridIdentity = false;
    };

    std::wstring TrimCompatibilityValue(std::wstring value)
    {
        const auto notSpace = [](wchar_t ch) { return iswspace(ch) == 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    bool TryParseCompatibilityUnsigned(const std::wstring& text, unsigned long long& value)
    {
        const std::wstring normalized = TrimCompatibilityValue(text);
        if (normalized.empty())
            return false;
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::wcstoull(normalized.c_str(), &end, 0);
        if (errno == ERANGE || end == normalized.c_str() || *end != L'\0')
            return false;
        value = parsed;
        return true;
    }

    std::wstring CompatibilityReadValue(
        const std::wstring& path,
        const std::wstring& section,
        const wchar_t* key,
        const wchar_t* fallback = L"")
    {
        wchar_t buffer[2048]{};
        GetPrivateProfileStringW(section.c_str(), key, fallback, buffer,
            static_cast<DWORD>(std::size(buffer)), path.c_str());
        return buffer;
    }

    std::vector<std::wstring> CompatibilityReadSections(const std::wstring& path)
    {
        std::vector<wchar_t> buffer(65536, L'\0');
        const DWORD count = GetPrivateProfileSectionNamesW(
            buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
        std::vector<std::wstring> sections;
        if (count == 0)
            return sections;
        const wchar_t* current = buffer.data();
        while (*current != L'\0')
        {
            sections.emplace_back(current);
            current += sections.back().size() + 1;
        }
        return sections;
    }

    std::wstring BridgeCompatibilityDatabasePath()
    {
        std::vector<wchar_t> path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            g_module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
            return L"MCST-Compatibility.ini";
        const std::wstring modulePath(path.data(), length);
        return (std::filesystem::path(modulePath).parent_path() / L"MCST-Compatibility.ini").wstring();
    }

    std::wstring CompatibilityHex(DWORD value)
    {
        std::wostringstream out;
        out << L"0x" << std::hex << std::uppercase << value;
        return out.str();
    }

    void EnsureTrackerCandidateTemplate(const Snapshot& snapshot)
    {
        if (!snapshot.atonpTrackerBase || snapshot.atonpTrackerPeTimestamp == 0 || snapshot.atonpTrackerSize == 0)
            return;

        const std::wstring path = BridgeCompatibilityDatabasePath();
        WritePrivateProfileStringW(L"Compatibility", L"schema_version", L"2", path.c_str());
        WritePrivateProfileStringW(L"Compatibility", L"unknown_build_policy", L"reject", path.c_str());

        std::wostringstream sectionBuilder;
        sectionBuilder << L"Candidate.ATOnPTracker-" << std::hex << std::uppercase
                       << snapshot.atonpTrackerPeTimestamp << L"-" << std::dec << snapshot.atonpTrackerSize;
        const std::wstring section = sectionBuilder.str();

        wchar_t existing[8]{};
        GetPrivateProfileStringW(section.c_str(), L"candidate_created", L"", existing,
            static_cast<DWORD>(std::size(existing)), path.c_str());
        if (existing[0] != L'\0')
            return;

        WritePrivateProfileStringW(section.c_str(), L"candidate_created", L"true", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"enabled", L"false", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"name", L"Unverified ATOnPTracker build", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"atonptracker_pe_timestamp", CompatibilityHex(snapshot.atonpTrackerPeTimestamp).c_str(), path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"atonptracker_image_size", std::to_wstring(snapshot.atonpTrackerSize).c_str(), path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_tabview_vtable_rva", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_accounts_page_offset", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_open_positions_page_offset", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_position_history_page_offset", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_logs_page_offset", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_grid_member_offset", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_rows_offset_1", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_rows_offset_2", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_gettext_slot", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_flexgrid_vtable_rva", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_gettext_rva", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_accounts_extractor_rva", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"tracker_open_positions_extractor_rva", L"", path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"verification", L"UNVERIFIED - Developer Mode research required before creating an enabled Profile.* section", path.c_str());
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    }

    TrackerCompatibilityProfile ResolveExternalTrackerCompatibilityProfile(const Snapshot& snapshot)
    {
        TrackerCompatibilityProfile profile;
        profile.mode = L"unknown";
        if (!snapshot.atonpTrackerBase)
        {
            profile.diagnostic = L"ATOnPTracker.dll is not loaded.";
            return profile;
        }
        if (snapshot.atonpTrackerPeTimestamp == 0 || snapshot.atonpTrackerSize == 0)
        {
            profile.diagnostic = L"ATOnPTracker.dll fingerprint could not be read.";
            return profile;
        }

        const std::wstring path = BridgeCompatibilityDatabasePath();
        std::wstring incompleteProfile;
        for (const auto& section : CompatibilityReadSections(path))
        {
            if (section.rfind(L"Profile.", 0) != 0)
                continue;
            const std::wstring enabled = CompatibilityReadValue(path, section, L"enabled", L"true");
            if (_wcsicmp(enabled.c_str(), L"false") == 0 || enabled == L"0")
                continue;

            unsigned long long timestamp = 0;
            unsigned long long imageSize = 0;
            if (!TryParseCompatibilityUnsigned(
                    CompatibilityReadValue(path, section, L"atonptracker_pe_timestamp"), timestamp) ||
                !TryParseCompatibilityUnsigned(
                    CompatibilityReadValue(path, section, L"atonptracker_image_size"), imageSize))
            {
                continue;
            }
            if (timestamp != snapshot.atonpTrackerPeTimestamp || imageSize != snapshot.atonpTrackerSize)
                continue;

            unsigned long long tabViewVtableRva = 0;
            unsigned long long accountsPageOffset = 0;
            unsigned long long openPositionsPageOffset = 0;
            unsigned long long positionHistoryPageOffset = 0;
            unsigned long long logsPageOffset = 0;
            unsigned long long gridMemberOffset = 0;
            unsigned long long rowsOffset1 = 0;
            unsigned long long rowsOffset2 = 0;
            unsigned long long getTextSlot = 0;
            unsigned long long flexGridVtableRva = 0;
            unsigned long long getTextRva = 0;
            if (!TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_tabview_vtable_rva"), tabViewVtableRva) ||
                !TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_accounts_page_offset"), accountsPageOffset) ||
                !TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_open_positions_page_offset"), openPositionsPageOffset) ||
                !TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_logs_page_offset"), logsPageOffset) ||
                !TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_grid_member_offset"), gridMemberOffset) ||
                !TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_rows_offset_1"), rowsOffset1) ||
                !TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_rows_offset_2"), rowsOffset2) ||
                !TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_gettext_slot"), getTextSlot) ||
                !TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_flexgrid_vtable_rva"), flexGridVtableRva) ||
                !TryParseCompatibilityUnsigned(CompatibilityReadValue(path, section, L"tracker_gettext_rva"), getTextRva))
            {
                incompleteProfile = section;
                continue;
            }

            // Position History is additive in V175. Existing verified profiles
            // remain valid without this optional key and simply omit the section.
            (void)TryParseCompatibilityUnsigned(
                CompatibilityReadValue(path, section, L"tracker_position_history_page_offset"),
                positionHistoryPageOffset);

            const bool layoutValuesSane =
                tabViewVtableRva > 0 && tabViewVtableRva < imageSize &&
                accountsPageOffset > 0 && accountsPageOffset < 0x10000 &&
                openPositionsPageOffset > 0 && openPositionsPageOffset < 0x10000 &&
                (positionHistoryPageOffset == 0 || positionHistoryPageOffset < 0x10000) &&
                logsPageOffset > 0 && logsPageOffset < 0x10000 &&
                gridMemberOffset > 0 && gridMemberOffset < 0x10000 &&
                rowsOffset1 > 0 && rowsOffset1 < 0x10000 &&
                rowsOffset2 > rowsOffset1 && rowsOffset2 < 0x10000 &&
                getTextSlot > 0 && getTextSlot < 512 &&
                flexGridVtableRva > 0 && flexGridVtableRva < imageSize &&
                getTextRva > 0 && getTextRva < imageSize;
            if (!layoutValuesSane)
            {
                incompleteProfile = section + L" (invalid Tracker layout value)";
                continue;
            }

            unsigned long long accountsExtractorRva = 0;
            unsigned long long openPositionsExtractorRva = 0;
            (void)TryParseCompatibilityUnsigned(
                CompatibilityReadValue(path, section, L"tracker_accounts_extractor_rva"), accountsExtractorRva);
            (void)TryParseCompatibilityUnsigned(
                CompatibilityReadValue(path, section, L"tracker_open_positions_extractor_rva"), openPositionsExtractorRva);

            profile.matched = true;
            profile.externalVerified = true;
            profile.name = CompatibilityReadValue(path, section, L"name", section.c_str());
            profile.section = section;
            profile.source = path + L" [" + section + L"]";
            profile.mode = L"external_verified";
            profile.atonpTrackerPeTimestamp = snapshot.atonpTrackerPeTimestamp;
            profile.atonpTrackerImageSize = snapshot.atonpTrackerSize;
            profile.tabViewVtableRva = static_cast<std::uintptr_t>(tabViewVtableRva);
            profile.accountsExtractorRva = static_cast<std::uintptr_t>(accountsExtractorRva);
            profile.openPositionsExtractorRva = static_cast<std::uintptr_t>(openPositionsExtractorRva);
            profile.accountsPageOffset = static_cast<std::size_t>(accountsPageOffset);
            profile.openPositionsPageOffset = static_cast<std::size_t>(openPositionsPageOffset);
            profile.positionHistoryPageOffset = static_cast<std::size_t>(positionHistoryPageOffset);
            profile.logsPageOffset = static_cast<std::size_t>(logsPageOffset);
            profile.gridMemberOffset = static_cast<std::size_t>(gridMemberOffset);
            profile.rowsOffset1 = static_cast<std::size_t>(rowsOffset1);
            profile.rowsOffset2 = static_cast<std::size_t>(rowsOffset2);
            profile.getTextSlot = static_cast<std::size_t>(getTextSlot);
            profile.flexGridVtableRva = static_cast<std::uintptr_t>(flexGridVtableRva);
            profile.getTextRva = static_cast<std::uintptr_t>(getTextRva);
            profile.allowAdaptiveFlexGridIdentity = false;
            profile.diagnostic = L"Verified Tracker compatibility profile selected: " + profile.name;
            return profile;
        }

        // Record the exact fingerprint as a disabled research candidate whenever
        // no complete external verified Tracker profile is available. This is
        // safe even when a bundled validated baseline can keep an older layout
        // operational because Candidate.* sections are never selected.
        EnsureTrackerCandidateTemplate(snapshot);

        std::wostringstream diagnostic;
        if (!incompleteProfile.empty())
        {
            diagnostic << L"Tracker compatibility profile " << incompleteProfile
                       << L" matches the ATOnPTracker.dll fingerprint but is incomplete.";
        }
        else
        {
            diagnostic << L"No verified Tracker compatibility profile for ATOnPTracker.dll timestamp "
                       << CompatibilityHex(snapshot.atonpTrackerPeTimestamp)
                       << L", image size " << snapshot.atonpTrackerSize << L".";
        }
        profile.diagnostic = diagnostic.str();
        return profile;
    }

    TrackerCompatibilityProfile EmbeddedLegacyTrackerCompatibilityProfile(const Snapshot& snapshot)
    {
        TrackerCompatibilityProfile profile;
        if (!snapshot.atonpTrackerBase || snapshot.atonpTrackerSize != kKnownAtonpSize)
            return profile;
        profile.matched = true;
        profile.externalVerified = false;
        profile.name = L"Embedded validated Tracker baseline";
        profile.source = L"MCST-TrackerBridge.dll embedded compatibility";
        profile.mode = L"embedded_legacy";
        profile.atonpTrackerPeTimestamp = snapshot.atonpTrackerPeTimestamp;
        profile.atonpTrackerImageSize = snapshot.atonpTrackerSize;
        // The exact V147 image exposes CATPTTabView's primary RTTI vtable at
        // RVA 0x1D78C8. Supplying it only for that fingerprint authorizes the
        // same bounded fresh recovery scan used by external exact profiles.
        // Other same-size legacy images retain RTTI-only normal discovery.
        profile.tabViewVtableRva =
            snapshot.atonpTrackerPeTimestamp == kPositionHistoryAtonpTimestamp
                ? kKnownTabViewPrimaryVtableRva
                : 0;
        profile.accountsExtractorRva = kExtractAccountsRva;
        profile.openPositionsExtractorRva = kExtractOpenPositionsRva;
        profile.accountsPageOffset = 0x58;
        profile.openPositionsPageOffset = 0x68;
        // The exact V147 image was verified from the live CATPTTabView object:
        // tabs are stored in UI order at 8-byte intervals and Positions History
        // is the +0x78 page. Never apply this new offset to another timestamp.
        profile.positionHistoryPageOffset =
            snapshot.atonpTrackerPeTimestamp == kPositionHistoryAtonpTimestamp ? 0x78 : 0;
        profile.logsPageOffset = 0x80;
        profile.gridMemberOffset = 0x118;
        profile.rowsOffset1 = 0xD20;
        profile.rowsOffset2 = 0xD24;
        profile.getTextSlot = 60;
        profile.allowAdaptiveFlexGridIdentity = true;
        profile.diagnostic = L"Using the embedded validated Tracker baseline for the original authorized ATOnPTracker.dll image size.";
        return profile;
    }

    // ITS_TradingCenter::ITC_TradeInfo IID observed in ATOnPTracker symbols.
    const GUID kIidTradeInfo =
        { 0x37f57e35, 0x641f, 0x4a43, { 0xbe, 0x8a, 0x66, 0xa7, 0x29, 0x97, 0x6d, 0xf1 } };

    struct ExtractorAttempt
    {
        std::string name;
        std::uintptr_t function = 0;
        std::uintptr_t tabView = 0;
        std::uintptr_t tradeInfo = 0;
        std::uintptr_t returnedInterface = 0;
        std::uintptr_t returnedVtable = 0;
        bool functionValid = false;
        bool tabViewValid = false;
        bool tradeInfoValid = false;
        bool gateEnabled = false;
        bool liveAttempted = false;
        bool liveSucceeded = false;
        DWORD sehCode = 0;
        HRESULT queryInterfaceHr = E_FAIL;
        std::string diagnostic;
    };

    struct TabViewCandidate
    {
        std::uintptr_t object = 0;
        std::uintptr_t vtable = 0;
        std::uintptr_t currentVtable = 0;
        std::uintptr_t secondaryVtable = 0;
        std::uintptr_t allocationBase = 0;
        std::uintptr_t regionBase = 0;
        std::size_t regionSize = 0;
        DWORD memoryType = 0;
        DWORD memoryProtect = 0;
        int trackerWindowReferences = 0;
        int pageWindowReferences = 0;
        int flexGridReferences = 0;
        int pageObjectPointers = 0;
        int debugFillQwords = 0;
        int bridgeModulePointers = 0;
        bool stableVtable = false;
        bool secondaryTabViewVtableAt48 = false;
        bool trackerLayoutSignature = false;
        bool rejected = false;
        std::string rejectedReason;
        int score = 0;
    };

    void ValidateAndScoreTabViewCandidate(
        const Snapshot& snapshot,
        const std::vector<std::uintptr_t>& targets,
        TabViewCandidate& candidate);

    SRWLOCK g_extractorLock = SRWLOCK_INIT;
    ExtractorAttempt g_accountsAttempt;
    ExtractorAttempt g_openPositionsAttempt;
    SRWLOCK g_tabViewCacheLock = SRWLOCK_INIT;
    std::uintptr_t g_cachedTabView = 0;
    std::uintptr_t g_cachedAtonpBase = 0;
    HWND g_cachedTrackerWindow = nullptr;
    SRWLOCK g_candidateCacheLock = SRWLOCK_INIT;
    std::vector<TabViewCandidate> g_cachedCandidates;
    std::string g_cachedCandidateScanDiagnostic;
    std::uintptr_t g_candidateCacheAtonpBase = 0;
    HWND g_candidateCacheTrackerWindow = nullptr;
    bool g_candidateCacheValid = false;
    ULONGLONG g_candidateCacheTick = 0;
    std::atomic<ULONGLONG> g_lastTabViewRecoveryTick{ 0 };
    std::atomic<bool> g_tabViewRecoveryModeActive{ false };
    std::atomic<bool> g_tabViewPersistentFailureActive{ false };
    std::atomic<unsigned int> g_tabViewRecoveryFailureStreak{ 0 };
    std::atomic<ULONGLONG> g_lastExpandedTabViewRecoveryTick{ 0 };
    std::atomic<ULONGLONG> g_lastWideTabViewRecoveryTick{ 0 };
    SRWLOCK g_tabViewHistoryLock = SRWLOCK_INIT;
    std::vector<std::uintptr_t> g_recentTabViewHints;
    constexpr ULONGLONG kTabViewRecoveryCooldownMs = 30000;
    constexpr ULONGLONG kTabViewCandidateCacheTtlMs = 30000;

    void RememberTabViewHint(std::uintptr_t object)
    {
        if (!object)
            return;
        AcquireSRWLockExclusive(&g_tabViewHistoryLock);
        g_recentTabViewHints.erase(
            std::remove(g_recentTabViewHints.begin(), g_recentTabViewHints.end(), object),
            g_recentTabViewHints.end());
        g_recentTabViewHints.insert(g_recentTabViewHints.begin(), object);
        if (g_recentTabViewHints.size() > 8)
            g_recentTabViewHints.resize(8);
        ReleaseSRWLockExclusive(&g_tabViewHistoryLock);
    }

    std::vector<std::uintptr_t> RecentTabViewHints()
    {
        AcquireSRWLockShared(&g_tabViewHistoryLock);
        const std::vector<std::uintptr_t> result = g_recentTabViewHints;
        ReleaseSRWLockShared(&g_tabViewHistoryLock);
        return result;
    }

    void InvalidateTabViewCaches()
    {
        // These are Bridge-owned, read-only discovery caches. Clearing them does
        // not write to MultiCharts memory or invoke an unknown target function.
        // Keep the two locks independent so cache invalidation cannot introduce
        // a lock-order dependency into the live Tracker read path.
        AcquireSRWLockExclusive(&g_tabViewCacheLock);
        g_cachedTabView = 0;
        g_cachedAtonpBase = 0;
        g_cachedTrackerWindow = nullptr;
        ReleaseSRWLockExclusive(&g_tabViewCacheLock);

        AcquireSRWLockExclusive(&g_candidateCacheLock);
        g_cachedCandidates.clear();
        g_cachedCandidateScanDiagnostic.clear();
        g_candidateCacheAtonpBase = 0;
        g_candidateCacheTrackerWindow = nullptr;
        g_candidateCacheValid = false;
        g_candidateCacheTick = 0;
        ReleaseSRWLockExclusive(&g_candidateCacheLock);
    }

    bool TryBeginTabViewRecovery()
    {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG previous = g_lastTabViewRecoveryTick.load();
        for (;;)
        {
            if (previous != 0 && now - previous < kTabViewRecoveryCooldownMs)
                return false;
            if (g_lastTabViewRecoveryTick.compare_exchange_weak(previous, now))
            {
                g_tabViewRecoveryModeActive.store(true);
                return true;
            }
        }
    }

    struct TabViewRecoveryModeScope
    {
        ~TabViewRecoveryModeScope()
        {
            // Recovery mode describes only the currently executing bounded
            // exact-profile scan. A failed scan must never suppress normal
            // discovery on later Watchdog requests.
            g_tabViewRecoveryModeActive.store(false);
        }
    };

    constexpr UINT kUiExtractorDispatchMessage = WM_APP + 0x4B3;
    constexpr DWORD kUiExtractorDispatchTimeoutMs = 15000;

    struct UiDispatchDiagnostics
    {
        DWORD requestReceivedThreadId = 0;
        DWORD dispatchTargetThreadId = 0;
        bool dispatchPosted = false;
        bool uiCallbackEntered = false;
        DWORD uiCallbackThreadId = 0;
        bool uiCallbackCompleted = false;
        DWORD waitResult = WAIT_FAILED;
        ULONGLONG waitElapsedMs = 0;
        bool pipeResponseWritten = false;
        std::string diagnostic;
    };

    SRWLOCK g_uiDispatchLock = SRWLOCK_INIT;
    HANDLE g_uiDispatchEvent = nullptr;
    HWND g_uiDispatchWindow = nullptr;
    WNDPROC g_uiDispatchOriginalWndProc = nullptr;
    bool g_uiDispatchActive = false;
    bool g_uiDispatchNoop = false;
    ExtractorAttempt g_uiDispatchAccounts;
    ExtractorAttempt g_uiDispatchPositions;
    UiDispatchDiagnostics g_uiDispatchDiagnostics;

    bool MemoryRangeHasProtection(const void* address, std::size_t bytes, bool executable)
    {
        if (!address || bytes == 0)
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(address);
        const auto regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd = regionBegin + mbi.RegionSize;
        if (begin < regionBegin || begin + bytes < begin || begin + bytes > regionEnd)
            return false;
        const DWORD basic = mbi.Protect & 0xFFu;
        if (executable)
        {
            return basic == PAGE_EXECUTE || basic == PAGE_EXECUTE_READ ||
                   basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
        }
        return basic == PAGE_READONLY || basic == PAGE_READWRITE ||
               basic == PAGE_WRITECOPY || basic == PAGE_EXECUTE_READ ||
               basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
    }

    bool SafeReadBytes(const void* address, void* destination, std::size_t bytes)
    {
        if (!address || !destination || bytes == 0 ||
            !MemoryRangeHasProtection(address, bytes, false))
            return false;

        DWORD sehCode = 0;
        return MCBridge_SafeCopyMemory(destination, address, bytes, &sehCode) != 0;
    }

    template <typename T>
    bool SafeReadValue(const void* address, T& value)
    {
        return SafeReadBytes(address, &value, sizeof(T));
    }

    bool PointerLooksLikeObject(std::uintptr_t pointer)
    {
        if (pointer < 0x10000 || !MemoryRangeHasProtection(reinterpret_cast<void*>(pointer), sizeof(void*), false))
            return false;
        std::uintptr_t vtable = 0;
        if (!SafeReadValue(reinterpret_cast<void*>(pointer), vtable))
            return false;
        return MemoryRangeHasProtection(reinterpret_cast<void*>(vtable), sizeof(void*), false);
    }

    bool VtableStartsWithExecutableCode(std::uintptr_t object, std::uintptr_t& vtable)
    {
        vtable = 0;
        if (!PointerLooksLikeObject(object) || !SafeReadValue(reinterpret_cast<void*>(object), vtable))
            return false;
        std::uintptr_t firstMethod = 0;
        return SafeReadValue(reinterpret_cast<void*>(vtable), firstMethod) &&
               MemoryRangeHasProtection(reinterpret_cast<void*>(firstMethod), 1, true);
    }

    std::uintptr_t DecodeAtlThunkThis(LONG_PTR wndProc)
    {
        const auto address = static_cast<std::uintptr_t>(wndProc);
        if (!MemoryRangeHasProtection(reinterpret_cast<void*>(address), 48, true))
            return 0;
        unsigned char bytes[48]{};
DWORD sehCode = 0;
        if (!MCBridge_SafeCopyMemory(bytes, reinterpret_cast<void*>(address), sizeof(bytes), &sehCode))
            return 0;
        for (std::size_t i = 0; i + 10 <= sizeof(bytes); ++i)
        {
            if (bytes[i] == 0x48 && bytes[i + 1] == 0xB9) // mov rcx, imm64
            {
                std::uint64_t candidate = 0;
                memcpy(&candidate, bytes + i + 2, sizeof(candidate));
                std::uintptr_t vtable = 0;
                if (VtableStartsWithExecutableCode(static_cast<std::uintptr_t>(candidate), vtable))
                    return static_cast<std::uintptr_t>(candidate);
            }
        }
        return 0;
    }

    bool IsReadableProtection(DWORD protect)
    {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        const DWORD basic = protect & 0xFFu;
        return basic == PAGE_READONLY || basic == PAGE_READWRITE ||
               basic == PAGE_WRITECOPY || basic == PAGE_EXECUTE_READ ||
               basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
    }

    int CountWindowReferencesInObject(
        std::uintptr_t object,
        const std::vector<std::uintptr_t>& values,
        std::size_t bytes = 0x500)
    {
        if (!object || values.empty() ||
            !MemoryRangeHasProtection(reinterpret_cast<void*>(object), bytes, false))
            return 0;

        int count = 0;
        for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= bytes;
             offset += sizeof(std::uintptr_t))
        {
            std::uintptr_t value = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(object + offset), value))
                continue;
            if (std::find(values.begin(), values.end(), value) != values.end())
                ++count;
        }
        return count;
    }

    std::vector<TabViewCandidate> FindProfileTabViewVtableObjects(
        const Snapshot& snapshot,
        std::uintptr_t tabViewVtableRva,
        std::uint64_t expectedImageSize,
        std::string& scanDiagnostic,
        ULONGLONG timeBudgetMs = 30000,
        std::size_t maxTotalInspectedBytes = 32ull * 1024ull * 1024ull)
    {
        // Build a small set of live ATL/WTL object anchors from Tracker-owned
        // window thunks and scan only their allocation neighborhoods for the exact
        // known CATPTTabView vtable. Callers choose the time/byte budget (the
        // diagnostic default is 30 seconds; production recovery uses 1.5 seconds).
        constexpr std::size_t kMaxAllocationBytes = 8ull * 1024ull * 1024ull;
        constexpr std::size_t kMaxCandidates = 64;

        const std::uintptr_t targetVtable = snapshot.atonpTrackerBase + tabViewVtableRva;
        const std::vector<std::uintptr_t> targetVtables = { targetVtable };
        std::vector<TabViewCandidate> candidates;
        if (!snapshot.atonpTrackerBase || tabViewVtableRva == 0 ||
            snapshot.atonpTrackerSize != expectedImageSize ||
            tabViewVtableRva >= snapshot.atonpTrackerSize ||
            !MemoryRangeHasProtection(reinterpret_cast<void*>(targetVtable), sizeof(void*), false))
        {
            scanDiagnostic = "profile CATPTTabView vtable unavailable or ATOnPTracker fingerprint/layout mismatch";
            return candidates;
        }

        std::vector<std::uintptr_t> trackerValues;
        std::vector<std::uintptr_t> pageValues;
        std::vector<std::uintptr_t> gridValues;
        std::vector<std::uintptr_t> anchors;
        if (snapshot.trackerWindow)
            trackerValues.push_back(reinterpret_cast<std::uintptr_t>(snapshot.trackerWindow));

        for (const WindowRecord& record : snapshot.windows)
        {
            const std::uintptr_t hwndValue = reinterpret_cast<std::uintptr_t>(record.hwnd);
            if (Lower(record.className) == L"#32770")
                pageValues.push_back(hwndValue);
            if (IsFlexGridClass(record.className))
                gridValues.push_back(hwndValue);

            const std::uintptr_t classObject = ParseAtlClassAddress(record.className);
            if (classObject)
                anchors.push_back(classObject);
            const std::uintptr_t thunkObject = DecodeAtlThunkThis(record.wndProc);
            if (thunkObject)
                anchors.push_back(thunkObject);
            const std::uintptr_t userObject = static_cast<std::uintptr_t>(record.userData);
            std::uintptr_t userVtable = 0;
            if (userObject && VtableStartsWithExecutableCode(userObject, userVtable))
                anchors.push_back(userObject);
        }

        // A recreated Tracker object commonly stays in the same allocator
        // neighborhood even when its exact address changes. Retain only Bridge-
        // owned address hints and validate every one again with VirtualQuery.
        // This expands recovery without dereferencing a stale pointer directly.
        const std::vector<std::uintptr_t> historicalHints = RecentTabViewHints();
        anchors.insert(anchors.end(), historicalHints.begin(), historicalHints.end());

        std::sort(anchors.begin(), anchors.end());
        anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());

        struct AllocationRange { std::uintptr_t begin = 0; std::uintptr_t end = 0; };
        std::vector<AllocationRange> ranges;
        for (const std::uintptr_t anchor : anchors)
        {
            MEMORY_BASIC_INFORMATION anchorMbi{};
            if (VirtualQuery(reinterpret_cast<void*>(anchor), &anchorMbi, sizeof(anchorMbi)) != sizeof(anchorMbi) ||
                anchorMbi.State != MEM_COMMIT || !IsReadableProtection(anchorMbi.Protect))
                continue;

            const std::uintptr_t allocationBase =
                reinterpret_cast<std::uintptr_t>(anchorMbi.AllocationBase);
            std::uintptr_t cursor = allocationBase;
            std::uintptr_t allocationEnd = allocationBase;
            std::size_t allocationBytes = 0;
            while (allocationBytes < kMaxAllocationBytes)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                    reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) != allocationBase)
                    break;
                const std::uintptr_t regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                if (mbi.RegionSize == 0 || regionBegin + mbi.RegionSize <= regionBegin)
                    break;
                allocationEnd = regionBegin + mbi.RegionSize;
                allocationBytes = static_cast<std::size_t>(allocationEnd - allocationBase);
                cursor = allocationEnd;
            }
            if (allocationEnd > allocationBase && allocationBytes <= kMaxAllocationBytes)
                ranges.push_back({ allocationBase, allocationEnd });
        }

        std::sort(ranges.begin(), ranges.end(), [](const AllocationRange& a, const AllocationRange& b) {
            if (a.begin != b.begin) return a.begin < b.begin;
            return a.end < b.end;
        });
        ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const AllocationRange& a, const AllocationRange& b) {
            return a.begin == b.begin && a.end == b.end;
        }), ranges.end());

        const ULONGLONG started = GetTickCount64();
        std::size_t inspectedBytes = 0;
        std::size_t readableRegions = 0;
        std::size_t exactVtableHits = 0;
        std::size_t invalidVtableHits = 0;
        std::size_t structurallyRejected = 0;
        int strongestRejectedScore = 0;
        std::string strongestRejectionReason;
        bool timedOut = false;
        bool byteLimitHit = false;
        bool candidateLimitHit = false;

        for (const AllocationRange& range : ranges)
        {
            if (GetTickCount64() - started >= timeBudgetMs)
            {
                timedOut = true;
                break;
            }
            if (inspectedBytes >= maxTotalInspectedBytes)
            {
                byteLimitHit = true;
                break;
            }

            std::uintptr_t cursor = range.begin;
            while (cursor < range.end)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
                    break;
                const std::uintptr_t regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                const std::uintptr_t regionEnd = regionBegin + mbi.RegionSize;
                if (mbi.RegionSize == 0 || regionEnd <= cursor)
                    break;

                if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect))
                {
                    ++readableRegions;
                    const std::uintptr_t scanBegin = (std::max)(cursor, regionBegin);
                    const std::size_t remainingByteBudget =
                        maxTotalInspectedBytes - inspectedBytes;
                    const std::uintptr_t byteBudgetEnd =
                        remainingByteBudget > (std::numeric_limits<std::uintptr_t>::max)() - scanBegin
                            ? (std::numeric_limits<std::uintptr_t>::max)()
                            : scanBegin + remainingByteBudget;
                    const std::uintptr_t scanEnd =
                        (std::min)((std::min)(range.end, regionEnd), byteBudgetEnd);
                    inspectedBytes += static_cast<std::size_t>(scanEnd - scanBegin);

                    for (std::uintptr_t address = scanBegin;
                         scanEnd >= sizeof(std::uintptr_t) &&
                         address <= scanEnd - sizeof(std::uintptr_t);
                         address += sizeof(std::uintptr_t))
                    {
                        if ((address & 0x3FFFu) == 0 && GetTickCount64() - started >= timeBudgetMs)
                        {
                            timedOut = true;
                            break;
                        }
                        std::uintptr_t value = 0;
                        if (!SafeReadValue(reinterpret_cast<void*>(address), value) || value != targetVtable)
                            continue;
                        ++exactVtableHits;

                        TabViewCandidate candidate;
                        candidate.object = address;
                        candidate.vtable = value;
                        std::uintptr_t verifiedVtable = 0;
                        if (!VtableStartsWithExecutableCode(candidate.object, verifiedVtable) ||
                            verifiedVtable != targetVtable)
                        {
                            ++invalidVtableHits;
                            continue;
                        }

                        candidate.trackerWindowReferences =
                            CountWindowReferencesInObject(candidate.object, trackerValues);
                        candidate.pageWindowReferences =
                            CountWindowReferencesInObject(candidate.object, pageValues, 0x2000);
                        candidate.flexGridReferences =
                            CountWindowReferencesInObject(candidate.object, gridValues, 0x2000);
                        ValidateAndScoreTabViewCandidate(snapshot, targetVtables, candidate);
                        if (candidate.rejected)
                        {
                            ++structurallyRejected;
                            if (candidate.score >= strongestRejectedScore)
                            {
                                strongestRejectedScore = candidate.score;
                                strongestRejectionReason = candidate.rejectedReason;
                            }
                            continue;
                        }
                        candidates.push_back(candidate);
                        if (candidates.size() >= kMaxCandidates)
                        {
                            candidateLimitHit = true;
                            break;
                        }
                    }
                }
                if (inspectedBytes >= maxTotalInspectedBytes)
                    byteLimitHit = true;
                if (timedOut || byteLimitHit || candidateLimitHit)
                    break;
                cursor = regionEnd;
            }
            if (timedOut || byteLimitHit || candidateLimitHit)
                break;
        }

        std::sort(candidates.begin(), candidates.end(), [](const TabViewCandidate& left,
                                                            const TabViewCandidate& right) {
            if (left.score != right.score) return left.score > right.score;
            return left.object < right.object;
        });
        candidates.erase(std::unique(candidates.begin(), candidates.end(),
            [](const TabViewCandidate& left, const TabViewCandidate& right) {
                return left.object == right.object;
            }), candidates.end());

        std::ostringstream diag;
        diag << "targeted locator anchors=" << anchors.size()
             << " allocations=" << ranges.size()
             << " readable_regions=" << readableRegions
             << " inspected_bytes=" << inspectedBytes
             << " elapsed_ms=" << (GetTickCount64() - started)
             << " exact_vtable_hits=" << exactVtableHits
             << " invalid_vtable_hits=" << invalidVtableHits
             << " structurally_rejected=" << structurallyRejected
             << " accepted_candidates=" << candidates.size();
        if (timedOut) diag << " timeout=true";
        if (byteLimitHit) diag << " byte_limit=true";
        if (candidateLimitHit) diag << " candidate_limit=true";
        if (!strongestRejectionReason.empty())
            diag << " strongest_rejected_score=" << strongestRejectedScore
                 << " strongest_rejection=" << strongestRejectionReason;
        scanDiagnostic = diag.str();
        return candidates;
    }


    struct NonVisualObjectRecord
    {
        std::uintptr_t object = 0;
        std::uintptr_t vtable = 0;
        std::vector<std::uintptr_t> methods;
        int accessorLikeMethods = 0;
    };

    bool LooksLikeSmallAccessor(std::uintptr_t function)
    {
        unsigned char bytes[48]{};
        if (!MemoryRangeHasProtection(reinterpret_cast<void*>(function), sizeof(bytes), true))
            return false;
DWORD sehCode = 0;
        if (!MCBridge_SafeCopyMemory(bytes, reinterpret_cast<void*>(function), sizeof(bytes), &sehCode))
            return false;
        bool sawCall = false;
        for (std::size_t i = 0; i < sizeof(bytes); ++i)
        {
            if (bytes[i] == 0xE8 || (bytes[i] == 0xFF && i + 1 < sizeof(bytes) && (bytes[i + 1] & 0x38) == 0x10))
                sawCall = true;
            if (bytes[i] == 0xC3 || bytes[i] == 0xC2)
                return i <= 32 && !sawCall;
        }
        return false;
    }

    std::string DiscoverRttiNames(const Snapshot& snapshot)
    {
        std::ostringstream out;
        if (!snapshot.atonpTrackerBase || !snapshot.atonpTrackerSize)
            return "RTTI module unavailable\r\n";
        constexpr std::size_t kMaxNames = 256;
        std::size_t names = 0;
        const std::uintptr_t begin = snapshot.atonpTrackerBase;
        const std::uintptr_t end = begin + snapshot.atonpTrackerSize;
        for (std::uintptr_t cursor = begin; cursor < end && names < kMaxNames; )
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
                break;
            const std::uintptr_t regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::uintptr_t regionEnd = regionBegin + mbi.RegionSize;
            if (mbi.RegionSize == 0 || regionEnd <= cursor)
                break;
            if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect))
            {
                const std::uintptr_t scanBegin = (std::max)(cursor, regionBegin);
                const std::uintptr_t scanEnd = (std::min)(end, regionEnd);
                const std::size_t size = static_cast<std::size_t>(scanEnd - scanBegin);
                std::vector<char> bytes(size);
                DWORD sehCode = 0;
                const bool copied = MCBridge_SafeCopyMemory(
                    bytes.data(), reinterpret_cast<void*>(scanBegin), size, &sehCode) != 0;
                if (copied)
                {
                    for (std::size_t i = 0; i + 8 < bytes.size() && names < kMaxNames; ++i)
                    {
                        if (memcmp(bytes.data() + i, ".?AV", 4) != 0)
                            continue;
                        std::size_t length = 0;
                        while (i + length < bytes.size() && length < 255)
                        {
                            const unsigned char ch = static_cast<unsigned char>(bytes[i + length]);
                            if (ch == 0) break;
                            if (ch < 0x20 || ch > 0x7E) { length = 0; break; }
                            ++length;
                        }
                        if (length >= 6 && i + length < bytes.size() && bytes[i + length] == '\0')
                        {
                            const std::uintptr_t address = scanBegin + i;
                            out << "rtti_type_descriptor_name="
                                << std::string(bytes.data() + i, length)
                                << " address=" << HexValue(address)
                                << " rva=" << HexValue(address - snapshot.atonpTrackerBase) << "\r\n";
                            ++names;
                            i += length;
                        }
                    }
                }
            }
            cursor = regionEnd;
        }
        out << "rtti_name_count=" << names << "\r\n";
        return out.str();
    }

    std::string WriteNonVisualObjectMethodReport(const Snapshot& snapshot)
    {
        constexpr ULONGLONG kTimeBudgetMs = 20000;
        constexpr std::size_t kMaxAllocationBytes = 8ull * 1024ull * 1024ull;
        constexpr std::size_t kMaxTotalBytes = 16ull * 1024ull * 1024ull;
        constexpr std::size_t kMaxObjects = 256;
        constexpr std::size_t kMaxMethods = 16;

        std::vector<std::uintptr_t> anchors;
        for (const WindowRecord& record : snapshot.windows)
        {
            const std::uintptr_t classObject = ParseAtlClassAddress(record.className);
            if (classObject) anchors.push_back(classObject);
            const std::uintptr_t thunkObject = DecodeAtlThunkThis(record.wndProc);
            if (thunkObject) anchors.push_back(thunkObject);
        }
        std::sort(anchors.begin(), anchors.end());
        anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());

        struct Range { std::uintptr_t begin = 0; std::uintptr_t end = 0; };
        std::vector<Range> ranges;
        for (const std::uintptr_t anchor : anchors)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(anchor), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect))
                continue;
            const std::uintptr_t allocationBase = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
            std::uintptr_t cursor = allocationBase;
            std::uintptr_t allocationEnd = allocationBase;
            while (allocationEnd - allocationBase < kMaxAllocationBytes)
            {
                MEMORY_BASIC_INFORMATION part{};
                if (VirtualQuery(reinterpret_cast<void*>(cursor), &part, sizeof(part)) != sizeof(part) ||
                    reinterpret_cast<std::uintptr_t>(part.AllocationBase) != allocationBase || part.RegionSize == 0)
                    break;
                const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
                if (next <= cursor) break;
                allocationEnd = next;
                cursor = next;
            }
            if (allocationEnd > allocationBase && allocationEnd - allocationBase <= kMaxAllocationBytes)
                ranges.push_back({ allocationBase, allocationEnd });
        }
        std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
        ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
            return a.begin == b.begin && a.end == b.end;
        }), ranges.end());

        const ULONGLONG started = GetTickCount64();
        std::size_t inspected = 0;
        std::vector<NonVisualObjectRecord> objects;
        for (const Range& range : ranges)
        {
            for (std::uintptr_t address = range.begin;
                 address + sizeof(std::uintptr_t) <= range.end && objects.size() < kMaxObjects;
                 address += sizeof(std::uintptr_t))
            {
                if (GetTickCount64() - started >= kTimeBudgetMs || inspected >= kMaxTotalBytes)
                    break;
                inspected += sizeof(std::uintptr_t);
                std::uintptr_t vtable = 0;
                if (!SafeReadValue(reinterpret_cast<void*>(address), vtable)) continue;
                if (vtable < snapshot.atonpTrackerBase ||
                    vtable >= snapshot.atonpTrackerBase + snapshot.atonpTrackerSize) continue;
                std::uintptr_t firstMethod = 0;
                if (!SafeReadValue(reinterpret_cast<void*>(vtable), firstMethod) ||
                    !MemoryRangeHasProtection(reinterpret_cast<void*>(firstMethod), 1, true)) continue;

                NonVisualObjectRecord record;
                record.object = address;
                record.vtable = vtable;
                for (std::size_t slot = 0; slot < kMaxMethods; ++slot)
                {
                    std::uintptr_t method = 0;
                    if (!SafeReadValue(reinterpret_cast<void*>(vtable + slot * sizeof(void*)), method)) break;
                    if (method < snapshot.atonpTrackerBase ||
                        method >= snapshot.atonpTrackerBase + snapshot.atonpTrackerSize ||
                        !MemoryRangeHasProtection(reinterpret_cast<void*>(method), 1, true)) break;
                    record.methods.push_back(method);
                    if (LooksLikeSmallAccessor(method)) ++record.accessorLikeMethods;
                }
                objects.push_back(record);
            }
            if (GetTickCount64() - started >= kTimeBudgetMs || inspected >= kMaxTotalBytes || objects.size() >= kMaxObjects)
                break;
        }
        std::sort(objects.begin(), objects.end(), [](const NonVisualObjectRecord& a, const NonVisualObjectRecord& b) {
            if (a.vtable != b.vtable) return a.vtable < b.vtable;
            return a.object < b.object;
        });
        objects.erase(std::unique(objects.begin(), objects.end(), [](const NonVisualObjectRecord& a, const NonVisualObjectRecord& b) {
            return a.object == b.object;
        }), objects.end());

        std::ostringstream report;
        report << "MC V147 Non-Visual C++ Object and Method Discovery\r\n"
               << "=================================================\r\n"
               << "process_id=" << snapshot.processId << "\r\n"
               << "atonptracker_base=" << HexValue(snapshot.atonpTrackerBase) << "\r\n"
               << "anchors=" << anchors.size() << " allocations=" << ranges.size()
               << " inspected_bytes=" << inspected << " elapsed_ms=" << (GetTickCount64() - started)
               << " object_candidates=" << objects.size() << "\r\n\r\n"
               << "RTTI TYPE NAMES\r\n"
               << "---------------\r\n" << DiscoverRttiNames(snapshot) << "\r\n"
               << "OBJECTS AND VTABLE METHODS\r\n"
               << "--------------------------\r\n";
        for (const NonVisualObjectRecord& object : objects)
        {
            report << "object=" << HexValue(object.object)
                   << " vtable=" << HexValue(object.vtable)
                   << " vtable_rva=" << HexValue(object.vtable - snapshot.atonpTrackerBase)
                   << " method_count=" << object.methods.size()
                   << " accessor_like=" << object.accessorLikeMethods << "\r\n";
            for (std::size_t slot = 0; slot < object.methods.size(); ++slot)
            {
                report << "  slot=" << slot
                       << " method=" << HexValue(object.methods[slot])
                       << " method_rva=" << HexValue(object.methods[slot] - snapshot.atonpTrackerBase)
                       << " accessor_like=" << (LooksLikeSmallAccessor(object.methods[slot]) ? "true" : "false")
                       << "\r\n";
            }
        }
        EnsureOutputDirectory();
        const std::wstring path = ReportPath(L"MC_V147_Bridge_NonVisual_Object_Method_Discovery", snapshot.processId);
        const bool written = WriteUtf8File(path, report.str());
        std::ostringstream summary;
        summary << "nonvisual_report=" << WideToUtf8(path)
                << " written=" << (written ? "true" : "false")
                << " anchors=" << anchors.size()
                << " allocations=" << ranges.size()
                << " objects=" << objects.size()
                << " inspected_bytes=" << inspected;
        return summary.str();
    }

    struct RttiVtableRecord
    {
        std::uintptr_t typeNameAddress = 0;
        std::uintptr_t typeDescriptor = 0;
        std::uintptr_t completeObjectLocator = 0;
        std::uintptr_t vtable = 0;
    };

    bool ModuleContains(const Snapshot& snapshot, std::uintptr_t address, std::size_t bytes = 1)
    {
        if (!snapshot.atonpTrackerBase || !snapshot.atonpTrackerSize || bytes == 0)
            return false;
        const std::uintptr_t end = snapshot.atonpTrackerBase + snapshot.atonpTrackerSize;
        return address >= snapshot.atonpTrackerBase && address + bytes >= address && address + bytes <= end;
    }

    std::vector<std::uintptr_t> FindAsciiStringInModule(
        const Snapshot& snapshot,
        const char* needle)
    {
        std::vector<std::uintptr_t> matches;
        if (!needle || !*needle || !snapshot.atonpTrackerBase || !snapshot.atonpTrackerSize)
            return matches;
        const std::size_t needleLength = strlen(needle);
        constexpr std::size_t kChunkBytes = 256u * 1024u;
        std::vector<unsigned char> buffer(kChunkBytes + 512u);
        const std::uintptr_t moduleEnd = snapshot.atonpTrackerBase + snapshot.atonpTrackerSize;
        for (std::uintptr_t cursor = snapshot.atonpTrackerBase; cursor < moduleEnd; )
        {
            const std::size_t bytes = static_cast<std::size_t>((std::min)(
                moduleEnd - cursor, static_cast<std::uintptr_t>(kChunkBytes)));
            DWORD sehCode = 0;
            if (MCBridge_SafeCopyMemory(buffer.data(), reinterpret_cast<void*>(cursor), bytes, &sehCode))
            {
                for (std::size_t i = 0; i + needleLength < bytes; ++i)
                {
                    if (memcmp(buffer.data() + i, needle, needleLength) == 0 &&
                        buffer[i + needleLength] == 0)
                    {
                        matches.push_back(cursor + i);
                    }
                }
            }
            cursor += bytes;
        }
        return matches;
    }

    std::vector<RttiVtableRecord> ResolveRttiVtables(
        const Snapshot& snapshot,
        const char* decoratedTypeName,
        std::string& diagnostic)
    {
        std::vector<RttiVtableRecord> result;
        const std::vector<std::uintptr_t> names = FindAsciiStringInModule(snapshot, decoratedTypeName);
        std::size_t colCandidates = 0;
        std::size_t colValidated = 0;
        std::size_t vtableReferences = 0;
        if (!snapshot.atonpTrackerBase || !snapshot.atonpTrackerSize)
        {
            diagnostic = "RTTI module unavailable";
            return result;
        }

        const std::uintptr_t moduleBegin = snapshot.atonpTrackerBase;
        const std::uintptr_t moduleEnd = moduleBegin + snapshot.atonpTrackerSize;
        constexpr std::size_t kTypeDescriptorPrefixBytes = sizeof(void*) * 2;
        constexpr std::size_t kColBytes = 6 * sizeof(std::int32_t);
        constexpr std::size_t kChunkBytes = 256u * 1024u;
        std::vector<unsigned char> buffer(kChunkBytes + 16u);
        std::vector<unsigned char> referenceBuffer(kChunkBytes + 16u);

        for (const std::uintptr_t nameAddress : names)
        {
            if (nameAddress < moduleBegin + kTypeDescriptorPrefixBytes)
                continue;
            const std::uintptr_t typeDescriptor = nameAddress - kTypeDescriptorPrefixBytes;
            const std::uint32_t typeDescriptorRva =
                static_cast<std::uint32_t>(typeDescriptor - moduleBegin);

            // MSVC x64 CompleteObjectLocator stores image-relative 32-bit fields.
            for (std::uintptr_t cursor = moduleBegin; cursor < moduleEnd; )
            {
                const std::size_t bytes = static_cast<std::size_t>((std::min)(
                    moduleEnd - cursor, static_cast<std::uintptr_t>(kChunkBytes)));
                DWORD sehCode = 0;
                if (MCBridge_SafeCopyMemory(buffer.data(), reinterpret_cast<void*>(cursor), bytes, &sehCode))
                {
                    for (std::size_t i = 12; i + sizeof(std::uint32_t) <= bytes; i += 4)
                    {
                        std::uint32_t value = 0;
                        memcpy(&value, buffer.data() + i, sizeof(value));
                        if (value != typeDescriptorRva)
                            continue;
                        ++colCandidates;
                        const std::uintptr_t col = cursor + i - 12;
                        if (!ModuleContains(snapshot, col, kColBytes))
                            continue;
                        std::int32_t fields[6]{};
                        if (!SafeReadValue(reinterpret_cast<void*>(col), fields))
                            continue;
                        const std::uint32_t signature = static_cast<std::uint32_t>(fields[0]);
                        const std::uint32_t classDescriptorRva = static_cast<std::uint32_t>(fields[4]);
                        const std::uint32_t selfRva = static_cast<std::uint32_t>(fields[5]);
                        if (signature > 1 || static_cast<std::uint32_t>(fields[3]) != typeDescriptorRva)
                            continue;
                        if (classDescriptorRva >= snapshot.atonpTrackerSize)
                            continue;
                        if (signature == 1 && selfRva != static_cast<std::uint32_t>(col - moduleBegin))
                            continue;
                        ++colValidated;

                        // A vftable is immediately after an absolute pointer to the COL.
                        for (std::uintptr_t refCursor = moduleBegin; refCursor < moduleEnd; )
                        {
                            const std::size_t refBytes = static_cast<std::size_t>((std::min)(
                                moduleEnd - refCursor, static_cast<std::uintptr_t>(kChunkBytes)));
                            DWORD refSeh = 0;
                            if (MCBridge_SafeCopyMemory(referenceBuffer.data(), reinterpret_cast<void*>(refCursor), refBytes, &refSeh))
                            {
                                for (std::size_t j = 0; j + sizeof(std::uintptr_t) <= refBytes; j += sizeof(std::uintptr_t))
                                {
                                    std::uintptr_t pointer = 0;
                                    memcpy(&pointer, referenceBuffer.data() + j, sizeof(pointer));
                                    if (pointer != col)
                                        continue;
                                    ++vtableReferences;
                                    const std::uintptr_t vtable = refCursor + j + sizeof(std::uintptr_t);
                                    std::uintptr_t firstMethod = 0;
                                    if (!ModuleContains(snapshot, vtable, sizeof(void*)) ||
                                        !SafeReadValue(reinterpret_cast<void*>(vtable), firstMethod) ||
                                        !MemoryRangeHasProtection(reinterpret_cast<void*>(firstMethod), 1, true))
                                        continue;
                                    result.push_back({ nameAddress, typeDescriptor, col, vtable });
                                }
                            }
                            refCursor += refBytes;
                        }
                    }
                }
                cursor += bytes;
            }
        }

        std::sort(result.begin(), result.end(), [](const RttiVtableRecord& a, const RttiVtableRecord& b) {
            return a.vtable < b.vtable;
        });
        result.erase(std::unique(result.begin(), result.end(), [](const RttiVtableRecord& a, const RttiVtableRecord& b) {
            return a.vtable == b.vtable;
        }), result.end());

        std::ostringstream out;
        out << "rtti_names=" << names.size()
            << " col_candidates=" << colCandidates
            << " col_validated=" << colValidated
            << " vtable_refs=" << vtableReferences
            << " resolved_vtables=" << result.size();
        for (const RttiVtableRecord& item : result)
        {
            out << " [td_rva=" << HexValue(item.typeDescriptor - moduleBegin)
                << " col_rva=" << HexValue(item.completeObjectLocator - moduleBegin)
                << " vtable_rva=" << HexValue(item.vtable - moduleBegin) << "]";
        }
        diagnostic = out.str();
        return result;
    }

    const WindowRecord* FindWindowRecord(const Snapshot& snapshot, std::uintptr_t value);

    bool IsKnownTargetVtable(std::uintptr_t value, const std::vector<std::uintptr_t>& targets)
    {
        return std::find(targets.begin(), targets.end(), value) != targets.end();
    }

    void ValidateAndScoreTabViewCandidate(
        const Snapshot& snapshot,
        const std::vector<std::uintptr_t>& targets,
        TabViewCandidate& candidate)
    {
        candidate.currentVtable = 0;
        candidate.stableVtable = SafeReadValue(reinterpret_cast<void*>(candidate.object), candidate.currentVtable) &&
                                 candidate.currentVtable == candidate.vtable &&
                                 IsKnownTargetVtable(candidate.currentVtable, targets);
        if (!candidate.stableVtable)
        {
            candidate.rejected = true;
            candidate.rejectedReason = "vtable_changed_after_scan";
        }

        std::uintptr_t value = 0;
        if (SafeReadValue(reinterpret_cast<void*>(candidate.object + 0x48), value))
        {
            candidate.secondaryVtable = value;
            candidate.secondaryTabViewVtableAt48 = IsKnownTargetVtable(value, targets) && value != candidate.vtable;
        }

        const std::uintptr_t tracker = reinterpret_cast<std::uintptr_t>(snapshot.trackerWindow);
        std::uintptr_t internalView = 0;
        for (const WindowRecord& record : snapshot.windows)
        {
            if (record.parent == snapshot.trackerWindow && ParseAtlClassAddress(record.className))
            {
                internalView = reinterpret_cast<std::uintptr_t>(record.hwnd);
                break;
            }
        }
        std::uintptr_t atPlus8 = 0;
        std::uintptr_t atMinus68 = 0;
        std::uintptr_t atMinus18 = 0;
        SafeReadValue(reinterpret_cast<void*>(candidate.object + 0x08), atPlus8);
        if (candidate.object >= 0x68) SafeReadValue(reinterpret_cast<void*>(candidate.object - 0x68), atMinus68);
        if (candidate.object >= 0x18) SafeReadValue(reinterpret_cast<void*>(candidate.object - 0x18), atMinus18);
        candidate.trackerLayoutSignature =
            (FindWindowRecord(snapshot, atPlus8) != nullptr) &&
            ((tracker && atMinus68 == tracker) || FindWindowRecord(snapshot, atMinus68) != nullptr) &&
            (FindWindowRecord(snapshot, atMinus18) != nullptr || (internalView && atMinus18 == internalView));

        for (std::size_t offset = 0x60; offset <= 0x88; offset += sizeof(std::uintptr_t))
        {
            std::uintptr_t pointer = 0;
            if (SafeReadValue(reinterpret_cast<void*>(candidate.object + offset), pointer) && PointerLooksLikeObject(pointer))
                ++candidate.pageObjectPointers;
        }

        const ModuleRecord* bridgeModule = nullptr;
        for (const ModuleRecord& module : snapshot.modules)
        {
            if (Lower(module.name).find(L"mctrackerbridge") != std::wstring::npos)
            {
                bridgeModule = &module;
                break;
            }
        }
        for (std::ptrdiff_t relative = -0x100; relative <= 0x400; relative += sizeof(std::uintptr_t))
        {
            const std::intptr_t signedAddress = static_cast<std::intptr_t>(candidate.object) + relative;
            if (signedAddress <= 0) continue;
            std::uintptr_t qword = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(static_cast<std::uintptr_t>(signedAddress)), qword)) continue;
            if (qword == 0xCCCCCCCCCCCCCCCCull || qword == 0xCDCDCDCDCDCDCDCDull ||
                qword == 0xFEEEFEEEFEEEFEEEull)
                ++candidate.debugFillQwords;
            if (bridgeModule && qword >= bridgeModule->base && qword < bridgeModule->base + bridgeModule->size)
                ++candidate.bridgeModulePointers;
        }

        candidate.score = 100 + candidate.trackerWindowReferences * 12 +
                          candidate.pageWindowReferences * 4 + candidate.flexGridReferences * 4;
        if (candidate.stableVtable) candidate.score += 40;
        else candidate.score -= 1000;
        if (candidate.secondaryTabViewVtableAt48) candidate.score += 45;
        if (candidate.trackerLayoutSignature) candidate.score += 55;
        candidate.score += (std::min)(candidate.pageObjectPointers, 6) * 8;
        candidate.score -= (std::min)(candidate.debugFillQwords, 20) * 10;
        candidate.score -= (std::min)(candidate.bridgeModulePointers, 20) * 4;
        if (candidate.debugFillQwords >= 4)
        {
            candidate.rejected = true;
            if (!candidate.rejectedReason.empty()) candidate.rejectedReason += ",";
            candidate.rejectedReason += "debug_fill_pattern";
        }
    }

    std::vector<TabViewCandidate> FindRttiVtableObjectsProcessWide(
        const Snapshot& snapshot,
        const std::vector<RttiVtableRecord>& vtables,
        std::string& diagnostic)
    {
        constexpr std::size_t kMaxInspectedBytes = 512ull * 1024ull * 1024ull;
        constexpr std::size_t kChunkBytes = 256u * 1024u;
        constexpr std::size_t kMaxCandidates = 64;
        std::vector<TabViewCandidate> candidates;
        if (vtables.empty())
        {
            diagnostic = "no RTTI vtables to scan";
            return candidates;
        }
        std::vector<std::uintptr_t> targets;
        for (const auto& item : vtables) targets.push_back(item.vtable);

        std::vector<std::uintptr_t> trackerValues;
        std::vector<std::uintptr_t> pageValues;
        std::vector<std::uintptr_t> gridValues;
        if (snapshot.trackerWindow)
            trackerValues.push_back(reinterpret_cast<std::uintptr_t>(snapshot.trackerWindow));
        for (const WindowRecord& record : snapshot.windows)
        {
            const std::uintptr_t hwndValue = reinterpret_cast<std::uintptr_t>(record.hwnd);
            if (Lower(record.className) == L"#32770") pageValues.push_back(hwndValue);
            if (IsFlexGridClass(record.className)) gridValues.push_back(hwndValue);
        }

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
        const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
        const ULONGLONG started = GetTickCount64();
        std::size_t inspectedBytes = 0;
        std::size_t readablePrivateRegions = 0;
        bool byteLimitHit = false;
        bool candidateLimitHit = false;
        std::vector<unsigned char> buffer(kChunkBytes);

        while (cursor < maximum)
        {
            if (inspectedBytes >= kMaxInspectedBytes) { byteLimitHit = true; break; }
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
                break;
            const std::uintptr_t regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::uintptr_t regionEnd = regionBegin + mbi.RegionSize;
            if (mbi.RegionSize == 0 || regionEnd <= cursor)
                break;
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && IsReadableProtection(mbi.Protect))
            {
                ++readablePrivateRegions;
                for (std::uintptr_t chunk = regionBegin; chunk < regionEnd; )
                {
                            if (inspectedBytes >= kMaxInspectedBytes) { byteLimitHit = true; break; }
                    const std::size_t bytes = static_cast<std::size_t>((std::min)(
                        regionEnd - chunk, static_cast<std::uintptr_t>(kChunkBytes)));
                    DWORD sehCode = 0;
                    if (MCBridge_SafeCopyMemory(buffer.data(), reinterpret_cast<void*>(chunk), bytes, &sehCode))
                    {
                        inspectedBytes += bytes;
                        const std::size_t first = static_cast<std::size_t>((sizeof(void*) - (chunk & (sizeof(void*) - 1))) & (sizeof(void*) - 1));
                        for (std::size_t offset = first; offset + sizeof(std::uintptr_t) <= bytes; offset += sizeof(std::uintptr_t))
                        {
                            std::uintptr_t value = 0;
                            memcpy(&value, buffer.data() + offset, sizeof(value));
                            if (std::find(targets.begin(), targets.end(), value) == targets.end())
                                continue;
                            TabViewCandidate candidate;
                            candidate.object = chunk + offset;
                            candidate.vtable = value;
                            MEMORY_BASIC_INFORMATION candidateMbi{};
                            if (VirtualQuery(reinterpret_cast<void*>(candidate.object), &candidateMbi, sizeof(candidateMbi)) == sizeof(candidateMbi))
                            {
                                candidate.allocationBase = reinterpret_cast<std::uintptr_t>(candidateMbi.AllocationBase);
                                candidate.regionBase = reinterpret_cast<std::uintptr_t>(candidateMbi.BaseAddress);
                                candidate.regionSize = candidateMbi.RegionSize;
                                candidate.memoryType = candidateMbi.Type;
                                candidate.memoryProtect = candidateMbi.Protect;
                            }
                            candidate.trackerWindowReferences = CountWindowReferencesInObject(candidate.object, trackerValues, 0x2000);
                            candidate.pageWindowReferences = CountWindowReferencesInObject(candidate.object, pageValues);
                            candidate.flexGridReferences = CountWindowReferencesInObject(candidate.object, gridValues);
                            ValidateAndScoreTabViewCandidate(snapshot, targets, candidate);
                            candidates.push_back(candidate);
                            if (candidates.size() >= kMaxCandidates) { candidateLimitHit = true; break; }
                        }
                    }
                    if (candidateLimitHit) break;
                    chunk += bytes;
                }
            }
            if (byteLimitHit || candidateLimitHit) break;
            cursor = regionEnd;
        }

        std::sort(candidates.begin(), candidates.end(), [](const TabViewCandidate& a, const TabViewCandidate& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.object < b.object;
        });
        candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const TabViewCandidate& a, const TabViewCandidate& b) {
            return a.object == b.object;
        }), candidates.end());
        std::ostringstream out;
        out << "time_limit=none process_scan_regions=" << readablePrivateRegions
            << " inspected_bytes=" << inspectedBytes
            << " elapsed_ms=" << (GetTickCount64() - started)
            << " candidates=" << candidates.size();
        if (byteLimitHit) out << " byte_limit=true";
        if (candidateLimitHit) out << " candidate_limit=true";
        diagnostic = out.str();
        return candidates;
    }

    std::string MemoryTypeName(DWORD type)
    {
        if (type == MEM_PRIVATE) return "MEM_PRIVATE";
        if (type == MEM_IMAGE) return "MEM_IMAGE";
        if (type == MEM_MAPPED) return "MEM_MAPPED";
        return "UNKNOWN";
    }

    const WindowRecord* FindWindowRecord(const Snapshot& snapshot, std::uintptr_t value)
    {
        for (const WindowRecord& record : snapshot.windows)
        {
            if (reinterpret_cast<std::uintptr_t>(record.hwnd) == value)
                return &record;
        }
        return nullptr;
    }

    const ModuleRecord* FindModuleRecord(const Snapshot& snapshot, std::uintptr_t value)
    {
        for (const ModuleRecord& module : snapshot.modules)
        {
            if (value >= module.base && value < module.base + module.size)
                return &module;
        }
        return nullptr;
    }

    std::string ClassifyCandidateValue(const Snapshot& snapshot, std::uintptr_t value,
        const std::vector<TabViewCandidate>& candidates)
    {
        if (!value) return "null";
        if (const WindowRecord* window = FindWindowRecord(snapshot, value))
        {
            std::ostringstream out;
            out << "HWND class=[" << WideToUtf8(window->className) << "] text=["
                << WideToUtf8(window->text) << "] parent="
                << HexValue(reinterpret_cast<std::uintptr_t>(window->parent))
                << " visible=" << (window->visible ? "yes" : "no");
            return out.str();
        }
        if (const ModuleRecord* module = FindModuleRecord(snapshot, value))
        {
            std::ostringstream out;
            out << "module=[" << WideToUtf8(module->name) << "]+"
                << HexValue(value - module->base);
            if (MemoryRangeHasProtection(reinterpret_cast<void*>(value), 1, true))
                out << " executable";
            return out.str();
        }
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            if (candidates[i].object == value)
            {
                std::ostringstream out;
                out << "CATPTTabView_candidate#" << (i + 1);
                return out.str();
            }
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(value), &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect))
        {
            std::ostringstream out;
            out << "readable_" << MemoryTypeName(mbi.Type)
                << " allocation_base=" << HexValue(reinterpret_cast<std::uintptr_t>(mbi.AllocationBase));
            return out.str();
        }
        return "scalar_or_unreadable";
    }

    void CollectIncomingReferencesInCandidateAllocations(
        const std::vector<TabViewCandidate>& candidates,
        std::vector<std::size_t>& counts,
        std::vector<std::vector<std::string>>& examples)
    {
        counts.assign(candidates.size(), 0);
        examples.assign(candidates.size(), {});
        std::vector<std::uintptr_t> visited;
        constexpr std::size_t kMaxAllocationScan = 16u * 1024u * 1024u;
        constexpr std::size_t kMaxExamples = 32;
        for (const TabViewCandidate& owner : candidates)
        {
            if (!owner.allocationBase || std::find(visited.begin(), visited.end(), owner.allocationBase) != visited.end())
                continue;
            visited.push_back(owner.allocationBase);
            MEMORY_BASIC_INFORMATION mbi{};
            std::uintptr_t cursor = owner.allocationBase;
            std::size_t scanned = 0;
            while (scanned < kMaxAllocationScan &&
                   VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                   reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) == owner.allocationBase)
            {
                const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                const std::uintptr_t end = begin + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect))
                {
                    for (std::uintptr_t address = begin; address + sizeof(std::uintptr_t) <= end;
                         address += sizeof(std::uintptr_t))
                    {
                        std::uintptr_t value = 0;
                        if (!SafeReadValue(reinterpret_cast<void*>(address), value))
                            continue;
                        for (std::size_t i = 0; i < candidates.size(); ++i)
                        {
                            if (value != candidates[i].object)
                                continue;
                            ++counts[i];
                            if (examples[i].size() < kMaxExamples)
                            {
                                std::ostringstream line;
                                line << "address=" << HexValue(address)
                                     << " owner_allocation=" << HexValue(owner.allocationBase)
                                     << " offset=" << HexValue(address - owner.allocationBase);
                                examples[i].push_back(line.str());
                            }
                        }
                    }
                }
                scanned += mbi.RegionSize;
                if (end <= cursor) break;
                cursor = end;
            }
        }
    }

    void WriteCatptCandidateReport(const Snapshot& snapshot,
        const std::vector<RttiVtableRecord>& primaryVtables,
        const std::vector<TabViewCandidate>& candidates,
        const std::string& rttiDiagnostic,
        const std::string& scanDiagnostic)
    {
        (void)primaryVtables;
        EnsureOutputDirectory();
        const std::wstring path = ReportPath(L"MC_V147_CATPTTabView_Candidates", snapshot.processId);
        std::ostringstream out;
        out << "MC V147 CATPTTabView Candidate Object Graph Analyzer\r\n"
            << "===================================================\r\n"
            << "process_id=" << snapshot.processId << "\r\n"
            << "tracker_hwnd=" << HexValue(reinterpret_cast<std::uintptr_t>(snapshot.trackerWindow)) << "\r\n"
            << "atonptracker_base=" << HexValue(snapshot.atonpTrackerBase) << "\r\n"
            << "candidate_count=" << candidates.size() << "\r\n"
            << "object_dump_range=object-0x100..object+0x1000\r\n"
            << "window_reference_scan_bytes=0x2000\r\n"
            << "incoming_reference_scope=candidate_allocation_regions\r\n"
            << "rtti=" << rttiDiagnostic << "\r\n"
            << "scan=" << scanDiagnostic << "\r\n\r\n";

        const char* relatedTypes[] = {
            ".?AVCATPTViewController@ATOnPTracker@@",
            ".?AVCATPTWindowImpl@ATOnPTracker@@",
            ".?AVCAccountsPage@ATOnPTracker@@",
            ".?AVCOpenPositionsPage@ATOnPTracker@@",
            ".?AVCPositionsHistoryPage@ATOnPTracker@@",
            ".?AVCStrategyPositionsPage@ATOnPTracker@@",
            ".?AVCLogsPage@ATOnPTracker@@"
        };
        out << "RELATED_RTTI_TYPES\r\n------------------\r\n";
        for (const char* typeName : relatedTypes)
        {
            std::string diag;
            const auto records = ResolveRttiVtables(snapshot, typeName, diag);
            out << "type=" << typeName << " " << diag << "\r\n";
        }

        std::vector<std::size_t> incomingCounts;
        std::vector<std::vector<std::string>> incomingExamplesByCandidate;
        CollectIncomingReferencesInCandidateAllocations(candidates, incomingCounts, incomingExamplesByCandidate);

        out << "\r\nSUMMARY\r\n-------\r\n";
        out << "index object scan_vtable current_vtable stable rejected score tracker_refs page_refs grid_refs page_objects secondary_at_48 layout_signature debug_fill bridge_ptrs allocation_base region_base region_size type protect incoming_refs\r\n";
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const std::size_t incoming = incomingCounts[i];
            const TabViewCandidate& c = candidates[i];
            out << (i + 1) << ' ' << HexValue(c.object) << ' ' << HexValue(c.vtable) << ' '
                << HexValue(c.currentVtable) << ' ' << (c.stableVtable ? "yes" : "no") << ' '
                << (c.rejected ? "yes" : "no") << ' ' << c.score << ' ' << c.trackerWindowReferences << ' '
                << c.pageWindowReferences << ' ' << c.flexGridReferences << ' ' << c.pageObjectPointers << ' '
                << (c.secondaryTabViewVtableAt48 ? "yes" : "no") << ' '
                << (c.trackerLayoutSignature ? "yes" : "no") << ' ' << c.debugFillQwords << ' '
                << c.bridgeModulePointers << ' ' << HexValue(c.allocationBase) << ' '
                << HexValue(c.regionBase) << ' ' << c.regionSize << ' '
                << MemoryTypeName(c.memoryType) << ' ' << HexValue(c.memoryProtect) << ' ' << incoming << "\r\n";
        }

        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const TabViewCandidate& c = candidates[i];
            out << "\r\nCANDIDATE #" << (i + 1) << "\r\n------------\r\n"
                << "object=" << HexValue(c.object) << "\r\n"
                << "scan_vtable=" << HexValue(c.vtable) << "\r\n"
                << "current_vtable=" << HexValue(c.currentVtable) << "\r\n"
                << "stable_vtable=" << (c.stableVtable ? "yes" : "no") << "\r\n"
                << "rejected=" << (c.rejected ? "yes" : "no") << "\r\n"
                << "rejected_reason=" << c.rejectedReason << "\r\n"
                << "secondary_vtable_at_0x48=" << HexValue(c.secondaryVtable) << "\r\n"
                << "secondary_tabview_vtable_at_0x48=" << (c.secondaryTabViewVtableAt48 ? "yes" : "no") << "\r\n"
                << "tracker_layout_signature=" << (c.trackerLayoutSignature ? "yes" : "no") << "\r\n"
                << "page_object_pointers_0x60_0x88=" << c.pageObjectPointers << "\r\n"
                << "debug_fill_qwords=" << c.debugFillQwords << "\r\n"
                << "bridge_module_pointers=" << c.bridgeModulePointers << "\r\n"
                << "vtable_rva=" << (snapshot.atonpTrackerBase && c.vtable >= snapshot.atonpTrackerBase ? HexValue(c.vtable - snapshot.atonpTrackerBase) : "n/a") << "\r\n"
                << "score=" << c.score << "\r\n"
                << "allocation_base=" << HexValue(c.allocationBase) << "\r\n"
                << "region_base=" << HexValue(c.regionBase) << "\r\n"
                << "region_size=" << c.regionSize << "\r\n"
                << "distance_from_allocation_base=" << (c.allocationBase ? HexValue(c.object - c.allocationBase) : "n/a") << "\r\n"
                << "memory_type=" << MemoryTypeName(c.memoryType) << "\r\n"
                << "memory_protect=" << HexValue(c.memoryProtect) << "\r\n"
                << "tracker_refs=" << c.trackerWindowReferences << " page_refs=" << c.pageWindowReferences
                << " grid_refs=" << c.flexGridReferences << "\r\n";

            const std::size_t incoming = incomingCounts[i];
            out << "incoming_reference_count=" << incoming << "\r\n";
            for (const std::string& example : incomingExamplesByCandidate[i])
                out << "incoming " << example << "\r\n";

            out << "QWORDS\r\n";
            for (std::ptrdiff_t relative = -0x100; relative <= 0x1000 - static_cast<std::ptrdiff_t>(sizeof(std::uintptr_t)); relative += sizeof(std::uintptr_t))
            {
                const std::uintptr_t address = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(c.object) + relative);
                std::uintptr_t value = 0;
                if (!SafeReadValue(reinterpret_cast<void*>(address), value))
                {
                    out << "offset=" << (relative < 0 ? "-" : "+") << HexValue(static_cast<std::uintptr_t>(relative < 0 ? -relative : relative))
                        << " address=" << HexValue(address) << " unreadable\r\n";
                    continue;
                }
                out << "offset=" << (relative < 0 ? "-" : "+") << HexValue(static_cast<std::uintptr_t>(relative < 0 ? -relative : relative))
                    << " address=" << HexValue(address)
                    << " value=" << HexValue(value)
                    << " class=" << ClassifyCandidateValue(snapshot, value, candidates) << "\r\n";
            }
        }
        WriteUtf8File(path, out.str());
    }

    std::uintptr_t FindTabViewObject(
        const Snapshot& snapshot,
        std::string& diagnostic,
        const TrackerCompatibilityProfile* trackerProfile = nullptr,
        bool forceFreshScan = false,
        ULONGLONG recoveryTimeBudgetMs = 900,
        std::size_t recoveryByteBudget = 16ull * 1024ull * 1024ull,
        const char* recoveryTier = "fast")
    {
        if (forceFreshScan)
        {
            InvalidateTabViewCaches();

            if (!trackerProfile || !trackerProfile->matched || trackerProfile->tabViewVtableRva == 0)
            {
                diagnostic = "bounded fresh scan blocked because no verified Tracker profile is authorized";
                return 0;
            }

            // Recovery must fit inside the normal Bridge request deadline. Use
            // the exact verified vtable and Tracker-window allocation anchors;
            // do not run the unbounded process-wide research scan here.
            std::string profileScanDiagnostic;
            std::vector<TabViewCandidate> profileCandidates = FindProfileTabViewVtableObjects(
                snapshot,
                trackerProfile->tabViewVtableRva,
                trackerProfile->atonpTrackerImageSize,
                profileScanDiagnostic,
                recoveryTimeBudgetMs,
                recoveryByteBudget);

            std::uintptr_t freshAccepted = 0;
            if (profileCandidates.size() == 1)
            {
                const TabViewCandidate& only = profileCandidates.front();
                if (only.score >= 140 &&
                    (only.secondaryTabViewVtableAt48 || only.trackerLayoutSignature ||
                     only.pageObjectPointers >= 5))
                {
                    freshAccepted = only.object;
                }
            }
            else if (profileCandidates.size() > 1)
            {
                const TabViewCandidate& best = profileCandidates[0];
                const TabViewCandidate& second = profileCandidates[1];
                if (best.score >= 140 && best.score >= second.score + 20 &&
                    (best.secondaryTabViewVtableAt48 || best.trackerLayoutSignature ||
                     best.pageObjectPointers >= 5))
                {
                    freshAccepted = best.object;
                }
            }

            const int bestScore = profileCandidates.empty() ? 0 : profileCandidates[0].score;
            const int secondScore = profileCandidates.size() < 2 ? 0 : profileCandidates[1].score;
            std::string decision;
            if (!freshAccepted)
            {
                if (profileCandidates.empty())
                    decision = "no_candidates";
                else if (profileCandidates.size() == 1)
                    decision = "insufficient_structure";
                else
                    decision = "ambiguous_candidates";
                std::ostringstream recoveryDiagnostic;
                recoveryDiagnostic
                    << "tier=" << (recoveryTier ? recoveryTier : "unknown")
                    << "; decision=" << decision
                    << "; candidates=" << profileCandidates.size()
                    << "; best_score=" << bestScore
                    << "; second_score=" << secondScore
                    << "; " << profileScanDiagnostic;
                diagnostic = recoveryDiagnostic.str();
                AppendExecutionTrace("tabview_recovery_scan", diagnostic);
                return 0;
            }

            AcquireSRWLockExclusive(&g_tabViewCacheLock);
            g_cachedTabView = freshAccepted;
            g_cachedAtonpBase = snapshot.atonpTrackerBase;
            g_cachedTrackerWindow = snapshot.trackerWindow;
            ReleaseSRWLockExclusive(&g_tabViewCacheLock);
            RememberTabViewHint(freshAccepted);
            std::ostringstream recoveryDiagnostic;
            recoveryDiagnostic
                << "tier=" << (recoveryTier ? recoveryTier : "unknown")
                << "; decision=accepted"
                << "; candidates=" << profileCandidates.size()
                << "; best_score=" << bestScore
                << "; second_score=" << secondScore
                << "; accepted=" << HexValue(freshAccepted)
                << "; " << profileScanDiagnostic;
            diagnostic = recoveryDiagnostic.str();
            AppendExecutionTrace("tabview_recovery_scan", diagnostic);
            return freshAccepted;
        }

        if (g_tabViewRecoveryModeActive.load() && trackerProfile && trackerProfile->matched)
        {
            diagnostic = "normal discovery deferred while bounded recovery mode is active";
            return 0;
        }

        std::string rttiDiagnostic;
        const std::vector<RttiVtableRecord> vtables = ResolveRttiVtables(
            snapshot, ".?AVCATPTTabView@ATOnPTracker@@", rttiDiagnostic);

        const auto profileVtableMatches = [&](std::uintptr_t object) -> bool {
            if (!trackerProfile || !trackerProfile->matched || trackerProfile->tabViewVtableRva == 0)
                return true;
            std::uintptr_t actualVtable = 0;
            if (!VtableStartsWithExecutableCode(object, actualVtable))
                return false;
            return actualVtable == snapshot.atonpTrackerBase + trackerProfile->tabViewVtableRva;
        };

        AcquireSRWLockShared(&g_tabViewCacheLock);
        const std::uintptr_t cached = g_cachedTabView;
        const std::uintptr_t cachedBase = g_cachedAtonpBase;
        const HWND cachedTracker = g_cachedTrackerWindow;
        ReleaseSRWLockShared(&g_tabViewCacheLock);
        if (cached && cachedBase == snapshot.atonpTrackerBase && cachedTracker == snapshot.trackerWindow)
        {
            std::uintptr_t cachedVtable = 0;
            const bool rttiValid = VtableStartsWithExecutableCode(cached, cachedVtable) &&
                std::any_of(vtables.begin(), vtables.end(), [cachedVtable](const RttiVtableRecord& item) {
                    return item.vtable == cachedVtable;
                });
            const bool profileValid = profileVtableMatches(cached);
            if ((rttiValid || (trackerProfile && trackerProfile->matched)) && profileValid)
            {
                diagnostic = "CATPTTabView cache hit object=" + HexValue(cached) +
                             " vtable=" + HexValue(cachedVtable) + "; " + rttiDiagnostic;
                return cached;
            }
        }

        // After a confirmed failure, do not repeat the process-wide RTTI scan on
        // every Watchdog retry. That path took 20-40 seconds in the R30 field
        // trace. The staged exact-profile recovery below remains available and
        // keeps each request inside the normal Watchdog deadline.
        if (g_tabViewPersistentFailureActive.load())
        {
            diagnostic = "process-wide discovery deferred during persistent recovery; staged exact-profile scan will run when its cooldown permits";
            return 0;
        }

        // Prefer the fingerprint-scoped locator before the expensive process-
        // wide RTTI walk. On the verified build this is both safer and much
        // faster, and it also benefits from the retained allocation hints.
        if (trackerProfile && trackerProfile->matched && trackerProfile->tabViewVtableRva != 0)
        {
            std::string startupScanDiagnostic;
            const std::vector<TabViewCandidate> startupCandidates =
                FindProfileTabViewVtableObjects(
                    snapshot,
                    trackerProfile->tabViewVtableRva,
                    trackerProfile->atonpTrackerImageSize,
                    startupScanDiagnostic,
                    1200,
                    32ull * 1024ull * 1024ull);
            std::uintptr_t startupAccepted = 0;
            if (startupCandidates.size() == 1)
            {
                const TabViewCandidate& only = startupCandidates.front();
                if (only.score >= 140 &&
                    (only.secondaryTabViewVtableAt48 || only.trackerLayoutSignature ||
                     only.pageObjectPointers >= 5))
                    startupAccepted = only.object;
            }
            else if (startupCandidates.size() > 1)
            {
                const TabViewCandidate& best = startupCandidates[0];
                const TabViewCandidate& second = startupCandidates[1];
                if (best.score >= 140 && best.score >= second.score + 20 &&
                    (best.secondaryTabViewVtableAt48 || best.trackerLayoutSignature ||
                     best.pageObjectPointers >= 5))
                    startupAccepted = best.object;
            }
            if (startupAccepted)
            {
                AcquireSRWLockExclusive(&g_tabViewCacheLock);
                g_cachedTabView = startupAccepted;
                g_cachedAtonpBase = snapshot.atonpTrackerBase;
                g_cachedTrackerWindow = snapshot.trackerWindow;
                ReleaseSRWLockExclusive(&g_tabViewCacheLock);
                RememberTabViewHint(startupAccepted);
                diagnostic = "exact-profile locator accepted object=" +
                    HexValue(startupAccepted) + "; " + startupScanDiagnostic;
                AppendExecutionTrace("tabview_exact_profile_startup", diagnostic);
                return startupAccepted;
            }
            AppendExecutionTrace(
                "tabview_exact_profile_startup_miss",
                "candidates=" + std::to_string(startupCandidates.size()) +
                    "; " + startupScanDiagnostic);
        }

        std::string scanDiagnostic;
        std::vector<TabViewCandidate> candidates;
        bool candidateCacheHit = false;
        const ULONGLONG candidateCacheNow = GetTickCount64();
        AcquireSRWLockShared(&g_candidateCacheLock);
        if (g_candidateCacheValid &&
            candidateCacheNow - g_candidateCacheTick < kTabViewCandidateCacheTtlMs &&
            g_candidateCacheAtonpBase == snapshot.atonpTrackerBase &&
            g_candidateCacheTrackerWindow == snapshot.trackerWindow &&
            snapshot.trackerWindow != nullptr)
        {
            candidates = g_cachedCandidates;
            scanDiagnostic = g_cachedCandidateScanDiagnostic;
            candidateCacheHit = true;
        }
        ReleaseSRWLockShared(&g_candidateCacheLock);
        if (!candidateCacheHit)
        {
            candidates = FindRttiVtableObjectsProcessWide(snapshot, vtables, scanDiagnostic);
            AcquireSRWLockExclusive(&g_candidateCacheLock);
            g_cachedCandidates = candidates;
            g_cachedCandidateScanDiagnostic = scanDiagnostic;
            g_candidateCacheAtonpBase = snapshot.atonpTrackerBase;
            g_candidateCacheTrackerWindow = snapshot.trackerWindow;
            g_candidateCacheValid = true;
            g_candidateCacheTick = GetTickCount64();
            ReleaseSRWLockExclusive(&g_candidateCacheLock);
            WriteCatptCandidateReport(snapshot, vtables, candidates, rttiDiagnostic, scanDiagnostic);
        }
        else
        {
            scanDiagnostic += " candidate_cache_hit=true";
        }

        std::ostringstream out;
        out << "RTTI Complete Object Locator " << rttiDiagnostic << "; " << scanDiagnostic;
        if (!candidates.empty())
        {
            const TabViewCandidate& best = candidates.front();
            out << " best_object=" << HexValue(best.object)
                << " vtable=" << HexValue(best.vtable)
                << " score=" << best.score
                << " tracker_refs=" << best.trackerWindowReferences
                << " page_refs=" << best.pageWindowReferences
                << " grid_refs=" << best.flexGridReferences;
        }

        std::vector<const TabViewCandidate*> validCandidates;
        for (const TabViewCandidate& candidate : candidates)
        {
            if (candidate.stableVtable && !candidate.rejected)
                validCandidates.push_back(&candidate);
        }
        std::sort(validCandidates.begin(), validCandidates.end(), [](const TabViewCandidate* left, const TabViewCandidate* right) {
            if (left->score != right->score) return left->score > right->score;
            return left->object < right->object;
        });

        std::uintptr_t accepted = 0;
        if (validCandidates.size() == 1)
        {
            accepted = validCandidates.front()->object;
            out << "; unique stable RTTI-vtable object accepted";
        }
        else if (validCandidates.size() > 1)
        {
            const TabViewCandidate& best = *validCandidates[0];
            const TabViewCandidate& second = *validCandidates[1];
            if (best.score >= 140 && best.score >= second.score + 20 &&
                (best.secondaryTabViewVtableAt48 || best.trackerLayoutSignature))
            {
                accepted = best.object;
                out << "; uniquely strongest stable structural RTTI-vtable object accepted";
            }
            else
            {
                out << "; ambiguous stable RTTI-vtable objects, live call blocked";
            }
        }
        else
        {
            out << "; no stable CATPTTabView RTTI-vtable object found";
        }

        if (accepted && !profileVtableMatches(accepted))
        {
            out << "; RTTI object rejected because its vtable does not match the selected Tracker profile";
            accepted = 0;
        }

        if (!accepted && trackerProfile && trackerProfile->matched && trackerProfile->tabViewVtableRva != 0)
        {
            std::string profileScanDiagnostic;
            std::vector<TabViewCandidate> profileCandidates = FindProfileTabViewVtableObjects(
                snapshot,
                trackerProfile->tabViewVtableRva,
                trackerProfile->atonpTrackerImageSize,
                profileScanDiagnostic,
                1500,
                16ull * 1024ull * 1024ull);
            out << "; profile_vtable_scan=" << profileScanDiagnostic;
            if (!profileCandidates.empty())
            {
                const TabViewCandidate& best = profileCandidates.front();
                if (profileCandidates.size() == 1)
                {
                    if (best.score >= 140 &&
                        (best.secondaryTabViewVtableAt48 || best.trackerLayoutSignature ||
                         best.pageObjectPointers >= 5))
                    {
                        accepted = best.object;
                        out << "; exact profile vtable produced one structural CATPTTabView candidate";
                    }
                    else
                    {
                        out << "; exact profile vtable produced one non-structural candidate";
                    }
                }
                else
                {
                    const TabViewCandidate& second = profileCandidates[1];
                    if (best.score >= 140 && best.score >= second.score + 20 &&
                        (best.secondaryTabViewVtableAt48 || best.trackerLayoutSignature ||
                         best.pageObjectPointers >= 5))
                    {
                        accepted = best.object;
                        out << "; exact profile vtable produced a uniquely strongest structural CATPTTabView candidate";
                    }
                    else
                    {
                        out << "; exact profile vtable candidates were ambiguous";
                    }
                }
            }
        }

        if (accepted)
        {
            AcquireSRWLockExclusive(&g_tabViewCacheLock);
            g_cachedTabView = accepted;
            g_cachedAtonpBase = snapshot.atonpTrackerBase;
            g_cachedTrackerWindow = snapshot.trackerWindow;
            ReleaseSRWLockExclusive(&g_tabViewCacheLock);
            RememberTabViewHint(accepted);
        }
        diagnostic = out.str();
        return accepted;
    }

    HRESULT SafeQueryTradeInfo(IUnknown* candidate, IUnknown** result, DWORD& sehCode)
    {
        if (result) *result = nullptr;
        sehCode = 0;
        if (!candidate || !result)
            return E_POINTER;
        return MCBridge_QueryInterfaceWithSeh(
            candidate, &kIidTradeInfo, reinterpret_cast<void**>(result), &sehCode);
    }

    std::uintptr_t FindTradeInfoPointer(std::uintptr_t tabView, std::string& diagnostic)
    {
        if (!tabView)
            return 0;
        // Search only the local CATPTTabView object prefix. A candidate must expose
        // the exact ITC_TradeInfo IID through QueryInterface before it is accepted.
        for (std::size_t offset = 0; offset < 0x800; offset += sizeof(void*))
        {
            std::uintptr_t raw = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(tabView + offset), raw) || raw == tabView)
                continue;
            std::uintptr_t vtable = 0;
            if (!VtableStartsWithExecutableCode(raw, vtable))
                continue;
            IUnknown* queried = nullptr;
            DWORD sehCode = 0;
            const HRESULT hr = SafeQueryTradeInfo(reinterpret_cast<IUnknown*>(raw), &queried, sehCode);
            if (SUCCEEDED(hr) && queried)
            {
                DWORD releaseSeh = 0;
                int releaseSucceeded = 0;
                (void)MCBridge_ReleaseWithSeh(queried, &releaseSeh, &releaseSucceeded);
                std::ostringstream out;
                out << "ITC_TradeInfo resolved at CATPTTabView+0x" << std::hex << offset;
                diagnostic = out.str();
                return raw;
            }
        }
        diagnostic = "no unique ITC_TradeInfo pointer in CATPTTabView prefix";
        return 0;
    }

    bool ValidateKnownExtractorFunction(const Snapshot& snapshot, std::uintptr_t rva, std::uintptr_t& function)
    {
        function = snapshot.atonpTrackerBase + rva;
        if (!snapshot.atonpTrackerBase || snapshot.atonpTrackerSize != kKnownAtonpSize)
            return false;
        if (rva >= snapshot.atonpTrackerSize)
            return false;
        return MemoryRangeHasProtection(reinterpret_cast<void*>(function), 32, true);
    }

    ExtractorAttempt BuildExtractorProbe(
        const Snapshot& snapshot,
        const char* name,
        std::uintptr_t rva)
    {
        ExtractorAttempt attempt;
        attempt.name = name;
        attempt.functionValid = ValidateKnownExtractorFunction(snapshot, rva, attempt.function);
        std::string tabDiagnostic;
        attempt.tabView = FindTabViewObject(snapshot, tabDiagnostic);
        attempt.tabViewValid = attempt.tabView != 0;
        std::string tradeDiagnostic;
        if (attempt.tabViewValid)
            attempt.tradeInfo = FindTradeInfoPointer(attempt.tabView, tradeDiagnostic);
        attempt.tradeInfoValid = attempt.tradeInfo != 0;
        std::ostringstream out;
        out << tabDiagnostic << "; " << tradeDiagnostic;
        if (!attempt.functionValid)
            out << "; function contract rejected (module size/RVA/executable validation)";
        else
            out << "; function_valid means executable address only, not a verified function begin";
        attempt.diagnostic = out.str();
        return attempt;
    }

    // V147 compile fix: keep the analysis byte reader next to its callers so
    // this section has no dependency on declaration order elsewhere.
    bool ReadAnalysisBytes(const void* address, void* destination, std::size_t bytes)
    {
        if (!address || !destination || bytes == 0)
            return false;
        DWORD sehCode = 0;
        return MCBridge_SafeCopyMemory(
            destination,
            address,
            bytes,
            &sehCode) != 0;
    }

    std::string HexBytesWithProtection(
        std::uintptr_t address,
        std::size_t count,
        bool executable)
    {
        if (!address || count == 0 || count > 512 ||
            !MemoryRangeHasProtection(reinterpret_cast<void*>(address), count, executable))
        {
            return {};
        }

        std::vector<unsigned char> bytes(count);
        if (!ReadAnalysisBytes(reinterpret_cast<void*>(address), bytes.data(), bytes.size()))
            return {};

        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            if (i) out << ' ';
            out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
        }
        return out.str();
    }

    std::string HexReadableBytes(std::uintptr_t address, std::size_t count)
    {
        return HexBytesWithProtection(address, count, false);
    }

    std::string HexModuleBytes(
        const Snapshot& snapshot,
        std::uintptr_t address,
        std::size_t count,
        bool executable)
    {
        if (!snapshot.atonpTrackerBase || !snapshot.atonpTrackerSize || !address || !count)
            return {};

        const std::uintptr_t moduleBegin = snapshot.atonpTrackerBase;
        const std::uintptr_t moduleEnd = moduleBegin + snapshot.atonpTrackerSize;
        if (moduleEnd < moduleBegin || address < moduleBegin || address >= moduleEnd)
            return {};

        const std::size_t available = static_cast<std::size_t>(moduleEnd - address);
        count = (std::min)(count, available);
        count = (std::min)(count, static_cast<std::size_t>(512));
        return HexBytesWithProtection(address, count, executable);
    }

    std::uintptr_t ResolveInitialThunk(std::uintptr_t function, std::string& kind)
    {
        kind = "none";
        unsigned char b[16]{};
        if (!function || !ReadAnalysisBytes(reinterpret_cast<void*>(function), b, sizeof(b)))
            return 0;
        if (b[0] == 0xE9)
        {
            std::int32_t rel = 0; std::memcpy(&rel, b + 1, sizeof(rel));
            kind = "jmp_rel32"; return function + 5 + rel;
        }
        if (b[0] == 0xEB)
        {
            std::int8_t rel = static_cast<std::int8_t>(b[1]);
            kind = "jmp_rel8"; return function + 2 + rel;
        }
        if (b[0] == 0xFF && b[1] == 0x25)
        {
            std::int32_t rel = 0; std::memcpy(&rel, b + 2, sizeof(rel));
            const std::uintptr_t slot = function + 6 + rel;
            std::uintptr_t target = 0;
            if (SafeReadValue(reinterpret_cast<void*>(slot), target))
            { kind = "jmp_rip_indirect"; return target; }
        }
        return function;
    }

#pragma pack(push, 1)
    struct RuntimeFunctionRaw
    {
        std::uint32_t beginAddress = 0;
        std::uint32_t endAddress = 0;
        std::uint32_t unwindData = 0;
    };
#pragma pack(pop)

    static_assert(sizeof(RuntimeFunctionRaw) == 12, "Unexpected x64 RUNTIME_FUNCTION size");

    struct PeExceptionDirectoryAnalysis
    {
        bool headersValid = false;
        bool pe64 = false;
        bool exceptionDirectoryValid = false;
        std::uint16_t machine = 0;
        std::uint32_t timeDateStamp = 0;
        std::uint32_t sizeOfImage = 0;
        std::uint32_t checksum = 0;
        std::uint32_t exceptionDirectoryRva = 0;
        std::uint32_t exceptionDirectorySize = 0;
        std::size_t runtimeFunctionCount = 0;
        std::size_t trailingBytes = 0;
        std::uintptr_t exceptionDirectoryVa = 0;
        std::string diagnostic;
    };

    struct UnwindInfoAnalysis
    {
        bool valid = false;
        std::uint32_t rawUnwindData = 0;
        std::uint32_t unwindInfoRva = 0;
        std::uintptr_t unwindInfoVa = 0;
        unsigned int version = 0;
        unsigned int flags = 0;
        unsigned int sizeOfProlog = 0;
        unsigned int countOfCodes = 0;
        unsigned int frameRegister = 0;
        unsigned int frameOffset = 0;
        bool hasExceptionHandler = false;
        bool hasTerminationHandler = false;
        bool hasChainInfo = false;
        std::uint32_t handlerRva = 0;
        RuntimeFunctionRaw chainedFunction{};
        std::string bytes64;
        std::string diagnostic;
    };

    struct RuntimeFunctionBoundaryAnalysis
    {
        std::uintptr_t targetVa = 0;
        std::uint32_t targetRva = 0;
        bool targetInModule = false;
        bool enclosingFound = false;
        std::size_t entryIndex = 0;
        std::uintptr_t pdataEntryVa = 0;
        RuntimeFunctionRaw entry{};
        bool previousFound = false;
        std::size_t previousIndex = 0;
        RuntimeFunctionRaw previous{};
        bool nextFound = false;
        std::size_t nextIndex = 0;
        RuntimeFunctionRaw next{};
        std::uint32_t functionSize = 0;
        std::uint32_t offsetInsideFunction = 0;
        std::uint32_t bytesUntilFunctionEnd = 0;
        std::string classification;
        std::string diagnostic;
        PeExceptionDirectoryAnalysis pe;
        UnwindInfoAnalysis unwind;
        std::uintptr_t candidateWindowStartVa = 0;
        std::size_t candidateWindowOffset = 0;
        std::string candidateWindowBytes;
        std::string functionHeadBytes;
        std::string functionTailBytes;
        std::string beforeFunctionBytes;
        std::string afterFunctionBytes;
    };

    bool ParsePeExceptionDirectory(
        const Snapshot& snapshot,
        PeExceptionDirectoryAnalysis& result)
    {
        result = PeExceptionDirectoryAnalysis{};
        if (!snapshot.atonpTrackerBase || snapshot.atonpTrackerSize < sizeof(IMAGE_DOS_HEADER))
        {
            result.diagnostic = "ATOnPTracker module base or size is unavailable";
            return false;
        }

        IMAGE_DOS_HEADER dos{};
        if (!SafeReadValue(reinterpret_cast<void*>(snapshot.atonpTrackerBase), dos))
        {
            result.diagnostic = "DOS header could not be read";
            return false;
        }
        if (dos.e_magic != IMAGE_DOS_SIGNATURE)
        {
            result.diagnostic = "DOS signature is invalid";
            return false;
        }
        if (dos.e_lfanew <= 0)
        {
            result.diagnostic = "PE header offset is invalid";
            return false;
        }

        const std::uintptr_t peOffset = static_cast<std::uintptr_t>(dos.e_lfanew);
        if (peOffset > snapshot.atonpTrackerSize ||
            sizeof(IMAGE_NT_HEADERS64) > snapshot.atonpTrackerSize - peOffset)
        {
            result.diagnostic = "PE headers are outside the loaded module range";
            return false;
        }

        IMAGE_NT_HEADERS64 nt{};
        const std::uintptr_t ntAddress = snapshot.atonpTrackerBase + peOffset;
        if (!SafeReadValue(reinterpret_cast<void*>(ntAddress), nt))
        {
            result.diagnostic = "PE headers could not be read";
            return false;
        }
        if (nt.Signature != IMAGE_NT_SIGNATURE)
        {
            result.diagnostic = "PE signature is invalid";
            return false;
        }

        result.machine = nt.FileHeader.Machine;
        result.timeDateStamp = nt.FileHeader.TimeDateStamp;
        result.pe64 = nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        result.sizeOfImage = nt.OptionalHeader.SizeOfImage;
        result.checksum = nt.OptionalHeader.CheckSum;
        result.headersValid = result.pe64 && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64;
        if (!result.headersValid)
        {
            result.diagnostic = "loaded image is not an AMD64 PE32+ image";
            return false;
        }
        if (nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
        {
            result.diagnostic = "PE optional header has no exception directory slot";
            return false;
        }

        const IMAGE_DATA_DIRECTORY directory =
            nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        result.exceptionDirectoryRva = directory.VirtualAddress;
        result.exceptionDirectorySize = directory.Size;
        if (!directory.VirtualAddress || directory.Size < sizeof(RuntimeFunctionRaw))
        {
            result.diagnostic = "x64 exception directory is empty";
            return false;
        }

        const std::uint64_t directoryEnd =
            static_cast<std::uint64_t>(directory.VirtualAddress) + directory.Size;
        const std::uint64_t imageLimit = result.sizeOfImage
            ? (std::min)(static_cast<std::uint64_t>(result.sizeOfImage),
                         static_cast<std::uint64_t>(snapshot.atonpTrackerSize))
            : static_cast<std::uint64_t>(snapshot.atonpTrackerSize);
        if (directoryEnd > imageLimit)
        {
            result.diagnostic = "exception directory extends outside the loaded image";
            return false;
        }

        result.exceptionDirectoryVa = snapshot.atonpTrackerBase + directory.VirtualAddress;
        result.runtimeFunctionCount = directory.Size / sizeof(RuntimeFunctionRaw);
        result.trailingBytes = directory.Size % sizeof(RuntimeFunctionRaw);
        if (!result.runtimeFunctionCount)
        {
            result.diagnostic = "exception directory contains no complete RUNTIME_FUNCTION entries";
            return false;
        }

        RuntimeFunctionRaw firstEntry{};
        RuntimeFunctionRaw lastEntry{};
        const std::uintptr_t lastEntryVa = result.exceptionDirectoryVa +
            (result.runtimeFunctionCount - 1) * sizeof(RuntimeFunctionRaw);
        if (!ReadAnalysisBytes(
                reinterpret_cast<void*>(result.exceptionDirectoryVa),
                &firstEntry,
                sizeof(firstEntry)) ||
            !ReadAnalysisBytes(
                reinterpret_cast<void*>(lastEntryVa),
                &lastEntry,
                sizeof(lastEntry)))
        {
            result.diagnostic = "first or last exception-directory entry is not readable";
            return false;
        }

        result.exceptionDirectoryValid = true;
        result.diagnostic = result.trailingBytes
            ? "exception directory valid; trailing non-entry bytes present"
            : "exception directory valid";
        return true;
    }

    bool ReadRuntimeFunctionEntry(
        const PeExceptionDirectoryAnalysis& pe,
        std::size_t index,
        RuntimeFunctionRaw& entry,
        std::uintptr_t& entryVa)
    {
        entry = RuntimeFunctionRaw{};
        entryVa = 0;
        if (!pe.exceptionDirectoryValid || index >= pe.runtimeFunctionCount)
            return false;
        entryVa = pe.exceptionDirectoryVa + index * sizeof(RuntimeFunctionRaw);
        return SafeReadValue(reinterpret_cast<void*>(entryVa), entry);
    }

    bool RuntimeFunctionLooksValid(
        const Snapshot& snapshot,
        const PeExceptionDirectoryAnalysis& pe,
        const RuntimeFunctionRaw& entry)
    {
        if (!entry.beginAddress || entry.beginAddress >= entry.endAddress)
            return false;
        const std::uint64_t imageLimit = pe.sizeOfImage
            ? (std::min)(static_cast<std::uint64_t>(pe.sizeOfImage),
                         static_cast<std::uint64_t>(snapshot.atonpTrackerSize))
            : static_cast<std::uint64_t>(snapshot.atonpTrackerSize);
        return entry.endAddress <= imageLimit;
    }

    UnwindInfoAnalysis AnalyzeUnwindInfo(
        const Snapshot& snapshot,
        const PeExceptionDirectoryAnalysis& pe,
        const RuntimeFunctionRaw& entry)
    {
        UnwindInfoAnalysis result;
        result.rawUnwindData = entry.unwindData;
        result.unwindInfoRva = entry.unwindData & ~static_cast<std::uint32_t>(3u);
        if (!result.unwindInfoRva)
        {
            result.diagnostic = "RUNTIME_FUNCTION has no UNWIND_INFO RVA";
            return result;
        }

        const std::uint64_t imageLimit = pe.sizeOfImage
            ? (std::min)(static_cast<std::uint64_t>(pe.sizeOfImage),
                         static_cast<std::uint64_t>(snapshot.atonpTrackerSize))
            : static_cast<std::uint64_t>(snapshot.atonpTrackerSize);
        if (static_cast<std::uint64_t>(result.unwindInfoRva) + 4 > imageLimit)
        {
            result.diagnostic = "UNWIND_INFO header is outside the image";
            return result;
        }

        result.unwindInfoVa = snapshot.atonpTrackerBase + result.unwindInfoRva;
        unsigned char header[4]{};
        if (!ReadAnalysisBytes(
                reinterpret_cast<void*>(result.unwindInfoVa),
                header,
                sizeof(header)))
        {
            result.diagnostic = "UNWIND_INFO header could not be read";
            return result;
        }

        result.version = header[0] & 0x07u;
        result.flags = header[0] >> 3u;
        result.sizeOfProlog = header[1];
        result.countOfCodes = header[2];
        result.frameRegister = header[3] & 0x0Fu;
        result.frameOffset = header[3] >> 4u;
        result.hasExceptionHandler = (result.flags & 0x01u) != 0;
        result.hasTerminationHandler = (result.flags & 0x02u) != 0;
        result.hasChainInfo = (result.flags & 0x04u) != 0;

        const std::size_t alignedCodeSlots =
            (static_cast<std::size_t>(result.countOfCodes) + 1u) & ~static_cast<std::size_t>(1u);
        const std::uint64_t optionalRva64 =
            static_cast<std::uint64_t>(result.unwindInfoRva) + 4u + alignedCodeSlots * 2u;
        if (optionalRva64 > imageLimit)
        {
            result.diagnostic = "UNWIND_INFO unwind-code array extends outside the image";
            return result;
        }

        const std::uint32_t optionalRva = static_cast<std::uint32_t>(optionalRva64);
        if (result.hasChainInfo)
        {
            if (static_cast<std::uint64_t>(optionalRva) + sizeof(RuntimeFunctionRaw) <= imageLimit)
            {
                (void)SafeReadValue(
                    reinterpret_cast<void*>(snapshot.atonpTrackerBase + optionalRva),
                    result.chainedFunction);
            }
        }
        else if (result.hasExceptionHandler || result.hasTerminationHandler)
        {
            if (static_cast<std::uint64_t>(optionalRva) + sizeof(std::uint32_t) <= imageLimit)
            {
                (void)SafeReadValue(
                    reinterpret_cast<void*>(snapshot.atonpTrackerBase + optionalRva),
                    result.handlerRva);
            }
        }

        const std::size_t unwindBytes = static_cast<std::size_t>(
            (std::min)(static_cast<std::uint64_t>(64), imageLimit - result.unwindInfoRva));
        result.bytes64 = HexReadableBytes(result.unwindInfoVa, unwindBytes);
        result.valid = result.version != 0;
        result.diagnostic = result.valid
            ? "UNWIND_INFO header parsed"
            : "UNWIND_INFO version is zero or invalid";
        return result;
    }

    RuntimeFunctionBoundaryAnalysis AnalyzeRuntimeFunctionBoundary(
        const Snapshot& snapshot,
        std::uintptr_t targetVa)
    {
        RuntimeFunctionBoundaryAnalysis result;
        result.targetVa = targetVa;
        if (!ParsePeExceptionDirectory(snapshot, result.pe))
        {
            result.classification = "pe_exception_directory_unavailable";
            result.diagnostic = result.pe.diagnostic;
            return result;
        }

        const std::uintptr_t moduleBegin = snapshot.atonpTrackerBase;
        const std::uintptr_t moduleEnd = moduleBegin + snapshot.atonpTrackerSize;
        if (!targetVa || moduleEnd < moduleBegin || targetVa < moduleBegin || targetVa >= moduleEnd)
        {
            result.classification = "target_outside_module";
            result.diagnostic = "target address is outside ATOnPTracker";
            return result;
        }

        result.targetInModule = true;
        const std::uintptr_t targetOffset = targetVa - moduleBegin;
        if (targetOffset > 0xFFFFFFFFu)
        {
            result.classification = "target_rva_overflow";
            result.diagnostic = "target RVA does not fit in 32 bits";
            return result;
        }
        result.targetRva = static_cast<std::uint32_t>(targetOffset);

        std::size_t low = 0;
        std::size_t high = result.pe.runtimeFunctionCount;
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            RuntimeFunctionRaw candidate{};
            std::uintptr_t ignored = 0;
            if (!ReadRuntimeFunctionEntry(result.pe, middle, candidate, ignored))
            {
                result.classification = "pdata_read_failed";
                result.diagnostic = "RUNTIME_FUNCTION binary search could not read an entry";
                return result;
            }
            if (candidate.beginAddress <= result.targetRva)
                low = middle + 1;
            else
                high = middle;
        }

        const bool hasFloorEntry = low > 0;
        const std::size_t floorIndex = hasFloorEntry ? low - 1 : 0;
        RuntimeFunctionRaw floorEntry{};
        std::uintptr_t floorEntryVa = 0;
        if (hasFloorEntry &&
            ReadRuntimeFunctionEntry(result.pe, floorIndex, floorEntry, floorEntryVa))
        {
            if (RuntimeFunctionLooksValid(snapshot, result.pe, floorEntry) &&
                result.targetRva >= floorEntry.beginAddress &&
                result.targetRva < floorEntry.endAddress)
            {
                result.enclosingFound = true;
                result.entryIndex = floorIndex;
                result.pdataEntryVa = floorEntryVa;
                result.entry = floorEntry;
            }
        }

        // Defensive local scan handles unusual duplicate begin RVAs or a malformed sorted entry.
        if (!result.enclosingFound)
        {
            const std::size_t scanBegin = floorIndex > 8 ? floorIndex - 8 : 0;
            const std::size_t scanEnd = (std::min)(
                result.pe.runtimeFunctionCount,
                floorIndex + static_cast<std::size_t>(10));
            for (std::size_t index = scanBegin; index < scanEnd; ++index)
            {
                RuntimeFunctionRaw candidate{};
                std::uintptr_t candidateVa = 0;
                if (!ReadRuntimeFunctionEntry(result.pe, index, candidate, candidateVa) ||
                    !RuntimeFunctionLooksValid(snapshot, result.pe, candidate))
                {
                    continue;
                }
                if (result.targetRva >= candidate.beginAddress && result.targetRva < candidate.endAddress)
                {
                    result.enclosingFound = true;
                    result.entryIndex = index;
                    result.pdataEntryVa = candidateVa;
                    result.entry = candidate;
                    break;
                }
            }
        }

        std::size_t previousIndex = 0;
        bool previousIndexValid = false;
        std::size_t nextIndex = 0;
        bool nextIndexValid = false;
        if (result.enclosingFound)
        {
            if (result.entryIndex > 0)
            {
                previousIndex = result.entryIndex - 1;
                previousIndexValid = true;
            }
            if (result.entryIndex + 1 < result.pe.runtimeFunctionCount)
            {
                nextIndex = result.entryIndex + 1;
                nextIndexValid = true;
            }
        }
        else
        {
            if (hasFloorEntry)
            {
                previousIndex = floorIndex;
                previousIndexValid = true;
            }
            if (low < result.pe.runtimeFunctionCount)
            {
                nextIndex = low;
                nextIndexValid = true;
            }
        }

        std::uintptr_t ignoredVa = 0;
        if (previousIndexValid &&
            ReadRuntimeFunctionEntry(result.pe, previousIndex, result.previous, ignoredVa) &&
            RuntimeFunctionLooksValid(snapshot, result.pe, result.previous))
        {
            result.previousFound = true;
            result.previousIndex = previousIndex;
        }
        if (nextIndexValid &&
            ReadRuntimeFunctionEntry(result.pe, nextIndex, result.next, ignoredVa) &&
            RuntimeFunctionLooksValid(snapshot, result.pe, result.next))
        {
            result.nextFound = true;
            result.nextIndex = nextIndex;
        }

        if (result.enclosingFound)
        {
            result.functionSize = result.entry.endAddress - result.entry.beginAddress;
            result.offsetInsideFunction = result.targetRva - result.entry.beginAddress;
            result.bytesUntilFunctionEnd = result.entry.endAddress - result.targetRva;
            result.classification = result.offsetInsideFunction == 0
                ? "exact_runtime_function_begin"
                : "inside_runtime_function";
            result.diagnostic = result.offsetInsideFunction == 0
                ? "target exactly matches a .pdata RUNTIME_FUNCTION begin RVA"
                : "target lies inside a .pdata RUNTIME_FUNCTION and is not its begin RVA";
            result.unwind = AnalyzeUnwindInfo(snapshot, result.pe, result.entry);

            const std::uintptr_t functionBeginVa = moduleBegin + result.entry.beginAddress;
            const std::uintptr_t functionEndVa = moduleBegin + result.entry.endAddress;
            const std::size_t functionSize = result.functionSize;
            result.functionHeadBytes = HexModuleBytes(
                snapshot, functionBeginVa, (std::min)(functionSize, static_cast<std::size_t>(256)), true);

            const std::size_t tailCount = (std::min)(functionSize, static_cast<std::size_t>(128));
            result.functionTailBytes = HexModuleBytes(
                snapshot, functionEndVa - tailCount, tailCount, true);

            const std::size_t beforeCount = (std::min)(
                static_cast<std::size_t>(64),
                static_cast<std::size_t>(functionBeginVa - moduleBegin));
            if (beforeCount)
                result.beforeFunctionBytes = HexModuleBytes(
                    snapshot, functionBeginVa - beforeCount, beforeCount, true);

            const std::size_t afterCount = (std::min)(
                static_cast<std::size_t>(64),
                static_cast<std::size_t>(moduleEnd - functionEndVa));
            if (afterCount)
                result.afterFunctionBytes = HexModuleBytes(
                    snapshot, functionEndVa, afterCount, true);
        }
        else if (result.previousFound && result.nextFound &&
                 result.targetRva >= result.previous.endAddress &&
                 result.targetRva < result.next.beginAddress)
        {
            result.classification = "between_runtime_functions";
            result.diagnostic = "target lies in a gap between adjacent .pdata functions";
        }
        else
        {
            result.classification = "no_enclosing_runtime_function";
            result.diagnostic = "no .pdata RUNTIME_FUNCTION encloses the target";
        }

        const std::size_t bytesBeforeTarget = (std::min)(
            static_cast<std::size_t>(96),
            static_cast<std::size_t>(targetVa - moduleBegin));
        result.candidateWindowStartVa = targetVa - bytesBeforeTarget;
        result.candidateWindowOffset = bytesBeforeTarget;
        const std::size_t candidateAvailable = static_cast<std::size_t>(moduleEnd - result.candidateWindowStartVa);
        const std::size_t candidateWindowCount = (std::min)(
            candidateAvailable,
            static_cast<std::size_t>(256));
        result.candidateWindowBytes = HexModuleBytes(
            snapshot,
            result.candidateWindowStartVa,
            candidateWindowCount,
            true);
        return result;
    }

    std::string RuntimeFunctionRawJson(
        const Snapshot& snapshot,
        const RuntimeFunctionRaw& entry,
        std::size_t index,
        bool present)
    {
        if (!present)
            return "null";
        std::ostringstream out;
        out << '{'
            << "\"index\":" << index << ','
            << "\"begin_rva\":\"" << HexValue(entry.beginAddress) << "\","
            << "\"begin_va\":\"" << HexValue(snapshot.atonpTrackerBase + entry.beginAddress) << "\","
            << "\"end_rva\":\"" << HexValue(entry.endAddress) << "\","
            << "\"end_va\":\"" << HexValue(snapshot.atonpTrackerBase + entry.endAddress) << "\","
            << "\"size\":" << (entry.endAddress >= entry.beginAddress
                ? entry.endAddress - entry.beginAddress : 0) << ','
            << "\"unwind_data\":\"" << HexValue(entry.unwindData) << "\"}"
            ;
        return out.str();
    }

    std::string UnwindInfoJson(const UnwindInfoAnalysis& unwind)
    {
        std::ostringstream out;
        out << '{'
            << "\"valid\":" << (unwind.valid ? "true" : "false") << ','
            << "\"raw_unwind_data\":\"" << HexValue(unwind.rawUnwindData) << "\","
            << "\"unwind_info_rva\":\"" << HexValue(unwind.unwindInfoRva) << "\","
            << "\"unwind_info_va\":\"" << HexValue(unwind.unwindInfoVa) << "\","
            << "\"version\":" << unwind.version << ','
            << "\"flags\":" << unwind.flags << ','
            << "\"size_of_prolog\":" << unwind.sizeOfProlog << ','
            << "\"count_of_codes\":" << unwind.countOfCodes << ','
            << "\"frame_register\":" << unwind.frameRegister << ','
            << "\"frame_offset\":" << unwind.frameOffset << ','
            << "\"has_exception_handler\":" << (unwind.hasExceptionHandler ? "true" : "false") << ','
            << "\"has_termination_handler\":" << (unwind.hasTerminationHandler ? "true" : "false") << ','
            << "\"has_chain_info\":" << (unwind.hasChainInfo ? "true" : "false") << ','
            << "\"handler_rva\":\"" << HexValue(unwind.handlerRva) << "\","
            << "\"chained_begin_rva\":\"" << HexValue(unwind.chainedFunction.beginAddress) << "\","
            << "\"chained_end_rva\":\"" << HexValue(unwind.chainedFunction.endAddress) << "\","
            << "\"chained_unwind_data\":\"" << HexValue(unwind.chainedFunction.unwindData) << "\","
            << "\"bytes_64\":\"" << unwind.bytes64 << "\","
            << "\"diagnostic\":\"" << JsonEscape(unwind.diagnostic) << "\"}"
            ;
        return out.str();
    }

    std::string RuntimeFunctionBoundaryJson(
        const Snapshot& snapshot,
        const RuntimeFunctionBoundaryAnalysis& boundary)
    {
        const std::uintptr_t functionBeginVa = boundary.enclosingFound
            ? snapshot.atonpTrackerBase + boundary.entry.beginAddress : 0;
        const std::uintptr_t functionEndVa = boundary.enclosingFound
            ? snapshot.atonpTrackerBase + boundary.entry.endAddress : 0;
        std::ostringstream out;
        out << '{'
            << "\"target_va\":\"" << HexValue(boundary.targetVa) << "\","
            << "\"target_rva\":\"" << HexValue(boundary.targetRva) << "\","
            << "\"target_in_module\":" << (boundary.targetInModule ? "true" : "false") << ','
            << "\"classification\":\"" << JsonEscape(boundary.classification) << "\","
            << "\"candidate_is_function_begin\":"
            << (boundary.enclosingFound && boundary.offsetInsideFunction == 0 ? "true" : "false") << ','
            << "\"candidate_is_inside_function\":"
            << (boundary.enclosingFound && boundary.offsetInsideFunction != 0 ? "true" : "false") << ','
            << "\"enclosing_found\":" << (boundary.enclosingFound ? "true" : "false") << ','
            << "\"pdata_entry_index\":" << (boundary.enclosingFound ? boundary.entryIndex : 0) << ','
            << "\"pdata_entry_va\":\"" << HexValue(boundary.pdataEntryVa) << "\","
            << "\"function_begin_rva\":\"" << HexValue(boundary.entry.beginAddress) << "\","
            << "\"function_begin_va\":\"" << HexValue(functionBeginVa) << "\","
            << "\"function_end_rva\":\"" << HexValue(boundary.entry.endAddress) << "\","
            << "\"function_end_va\":\"" << HexValue(functionEndVa) << "\","
            << "\"function_size\":" << boundary.functionSize << ','
            << "\"offset_inside_function\":" << boundary.offsetInsideFunction << ','
            << "\"bytes_until_function_end\":" << boundary.bytesUntilFunctionEnd << ','
            << "\"unwind_data\":\"" << HexValue(boundary.entry.unwindData) << "\","
            << "\"previous_function\":"
            << RuntimeFunctionRawJson(snapshot, boundary.previous, boundary.previousIndex, boundary.previousFound) << ','
            << "\"next_function\":"
            << RuntimeFunctionRawJson(snapshot, boundary.next, boundary.nextIndex, boundary.nextFound) << ','
            << "\"pe_headers_valid\":" << (boundary.pe.headersValid ? "true" : "false") << ','
            << "\"pe64\":" << (boundary.pe.pe64 ? "true" : "false") << ','
            << "\"machine\":\"" << HexValue(boundary.pe.machine) << "\","
            << "\"pe_timestamp\":\"" << HexValue(boundary.pe.timeDateStamp) << "\","
            << "\"pe_size_of_image\":" << boundary.pe.sizeOfImage << ','
            << "\"pe_checksum\":\"" << HexValue(boundary.pe.checksum) << "\","
            << "\"exception_directory_valid\":"
            << (boundary.pe.exceptionDirectoryValid ? "true" : "false") << ','
            << "\"exception_directory_rva\":\"" << HexValue(boundary.pe.exceptionDirectoryRva) << "\","
            << "\"exception_directory_va\":\"" << HexValue(boundary.pe.exceptionDirectoryVa) << "\","
            << "\"exception_directory_size\":" << boundary.pe.exceptionDirectorySize << ','
            << "\"runtime_function_count\":" << boundary.pe.runtimeFunctionCount << ','
            << "\"exception_directory_trailing_bytes\":" << boundary.pe.trailingBytes << ','
            << "\"unwind_info\":" << UnwindInfoJson(boundary.unwind) << ','
            << "\"candidate_window_start_va\":\"" << HexValue(boundary.candidateWindowStartVa) << "\","
            << "\"candidate_window_offset\":" << boundary.candidateWindowOffset << ','
            << "\"candidate_window_bytes_256\":\"" << boundary.candidateWindowBytes << "\","
            << "\"function_head_bytes_256\":\"" << boundary.functionHeadBytes << "\","
            << "\"function_tail_bytes_128\":\"" << boundary.functionTailBytes << "\","
            << "\"bytes_before_function_begin_64\":\"" << boundary.beforeFunctionBytes << "\","
            << "\"bytes_after_function_end_64\":\"" << boundary.afterFunctionBytes << "\","
            << "\"pe_diagnostic\":\"" << JsonEscape(boundary.pe.diagnostic) << "\","
            << "\"diagnostic\":\"" << JsonEscape(boundary.diagnostic) << "\"}"
            ;
        return out.str();
    }

    std::string ExtractorContractAnalysisJson(
        const Snapshot& snapshot,
        const ExtractorAttempt& attempt)
    {
        std::uintptr_t primaryVtable = 0;
        std::uintptr_t secondaryVtable = 0;
        if (attempt.tabView)
            (void)SafeReadValue(reinterpret_cast<void*>(attempt.tabView), primaryVtable);
        if (attempt.tabView)
            (void)SafeReadValue(reinterpret_cast<void*>(attempt.tabView + 0x48), secondaryVtable);

        std::string thunkKind;
        const std::uintptr_t resolved = ResolveInitialThunk(attempt.function, thunkKind);
        const RuntimeFunctionBoundaryAnalysis boundary =
            AnalyzeRuntimeFunctionBoundary(snapshot, attempt.function);
        RuntimeFunctionBoundaryAnalysis resolvedBoundary;
        const bool resolvedDiffers = resolved && resolved != attempt.function;
        if (resolvedDiffers)
            resolvedBoundary = AnalyzeRuntimeFunctionBoundary(snapshot, resolved);

        std::ostringstream out;
        out << '{'
            << "\"name\":\"" << JsonEscape(attempt.name) << "\","
            << "\"module_base\":\"" << HexValue(snapshot.atonpTrackerBase) << "\","
            << "\"module_size\":" << snapshot.atonpTrackerSize << ','
            << "\"candidate_va\":\"" << HexValue(attempt.function) << "\","
            << "\"candidate_rva\":\""
            << HexValue(attempt.function >= snapshot.atonpTrackerBase
                ? attempt.function - snapshot.atonpTrackerBase : 0) << "\","
            << "\"resolved_entry\":\"" << HexValue(resolved) << "\","
            << "\"initial_thunk\":\"" << JsonEscape(thunkKind) << "\","
            << "\"boundary\":" << RuntimeFunctionBoundaryJson(snapshot, boundary) << ','
            << "\"resolved_boundary\":"
            << (resolvedDiffers ? RuntimeFunctionBoundaryJson(snapshot, resolvedBoundary) : "null") << ','
            << "\"primary_this\":\"" << HexValue(attempt.tabView) << "\","
            << "\"primary_vtable\":\"" << HexValue(primaryVtable) << "\","
            << "\"secondary_this\":\"" << HexValue(attempt.tabView ? attempt.tabView + 0x48 : 0) << "\","
            << "\"secondary_vtable\":\"" << HexValue(secondaryVtable) << "\","
            << "\"trade_info\":\"" << HexValue(attempt.tradeInfo) << "\","
            << "\"live_call_performed\":false,"
            << "\"safety_note\":\"V147 parses PE .pdata and UNWIND_INFO only; no extractor is called\"}"
            ;
        return out.str();
    }

    void AppendBoundaryReportSection(
        std::ostringstream& out,
        const Snapshot& snapshot,
        const ExtractorAttempt& attempt)
    {
        std::string thunkKind;
        const std::uintptr_t resolved = ResolveInitialThunk(attempt.function, thunkKind);
        const RuntimeFunctionBoundaryAnalysis boundary =
            AnalyzeRuntimeFunctionBoundary(snapshot, attempt.function);
        const std::uintptr_t functionBeginVa = boundary.enclosingFound
            ? snapshot.atonpTrackerBase + boundary.entry.beginAddress : 0;
        const std::uintptr_t functionEndVa = boundary.enclosingFound
            ? snapshot.atonpTrackerBase + boundary.entry.endAddress : 0;
        out << "\r\n[" << attempt.name << "]\r\n"
            << "candidate_va=" << HexValue(attempt.function) << "\r\n"
            << "candidate_rva=" << HexValue(boundary.targetRva) << "\r\n"
            << "initial_thunk=" << thunkKind << "\r\n"
            << "resolved_entry=" << HexValue(resolved) << "\r\n"
            << "classification=" << boundary.classification << "\r\n"
            << "candidate_is_function_begin="
            << (boundary.enclosingFound && boundary.offsetInsideFunction == 0 ? "yes" : "no") << "\r\n"
            << "candidate_is_inside_function="
            << (boundary.enclosingFound && boundary.offsetInsideFunction != 0 ? "yes" : "no") << "\r\n"
            << "enclosing_found=" << (boundary.enclosingFound ? "yes" : "no") << "\r\n"
            << "pdata_entry_index=" << (boundary.enclosingFound ? boundary.entryIndex : 0) << "\r\n"
            << "pdata_entry_va=" << HexValue(boundary.pdataEntryVa) << "\r\n"
            << "function_begin_rva=" << HexValue(boundary.entry.beginAddress) << "\r\n"
            << "function_begin_va=" << HexValue(functionBeginVa) << "\r\n"
            << "function_end_rva=" << HexValue(boundary.entry.endAddress) << "\r\n"
            << "function_end_va=" << HexValue(functionEndVa) << "\r\n"
            << "function_size=" << boundary.functionSize << "\r\n"
            << "offset_inside_function=" << boundary.offsetInsideFunction << "\r\n"
            << "bytes_until_function_end=" << boundary.bytesUntilFunctionEnd << "\r\n"
            << "unwind_data=" << HexValue(boundary.entry.unwindData) << "\r\n"
            << "unwind_info_rva=" << HexValue(boundary.unwind.unwindInfoRva) << "\r\n"
            << "unwind_version=" << boundary.unwind.version << "\r\n"
            << "unwind_flags=" << boundary.unwind.flags << "\r\n"
            << "unwind_size_of_prolog=" << boundary.unwind.sizeOfProlog << "\r\n"
            << "unwind_code_count=" << boundary.unwind.countOfCodes << "\r\n"
            << "previous_function="
            << RuntimeFunctionRawJson(snapshot, boundary.previous, boundary.previousIndex, boundary.previousFound)
            << "\r\n"
            << "next_function="
            << RuntimeFunctionRawJson(snapshot, boundary.next, boundary.nextIndex, boundary.nextFound)
            << "\r\n"
            << "candidate_window_start_va=" << HexValue(boundary.candidateWindowStartVa) << "\r\n"
            << "candidate_window_target_offset=" << boundary.candidateWindowOffset << "\r\n"
            << "candidate_window_bytes_256=" << boundary.candidateWindowBytes << "\r\n"
            << "function_head_bytes_256=" << boundary.functionHeadBytes << "\r\n"
            << "function_tail_bytes_128=" << boundary.functionTailBytes << "\r\n"
            << "bytes_before_function_begin_64=" << boundary.beforeFunctionBytes << "\r\n"
            << "bytes_after_function_end_64=" << boundary.afterFunctionBytes << "\r\n"
            << "unwind_info_bytes_64=" << boundary.unwind.bytes64 << "\r\n"
            << "diagnostic=" << boundary.diagnostic << "\r\n";
    }

    bool WriteContractAnalysisReport(
        const Snapshot& snapshot,
        const ExtractorAttempt& accounts,
        const ExtractorAttempt& positions,
        std::wstring& path)
    {
        path = ReportPath(L"MC_V147_Extractor_Function_Boundary_Analysis", snapshot.processId);
        RuntimeFunctionBoundaryAnalysis headerBoundary =
            AnalyzeRuntimeFunctionBoundary(snapshot, accounts.function);
        std::ostringstream out;
        out << "MC V147 Extractor Function Boundary Analysis\r\n"
            << "============================================\r\n"
            << "NO LIVE EXTRACTOR CALLS ARE PERFORMED IN V147.\r\n"
            << "This report parses the loaded ATOnPTracker PE32+ exception directory (.pdata).\r\n"
            << "process_id=" << snapshot.processId << "\r\n"
            << "tracker_thread_id=" << snapshot.trackerThreadId << "\r\n"
            << "module_base=" << HexValue(snapshot.atonpTrackerBase) << "\r\n"
            << "module_size=" << snapshot.atonpTrackerSize << "\r\n"
            << "pe_headers_valid=" << (headerBoundary.pe.headersValid ? "yes" : "no") << "\r\n"
            << "pe64=" << (headerBoundary.pe.pe64 ? "yes" : "no") << "\r\n"
            << "machine=" << HexValue(headerBoundary.pe.machine) << "\r\n"
            << "pe_timestamp=" << HexValue(headerBoundary.pe.timeDateStamp) << "\r\n"
            << "pe_size_of_image=" << headerBoundary.pe.sizeOfImage << "\r\n"
            << "pe_checksum=" << HexValue(headerBoundary.pe.checksum) << "\r\n"
            << "exception_directory_rva=" << HexValue(headerBoundary.pe.exceptionDirectoryRva) << "\r\n"
            << "exception_directory_va=" << HexValue(headerBoundary.pe.exceptionDirectoryVa) << "\r\n"
            << "exception_directory_size=" << headerBoundary.pe.exceptionDirectorySize << "\r\n"
            << "runtime_function_count=" << headerBoundary.pe.runtimeFunctionCount << "\r\n"
            << "exception_directory_trailing_bytes=" << headerBoundary.pe.trailingBytes << "\r\n"
            << "pe_diagnostic=" << headerBoundary.pe.diagnostic << "\r\n";
        AppendBoundaryReportSection(out, snapshot, accounts);
        AppendBoundaryReportSection(out, snapshot, positions);
        out << "\r\naccounts_json=" << ExtractorContractAnalysisJson(snapshot, accounts) << "\r\n"
            << "open_positions_json=" << ExtractorContractAnalysisJson(snapshot, positions) << "\r\n";
        return WriteUtf8File(path, out.str());
    }


    struct PeSectionAnalysis
    {
        std::string name;
        std::uint32_t virtualAddress = 0;
        std::uint32_t virtualSize = 0;
        std::uint32_t rawSize = 0;
        std::uint32_t characteristics = 0;
        std::uintptr_t beginVa = 0;
        std::uintptr_t endVa = 0;
        bool executable = false;
        bool readable = false;
    };

    struct RuntimeFunctionTableAnalysis
    {
        PeExceptionDirectoryAnalysis pe;
        std::vector<RuntimeFunctionRaw> entries;
        bool valid = false;
        bool sortedByBegin = false;
        std::string diagnostic;
    };

    struct VtableSlotAnalysis
    {
        std::string tableName;
        std::size_t slotIndex = 0;
        std::uintptr_t slotVa = 0;
        std::uintptr_t methodVa = 0;
        bool executable = false;
        bool inAtonpTracker = false;
        std::string moduleName;
        bool boundaryFound = false;
        std::size_t boundaryIndex = 0;
        RuntimeFunctionRaw boundary{};
        std::uint32_t offsetInsideFunction = 0;
    };

    struct VtableAnalysis
    {
        std::string name;
        std::uintptr_t objectVa = 0;
        std::uintptr_t vtableVa = 0;
        std::size_t requestedSlots = 0;
        bool exactSlotLimit = false;
        std::vector<VtableSlotAnalysis> slots;
        std::string stopReason;
    };

    struct RawCodeReference
    {
        std::string kind;
        std::uintptr_t siteVa = 0;
        std::uint32_t siteRva = 0;
        std::uintptr_t targetVa = 0;
        std::uint32_t targetRva = 0;
        std::uintptr_t pointerSlotVa = 0;
    };

    struct CallerGraphEdge
    {
        RawCodeReference reference;
        unsigned int rootMask = 0;
        unsigned int depth = 0;
        bool callerFound = false;
        std::size_t callerIndex = 0;
        RuntimeFunctionRaw caller{};
        std::uint32_t callerOffset = 0;
        std::string callWindowBytes;
    };

    struct CallerGraphNode
    {
        std::size_t functionIndex = 0;
        RuntimeFunctionRaw function{};
        unsigned int rootMask = 0;
        unsigned int minAccountsDepth = 0;
        unsigned int minPositionsDepth = 0;
        std::vector<std::size_t> edgeIndexes;
        std::vector<std::string> vtableMatches;
        std::size_t displacement1d0Count = 0;
        std::size_t displacement48Count = 0;
        int score = 0;
        std::string classification;
        std::string functionHeadBytes;
        std::string functionTailBytes;
    };

    struct CallerXrefAnalysis
    {
        RuntimeFunctionTableAnalysis runtimeTable;
        std::vector<PeSectionAnalysis> sections;
        std::size_t executableBytesScanned = 0;
        std::size_t rawReferenceCount = 0;
        bool rawReferenceLimitReached = false;
        RuntimeFunctionBoundaryAnalysis accountsBoundary;
        RuntimeFunctionBoundaryAnalysis positionsBoundary;
        std::vector<RawCodeReference> rawReferences;
        std::vector<CallerGraphEdge> selectedEdges;
        std::vector<CallerGraphNode> rankedNodes;
        VtableAnalysis primaryVtable;
        VtableAnalysis secondaryVtable;
        std::size_t accountsInteriorReferenceCount = 0;
        std::size_t positionsInteriorReferenceCount = 0;
        std::string diagnostic;
    };

    constexpr unsigned int kCallerRootAccounts = 0x01u;
    constexpr unsigned int kCallerRootPositions = 0x02u;
    constexpr unsigned int kCallerGraphMaximumDepth = 3u;
    constexpr std::size_t kMaximumRawCodeReferences = 250000u;
    constexpr std::size_t kMaximumSelectedGraphEdges = 8192u;
    constexpr std::size_t kMaximumVtableSlots = 128u;

    bool ParsePeSections(
        const Snapshot& snapshot,
        std::vector<PeSectionAnalysis>& sections,
        std::string& diagnostic)
    {
        sections.clear();
        diagnostic.clear();
        if (!snapshot.atonpTrackerBase || snapshot.atonpTrackerSize < sizeof(IMAGE_DOS_HEADER))
        {
            diagnostic = "ATOnPTracker module is unavailable";
            return false;
        }

        IMAGE_DOS_HEADER dos{};
        if (!SafeReadValue(reinterpret_cast<void*>(snapshot.atonpTrackerBase), dos) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
        {
            diagnostic = "DOS header is invalid";
            return false;
        }

        const std::uintptr_t ntAddress = snapshot.atonpTrackerBase +
            static_cast<std::uintptr_t>(dos.e_lfanew);
        IMAGE_NT_HEADERS64 nt{};
        if (!SafeReadValue(reinterpret_cast<void*>(ntAddress), nt) ||
            nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            diagnostic = "PE32+ header is invalid";
            return false;
        }
        if (!nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96)
        {
            diagnostic = "PE section count is invalid";
            return false;
        }

        const std::uintptr_t sectionTableVa = ntAddress +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            nt.FileHeader.SizeOfOptionalHeader;
        const std::size_t sectionTableBytes =
            static_cast<std::size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (!ModuleContains(snapshot, sectionTableVa, sectionTableBytes))
        {
            diagnostic = "PE section table is outside the module";
            return false;
        }

        for (std::size_t index = 0; index < nt.FileHeader.NumberOfSections; ++index)
        {
            IMAGE_SECTION_HEADER raw{};
            const std::uintptr_t entryVa = sectionTableVa + index * sizeof(IMAGE_SECTION_HEADER);
            if (!SafeReadValue(reinterpret_cast<void*>(entryVa), raw))
            {
                diagnostic = "PE section header could not be read";
                return false;
            }

            PeSectionAnalysis section;
            char name[9]{};
            std::memcpy(name, raw.Name, 8);
            std::size_t nameLength = 0;
            while (nameLength < 8 && name[nameLength] != '\0') ++nameLength;
            section.name.assign(name, nameLength);
            section.virtualAddress = raw.VirtualAddress;
            section.virtualSize = raw.Misc.VirtualSize;
            section.rawSize = raw.SizeOfRawData;
            section.characteristics = raw.Characteristics;
            section.executable = (raw.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            section.readable = (raw.Characteristics & IMAGE_SCN_MEM_READ) != 0;

            if (section.virtualAddress >= snapshot.atonpTrackerSize)
                continue;
            std::uint64_t span = (std::max)(
                static_cast<std::uint64_t>(section.virtualSize),
                static_cast<std::uint64_t>(section.rawSize));
            span = (std::min)(span,
                static_cast<std::uint64_t>(snapshot.atonpTrackerSize - section.virtualAddress));
            section.beginVa = snapshot.atonpTrackerBase + section.virtualAddress;
            section.endVa = section.beginVa + static_cast<std::uintptr_t>(span);
            sections.push_back(section);
        }

        diagnostic = "PE section table parsed";
        return !sections.empty();
    }

    bool LoadRuntimeFunctionTable(
        const Snapshot& snapshot,
        RuntimeFunctionTableAnalysis& table)
    {
        table = RuntimeFunctionTableAnalysis{};
        if (!ParsePeExceptionDirectory(snapshot, table.pe))
        {
            table.diagnostic = table.pe.diagnostic;
            return false;
        }
        if (table.pe.runtimeFunctionCount > 1000000u)
        {
            table.diagnostic = "RUNTIME_FUNCTION count exceeds safety limit";
            return false;
        }

        table.entries.resize(table.pe.runtimeFunctionCount);
        const std::size_t bytes = table.entries.size() * sizeof(RuntimeFunctionRaw);
        if (!ReadAnalysisBytes(
                reinterpret_cast<void*>(table.pe.exceptionDirectoryVa),
                table.entries.data(),
                bytes))
        {
            table.entries.clear();
            table.diagnostic = "RUNTIME_FUNCTION table could not be copied";
            return false;
        }

        table.sortedByBegin = true;
        for (std::size_t index = 0; index < table.entries.size(); ++index)
        {
            if (!RuntimeFunctionLooksValid(snapshot, table.pe, table.entries[index]))
            {
                table.diagnostic = "RUNTIME_FUNCTION table contains an invalid entry";
                return false;
            }
            if (index && table.entries[index - 1].beginAddress > table.entries[index].beginAddress)
                table.sortedByBegin = false;
        }
        if (!table.sortedByBegin)
        {
            table.diagnostic = "RUNTIME_FUNCTION table is not sorted by begin RVA";
            return false;
        }

        table.valid = true;
        std::ostringstream out;
        out << "RUNTIME_FUNCTION table loaded entries=" << table.entries.size();
        table.diagnostic = out.str();
        return true;
    }

    bool FindRuntimeFunctionInTable(
        const RuntimeFunctionTableAnalysis& table,
        std::uint32_t targetRva,
        std::size_t& index,
        RuntimeFunctionRaw& entry)
    {
        index = 0;
        entry = RuntimeFunctionRaw{};
        if (!table.valid || table.entries.empty())
            return false;

        std::size_t low = 0;
        std::size_t high = table.entries.size();
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            if (table.entries[middle].beginAddress <= targetRva)
                low = middle + 1;
            else
                high = middle;
        }

        const std::size_t floorIndex = low ? low - 1 : 0;
        const std::size_t scanBegin = floorIndex > 8 ? floorIndex - 8 : 0;
        const std::size_t scanEnd = (std::min)(table.entries.size(), floorIndex + 10u);
        for (std::size_t candidateIndex = scanBegin; candidateIndex < scanEnd; ++candidateIndex)
        {
            const RuntimeFunctionRaw& candidate = table.entries[candidateIndex];
            if (targetRva >= candidate.beginAddress && targetRva < candidate.endAddress)
            {
                index = candidateIndex;
                entry = candidate;
                return true;
            }
        }
        return false;
    }

    std::string ModuleNameForAddress(const Snapshot& snapshot, std::uintptr_t address)
    {
        for (const ModuleRecord& module : snapshot.modules)
        {
            const std::uintptr_t end = module.base + module.size;
            if (end >= module.base && address >= module.base && address < end)
            {
                const std::wstring display = module.name.empty()
                    ? BaseName(module.path) : module.name;
                return WideToUtf8(display);
            }
        }
        return "unknown";
    }

    VtableAnalysis AnalyzeVtableSlots(
        const Snapshot& snapshot,
        const RuntimeFunctionTableAnalysis& runtimeTable,
        const char* name,
        std::uintptr_t objectVa,
        std::uintptr_t vtableVa,
        std::uintptr_t exactEndVa)
    {
        VtableAnalysis result;
        result.name = name ? name : "vtable";
        result.objectVa = objectVa;
        result.vtableVa = vtableVa;
        if (!vtableVa)
        {
            result.stopReason = "vtable address is zero";
            return result;
        }

        std::size_t slotLimit = kMaximumVtableSlots;
        if (exactEndVa > vtableVa &&
            (exactEndVa - vtableVa) % sizeof(std::uintptr_t) == 0 &&
            exactEndVa - vtableVa <= kMaximumVtableSlots * sizeof(std::uintptr_t))
        {
            slotLimit = static_cast<std::size_t>((exactEndVa - vtableVa) / sizeof(std::uintptr_t));
            result.exactSlotLimit = true;
        }
        result.requestedSlots = slotLimit;

        for (std::size_t slotIndex = 0; slotIndex < slotLimit; ++slotIndex)
        {
            const std::uintptr_t slotVa = vtableVa + slotIndex * sizeof(std::uintptr_t);
            std::uintptr_t methodVa = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(slotVa), methodVa))
            {
                result.stopReason = "vtable slot is unreadable";
                break;
            }
            if (!methodVa)
            {
                result.stopReason = "zero method pointer terminates vtable";
                break;
            }
            if (!MemoryRangeHasProtection(reinterpret_cast<void*>(methodVa), 1, true))
            {
                result.stopReason = "first non-executable pointer terminates vtable";
                break;
            }

            VtableSlotAnalysis slot;
            slot.tableName = result.name;
            slot.slotIndex = slotIndex;
            slot.slotVa = slotVa;
            slot.methodVa = methodVa;
            slot.executable = true;
            slot.inAtonpTracker = ModuleContains(snapshot, methodVa, 1);
            slot.moduleName = ModuleNameForAddress(snapshot, methodVa);
            if (slot.inAtonpTracker)
            {
                const std::uint32_t methodRva = static_cast<std::uint32_t>(methodVa - snapshot.atonpTrackerBase);
                if (FindRuntimeFunctionInTable(
                        runtimeTable,
                        methodRva,
                        slot.boundaryIndex,
                        slot.boundary))
                {
                    slot.boundaryFound = true;
                    slot.offsetInsideFunction = methodRva - slot.boundary.beginAddress;
                }
            }
            result.slots.push_back(slot);
        }

        if (result.stopReason.empty())
        {
            result.stopReason = result.exactSlotLimit
                ? "exact next-vtable boundary reached"
                : "maximum vtable slot safety limit reached";
        }
        return result;
    }

    bool AddressInsideAtonp(const Snapshot& snapshot, std::uintptr_t address)
    {
        return ModuleContains(snapshot, address, 1);
    }

    void AddRawReference(
        const Snapshot& snapshot,
        std::vector<RawCodeReference>& references,
        bool& limitReached,
        const char* kind,
        std::uintptr_t siteVa,
        std::uintptr_t targetVa,
        std::uintptr_t pointerSlotVa)
    {
        if (!AddressInsideAtonp(snapshot, targetVa))
            return;
        if (references.size() >= kMaximumRawCodeReferences)
        {
            limitReached = true;
            return;
        }
        RawCodeReference reference;
        reference.kind = kind ? kind : "unknown";
        reference.siteVa = siteVa;
        reference.siteRva = static_cast<std::uint32_t>(siteVa - snapshot.atonpTrackerBase);
        reference.targetVa = targetVa;
        reference.targetRva = static_cast<std::uint32_t>(targetVa - snapshot.atonpTrackerBase);
        reference.pointerSlotVa = pointerSlotVa;
        references.push_back(reference);
    }

    bool ScanExecutableCodeReferences(
        const Snapshot& snapshot,
        const std::vector<PeSectionAnalysis>& sections,
        std::vector<RawCodeReference>& references,
        std::size_t& bytesScanned,
        bool& limitReached,
        std::string& diagnostic)
    {
        references.clear();
        bytesScanned = 0;
        limitReached = false;
        diagnostic.clear();

        for (const PeSectionAnalysis& section : sections)
        {
            if (!section.executable || section.endVa <= section.beginVa)
                continue;
            const std::size_t sectionBytes = static_cast<std::size_t>(section.endVa - section.beginVa);
            if (!sectionBytes || sectionBytes > 64u * 1024u * 1024u)
                continue;

            std::vector<unsigned char> bytes(sectionBytes);
            if (!ReadAnalysisBytes(
                    reinterpret_cast<void*>(section.beginVa),
                    bytes.data(),
                    bytes.size()))
            {
                diagnostic += " unreadable_section=" + section.name;
                continue;
            }
            bytesScanned += bytes.size();

            for (std::size_t offset = 0; offset < bytes.size(); ++offset)
            {
                if (limitReached)
                    break;
                const std::uintptr_t siteVa = section.beginVa + offset;
                const unsigned char opcode = bytes[offset];
                if ((opcode == 0xE8 || opcode == 0xE9) && offset + 5 <= bytes.size())
                {
                    std::int32_t relative = 0;
                    std::memcpy(&relative, bytes.data() + offset + 1, sizeof(relative));
                    const std::intptr_t targetSigned =
                        static_cast<std::intptr_t>(siteVa + 5) + relative;
                    if (targetSigned > 0)
                    {
                        AddRawReference(
                            snapshot,
                            references,
                            limitReached,
                            opcode == 0xE8 ? "call_rel32" : "jmp_rel32",
                            siteVa,
                            static_cast<std::uintptr_t>(targetSigned),
                            0);
                    }
                }
                else if (opcode == 0xFF && offset + 6 <= bytes.size() &&
                         (bytes[offset + 1] == 0x15 || bytes[offset + 1] == 0x25))
                {
                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, bytes.data() + offset + 2, sizeof(displacement));
                    const std::intptr_t slotSigned =
                        static_cast<std::intptr_t>(siteVa + 6) + displacement;
                    if (slotSigned > 0)
                    {
                        const std::uintptr_t pointerSlotVa = static_cast<std::uintptr_t>(slotSigned);
                        std::uintptr_t targetVa = 0;
                        if (SafeReadValue(reinterpret_cast<void*>(pointerSlotVa), targetVa))
                        {
                            AddRawReference(
                                snapshot,
                                references,
                                limitReached,
                                bytes[offset + 1] == 0x15
                                    ? "call_rip_indirect" : "jmp_rip_indirect",
                                siteVa,
                                targetVa,
                                pointerSlotVa);
                        }
                    }
                }
            }
        }

        std::sort(references.begin(), references.end(), [](const RawCodeReference& a, const RawCodeReference& b) {
            if (a.siteRva != b.siteRva) return a.siteRva < b.siteRva;
            if (a.targetRva != b.targetRva) return a.targetRva < b.targetRva;
            return a.kind < b.kind;
        });
        references.erase(std::unique(references.begin(), references.end(),
            [](const RawCodeReference& a, const RawCodeReference& b) {
                return a.siteRva == b.siteRva && a.targetRva == b.targetRva && a.kind == b.kind;
            }), references.end());

        std::ostringstream out;
        out << "raw x64 opcode scan completed executable_bytes=" << bytesScanned
            << " internal_references=" << references.size()
            << " limit_reached=" << (limitReached ? "yes" : "no");
        if (!diagnostic.empty()) out << diagnostic;
        diagnostic = out.str();
        return bytesScanned != 0;
    }

    bool ReferenceIsTailTransfer(
        const RawCodeReference& reference,
        const RuntimeFunctionRaw& caller)
    {
        if (reference.kind == "call_rel32" || reference.kind == "call_rip_indirect")
            return true;
        if (reference.kind != "jmp_rel32" && reference.kind != "jmp_rip_indirect")
            return false;
        if (reference.targetRva >= caller.beginAddress && reference.targetRva < caller.endAddress)
            return false;
        return reference.siteRva + 32u >= caller.endAddress;
    }

    std::size_t CountDwordPattern(
        const std::vector<unsigned char>& bytes,
        std::uint32_t value)
    {
        unsigned char pattern[4]{};
        std::memcpy(pattern, &value, sizeof(pattern));
        std::size_t count = 0;
        for (std::size_t index = 0; index + sizeof(pattern) <= bytes.size(); ++index)
        {
            if (std::memcmp(bytes.data() + index, pattern, sizeof(pattern)) == 0)
                ++count;
        }
        return count;
    }

    std::vector<std::string> FindVtableMatches(
        const CallerGraphNode& node,
        const VtableAnalysis& primary,
        const VtableAnalysis& secondary)
    {
        std::vector<std::string> matches;
        auto appendMatches = [&](const VtableAnalysis& table) {
            for (const VtableSlotAnalysis& slot : table.slots)
            {
                if (!slot.boundaryFound ||
                    slot.boundary.beginAddress != node.function.beginAddress)
                {
                    continue;
                }
                std::ostringstream label;
                label << table.name << '[' << slot.slotIndex << ']';
                matches.push_back(label.str());
            }
        };
        appendMatches(primary);
        appendMatches(secondary);
        std::sort(matches.begin(), matches.end());
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
        return matches;
    }

    void AnalyzeCallerNodeCode(
        const Snapshot& snapshot,
        CallerGraphNode& node)
    {
        const std::uintptr_t beginVa = snapshot.atonpTrackerBase + node.function.beginAddress;
        const std::size_t functionSize = node.function.endAddress - node.function.beginAddress;
        const std::size_t bytesToRead = (std::min)(functionSize, static_cast<std::size_t>(65536));
        std::vector<unsigned char> bytes(bytesToRead);
        if (!bytes.empty() && ReadAnalysisBytes(
                reinterpret_cast<void*>(beginVa), bytes.data(), bytes.size()))
        {
            node.displacement1d0Count = CountDwordPattern(bytes, 0x000001D0u);
            node.displacement48Count = CountDwordPattern(bytes, 0x00000048u);
        }
        node.functionHeadBytes = HexModuleBytes(
            snapshot, beginVa, (std::min)(functionSize, static_cast<std::size_t>(256)), true);
        const std::size_t tailBytes = (std::min)(functionSize, static_cast<std::size_t>(128));
        node.functionTailBytes = HexModuleBytes(
            snapshot,
            snapshot.atonpTrackerBase + node.function.endAddress - tailBytes,
            tailBytes,
            true);
    }

    void BuildCallerGraph(
        const Snapshot& snapshot,
        CallerXrefAnalysis& analysis)
    {
        if (!analysis.runtimeTable.valid ||
            !analysis.accountsBoundary.enclosingFound ||
            !analysis.positionsBoundary.enclosingFound)
        {
            analysis.diagnostic += "; caller graph roots are unavailable";
            return;
        }

        const std::uint32_t accountsRoot = analysis.accountsBoundary.entry.beginAddress;
        const std::uint32_t positionsRoot = analysis.positionsBoundary.entry.beginAddress;
        std::map<std::uint32_t, unsigned int> frontier;
        frontier[accountsRoot] |= kCallerRootAccounts;
        frontier[positionsRoot] |= kCallerRootPositions;
        std::map<std::uint32_t, unsigned int> discovered;
        discovered[accountsRoot] |= kCallerRootAccounts;
        discovered[positionsRoot] |= kCallerRootPositions;
        std::map<std::uint32_t, CallerGraphNode> nodes;

        for (unsigned int depth = 1; depth <= kCallerGraphMaximumDepth && !frontier.empty(); ++depth)
        {
            std::map<std::uint32_t, unsigned int> nextFrontier;
            for (const RawCodeReference& reference : analysis.rawReferences)
            {
                const auto target = frontier.find(reference.targetRva);
                if (target == frontier.end())
                    continue;

                std::size_t callerIndex = 0;
                RuntimeFunctionRaw caller{};
                if (!FindRuntimeFunctionInTable(
                        analysis.runtimeTable,
                        reference.siteRva,
                        callerIndex,
                        caller))
                {
                    continue;
                }
                if (!ReferenceIsTailTransfer(reference, caller))
                    continue;
                if (analysis.selectedEdges.size() >= kMaximumSelectedGraphEdges)
                {
                    analysis.diagnostic += "; selected caller edge limit reached";
                    break;
                }

                CallerGraphEdge edge;
                edge.reference = reference;
                edge.rootMask = target->second;
                edge.depth = depth;
                edge.callerFound = true;
                edge.callerIndex = callerIndex;
                edge.caller = caller;
                edge.callerOffset = reference.siteRva - caller.beginAddress;
                const std::uintptr_t callerBeginVa =
                    snapshot.atonpTrackerBase + caller.beginAddress;
                const std::uintptr_t callerEndVa =
                    snapshot.atonpTrackerBase + caller.endAddress;
                const std::uintptr_t windowStart = reference.siteVa >= callerBeginVa + 64
                    ? reference.siteVa - 64 : callerBeginVa;
                const std::size_t windowBytes = callerEndVa > windowStart
                    ? (std::min)(static_cast<std::size_t>(callerEndVa - windowStart),
                                 static_cast<std::size_t>(160))
                    : 0;
                edge.callWindowBytes = HexModuleBytes(snapshot, windowStart, windowBytes, true);
                const std::size_t edgeIndex = analysis.selectedEdges.size();
                analysis.selectedEdges.push_back(edge);

                CallerGraphNode& node = nodes[caller.beginAddress];
                node.functionIndex = callerIndex;
                node.function = caller;
                node.rootMask |= target->second;
                node.edgeIndexes.push_back(edgeIndex);
                if ((target->second & kCallerRootAccounts) != 0 &&
                    (!node.minAccountsDepth || depth < node.minAccountsDepth))
                {
                    node.minAccountsDepth = depth;
                }
                if ((target->second & kCallerRootPositions) != 0 &&
                    (!node.minPositionsDepth || depth < node.minPositionsDepth))
                {
                    node.minPositionsDepth = depth;
                }

                const unsigned int newMask = target->second & ~discovered[caller.beginAddress];
                if (newMask)
                {
                    discovered[caller.beginAddress] |= newMask;
                    nextFrontier[caller.beginAddress] |= newMask;
                }
            }
            frontier.swap(nextFrontier);
        }

        for (auto& item : nodes)
        {
            CallerGraphNode& node = item.second;
            AnalyzeCallerNodeCode(snapshot, node);
            node.vtableMatches = FindVtableMatches(
                node, analysis.primaryVtable, analysis.secondaryVtable);

            const bool accounts = (node.rootMask & kCallerRootAccounts) != 0;
            const bool positions = (node.rootMask & kCallerRootPositions) != 0;
            if (accounts) node.score += 80;
            if (positions) node.score += 80;
            if (accounts && positions) node.score += 90;
            if (!node.vtableMatches.empty()) node.score += 70;
            if (node.displacement1d0Count) node.score += 60;
            if (node.displacement48Count) node.score += 10;
            const std::size_t functionSize = node.function.endAddress - node.function.beginAddress;
            if (functionSize <= 8192) node.score += 10;
            if (node.minAccountsDepth)
                node.score += static_cast<int>(40u - (std::min)(node.minAccountsDepth, 3u) * 8u);
            if (node.minPositionsDepth)
                node.score += static_cast<int>(40u - (std::min)(node.minPositionsDepth, 3u) * 8u);

            if (accounts && positions &&
                (!node.vtableMatches.empty() || node.displacement1d0Count))
                node.classification = "strong_shared_high_level_candidate";
            else if (!node.vtableMatches.empty() && (accounts || positions))
                node.classification = "CATPTTabView_vtable_path_candidate";
            else if (node.displacement1d0Count && (accounts || positions))
                node.classification = "trade_info_field_path_candidate";
            else if (accounts && positions)
                node.classification = "shared_caller_graph_candidate";
            else
                node.classification = "single_branch_caller_graph_candidate";

            analysis.rankedNodes.push_back(node);
        }

        std::sort(analysis.rankedNodes.begin(), analysis.rankedNodes.end(),
            [](const CallerGraphNode& a, const CallerGraphNode& b) {
                if (a.score != b.score) return a.score > b.score;
                return a.function.beginAddress < b.function.beginAddress;
            });
    }

    std::string RootMaskText(unsigned int mask)
    {
        if (mask == (kCallerRootAccounts | kCallerRootPositions)) return "accounts+open_positions";
        if ((mask & kCallerRootAccounts) != 0) return "accounts";
        if ((mask & kCallerRootPositions) != 0) return "open_positions";
        return "none";
    }

    CallerXrefAnalysis BuildCallerXrefAnalysis(
        const Snapshot& snapshot,
        const ExtractorAttempt& accounts,
        const ExtractorAttempt& positions)
    {
        CallerXrefAnalysis analysis;
        analysis.accountsBoundary = AnalyzeRuntimeFunctionBoundary(snapshot, accounts.function);
        analysis.positionsBoundary = AnalyzeRuntimeFunctionBoundary(snapshot, positions.function);

        std::string sectionDiagnostic;
        (void)ParsePeSections(snapshot, analysis.sections, sectionDiagnostic);
        (void)LoadRuntimeFunctionTable(snapshot, analysis.runtimeTable);

        std::uintptr_t primaryVtable = 0;
        std::uintptr_t secondaryVtable = 0;
        if (accounts.tabView)
        {
            (void)SafeReadValue(reinterpret_cast<void*>(accounts.tabView), primaryVtable);
            (void)SafeReadValue(reinterpret_cast<void*>(accounts.tabView + 0x48), secondaryVtable);
        }
        analysis.primaryVtable = AnalyzeVtableSlots(
            snapshot,
            analysis.runtimeTable,
            "primary",
            accounts.tabView,
            primaryVtable,
            secondaryVtable > primaryVtable ? secondaryVtable : 0);
        analysis.secondaryVtable = AnalyzeVtableSlots(
            snapshot,
            analysis.runtimeTable,
            "secondary_at_0x48",
            accounts.tabView ? accounts.tabView + 0x48 : 0,
            secondaryVtable,
            0);

        std::string scanDiagnostic;
        (void)ScanExecutableCodeReferences(
            snapshot,
            analysis.sections,
            analysis.rawReferences,
            analysis.executableBytesScanned,
            analysis.rawReferenceLimitReached,
            scanDiagnostic);
        analysis.rawReferenceCount = analysis.rawReferences.size();

        for (const RawCodeReference& reference : analysis.rawReferences)
        {
            if (reference.targetRva == analysis.accountsBoundary.targetRva)
                ++analysis.accountsInteriorReferenceCount;
            if (reference.targetRva == analysis.positionsBoundary.targetRva)
                ++analysis.positionsInteriorReferenceCount;
        }

        analysis.diagnostic = sectionDiagnostic + "; " +
            analysis.runtimeTable.diagnostic + "; " + scanDiagnostic;
        BuildCallerGraph(snapshot, analysis);
        return analysis;
    }

    std::string VtableSummaryJson(
        const Snapshot& snapshot,
        const VtableAnalysis& table)
    {
        std::ostringstream out;
        out << '{'
            << "\"name\":\"" << JsonEscape(table.name) << "\","
            << "\"object_va\":\"" << HexValue(table.objectVa) << "\","
            << "\"vtable_va\":\"" << HexValue(table.vtableVa) << "\","
            << "\"vtable_rva\":\""
            << HexValue(table.vtableVa >= snapshot.atonpTrackerBase
                ? table.vtableVa - snapshot.atonpTrackerBase : 0) << "\","
            << "\"slot_count\":" << table.slots.size() << ','
            << "\"exact_slot_limit\":" << (table.exactSlotLimit ? "true" : "false") << ','
            << "\"stop_reason\":\"" << JsonEscape(table.stopReason) << "\"}";
        return out.str();
    }

    std::string CallerNodeSummaryJson(
        const Snapshot& snapshot,
        const CallerGraphNode& node)
    {
        std::ostringstream out;
        out << '{'
            << "\"function_begin_rva\":\"" << HexValue(node.function.beginAddress) << "\","
            << "\"function_begin_va\":\""
            << HexValue(snapshot.atonpTrackerBase + node.function.beginAddress) << "\","
            << "\"function_end_rva\":\"" << HexValue(node.function.endAddress) << "\","
            << "\"score\":" << node.score << ','
            << "\"root_paths\":\"" << RootMaskText(node.rootMask) << "\","
            << "\"accounts_depth\":" << node.minAccountsDepth << ','
            << "\"open_positions_depth\":" << node.minPositionsDepth << ','
            << "\"vtable_match_count\":" << node.vtableMatches.size() << ','
            << "\"disp_0x1d0_count\":" << node.displacement1d0Count << ','
            << "\"classification\":\"" << JsonEscape(node.classification) << "\"}";
        return out.str();
    }

    std::string CallerXrefSummaryJson(
        const Snapshot& snapshot,
        const CallerXrefAnalysis& analysis,
        const std::wstring& reportPath,
        bool reportWritten)
    {
        std::ostringstream out;
        out << '{'
            << "\"bridge_version\":" << kBridgeVersion << ','
            << "\"analysis_mode\":\"safe_recursive_caller_xref_v147\","
            << "\"live_call_performed\":false,"
            << "\"executable_bytes_scanned\":" << analysis.executableBytesScanned << ','
            << "\"raw_internal_reference_count\":" << analysis.rawReferenceCount << ','
            << "\"selected_graph_edge_count\":" << analysis.selectedEdges.size() << ','
            << "\"ranked_caller_count\":" << analysis.rankedNodes.size() << ','
            << "\"accounts_candidate_interior_xrefs\":"
            << analysis.accountsInteriorReferenceCount << ','
            << "\"open_positions_candidate_interior_xrefs\":"
            << analysis.positionsInteriorReferenceCount << ','
            << "\"primary_vtable\":" << VtableSummaryJson(snapshot, analysis.primaryVtable) << ','
            << "\"secondary_vtable\":" << VtableSummaryJson(snapshot, analysis.secondaryVtable) << ','
            << "\"top_callers\":[";
        const std::size_t topCount = (std::min)(analysis.rankedNodes.size(), static_cast<std::size_t>(10));
        for (std::size_t index = 0; index < topCount; ++index)
        {
            if (index) out << ',';
            out << CallerNodeSummaryJson(snapshot, analysis.rankedNodes[index]);
        }
        out << "],\"report_written\":" << (reportWritten ? "true" : "false")
            << ",\"report_path\":" << JsonString(reportPath)
            << ",\"diagnostic\":\"" << JsonEscape(analysis.diagnostic) << "\"}";
        return out.str();
    }

    void AppendVtableReport(
        std::ostringstream& out,
        const Snapshot& snapshot,
        const VtableAnalysis& table)
    {
        out << "\r\n[VTABLE " << table.name << "]\r\n"
            << "object_va=" << HexValue(table.objectVa) << "\r\n"
            << "vtable_va=" << HexValue(table.vtableVa) << "\r\n"
            << "vtable_rva="
            << HexValue(table.vtableVa >= snapshot.atonpTrackerBase
                ? table.vtableVa - snapshot.atonpTrackerBase : 0) << "\r\n"
            << "slot_count=" << table.slots.size() << "\r\n"
            << "exact_slot_limit=" << (table.exactSlotLimit ? "yes" : "no") << "\r\n"
            << "stop_reason=" << table.stopReason << "\r\n";
        for (const VtableSlotAnalysis& slot : table.slots)
        {
            out << "slot=" << slot.slotIndex
                << " slot_va=" << HexValue(slot.slotVa)
                << " method_va=" << HexValue(slot.methodVa)
                << " module=" << slot.moduleName
                << " in_ATOnPTracker=" << (slot.inAtonpTracker ? "yes" : "no")
                << " boundary_found=" << (slot.boundaryFound ? "yes" : "no")
                << " function_begin_rva=" << HexValue(slot.boundary.beginAddress)
                << " function_end_rva=" << HexValue(slot.boundary.endAddress)
                << " offset_inside_function=" << slot.offsetInsideFunction
                << "\r\n";
        }
    }

    bool WriteCallerXrefAnalysisReport(
        const Snapshot& snapshot,
        const ExtractorAttempt& accounts,
        const ExtractorAttempt& positions,
        const CallerXrefAnalysis& analysis,
        std::wstring& path)
    {
        path = ReportPath(L"MC_V147_Extractor_Caller_Xref_Analysis", snapshot.processId);
        std::ostringstream out;
        out << "MC V147 Extractor Caller and Cross-Reference Analysis\r\n"
            << "=====================================================\r\n"
            << "NO INTERNAL EXTRACTOR OR CANDIDATE FUNCTION IS CALLED IN V147.\r\n"
            << "The xref scanner is a conservative raw x64 opcode scan. It does not prove instruction boundaries.\r\n"
            << "Caller candidates are ranked heuristically and must not be called without further validation.\r\n"
            << "process_id=" << snapshot.processId << "\r\n"
            << "tracker_thread_id=" << snapshot.trackerThreadId << "\r\n"
            << "module_base=" << HexValue(snapshot.atonpTrackerBase) << "\r\n"
            << "module_size=" << snapshot.atonpTrackerSize << "\r\n"
            << "runtime_function_count=" << analysis.runtimeTable.entries.size() << "\r\n"
            << "executable_bytes_scanned=" << analysis.executableBytesScanned << "\r\n"
            << "raw_internal_reference_count=" << analysis.rawReferenceCount << "\r\n"
            << "raw_reference_limit_reached=" << (analysis.rawReferenceLimitReached ? "yes" : "no") << "\r\n"
            << "caller_graph_maximum_depth=" << kCallerGraphMaximumDepth << "\r\n"
            << "selected_graph_edge_count=" << analysis.selectedEdges.size() << "\r\n"
            << "ranked_caller_count=" << analysis.rankedNodes.size() << "\r\n"
            << "diagnostic=" << analysis.diagnostic << "\r\n";

        out << "\r\n[TARGETS]\r\n"
            << "accounts_candidate_va=" << HexValue(accounts.function) << "\r\n"
            << "accounts_candidate_rva=" << HexValue(analysis.accountsBoundary.targetRva) << "\r\n"
            << "accounts_enclosing_begin_rva=" << HexValue(analysis.accountsBoundary.entry.beginAddress) << "\r\n"
            << "accounts_enclosing_end_rva=" << HexValue(analysis.accountsBoundary.entry.endAddress) << "\r\n"
            << "accounts_candidate_interior_xrefs=" << analysis.accountsInteriorReferenceCount << "\r\n"
            << "open_positions_candidate_va=" << HexValue(positions.function) << "\r\n"
            << "open_positions_candidate_rva=" << HexValue(analysis.positionsBoundary.targetRva) << "\r\n"
            << "open_positions_enclosing_begin_rva=" << HexValue(analysis.positionsBoundary.entry.beginAddress) << "\r\n"
            << "open_positions_enclosing_end_rva=" << HexValue(analysis.positionsBoundary.entry.endAddress) << "\r\n"
            << "open_positions_candidate_interior_xrefs=" << analysis.positionsInteriorReferenceCount << "\r\n";

        out << "\r\n[PE SECTIONS]\r\n";
        for (const PeSectionAnalysis& section : analysis.sections)
        {
            out << "name=" << section.name
                << " rva=" << HexValue(section.virtualAddress)
                << " virtual_size=" << section.virtualSize
                << " raw_size=" << section.rawSize
                << " characteristics=" << HexValue(section.characteristics)
                << " executable=" << (section.executable ? "yes" : "no")
                << " readable=" << (section.readable ? "yes" : "no")
                << " begin_va=" << HexValue(section.beginVa)
                << " end_va=" << HexValue(section.endVa)
                << "\r\n";
        }

        AppendVtableReport(out, snapshot, analysis.primaryVtable);
        AppendVtableReport(out, snapshot, analysis.secondaryVtable);

        out << "\r\n[SELECTED CALLER GRAPH EDGES]\r\n";
        for (std::size_t index = 0; index < analysis.selectedEdges.size(); ++index)
        {
            const CallerGraphEdge& edge = analysis.selectedEdges[index];
            out << "edge=" << index
                << " depth=" << edge.depth
                << " root=" << RootMaskText(edge.rootMask)
                << " kind=" << edge.reference.kind
                << " site_rva=" << HexValue(edge.reference.siteRva)
                << " site_va=" << HexValue(edge.reference.siteVa)
                << " target_rva=" << HexValue(edge.reference.targetRva)
                << " target_va=" << HexValue(edge.reference.targetVa)
                << " pointer_slot_va=" << HexValue(edge.reference.pointerSlotVa)
                << " caller_index=" << edge.callerIndex
                << " caller_begin_rva=" << HexValue(edge.caller.beginAddress)
                << " caller_end_rva=" << HexValue(edge.caller.endAddress)
                << " caller_offset=" << edge.callerOffset
                << "\r\n"
                << "call_window_bytes_160=" << edge.callWindowBytes << "\r\n";
        }

        out << "\r\n[RANKED CALLER CANDIDATES]\r\n";
        for (std::size_t rank = 0; rank < analysis.rankedNodes.size(); ++rank)
        {
            const CallerGraphNode& node = analysis.rankedNodes[rank];
            out << "\r\nrank=" << (rank + 1) << "\r\n"
                << "score=" << node.score << "\r\n"
                << "classification=" << node.classification << "\r\n"
                << "root_paths=" << RootMaskText(node.rootMask) << "\r\n"
                << "accounts_depth=" << node.minAccountsDepth << "\r\n"
                << "open_positions_depth=" << node.minPositionsDepth << "\r\n"
                << "function_index=" << node.functionIndex << "\r\n"
                << "function_begin_rva=" << HexValue(node.function.beginAddress) << "\r\n"
                << "function_begin_va=" << HexValue(snapshot.atonpTrackerBase + node.function.beginAddress) << "\r\n"
                << "function_end_rva=" << HexValue(node.function.endAddress) << "\r\n"
                << "function_end_va=" << HexValue(snapshot.atonpTrackerBase + node.function.endAddress) << "\r\n"
                << "function_size=" << (node.function.endAddress - node.function.beginAddress) << "\r\n"
                << "disp_0x1d0_count=" << node.displacement1d0Count << "\r\n"
                << "disp_0x48_count=" << node.displacement48Count << "\r\n"
                << "vtable_matches=";
            if (node.vtableMatches.empty()) out << "none";
            for (std::size_t match = 0; match < node.vtableMatches.size(); ++match)
            {
                if (match) out << ',';
                out << node.vtableMatches[match];
            }
            out << "\r\nedge_indexes=";
            for (std::size_t edgeIndex = 0; edgeIndex < node.edgeIndexes.size(); ++edgeIndex)
            {
                if (edgeIndex) out << ',';
                out << node.edgeIndexes[edgeIndex];
            }
            out << "\r\nfunction_head_bytes_256=" << node.functionHeadBytes << "\r\n"
                << "function_tail_bytes_128=" << node.functionTailBytes << "\r\n";
        }

        out << "\r\n[INTERPRETATION RULES]\r\n"
            << "- A raw E8/E9/FF15/FF25 match is only a possible xref because V147 does not use a full disassembler.\r\n"
            << "- A CATPTTabView vtable match means the caller function shares a .pdata boundary with a vtable slot.\r\n"
            << "- disp_0x1d0_count is a byte-pattern hint for CATPTTabView+0x1d0, not proof of object usage.\r\n"
            << "- A candidate that reaches both roots, matches a vtable slot, and contains 0x1d0 is the strongest static lead.\r\n"
            << "- No address reported here is authorized for live execution.\r\n";
        return WriteUtf8File(path, out.str());
    }

    LRESULT CALLBACK TrackerDispatchWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == kUiExtractorDispatchMessage)
        {
            AppendExecutionTrace("ui_callback_entered");
            AcquireSRWLockExclusive(&g_uiDispatchLock);
            if (g_uiDispatchActive)
            {
                g_uiDispatchDiagnostics.uiCallbackEntered = true;
                g_uiDispatchDiagnostics.uiCallbackThreadId = GetCurrentThreadId();
                if (g_uiDispatchNoop)
                {
                    AppendExecutionTrace("ui_noop_test_entered");
                    g_uiDispatchAccounts.diagnostic += "; V147 no-op UI dispatch completed; extractor not called";
                    g_uiDispatchPositions.diagnostic += "; V147 no-op UI dispatch completed; extractor not called";
                    AppendExecutionTrace("ui_noop_test_completed");
                }
                else
                {
                    // V147 deliberately blocks all live internal extractor calls.
                    g_uiDispatchAccounts.diagnostic += "; V147 safety block: live extractor call disabled";
                    g_uiDispatchPositions.diagnostic += "; V147 safety block: live extractor call disabled";
                }
                g_uiDispatchDiagnostics.uiCallbackCompleted = true;
                g_uiDispatchActive = false;
            }
            ReleaseSRWLockExclusive(&g_uiDispatchLock);
            if (g_uiDispatchEvent) SetEvent(g_uiDispatchEvent);
            AppendExecutionTrace("ui_callback_exit");
            return 0;
        }

        WNDPROC original = g_uiDispatchOriginalWndProc;
        return original ? CallWindowProcW(original, hwnd, message, wParam, lParam)
                        : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool EnsureTrackerDispatchSubclass(HWND trackerWindow, std::string& diagnostic)
    {
        diagnostic.clear();
        if (!trackerWindow || !IsWindow(trackerWindow))
        {
            diagnostic = "tracker window is not valid";
            return false;
        }

        if (g_uiDispatchWindow == trackerWindow &&
            reinterpret_cast<WNDPROC>(GetWindowLongPtrW(trackerWindow, GWLP_WNDPROC)) == TrackerDispatchWndProc)
            return true;

        if (g_uiDispatchWindow && IsWindow(g_uiDispatchWindow) && g_uiDispatchOriginalWndProc)
        {
            if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_uiDispatchWindow, GWLP_WNDPROC)) == TrackerDispatchWndProc)
                SetWindowLongPtrW(g_uiDispatchWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_uiDispatchOriginalWndProc));
        }
        g_uiDispatchWindow = nullptr;
        g_uiDispatchOriginalWndProc = nullptr;

        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previous = SetWindowLongPtrW(
            trackerWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(TrackerDispatchWndProc));
        if (previous == 0 && GetLastError() != ERROR_SUCCESS)
        {
            std::ostringstream out;
            out << "SetWindowLongPtrW failed error=" << GetLastError();
            diagnostic = out.str();
            return false;
        }
        g_uiDispatchWindow = trackerWindow;
        g_uiDispatchOriginalWndProc = reinterpret_cast<WNDPROC>(previous);
        return true;
    }

    bool DispatchBothExtractorsOnTrackerThread(
        const Snapshot& snapshot,
        ExtractorAttempt& accounts,
        ExtractorAttempt& positions,
        UiDispatchDiagnostics& diagnostics)
    {
        diagnostics = UiDispatchDiagnostics{};
        diagnostics.requestReceivedThreadId = GetCurrentThreadId();
        diagnostics.dispatchTargetThreadId = snapshot.trackerThreadId;

        std::string subclassDiagnostic;
        if (!EnsureTrackerDispatchSubclass(snapshot.trackerWindow, subclassDiagnostic))
        {
            diagnostics.diagnostic = subclassDiagnostic;
            return false;
        }

        if (!g_uiDispatchEvent)
            g_uiDispatchEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_uiDispatchEvent)
        {
            std::ostringstream out;
            out << "CreateEventW failed error=" << GetLastError();
            diagnostics.diagnostic = out.str();
            return false;
        }

        AcquireSRWLockExclusive(&g_uiDispatchLock);
        if (g_uiDispatchActive)
        {
            ReleaseSRWLockExclusive(&g_uiDispatchLock);
            diagnostics.diagnostic = "previous UI extractor dispatch is still active";
            return false;
        }
        ResetEvent(g_uiDispatchEvent);
        g_uiDispatchAccounts = accounts;
        g_uiDispatchPositions = positions;
        g_uiDispatchNoop = true;
        g_uiDispatchDiagnostics = diagnostics;
        g_uiDispatchActive = true;
        ReleaseSRWLockExclusive(&g_uiDispatchLock);

        const ULONGLONG started = GetTickCount64();
        AppendExecutionTrace("dispatch_before_post");
        const BOOL posted = PostMessageW(snapshot.trackerWindow, kUiExtractorDispatchMessage, 0, 0);
        AppendExecutionTrace(posted ? "dispatch_posted" : "dispatch_post_failed");
        AcquireSRWLockExclusive(&g_uiDispatchLock);
        g_uiDispatchDiagnostics.dispatchPosted = posted != FALSE;
        if (!posted)
        {
            std::ostringstream out;
            out << "PostMessageW failed error=" << GetLastError();
            g_uiDispatchDiagnostics.diagnostic = out.str();
            g_uiDispatchActive = false;
        }
        ReleaseSRWLockExclusive(&g_uiDispatchLock);
        if (!posted)
        {
            AcquireSRWLockShared(&g_uiDispatchLock);
            diagnostics = g_uiDispatchDiagnostics;
            ReleaseSRWLockShared(&g_uiDispatchLock);
            return false;
        }

        AppendExecutionTrace("dispatch_wait_begin");
        const DWORD waitResult = WaitForSingleObject(g_uiDispatchEvent, kUiExtractorDispatchTimeoutMs);
        const ULONGLONG elapsed = GetTickCount64() - started;
        {
            std::ostringstream trace;
            trace << "result=" << waitResult << " elapsed_ms=" << elapsed;
            AppendExecutionTrace("dispatch_wait_end", trace.str());
        }
        AcquireSRWLockExclusive(&g_uiDispatchLock);
        g_uiDispatchDiagnostics.waitResult = waitResult;
        g_uiDispatchDiagnostics.waitElapsedMs = elapsed;
        if (waitResult == WAIT_OBJECT_0 && g_uiDispatchDiagnostics.uiCallbackCompleted)
        {
            accounts = g_uiDispatchAccounts;
            positions = g_uiDispatchPositions;
        }
        else if (waitResult == WAIT_TIMEOUT)
        {
            g_uiDispatchDiagnostics.diagnostic = "tracker UI dispatch timed out";
        }
        else
        {
            std::ostringstream out;
            out << "tracker UI dispatch wait failed result=" << waitResult
                << " error=" << GetLastError();
            g_uiDispatchDiagnostics.diagnostic = out.str();
        }
        diagnostics = g_uiDispatchDiagnostics;
        const bool completed = waitResult == WAIT_OBJECT_0 && diagnostics.uiCallbackCompleted;
        ReleaseSRWLockExclusive(&g_uiDispatchLock);
        return completed;
    }

    std::string UiDispatchDiagnosticsJson(const UiDispatchDiagnostics& value)
    {
        std::ostringstream out;
        out << '{'
            << "\"request_received_thread_id\":" << value.requestReceivedThreadId << ','
            << "\"dispatch_target_thread_id\":" << value.dispatchTargetThreadId << ','
            << "\"dispatch_posted\":" << (value.dispatchPosted ? "true" : "false") << ','
            << "\"ui_callback_entered\":" << (value.uiCallbackEntered ? "true" : "false") << ','
            << "\"ui_callback_thread_id\":" << value.uiCallbackThreadId << ','
            << "\"ui_callback_completed\":" << (value.uiCallbackCompleted ? "true" : "false") << ','
            << "\"wait_result\":" << value.waitResult << ','
            << "\"wait_elapsed_ms\":" << value.waitElapsedMs << ','
            << "\"pipe_response_written\":" << (value.pipeResponseWritten ? "true" : "false") << ','
            << "\"diagnostic\":\"" << JsonEscape(value.diagnostic) << "\"}";
        return out.str();
    }

    std::string ExtractorAttemptJson(const ExtractorAttempt& attempt)
    {
        std::ostringstream out;
        out << '{'
            << "\"name\":\"" << JsonEscape(attempt.name) << "\"," 
            << "\"function\":\"" << HexValue(attempt.function) << "\"," 
            << "\"tab_view\":\"" << HexValue(attempt.tabView) << "\"," 
            << "\"trade_info\":\"" << HexValue(attempt.tradeInfo) << "\"," 
            << "\"function_valid\":" << (attempt.functionValid ? "true" : "false") << ','
            << "\"function_valid_meaning\":\"executable_address_only\","
            << "\"tab_view_valid\":" << (attempt.tabViewValid ? "true" : "false") << ','
            << "\"trade_info_valid\":" << (attempt.tradeInfoValid ? "true" : "false") << ','
            << "\"gate_enabled\":" << (attempt.gateEnabled ? "true" : "false") << ','
            << "\"live_attempted\":" << (attempt.liveAttempted ? "true" : "false") << ','
            << "\"live_succeeded\":" << (attempt.liveSucceeded ? "true" : "false") << ','
            << "\"seh_code\":" << attempt.sehCode << ','
            << "\"returned_interface\":\"" << HexValue(attempt.returnedInterface) << "\"," 
            << "\"returned_vtable\":\"" << HexValue(attempt.returnedVtable) << "\"," 
            << "\"query_interface_hr\":" << static_cast<long>(attempt.queryInterfaceHr) << ','
            << "\"diagnostic\":\"" << JsonEscape(attempt.diagnostic) << "\"}";
        return out.str();
    }

    void StoreExtractorAttempts(const ExtractorAttempt& accounts, const ExtractorAttempt& openPositions)
    {
        AcquireSRWLockExclusive(&g_extractorLock);
        g_accountsAttempt = accounts;
        g_openPositionsAttempt = openPositions;
        ReleaseSRWLockExclusive(&g_extractorLock);
    }

    std::string CurrentExtractorStatusJson()
    {
        AcquireSRWLockShared(&g_extractorLock);
        const ExtractorAttempt accounts = g_accountsAttempt;
        const ExtractorAttempt openPositions = g_openPositionsAttempt;
        ReleaseSRWLockShared(&g_extractorLock);
        AcquireSRWLockShared(&g_uiDispatchLock);
        const UiDispatchDiagnostics dispatch = g_uiDispatchDiagnostics;
        ReleaseSRWLockShared(&g_uiDispatchLock);
        std::ostringstream out;
        out << "{\"bridge_version\":" << kBridgeVersion
            << ",\"thread_mode\":\"safe_recursive_caller_xref_analysis_v147\""
            << ",\"accounts\":" << ExtractorAttemptJson(accounts)
            << ",\"open_positions\":" << ExtractorAttemptJson(openPositions)
            << ",\"ui_dispatch\":" << UiDispatchDiagnosticsJson(dispatch) << '}';
        return out.str();
    }

    bool WriteExtractorReport(const Snapshot& snapshot, const ExtractorAttempt& accounts,
        const ExtractorAttempt& openPositions, std::wstring& path)
    {
        path = ReportPath(L"MC_V147_Bridge_Dual_Extractor_Probe", snapshot.processId);
        std::ostringstream out;
        out << "MC V147 Dual Extractor Probe\r\n"
            << "============================\r\n"
            << "WARNING: experimental internal calls can crash MultiCharts.\r\n"
            << "process_id=" << snapshot.processId << "\r\n"
            << "tracker_thread_id=" << snapshot.trackerThreadId << "\r\n"
            << "worker_thread_id=" << snapshot.workerThreadId << "\r\n"
            << "same_thread=" << (snapshot.trackerThreadId == snapshot.workerThreadId ? "yes" : "no") << "\r\n"
            << "accounts=" << ExtractorAttemptJson(accounts) << "\r\n"
            << "open_positions=" << ExtractorAttemptJson(openPositions) << "\r\n";
        return WriteUtf8File(path, out.str());
    }

    bool ReadExact(HANDLE handle, void* buffer, DWORD bytes)
    {
        auto* cursor = static_cast<unsigned char*>(buffer);
        DWORD remaining = bytes;
        while (remaining > 0)
        {
            DWORD read = 0;
            if (!ReadFile(handle, cursor, remaining, &read, nullptr) || read == 0)
                return false;
            cursor += read;
            remaining -= read;
        }
        return true;
    }

    bool WriteExact(HANDLE handle, const void* buffer, DWORD bytes)
    {
        const auto* cursor = static_cast<const unsigned char*>(buffer);
        DWORD remaining = bytes;
        while (remaining > 0)
        {
            DWORD written = 0;
            if (!WriteFile(handle, cursor, remaining, &written, nullptr) || written == 0)
                return false;
            cursor += written;
            remaining -= written;
        }
        return true;
    }

    bool SendResponse(
        HANDLE pipe,
        const mcbridge::MessageHeader& request,
        mcbridge::Status status,
        const std::string& payload)
    {
        mcbridge::MessageHeader response;
        response.command = request.command;
        response.requestId = request.requestId;
        response.status = static_cast<std::uint32_t>(status);
        response.payloadBytes = static_cast<std::uint32_t>(payload.size());
        if (!WriteExact(pipe, &response, sizeof(response)))
            return false;
        return payload.empty() || WriteExact(pipe, payload.data(), response.payloadBytes);
    }


    struct ThreeTabTypeSummary
    {
        std::string label;
        std::string typeName;
        std::string diagnostic;
        std::vector<RttiVtableRecord> records;
    };

    bool WriteThreeTabAnchorReport(
        const Snapshot& snapshot,
        std::wstring& path,
        std::string& summaryJson)
    {
        const std::pair<const char*, const char*> types[] = {
            { "accounts", ".?AVCAccountsPage@ATOnPTracker@@" },
            { "open_positions", ".?AVCOpenPositionsPage@ATOnPTracker@@" },
            { "logs", ".?AVCLogsPage@ATOnPTracker@@" }
        };

        std::vector<ThreeTabTypeSummary> summaries;
        for (const auto& item : types)
        {
            ThreeTabTypeSummary summary;
            summary.label = item.first;
            summary.typeName = item.second;
            summary.records = ResolveRttiVtables(snapshot, item.second, summary.diagnostic);
            summaries.push_back(std::move(summary));
        }

        path = ReportPath(L"MC_V147_Three_Tab_Anchor_Comparison", snapshot.processId);
        EnsureOutputDirectory();
        std::ostringstream out;
        out << "MC V147 Accounts / Open Positions / Logs Anchor Comparison\r\n"
            << "===========================================================\r\n"
            << "STATIC ANALYSIS ONLY. NO INTERNAL METHOD OR EXTRACTOR IS CALLED.\r\n"
            << "process_id=" << snapshot.processId << "\r\n"
            << "tracker_hwnd=" << HexValue(reinterpret_cast<std::uintptr_t>(snapshot.trackerWindow)) << "\r\n"
            << "atonptracker_base=" << HexValue(snapshot.atonpTrackerBase) << "\r\n\r\n";

        out << "PAGE RTTI ANCHORS\r\n-----------------\r\n";
        for (const auto& summary : summaries)
        {
            out << "tab=" << summary.label << "\r\n"
                << "type=" << summary.typeName << "\r\n"
                << "diagnostic=" << summary.diagnostic << "\r\n"
                << "vtable_count=" << summary.records.size() << "\r\n";
            for (std::size_t i = 0; i < summary.records.size(); ++i)
            {
                const auto& record = summary.records[i];
                std::uintptr_t firstMethod = 0;
                SafeReadValue(reinterpret_cast<void*>(record.vtable), firstMethod);
                out << "  record=" << (i + 1)
                    << " type_descriptor=" << HexValue(record.typeDescriptor)
                    << " col=" << HexValue(record.completeObjectLocator)
                    << " vtable=" << HexValue(record.vtable)
                    << " vtable_rva=" << HexValue(record.vtable - snapshot.atonpTrackerBase)
                    << " first_method=" << HexValue(firstMethod);
                if (firstMethod >= snapshot.atonpTrackerBase &&
                    firstMethod < snapshot.atonpTrackerBase + snapshot.atonpTrackerSize)
                {
                    out << " first_method_rva=" << HexValue(firstMethod - snapshot.atonpTrackerBase);
                }
                out << "\r\n";
            }
            out << "\r\n";
        }

        out << "TRACKER WINDOW TEXT ANCHORS\r\n---------------------------\r\n";
        std::size_t textMatches = 0;
        for (const WindowRecord& window : snapshot.windows)
        {
            std::wstring lower = window.text;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            if (lower.find(L"account") == std::wstring::npos &&
                lower.find(L"open position") == std::wstring::npos &&
                lower.find(L"logs") == std::wstring::npos &&
                lower.find(L"log") == std::wstring::npos)
                continue;
            ++textMatches;
            out << "hwnd=" << HexValue(reinterpret_cast<std::uintptr_t>(window.hwnd))
                << " parent=" << HexValue(reinterpret_cast<std::uintptr_t>(window.parent))
                << " class=" << WideToUtf8(window.className)
                << " text=" << WideToUtf8(window.text)
                << " visible=" << (window.visible ? "yes" : "no")
                << " thread_id=" << window.threadId << "\r\n";
        }
        if (!textMatches) out << "none\r\n";

        out << "\r\nINTERPRETATION RULES\r\n--------------------\r\n"
            << "- A page RTTI/vtable anchor is an identity clue, not a callable extractor.\r\n"
            << "- Logs is included as a comparison branch because its UI and text path is easier to recognize.\r\n"
            << "- Shared vtable methods suggest generic page infrastructure; distinct methods suggest tab-specific logic.\r\n"
            << "- No address in this report is approved for live execution.\r\n";

        const bool written = WriteUtf8File(path, out.str());
        std::ostringstream json;
        json << "{\"analysis\":\"accounts_open_positions_logs\",\"static_only\":true,"
             << "\"report_written\":" << (written ? "true" : "false")
             << ",\"report_path\":" << JsonString(path)
             << ",\"window_text_matches\":" << textMatches << ",\"tabs\":[";
        for (std::size_t i = 0; i < summaries.size(); ++i)
        {
            if (i) json << ',';
            json << "{\"name\":\"" << summaries[i].label << "\",\"rtti_vtable_count\":"
                 << summaries[i].records.size() << "}";
        }
        json << "]}";
        summaryJson = json.str();
        return written;
    }


    struct PageMethodMetric
    {
        std::string tab;
        std::size_t recordIndex = 0;
        std::size_t slotIndex = 0;
        std::uintptr_t vtableVa = 0;
        std::uintptr_t methodVa = 0;
        bool boundaryFound = false;
        RuntimeFunctionRaw boundary{};
        std::uint32_t methodRva = 0;
        std::uint32_t functionSize = 0;
        std::size_t callOpcodeCount = 0;
        std::size_t indirectCallOpcodeCount = 0;
        std::size_t displacement1d0Count = 0;
        std::size_t displacement48Count = 0;
        std::size_t displacementKnownGridCount = 0;
        int score = 0;
        std::string headBytes;
    };

    std::size_t CountBytePattern(const std::vector<unsigned char>& bytes, const std::vector<unsigned char>& pattern)
    {
        if (pattern.empty() || bytes.size() < pattern.size()) return 0;
        std::size_t count = 0;
        for (std::size_t i = 0; i + pattern.size() <= bytes.size(); ++i)
        {
            if (std::equal(pattern.begin(), pattern.end(), bytes.begin() + i)) ++count;
        }
        return count;
    }

    bool ReadFunctionBytesForMetric(
        const Snapshot& snapshot,
        const RuntimeFunctionRaw& boundary,
        std::vector<unsigned char>& bytes)
    {
        bytes.clear();
        if (boundary.endAddress <= boundary.beginAddress) return false;
        const std::size_t size = static_cast<std::size_t>(boundary.endAddress - boundary.beginAddress);
        const std::size_t capped = (std::min)(size, static_cast<std::size_t>(4096));
        bytes.resize(capped);
        const std::uintptr_t address = snapshot.atonpTrackerBase + boundary.beginAddress;
        DWORD sehCode = 0;
        if (!MCBridge_SafeCopyMemory(
                bytes.data(),
                reinterpret_cast<const void*>(address),
                bytes.size(),
                &sehCode))
        {
            bytes.clear();
            return false;
        }
        return true;
    }

    bool WritePageVirtualMethodExplorerReport(
        const Snapshot& snapshot,
        std::wstring& path,
        std::string& summaryJson)
    {
        const std::pair<const char*, const char*> types[] = {
            { "accounts", ".?AVCAccountsPage@ATOnPTracker@@" },
            { "open_positions", ".?AVCOpenPositionsPage@ATOnPTracker@@" },
            { "logs", ".?AVCLogsPage@ATOnPTracker@@" }
        };

        RuntimeFunctionTableAnalysis runtimeTable;
        (void)LoadRuntimeFunctionTable(snapshot, runtimeTable);
        std::vector<PageMethodMetric> metrics;
        std::map<std::uintptr_t, std::set<std::string>> methodTabs;
        std::ostringstream out;
        path = ReportPath(L"MC_V147_Page_Virtual_Method_Explorer", snapshot.processId);
        EnsureOutputDirectory();
        out << "MC V147 Page Virtual Method Explorer\r\n"
            << "====================================\r\n"
            << "STATIC ANALYSIS ONLY. NO VTABLE METHOD OR EXTRACTOR IS CALLED.\r\n"
            << "process_id=" << snapshot.processId << "\r\n"
            << "atonptracker_base=" << HexValue(snapshot.atonpTrackerBase) << "\r\n"
            << "runtime_table=" << runtimeTable.diagnostic << "\r\n\r\n";

        for (const auto& type : types)
        {
            std::string diagnostic;
            std::vector<RttiVtableRecord> records = ResolveRttiVtables(snapshot, type.second, diagnostic);
            std::sort(records.begin(), records.end(), [](const RttiVtableRecord& a, const RttiVtableRecord& b) {
                return a.vtable < b.vtable;
            });
            out << "TAB=" << type.first << "\r\n"
                << "type=" << type.second << "\r\n"
                << "diagnostic=" << diagnostic << "\r\n"
                << "vtable_count=" << records.size() << "\r\n";

            for (std::size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex)
            {
                const std::uintptr_t exactEnd = recordIndex + 1 < records.size()
                    ? records[recordIndex + 1].vtable : 0;
                VtableAnalysis table = AnalyzeVtableSlots(
                    snapshot, runtimeTable, type.first, 0, records[recordIndex].vtable, exactEnd);
                out << "  VTABLE record=" << (recordIndex + 1)
                    << " address=" << HexValue(records[recordIndex].vtable)
                    << " rva=" << HexValue(records[recordIndex].vtable - snapshot.atonpTrackerBase)
                    << " slots=" << table.slots.size()
                    << " stop=" << table.stopReason << "\r\n";

                for (const VtableSlotAnalysis& slot : table.slots)
                {
                    PageMethodMetric metric;
                    metric.tab = type.first;
                    metric.recordIndex = recordIndex;
                    metric.slotIndex = slot.slotIndex;
                    metric.vtableVa = records[recordIndex].vtable;
                    metric.methodVa = slot.methodVa;
                    metric.methodRva = slot.inAtonpTracker
                        ? static_cast<std::uint32_t>(slot.methodVa - snapshot.atonpTrackerBase) : 0;
                    metric.boundaryFound = slot.boundaryFound;
                    metric.boundary = slot.boundary;
                    if (slot.boundaryFound)
                    {
                        metric.functionSize = slot.boundary.endAddress - slot.boundary.beginAddress;
                        std::vector<unsigned char> bytes;
                        if (ReadFunctionBytesForMetric(snapshot, slot.boundary, bytes))
                        {
                            metric.callOpcodeCount = static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), 0xE8));
                            metric.indirectCallOpcodeCount = CountBytePattern(bytes, {0xFF, 0x50}) +
                                CountBytePattern(bytes, {0xFF, 0x90}) + CountBytePattern(bytes, {0xFF, 0x15});
                            metric.displacement1d0Count = CountBytePattern(bytes, {0xD0, 0x01, 0x00, 0x00});
                            metric.displacement48Count = CountBytePattern(bytes, {0x48, 0x00, 0x00, 0x00});
                            metric.displacementKnownGridCount = CountBytePattern(bytes, {0x20, 0x03, 0x00, 0x00}) +
                                CountBytePattern(bytes, {0x28, 0x03, 0x00, 0x00});
                            metric.headBytes = HexBytesWithProtection(
                                snapshot.atonpTrackerBase + slot.boundary.beginAddress,
                                (std::min)(static_cast<std::size_t>(64), bytes.size()), true);
                        }
                    }
                    metric.score = static_cast<int>(metric.displacement1d0Count * 40 +
                        metric.indirectCallOpcodeCount * 8 + metric.callOpcodeCount * 2 +
                        metric.displacementKnownGridCount * 12);
                    if (metric.functionSize >= 80 && metric.functionSize <= 2000) metric.score += 5;
                    metrics.push_back(metric);
                    methodTabs[metric.methodVa].insert(metric.tab);
                    out << "    slot=" << slot.slotIndex
                        << " method=" << HexValue(slot.methodVa)
                        << " method_rva=" << HexValue(metric.methodRva)
                        << " boundary=" << (slot.boundaryFound ? "yes" : "no");
                    if (slot.boundaryFound)
                    {
                        out << " function_rva=" << HexValue(slot.boundary.beginAddress)
                            << "-" << HexValue(slot.boundary.endAddress)
                            << " size=" << metric.functionSize
                            << " calls_e8=" << metric.callOpcodeCount
                            << " indirect_call_patterns=" << metric.indirectCallOpcodeCount
                            << " disp_1d0=" << metric.displacement1d0Count
                            << " disp_48=" << metric.displacement48Count
                            << " grid_disp_hints=" << metric.displacementKnownGridCount
                            << " score=" << metric.score;
                    }
                    out << "\r\n";
                }
            }
            out << "\r\n";
        }

        for (PageMethodMetric& metric : metrics)
        {
            const auto found = methodTabs.find(metric.methodVa);
            if (found != methodTabs.end() && found->second.size() > 1) metric.score -= 25;
            else metric.score += 15;
        }
        std::sort(metrics.begin(), metrics.end(), [](const PageMethodMetric& a, const PageMethodMetric& b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.tab != b.tab) return a.tab < b.tab;
            if (a.recordIndex != b.recordIndex) return a.recordIndex < b.recordIndex;
            return a.slotIndex < b.slotIndex;
        });

        out << "SHARED METHODS ACROSS TABS\r\n--------------------------\r\n";
        std::size_t sharedCount = 0;
        for (const auto& item : methodTabs)
        {
            if (item.second.size() < 2) continue;
            ++sharedCount;
            out << "method=" << HexValue(item.first) << " tabs=";
            bool first = true;
            for (const std::string& tab : item.second) { if (!first) out << ','; out << tab; first = false; }
            out << "\r\n";
        }
        if (!sharedCount) out << "none\r\n";

        out << "\r\nTOP TAB-SPECIFIC METHOD CANDIDATES\r\n----------------------------------\r\n";
        const std::size_t limit = (std::min)(metrics.size(), static_cast<std::size_t>(60));
        for (std::size_t i = 0; i < limit; ++i)
        {
            const PageMethodMetric& m = metrics[i];
            const bool shared = methodTabs[m.methodVa].size() > 1;
            out << "rank=" << (i + 1) << " tab=" << m.tab
                << " record=" << (m.recordIndex + 1) << " slot=" << m.slotIndex
                << " method_rva=" << HexValue(m.methodRva)
                << " score=" << m.score << " shared=" << (shared ? "yes" : "no")
                << " function_size=" << m.functionSize
                << " calls_e8=" << m.callOpcodeCount
                << " indirect_calls=" << m.indirectCallOpcodeCount
                << " disp_1d0=" << m.displacement1d0Count
                << " disp_48=" << m.displacement48Count
                << " grid_hints=" << m.displacementKnownGridCount << "\r\n"
                << "  head=" << m.headBytes << "\r\n";
        }
        out << "\r\nINTERPRETATION\r\n--------------\r\n"
            << "- Shared methods are likely common page infrastructure and receive a score penalty.\r\n"
            << "- Tab-specific methods, indirect-call patterns, +0x1D0 references and moderate function size increase interest.\r\n"
            << "- Opcode counts are conservative byte-pattern hints, not a complete x64 disassembly.\r\n"
            << "- No candidate in this report is approved for execution.\r\n";

        const bool written = WriteUtf8File(path, out.str());
        std::ostringstream json;
        json << "{\"analysis\":\"page_virtual_method_explorer\",\"static_only\":true,"
             << "\"method_entries\":" << metrics.size()
             << ",\"shared_method_count\":" << sharedCount
             << ",\"report_written\":" << (written ? "true" : "false")
             << ",\"report_path\":" << JsonString(path) << "}";
        summaryJson = json.str();
        return written;
    }


    struct Slot17Instruction
    {
        std::uint32_t rva = 0;
        std::size_t length = 1;
        std::string kind = "other";
        std::string text;
        bool directCall = false;
        bool indirectCall = false;
        bool ripIndirect = false;
        bool field1d0 = false;
        bool field48 = false;
        bool gridHint = false;
        std::uint32_t targetRva = 0;
        int virtualSlot = -1;
    };

    struct Slot17Target
    {
        const char* page = "";
        std::uint32_t methodRva = 0;
        RuntimeFunctionRaw boundary{};
        bool boundaryFound = false;
        bool decodeAborted = false;
        std::string decodeStopReason;
        std::vector<unsigned char> bytes;
        std::vector<Slot17Instruction> instructions;
    };

    struct Slot17ProgressContext
    {
        std::wstring progressPath;
        std::wstring diagnosticsPath;
        std::wstring errorsPath;
        std::wstring cancelPath;
        ULONGLONG analysisStarted = 0;
        ULONGLONG lastHeartbeat = 0;

        void Progress(const std::string& line)
        {
            AppendUtf8Line(progressPath, line);
        }
        void Diagnostic(const std::string& line)
        {
            AppendUtf8Line(diagnosticsPath, line);
        }
        void Error(const std::string& line)
        {
            AppendUtf8Line(errorsPath, line);
        }
        bool CancelRequested() const
        {
            return FileExists(cancelPath);
        }
    };

    bool ReadBoundaryBytes(
        const Snapshot& snapshot,
        const RuntimeFunctionRaw& boundary,
        std::vector<unsigned char>& bytes,
        std::size_t cap = 16384)
    {
        bytes.clear();
        if (boundary.endAddress <= boundary.beginAddress) return false;
        const std::size_t size = static_cast<std::size_t>(boundary.endAddress - boundary.beginAddress);
        bytes.resize((std::min)(size, cap));
        DWORD sehCode = 0;
        if (!MCBridge_SafeCopyMemory(
                bytes.data(),
                reinterpret_cast<const void*>(snapshot.atonpTrackerBase + boundary.beginAddress),
                bytes.size(),
                &sehCode))
        {
            bytes.clear();
            return false;
        }
        return true;
    }

    std::size_t ConservativeX64Length(
        const unsigned char* p,
        std::size_t remaining,
        bool& hasModrm,
        unsigned char& modrm,
        std::size_t& dispOffset,
        std::size_t& dispSize,
        std::size_t& immOffset,
        std::size_t& immSize)
    {
        hasModrm = false;
        modrm = 0;
        dispOffset = dispSize = immOffset = immSize = 0;
        if (!p || remaining == 0) return 0;

        std::size_t i = 0;
        bool rexW = false;
        while (i < remaining)
        {
            const unsigned char b = p[i];
            if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 ||
                b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
                b == 0x64 || b == 0x65)
            {
                ++i;
                continue;
            }
            if ((b & 0xF0) == 0x40)
            {
                rexW = (b & 0x08) != 0;
                ++i;
                continue;
            }
            break;
        }
        if (i >= remaining) return 1;

        const unsigned char op = p[i++];
        bool twoByte = false;
        unsigned char op2 = 0;
        if (op == 0x0F)
        {
            if (i >= remaining) return i;
            twoByte = true;
            op2 = p[i++];
        }

        auto needModrm = [&](unsigned char one, bool isTwo, unsigned char two) -> bool
        {
            if (isTwo)
            {
                if ((two >= 0x10 && two <= 0x1F) ||
                    (two >= 0x40 && two <= 0x4F) ||
                    (two >= 0x90 && two <= 0x9F) ||
                    two == 0xAF || two == 0xB6 || two == 0xB7 ||
                    two == 0xBE || two == 0xBF) return true;
                return false;
            }
            if ((one >= 0x00 && one <= 0x03) ||
                (one >= 0x08 && one <= 0x0B) ||
                (one >= 0x20 && one <= 0x23) ||
                (one >= 0x28 && one <= 0x2B) ||
                (one >= 0x30 && one <= 0x33) ||
                (one >= 0x38 && one <= 0x3B) ||
                one == 0x63 || one == 0x69 || one == 0x6B ||
                (one >= 0x80 && one <= 0x8F) ||
                one == 0xC0 || one == 0xC1 || one == 0xC6 || one == 0xC7 ||
                one == 0xD0 || one == 0xD1 || one == 0xD2 || one == 0xD3 ||
                one == 0xF6 || one == 0xF7 || one == 0xFE || one == 0xFF)
                return true;
            return false;
        };

        if (!twoByte)
        {
            if (op == 0xE8 || op == 0xE9)
            {
                immOffset = i; immSize = 4; return (std::min)(remaining, i + 4);
            }
            if (op == 0xEB || (op >= 0x70 && op <= 0x7F))
            {
                immOffset = i; immSize = 1; return (std::min)(remaining, i + 1);
            }
            if (op >= 0xB8 && op <= 0xBF)
            {
                immOffset = i; immSize = rexW ? 8 : 4;
                return (std::min)(remaining, i + immSize);
            }
            if (op == 0x68) { immOffset = i; immSize = 4; return (std::min)(remaining, i + 4); }
            if (op == 0x6A) { immOffset = i; immSize = 1; return (std::min)(remaining, i + 1); }
            if (op == 0xC2) { immOffset = i; immSize = 2; return (std::min)(remaining, i + 2); }
            if (op == 0xA1 || op == 0xA3)
            {
                immOffset = i; immSize = 8; return (std::min)(remaining, i + 8);
            }
        }
        else if (op2 >= 0x80 && op2 <= 0x8F)
        {
            immOffset = i; immSize = 4; return (std::min)(remaining, i + 4);
        }

        hasModrm = needModrm(op, twoByte, op2);
        if (!hasModrm) return (std::max)(static_cast<std::size_t>(1), i);
        if (i >= remaining) return i;

        modrm = p[i++];
        const unsigned char mod = (modrm >> 6) & 3;
        const unsigned char rm = modrm & 7;
        if (mod != 3 && rm == 4)
        {
            if (i >= remaining) return i;
            const unsigned char sib = p[i++];
            const unsigned char base = sib & 7;
            if (mod == 0 && base == 5) { dispOffset = i; dispSize = 4; i += (std::min)(remaining - i, static_cast<std::size_t>(4)); }
        }
        if (mod == 0 && rm == 5) { dispOffset = i; dispSize = 4; i += (std::min)(remaining - i, static_cast<std::size_t>(4)); }
        else if (mod == 1) { dispOffset = i; dispSize = 1; i += (std::min)(remaining - i, static_cast<std::size_t>(1)); }
        else if (mod == 2) { dispOffset = i; dispSize = 4; i += (std::min)(remaining - i, static_cast<std::size_t>(4)); }

        if (!twoByte)
        {
            if (op == 0x69 || op == 0x81 || op == 0xC7) { immOffset = i; immSize = 4; i += (std::min)(remaining - i, static_cast<std::size_t>(4)); }
            else if (op == 0x6B || op == 0x80 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6)
            { immOffset = i; immSize = 1; i += (std::min)(remaining - i, static_cast<std::size_t>(1)); }
        }
        return (std::max)(static_cast<std::size_t>(1), i);
    }

    std::vector<Slot17Instruction> DecodeSlot17Instructions(
        Slot17Target& target,
        Slot17ProgressContext& progress)
    {
        constexpr std::size_t kMaxInstructionsPerMethod = 20000;
        constexpr ULONGLONG kMaxMillisecondsPerMethod = 120000;
        constexpr ULONGLONG kHeartbeatMilliseconds = 5000;
        constexpr std::size_t kProgressInstructionInterval = 256;

        std::vector<Slot17Instruction> result;
        std::size_t offset = 0;
        const ULONGLONG started = GetTickCount64();
        ULONGLONG lastHeartbeat = started;
        std::size_t repeatedOffsetCount = 0;
        std::size_t previousOffset = static_cast<std::size_t>(-1);

        while (offset < target.bytes.size())
        {
            const ULONGLONG now = GetTickCount64();
            if (progress.CancelRequested())
            {
                target.decodeAborted = true;
                target.decodeStopReason = "cancelled_by_user";
                progress.Error(std::string("page=") + target.page + " status=cancelled_by_user");
                break;
            }
            if (result.size() >= kMaxInstructionsPerMethod)
            {
                target.decodeAborted = true;
                target.decodeStopReason = "instruction_limit";
                progress.Error(std::string("page=") + target.page + " status=aborted_safely reason=instruction_limit");
                break;
            }
            if (now - started >= kMaxMillisecondsPerMethod)
            {
                target.decodeAborted = true;
                target.decodeStopReason = "method_timeout";
                progress.Error(std::string("page=") + target.page + " status=aborted_safely reason=method_timeout");
                break;
            }
            if (offset == previousOffset)
            {
                ++repeatedOffsetCount;
                if (repeatedOffsetCount > 1)
                {
                    target.decodeAborted = true;
                    target.decodeStopReason = "decoder_did_not_advance";
                    progress.Error(std::string("page=") + target.page + " status=aborted_safely reason=decoder_did_not_advance");
                    break;
                }
            }
            else
            {
                repeatedOffsetCount = 0;
            }
            previousOffset = offset;

            const unsigned char* p = target.bytes.data() + offset;
            const std::size_t remaining = target.bytes.size() - offset;
            bool hasModrm = false;
            unsigned char modrm = 0;
            std::size_t dispOffset = 0, dispSize = 0, immOffset = 0, immSize = 0;
            std::size_t length = ConservativeX64Length(
                p, remaining, hasModrm, modrm, dispOffset, dispSize, immOffset, immSize);
            if (length == 0 || length > remaining)
            {
                std::ostringstream warning;
                warning << "page=" << target.page << " decode_warning=invalid_length"
                        << " offset=" << offset << " length=" << length
                        << " remaining=" << remaining << " fallback_length=1";
                progress.Diagnostic(warning.str());
                length = 1;
            }

            Slot17Instruction ins;
            ins.rva = target.boundary.beginAddress + static_cast<std::uint32_t>(offset);
            ins.length = length;

            std::size_t opcodeIndex = 0;
            while (opcodeIndex < length)
            {
                const unsigned char b = p[opcodeIndex];
                if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 ||
                    b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
                    b == 0x64 || b == 0x65 || (b & 0xF0) == 0x40)
                    ++opcodeIndex;
                else break;
            }

            if (opcodeIndex < length && p[opcodeIndex] == 0xE8 && opcodeIndex + 5 <= length)
            {
                std::int32_t rel = 0;
                std::memcpy(&rel, p + opcodeIndex + 1, sizeof(rel));
                ins.directCall = true;
                ins.kind = "direct_call";
                ins.targetRva = static_cast<std::uint32_t>(
                    static_cast<std::int64_t>(ins.rva) +
                    static_cast<std::int64_t>(length) + rel);
            }
            else if (opcodeIndex < length && p[opcodeIndex] == 0xFF && hasModrm)
            {
                const unsigned char reg = (modrm >> 3) & 7;
                if (reg == 2)
                {
                    ins.indirectCall = true;
                    ins.kind = "indirect_call";
                    const unsigned char mod = (modrm >> 6) & 3;
                    const unsigned char rm = modrm & 7;
                    if (mod == 0 && rm == 5) ins.ripIndirect = true;
                    if (dispSize == 1 && dispOffset < length)
                    {
                        const int disp = static_cast<signed char>(p[dispOffset]);
                        if (disp >= 0 && (disp % 8) == 0) ins.virtualSlot = disp / 8;
                    }
                    else if (dispSize == 4 && dispOffset + 4 <= length)
                    {
                        std::int32_t disp = 0;
                        std::memcpy(&disp, p + dispOffset, sizeof(disp));
                        if (disp >= 0 && (disp % 8) == 0 && disp <= 0x800)
                            ins.virtualSlot = disp / 8;
                    }
                }
            }

            if (dispSize == 4 && dispOffset + 4 <= length)
            {
                std::uint32_t disp = 0;
                std::memcpy(&disp, p + dispOffset, sizeof(disp));
                ins.field1d0 = disp == 0x1D0;
                ins.field48 = disp == 0x48;
                ins.gridHint = disp == 0x320 || disp == 0x328;
            }
            else if (dispSize == 1 && dispOffset < length)
            {
                const unsigned char disp = p[dispOffset];
                ins.field48 = disp == 0x48;
            }

            std::ostringstream text;
            text << HexValue(ins.rva) << " len=" << length << " bytes=";
            for (std::size_t j = 0; j < length; ++j)
            {
                if (j) text << ' ';
                text << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                     << static_cast<unsigned int>(p[j]);
            }
            if (ins.directCall) text << " CALL target_rva=" << HexValue(ins.targetRva);
            if (ins.indirectCall)
            {
                text << " INDIRECT_CALL";
                if (ins.ripIndirect) text << " rip_relative=yes";
                if (ins.virtualSlot >= 0) text << " possible_vslot=" << ins.virtualSlot;
            }
            if (ins.field1d0) text << " field_disp=0x1D0";
            if (ins.field48) text << " field_disp=0x48";
            if (ins.gridHint) text << " grid_disp_hint=yes";
            ins.text = text.str();
            result.push_back(ins);
            const std::size_t nextOffset = offset + length;
            if (nextOffset <= offset)
            {
                target.decodeAborted = true;
                target.decodeStopReason = "offset_overflow_or_no_progress";
                progress.Error(std::string("page=") + target.page + " status=aborted_safely reason=offset_overflow_or_no_progress");
                break;
            }
            offset = nextOffset;

            if ((result.size() % kProgressInstructionInterval) == 0 || now - lastHeartbeat >= kHeartbeatMilliseconds)
            {
                const unsigned int percent = target.bytes.empty() ? 100u :
                    static_cast<unsigned int>((offset * 100u) / target.bytes.size());
                std::ostringstream heartbeat;
                heartbeat << "heartbeat page=" << target.page
                          << " percent=" << percent
                          << " offset=" << offset << '/' << target.bytes.size()
                          << " instructions=" << result.size()
                          << " elapsed_ms=" << (now - started);
                progress.Progress(heartbeat.str());
                progress.Diagnostic(heartbeat.str());
                lastHeartbeat = now;
            }
        }

        std::ostringstream completed;
        completed << "decode_finished page=" << target.page
                  << " decoded_bytes=" << offset << '/' << target.bytes.size()
                  << " instructions=" << result.size()
                  << " elapsed_ms=" << (GetTickCount64() - started)
                  << " status=" << (target.decodeAborted ? "aborted_safely" : "completed");
        if (target.decodeAborted) completed << " reason=" << target.decodeStopReason;
        progress.Progress(completed.str());
        progress.Diagnostic(completed.str());
        return result;
    }

    bool WriteSlot17DeepAnalysisReport(
        const Snapshot& snapshot,
        std::wstring& reportPath,
        std::wstring& csvPath,
        std::string& summaryJson)
    {
        const struct { const char* page; std::uint32_t rva; } definitions[] = {
            { "accounts", 0x78A00u },
            { "open_positions", 0xADFD0u },
            { "logs", 0xCFAB0u }
        };

        EnsureOutputDirectory();
        Slot17ProgressContext progress;
        progress.progressPath = ReportPath(L"MC_V147_Progress", snapshot.processId);
        progress.diagnosticsPath = ReportPath(L"MC_V147_Diagnostics", snapshot.processId);
        progress.errorsPath = ReportPath(L"MC_V147_Errors", snapshot.processId);
        progress.cancelPath = std::wstring(kOutputDirectory) + L"\\MC_V147_CANCEL";
        progress.analysisStarted = GetTickCount64();
        progress.lastHeartbeat = progress.analysisStarted;
        (void)WriteUtf8File(progress.progressPath,
            "MC V147 progress log\r\nstatus=running\r\nphase=initializing\r\n");
        (void)WriteUtf8File(progress.diagnosticsPath,
            "MC V147 detailed diagnostics\r\nstatic_only=true\r\n");
        (void)WriteUtf8File(progress.errorsPath,
            "MC V147 errors and safety stops\r\n");
        progress.Progress("phase=1/7 snapshot_ready");
        progress.Diagnostic("analysis_start process_id=" + std::to_string(snapshot.processId));

        RuntimeFunctionTableAnalysis runtimeTable;
        progress.Progress("phase=2/7 loading_runtime_function_table");
        (void)LoadRuntimeFunctionTable(snapshot, runtimeTable);
        progress.Diagnostic("runtime_table " + runtimeTable.diagnostic);

        std::vector<Slot17Target> targets;
        std::size_t pageIndex = 0;
        for (const auto& def : definitions)
        {
            ++pageIndex;
            if (progress.CancelRequested())
            {
                progress.Error("status=cancelled_by_user before_page=" + std::string(def.page));
                break;
            }
            if (GetTickCount64() - progress.analysisStarted >= 300000)
            {
                progress.Error("status=aborted_safely reason=total_timeout before_page=" + std::string(def.page));
                break;
            }
            std::ostringstream phase;
            phase << "phase=" << (pageIndex + 2) << "/7 page=" << def.page << " status=started";
            progress.Progress(phase.str());
            progress.Diagnostic(phase.str());

            Slot17Target target;
            target.page = def.page;
            target.methodRva = def.rva;
            RuntimeFunctionBoundaryAnalysis boundary =
                AnalyzeRuntimeFunctionBoundary(snapshot, snapshot.atonpTrackerBase + def.rva);
            target.boundaryFound = boundary.enclosingFound;
            if (target.boundaryFound)
            {
                target.boundary = boundary.entry;
                std::ostringstream boundaryLine;
                boundaryLine << "page=" << def.page
                             << " boundary_begin=" << HexValue(target.boundary.beginAddress)
                             << " boundary_end=" << HexValue(target.boundary.endAddress);
                progress.Diagnostic(boundaryLine.str());
                if (ReadBoundaryBytes(snapshot, target.boundary, target.bytes))
                    target.instructions = DecodeSlot17Instructions(target, progress);
                else
                {
                    target.decodeAborted = true;
                    target.decodeStopReason = "read_boundary_bytes_failed";
                    progress.Error("page=" + std::string(def.page) + " status=aborted_safely reason=read_boundary_bytes_failed");
                }
            }
            else
            {
                target.decodeAborted = true;
                target.decodeStopReason = "boundary_not_found";
                progress.Error("page=" + std::string(def.page) + " status=aborted_safely reason=boundary_not_found");
            }
            targets.push_back(target);
        }

        progress.Progress("phase=6/7 building_unique_callee_cache");
        std::map<std::uint32_t, unsigned int> callMasks;
        for (std::size_t i = 0; i < targets.size(); ++i)
            for (const Slot17Instruction& ins : targets[i].instructions)
                if (ins.directCall) callMasks[ins.targetRva] |= (1u << static_cast<unsigned int>(i));

        struct CalleeBoundaryCacheEntry
        {
            bool found = false;
            std::uint32_t size = 0;
        };
        std::map<std::uint32_t, CalleeBoundaryCacheEntry> calleeCache;
        std::size_t calleeIndex = 0;
        for (const auto& callEntry : callMasks)
        {
            ++calleeIndex;
            if (progress.CancelRequested())
            {
                progress.Error("status=cancelled_by_user phase=callee_cache");
                break;
            }
            if (GetTickCount64() - progress.analysisStarted >= 300000)
            {
                progress.Error("status=aborted_safely reason=total_timeout phase=callee_cache");
                break;
            }
            RuntimeFunctionBoundaryAnalysis calleeBoundary =
                AnalyzeRuntimeFunctionBoundary(snapshot, snapshot.atonpTrackerBase + callEntry.first);
            CalleeBoundaryCacheEntry cacheEntry;
            cacheEntry.found = calleeBoundary.enclosingFound;
            if (cacheEntry.found)
                cacheEntry.size = calleeBoundary.entry.endAddress - calleeBoundary.entry.beginAddress;
            calleeCache[callEntry.first] = cacheEntry;
            if ((calleeIndex % 25) == 0 || calleeIndex == callMasks.size())
            {
                std::ostringstream line;
                line << "callee_cache_progress=" << calleeIndex << '/' << callMasks.size()
                     << " elapsed_ms=" << (GetTickCount64() - progress.analysisStarted);
                progress.Progress(line.str());
                progress.Diagnostic(line.str());
            }
        }

        reportPath = ReportPath(L"MC_V147_Slot17_Deep_Analysis", snapshot.processId);
        csvPath = ReportPath(L"MC_V147_Slot17_Candidates", snapshot.processId);
        const std::size_t dot = csvPath.find_last_of(L'.');
        if (dot != std::wstring::npos) csvPath.replace(dot, std::wstring::npos, L".csv");
        EnsureOutputDirectory();

        std::ostringstream out;
        out << "MC V147 Slot 17 Deep Analysis\r\n"
            << "================================\r\n"
            << "STATIC ANALYSIS ONLY. NO PAGE, VTABLE, GRID OR EXTRACTOR METHOD IS CALLED.\r\n"
            << "internal_function_calls_enabled=false\r\n"
            << "state_changing_messages_enabled=false\r\n"
            << "live_attempted=false\r\n"
            << "process_id=" << snapshot.processId << "\r\n"
            << "atonptracker_base=" << HexValue(snapshot.atonpTrackerBase) << "\r\n"
            << "runtime_table=" << runtimeTable.diagnostic << "\r\n\r\n";

        std::ostringstream csv;
        csv << "page,caller_rva,call_site_rva,callee_rva,call_type,shared_mask,function_size,score,reasons\r\n";

        for (const Slot17Target& target : targets)
        {
            std::size_t directCalls = 0, indirectCalls = 0, field1d0 = 0, field48 = 0, gridHints = 0;
            for (const Slot17Instruction& ins : target.instructions)
            {
                directCalls += ins.directCall ? 1 : 0;
                indirectCalls += ins.indirectCall ? 1 : 0;
                field1d0 += ins.field1d0 ? 1 : 0;
                field48 += ins.field48 ? 1 : 0;
                gridHints += ins.gridHint ? 1 : 0;
            }
            out << "TARGET page=" << target.page
                << " method_rva=" << HexValue(target.methodRva)
                << " boundary_found=" << (target.boundaryFound ? "true" : "false");
            if (target.boundaryFound)
                out << " function_rva=" << HexValue(target.boundary.beginAddress)
                    << "-" << HexValue(target.boundary.endAddress)
                    << " function_size=" << (target.boundary.endAddress - target.boundary.beginAddress);
            out << "\r\n"
                << "decoded_instruction_count=" << target.instructions.size()
                << " direct_calls=" << directCalls
                << " indirect_calls=" << indirectCalls
                << " disp_1d0=" << field1d0
                << " disp_48=" << field48
                << " grid_hints=" << gridHints
                << " decode_status=" << (target.decodeAborted ? "aborted_safely" : "completed");
            if (target.decodeAborted) out << " decode_reason=" << target.decodeStopReason;
            out << "\r\n\r\n";

            out << "FIELD ACCESS ANALYSIS\r\n";
            bool anyField = false;
            for (const Slot17Instruction& ins : target.instructions)
            {
                if (ins.field1d0 || ins.field48 || ins.gridHint)
                {
                    anyField = true;
                    out << "  " << ins.text << "\r\n";
                }
            }
            if (!anyField) out << "  none\r\n";
            out << "\r\nDIRECT AND INDIRECT CALLS\r\n";
            for (const Slot17Instruction& ins : target.instructions)
            {
                if (!ins.directCall && !ins.indirectCall) continue;
                out << "  " << ins.text;
                if (ins.directCall)
                {
                    const unsigned int mask = callMasks[ins.targetRva];
                    out << " shared_mask=" << mask;
                    int score = 10;
                    std::string reasons = "direct_call";
                    if (mask == 1 || mask == 2 || mask == 4) { score += 20; reasons += "|page_specific"; }
                    else if (mask == 7) { score -= 8; reasons += "|shared_all_three"; }
                    std::uint32_t calleeSize = 0;
                    const auto cacheIt = calleeCache.find(ins.targetRva);
                    if (cacheIt != calleeCache.end() && cacheIt->second.found)
                    {
                        calleeSize = cacheIt->second.size;
                        if (calleeSize >= 80 && calleeSize <= 2400)
                        {
                            score += 5;
                            reasons += "|moderate_size";
                        }
                    }
                    csv << target.page << ','
                        << HexValue(target.methodRva) << ','
                        << HexValue(ins.rva) << ','
                        << HexValue(ins.targetRva) << ','
                        << "direct," << mask << ',' << calleeSize << ','
                        << score << ',' << reasons << "\r\n";
                }
                out << "\r\n";
            }

            out << "\r\nDISASSEMBLY (CONSERVATIVE INSTRUCTION WALK)\r\n";
            for (const Slot17Instruction& ins : target.instructions)
                out << "  " << ins.text << "\r\n";
            out << "\r\n";
        }

        out << "SHARED CALL COMPARISON\r\n----------------------\r\n";
        for (const auto& entry : callMasks)
        {
            out << "callee_rva=" << HexValue(entry.first)
                << " shared_mask=" << entry.second
                << " pages="
                << ((entry.second & 1) ? "accounts " : "")
                << ((entry.second & 2) ? "open_positions " : "")
                << ((entry.second & 4) ? "logs " : "") << "\r\n";
        }
        out << "\r\nSAFETY CONCLUSION\r\n-----------------\r\n"
            << "- The decoder is conservative and may stop short of full semantic reconstruction.\r\n"
            << "- Direct CALL targets are resolved only from instruction boundaries found by the walker.\r\n"
            << "- Possible virtual slots are reported only for simple FF /2 memory forms.\r\n"
            << "- No listed candidate is approved for execution.\r\n";

        progress.Progress("phase=7/7 writing_final_reports");
        const bool reportWritten = WriteUtf8File(reportPath, out.str());
        const bool csvWritten = WriteUtf8File(csvPath, csv.str());
        const bool cancelled = progress.CancelRequested();
        std::ostringstream finalLine;
        finalLine << "status=" << (cancelled ? "cancelled_by_user" :
            ((reportWritten && csvWritten) ? "completed" : "completed_with_write_error"))
                  << " total_elapsed_ms=" << (GetTickCount64() - progress.analysisStarted)
                  << " report_written=" << (reportWritten ? "true" : "false")
                  << " csv_written=" << (csvWritten ? "true" : "false");
        progress.Progress(finalLine.str());
        progress.Diagnostic(finalLine.str());
        std::ostringstream json;
        json << "{\"analysis\":\"slot17_deep\",\"static_only\":true,"
             << "\"targets\":" << targets.size()
             << ",\"unique_direct_callees\":" << callMasks.size()
             << ",\"report_written\":" << (reportWritten ? "true" : "false")
             << ",\"csv_written\":" << (csvWritten ? "true" : "false")
             << ",\"report_path\":" << JsonString(reportPath)
             << ",\"csv_path\":" << JsonString(csvPath)
             << ",\"progress_path\":" << JsonString(progress.progressPath)
             << ",\"diagnostics_path\":" << JsonString(progress.diagnosticsPath)
             << ",\"errors_path\":" << JsonString(progress.errorsPath) << "}";
        summaryJson = json.str();
        return reportWritten && csvWritten;
    }


    std::string ProtectionName(DWORD protection)
    {
        const DWORD basic = protection & 0xFFu;
        switch (basic)
        {
        case PAGE_NOACCESS: return "NOACCESS";
        case PAGE_READONLY: return "READONLY";
        case PAGE_READWRITE: return "READWRITE";
        case PAGE_WRITECOPY: return "WRITECOPY";
        case PAGE_EXECUTE: return "EXECUTE";
        case PAGE_EXECUTE_READ: return "EXECUTE_READ";
        case PAGE_EXECUTE_READWRITE: return "EXECUTE_READWRITE";
        case PAGE_EXECUTE_WRITECOPY: return "EXECUTE_WRITECOPY";
        default: return "UNKNOWN";
        }
    }

    bool WriteLoadedModuleMemoryImage(
        const Snapshot& snapshot,
        std::wstring& imagePath,
        std::wstring& regionPath,
        std::wstring& manifestPath,
        std::string& diagnostic)
    {
        diagnostic.clear();
        if (!snapshot.atonpTrackerBase || snapshot.atonpTrackerSize == 0)
        {
            diagnostic = "ATOnPTracker module is not loaded";
            return false;
        }

        imagePath = ReportPath(L"MC_V147_ATOnPTracker_Loaded_Image", snapshot.processId);
        const std::wstring suffix = L".txt";
        if (imagePath.size() >= suffix.size() && imagePath.substr(imagePath.size() - suffix.size()) == suffix)
            imagePath.replace(imagePath.size() - suffix.size(), suffix.size(), L".bin");
        regionPath = ReportPath(L"MC_V147_ATOnPTracker_Memory_Regions", snapshot.processId);
        manifestPath = ReportPath(L"MC_V147_Research_Manifest", snapshot.processId);

        std::ofstream image(imagePath, std::ios::binary | std::ios::trunc);
        std::ofstream regions(regionPath, std::ios::binary | std::ios::trunc);
        if (!image || !regions)
        {
            diagnostic = "could not create module image or region map";
            return false;
        }

        regions << "region_index,va_begin,va_end,rva_begin,size,state,type,protection,readable,dumped_bytes,zero_filled_bytes\r\n";
        const std::uintptr_t moduleBegin = snapshot.atonpTrackerBase;
        const std::uintptr_t moduleEnd = moduleBegin + snapshot.atonpTrackerSize;
        std::uintptr_t cursor = moduleBegin;
        std::uint64_t dumped = 0;
        std::uint64_t zeroFilled = 0;
        std::size_t regionIndex = 0;
        std::vector<unsigned char> buffer(64u * 1024u, 0);

        while (cursor < moduleEnd)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
            {
                diagnostic = "VirtualQuery failed inside module range";
                return false;
            }
            const std::uintptr_t regionBegin = std::max(cursor, reinterpret_cast<std::uintptr_t>(mbi.BaseAddress));
            const std::uintptr_t rawRegionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            const std::uintptr_t regionEnd = std::min(moduleEnd, rawRegionEnd);
            if (regionEnd <= regionBegin)
            {
                diagnostic = "non-advancing memory region";
                return false;
            }

            const bool readable = mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
            std::uint64_t regionDumped = 0;
            std::uint64_t regionZero = 0;
            std::uintptr_t part = regionBegin;
            while (part < regionEnd)
            {
                const std::size_t count = static_cast<std::size_t>(
                    std::min<std::uintptr_t>(buffer.size(), regionEnd - part));
                std::fill(buffer.begin(), buffer.begin() + count, static_cast<unsigned char>(0));
                bool copied = false;
                if (readable)
                    copied = SafeReadBytes(reinterpret_cast<void*>(part), buffer.data(), count);
                image.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(count));
                if (!image)
                {
                    diagnostic = "module image write failed";
                    return false;
                }
                if (copied) { dumped += count; regionDumped += count; }
                else { zeroFilled += count; regionZero += count; }
                part += count;
            }

            regions << regionIndex++ << ','
                    << HexValue(regionBegin) << ',' << HexValue(regionEnd) << ','
                    << HexValue(regionBegin - moduleBegin) << ','
                    << (regionEnd - regionBegin) << ','
                    << HexValue(mbi.State) << ',' << HexValue(mbi.Type) << ','
                    << ProtectionName(mbi.Protect) << ','
                    << (readable ? "yes" : "no") << ','
                    << regionDumped << ',' << regionZero << "\r\n";
            regions.flush();
            image.flush();
            cursor = regionEnd;
        }

        std::ostringstream manifest;
        manifest << "{\r\n"
                 << "  \"capture_version\": 143,\r\n"
                 << "  \"architecture\": \"x64\",\r\n"
                 << "  \"pointer_size\": 8,\r\n"
                 << "  \"process_id\": " << snapshot.processId << ",\r\n"
                 << "  \"tracker_thread_id\": " << snapshot.trackerThreadId << ",\r\n"
                 << "  \"module_base\": " << JsonString(HexValue(snapshot.atonpTrackerBase)) << ",\r\n"
                 << "  \"module_size\": " << snapshot.atonpTrackerSize << ",\r\n"
                 << "  \"loaded_image_file\": " << JsonString(imagePath) << ",\r\n"
                 << "  \"region_map_file\": " << JsonString(regionPath) << ",\r\n"
                 << "  \"bytes_copied\": " << dumped << ",\r\n"
                 << "  \"bytes_zero_filled\": " << zeroFilled << ",\r\n"
                 << "  \"known_rvas\": {\r\n"
                 << "    \"accounts_slot17\": \"0x78A00\",\r\n"
                 << "    \"open_positions_slot17\": \"0xADFD0\",\r\n"
                 << "    \"logs_slot17\": \"0xCFAB0\"\r\n"
                 << "  }\r\n"
                 << "}\r\n";
        const bool manifestWritten = WriteUtf8File(manifestPath, manifest.str());
        diagnostic = manifestWritten ? "completed" : "manifest write failed";
        return manifestWritten;
    }


    struct LiveGraphNode
    {
        std::size_t id = 0;
        std::uintptr_t address = 0;
        std::size_t depth = 0;
        int priority = 0;
        std::string label;
        std::string sourceRoot;
        std::size_t parentId = static_cast<std::size_t>(-1);
        std::size_t parentOffset = 0;
    };

    bool QueryReadableSpan(std::uintptr_t address, std::uintptr_t& regionEnd, MEMORY_BASIC_INFORMATION& mbi)
    {
        regionEnd = 0;
        std::memset(&mbi, 0, sizeof(mbi));
        if (address < 0x10000 || VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD basic = mbi.Protect & 0xFFu;
        const bool readable = basic == PAGE_READONLY || basic == PAGE_READWRITE || basic == PAGE_WRITECOPY ||
            basic == PAGE_EXECUTE_READ || basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
        if (!readable)
            return false;
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        regionEnd = begin + mbi.RegionSize;
        return address >= begin && address < regionEnd;
    }

    bool WriteLiveObjectGraphCapture(const Snapshot& snapshot, std::string& summaryJson, const char* requestedPage)
    {
        CreateDirectoryW(kOutputDirectory, nullptr);
        const std::string page = (requestedPage && *requestedPage) ? requestedPage : "unspecified";
        std::wstring pageWide(page.begin(), page.end());
        const std::wstring prefix = L"MC_V147_" + pageWide + L"_Live_Object_Graph_";
        const std::wstring nodesPath = ReportPath((prefix + L"Nodes").c_str(), snapshot.processId);
        const std::wstring edgesPath = ReportPath((prefix + L"Edges").c_str(), snapshot.processId);
        const std::wstring vtablesPath = ReportPath((prefix + L"Vtables").c_str(), snapshot.processId);
        const std::wstring stringsPath = ReportPath((prefix + L"Strings").c_str(), snapshot.processId);
        const std::wstring containersPath = ReportPath((prefix + L"Containers").c_str(), snapshot.processId);
        const std::wstring progressPath = ReportPath((prefix + L"Progress").c_str(), snapshot.processId);
        const std::wstring diagnosticsPath = ReportPath((prefix + L"Diagnostics").c_str(), snapshot.processId);
        const std::wstring manifestPath = ReportPath((prefix + L"Manifest").c_str(), snapshot.processId);
        std::wstring binaryPath = ReportPath((prefix + L"Memory").c_str(), snapshot.processId);
        const std::wstring suffix = L".txt";
        if (binaryPath.size() >= suffix.size() && binaryPath.substr(binaryPath.size() - suffix.size()) == suffix)
            binaryPath.replace(binaryPath.size() - suffix.size(), suffix.size(), L".bin");

        std::ofstream nodes(nodesPath, std::ios::binary | std::ios::trunc);
        std::ofstream edges(edgesPath, std::ios::binary | std::ios::trunc);
        std::ofstream vtables(vtablesPath, std::ios::binary | std::ios::trunc);
        std::ofstream strings(stringsPath, std::ios::binary | std::ios::trunc);
        std::ofstream containers(containersPath, std::ios::binary | std::ios::trunc);
        std::ofstream progress(progressPath, std::ios::binary | std::ios::trunc);
        std::ofstream diagnostics(diagnosticsPath, std::ios::binary | std::ios::trunc);
        std::ofstream binary(binaryPath, std::ios::binary | std::ios::trunc);
        if (!nodes || !edges || !vtables || !strings || !containers || !progress || !diagnostics || !binary)
        {
            summaryJson = "{\"capture\":\"live_object_graph\",\"version\":145,\"error\":\"could_not_create_output_files\"}";
            return false;
        }

        nodes << "node_id,address,depth,priority,source_root,label,parent_id,parent_offset,region_base,region_size,protection,type,object_candidate,vtable,dump_offset,dump_size,pointer_slots_scanned\r\n";
        edges << "from_node,from_address,field_offset,to_address,to_node,classification,source_root\r\n";
        vtables << "object_node,object_address,vtable_address,slot,target,executable,target_module_rva\r\n";
        strings << "node_id,address,offset,encoding,length,text_preview\r\n";
        containers << "node_id,address,offset,kind,begin_ptr,end_ptr,capacity_ptr,element_count,confidence\r\n";
        progress << "MC V147 Prioritized Live Object Graph Capture\r\npage=" << page << "\r\nstatus=started\r\n" << std::flush;
        diagnostics << "MC V147 diagnostics\r\nread_only=yes\r\nunknown_functions_called=no\r\n" << std::flush;

        ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "accounts", kExtractAccountsRva);
        ExtractorAttempt positions = BuildExtractorProbe(snapshot, "open_positions", kExtractOpenPositionsRva);

        constexpr std::size_t kMaximumNodes = 20000;
        constexpr std::size_t kMaximumDepth = 5;
        constexpr std::uint64_t kMaximumTotalBytes = 512ull * 1024ull * 1024ull;
        constexpr std::size_t kLiveGraphMaximumVtableSlots = 512;
        constexpr std::size_t kMaximumChildrenPerNode = 256;
        constexpr std::uint64_t kMaximumRuntimeMilliseconds = 10ull * 60ull * 1000ull;

        std::deque<LiveGraphNode> priorityPending;
        std::deque<LiveGraphNode> normalPending;
        std::map<std::uintptr_t, std::size_t> ids;
        std::vector<LiveGraphNode> allNodes;
        auto enqueue = [&](std::uintptr_t address, std::size_t depth, int priority, const std::string& sourceRoot,
                           const std::string& label, std::size_t parentId, std::size_t parentOffset) -> std::size_t
        {
            auto found = ids.find(address);
            if (found != ids.end()) return found->second;
            if (!address || allNodes.size() >= kMaximumNodes) return static_cast<std::size_t>(-1);
            std::uintptr_t end = 0; MEMORY_BASIC_INFORMATION mbi{};
            if (!QueryReadableSpan(address, end, mbi)) return static_cast<std::size_t>(-1);
            LiveGraphNode node;
            node.id = allNodes.size(); node.address = address; node.depth = depth; node.priority = priority;
            node.sourceRoot = sourceRoot; node.label = label; node.parentId = parentId; node.parentOffset = parentOffset;
            ids[address] = node.id; allNodes.push_back(node);
            if (priority >= 2) priorityPending.push_back(node); else normalPending.push_back(node);
            return node.id;
        };

        const std::size_t noParent = static_cast<std::size_t>(-1);
        const std::size_t tabRoot = enqueue(accounts.tabView, 0, 3, "CATPTTabView", "CATPTTabView_primary", noParent, 0);
        if (accounts.tabView) enqueue(accounts.tabView + 0x48, 0, 3, "CATPTTabView", "CATPTTabView_secondary_subobject", noParent, 0x48);
        enqueue(accounts.tradeInfo, 0, 3, "ITC_TradeInfo", "ITC_TradeInfo_accounts", noParent, 0);
        enqueue(positions.tradeInfo, 0, 3, "ITC_TradeInfo", "ITC_TradeInfo_open_positions", noParent, 0);

        const std::size_t specialOffsets[] = { 0x48, 0x1D0, 0x320, 0x328 };
        if (accounts.tabView)
        {
            for (std::size_t off : specialOffsets)
            {
                std::uintptr_t value = 0;
                if (SafeReadValue(reinterpret_cast<void*>(accounts.tabView + off), value))
                    enqueue(value, 1, 3, "CATPTTabView", std::string("CATPTTabView_field_") + HexValue(off), tabRoot, off);
            }
        }

        auto startTime = std::chrono::steady_clock::now();
        std::uint64_t totalBytes = 0;
        std::size_t processed = 0;
        std::string stopReason = "completed";
        while ((!priorityPending.empty() || !normalPending.empty()) && processed < kMaximumNodes && totalBytes < kMaximumTotalBytes)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
            if (elapsed >= static_cast<long long>(kMaximumRuntimeMilliseconds)) { stopReason = "runtime_limit"; break; }
            LiveGraphNode node;
            if (!priorityPending.empty()) { node = priorityPending.front(); priorityPending.pop_front(); }
            else { node = normalPending.front(); normalPending.pop_front(); }

            std::uintptr_t regionEnd = 0; MEMORY_BASIC_INFORMATION mbi{};
            if (!QueryReadableSpan(node.address, regionEnd, mbi)) continue;
            const std::size_t available = static_cast<std::size_t>(regionEnd - node.address);
            std::size_t requestedBytes = 1024;
            if (node.depth == 0 || node.priority >= 3) requestedBytes = 16u * 1024u;
            else if (node.priority == 2) requestedBytes = 8u * 1024u;
            else if (PointerLooksLikeObject(node.address)) requestedBytes = 4u * 1024u;
            const std::size_t dumpSize = std::min<std::size_t>(requestedBytes, available);
            if (dumpSize == 0 || totalBytes + dumpSize > kMaximumTotalBytes) { stopReason = "byte_limit"; break; }
            std::vector<unsigned char> bytes(dumpSize, 0);
            if (!SafeReadBytes(reinterpret_cast<void*>(node.address), bytes.data(), dumpSize)) continue;
            const std::uint64_t dumpOffset = totalBytes;
            binary.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            binary.flush(); totalBytes += dumpSize;

            std::uintptr_t vtable = 0;
            const bool objectCandidate = VtableStartsWithExecutableCode(node.address, vtable);
            const std::size_t pointerSlots = dumpSize / sizeof(std::uintptr_t);
            nodes << node.id << ',' << HexValue(node.address) << ',' << node.depth << ',' << node.priority << ','
                  << '"' << node.sourceRoot << '"' << ',' << '"' << node.label << '"' << ',';
            if (node.parentId == noParent) nodes << -1; else nodes << node.parentId;
            nodes << ',' << HexValue(node.parentOffset) << ',' << HexValue(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress))
                  << ',' << mbi.RegionSize << ',' << ProtectionName(mbi.Protect) << ',' << HexValue(mbi.Type) << ','
                  << (objectCandidate ? "yes" : "no") << ',' << HexValue(vtable) << ',' << dumpOffset << ','
                  << dumpSize << ',' << pointerSlots << "\r\n" << std::flush;

            if (objectCandidate)
            {
                for (std::size_t slot = 0; slot < kLiveGraphMaximumVtableSlots; ++slot)
                {
                    std::uintptr_t target = 0;
                    if (!SafeReadValue(reinterpret_cast<void*>(vtable + slot * sizeof(void*)), target)) break;
                    const bool executable = MemoryRangeHasProtection(reinterpret_cast<void*>(target), 1, true);
                    vtables << node.id << ',' << HexValue(node.address) << ',' << HexValue(vtable) << ',' << slot << ','
                            << HexValue(target) << ',' << (executable ? "yes" : "no") << ',';
                    if (target >= snapshot.atonpTrackerBase && target < snapshot.atonpTrackerBase + snapshot.atonpTrackerSize)
                        vtables << HexValue(target - snapshot.atonpTrackerBase);
                    vtables << "\r\n";
                    if (!executable && slot > 8) break;
                }
                vtables.flush();
            }

            // Conservative ASCII and UTF-16 string previews.
            for (std::size_t i = 0; i + 5 < dumpSize; ++i)
            {
                if (bytes[i] >= 32 && bytes[i] <= 126)
                {
                    std::size_t j = i;
                    while (j < dumpSize && bytes[j] >= 32 && bytes[j] <= 126 && j - i < 160) ++j;
                    if (j - i >= 5)
                    {
                        std::string text(reinterpret_cast<const char*>(bytes.data() + i), j - i);
                        for (char& c : text) if (c == '"') c = '\'';
                        strings << node.id << ',' << HexValue(node.address) << ',' << HexValue(i) << ",ascii," << (j-i) << ",\"" << text << "\"\r\n";
                        i = j;
                    }
                }
            }
            for (std::size_t i = 0; i + 10 < dumpSize; i += 2)
            {
                std::size_t j = i; std::string text;
                while (j + 1 < dumpSize && bytes[j] >= 32 && bytes[j] <= 126 && bytes[j+1] == 0 && text.size() < 160)
                { text.push_back(static_cast<char>(bytes[j])); j += 2; }
                if (text.size() >= 5)
                {
                    for (char& c : text) if (c == '"') c = '\'';
                    strings << node.id << ',' << HexValue(node.address) << ',' << HexValue(i) << ",utf16le," << text.size() << ",\"" << text << "\"\r\n";
                    i = j;
                }
            }
            strings.flush();

            // Conservative MSVC std::vector-like triple detection: begin <= end <= capacity.
            for (std::size_t offset = 0; offset + 3 * sizeof(std::uintptr_t) <= dumpSize; offset += sizeof(std::uintptr_t))
            {
                std::uintptr_t beginPtr=0,endPtr=0,capPtr=0;
                std::memcpy(&beginPtr, bytes.data()+offset, sizeof(beginPtr));
                std::memcpy(&endPtr, bytes.data()+offset+sizeof(beginPtr), sizeof(endPtr));
                std::memcpy(&capPtr, bytes.data()+offset+2*sizeof(beginPtr), sizeof(capPtr));
                if (beginPtr && beginPtr <= endPtr && endPtr <= capPtr && capPtr-beginPtr <= 64ull*1024ull*1024ull)
                {
                    std::uintptr_t spanEnd=0; MEMORY_BASIC_INFORMATION vectorMbi{};
                    if (QueryReadableSpan(beginPtr, spanEnd, vectorMbi) && capPtr <= spanEnd)
                    {
                        containers << node.id << ',' << HexValue(node.address) << ',' << HexValue(offset)
                                   << ",vector_like," << HexValue(beginPtr) << ',' << HexValue(endPtr) << ',' << HexValue(capPtr)
                                   << ',' << (endPtr-beginPtr) << ",medium\r\n";
                    }
                }
            }
            containers.flush();

            if (node.depth < kMaximumDepth)
            {
                std::size_t acceptedChildren = 0;
                for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= dumpSize && acceptedChildren < kMaximumChildrenPerNode; offset += sizeof(std::uintptr_t))
                {
                    std::uintptr_t value = 0;
                    std::memcpy(&value, bytes.data() + offset, sizeof(value));
                    std::uintptr_t targetEnd = 0; MEMORY_BASIC_INFORMATION targetMbi{};
                    if (!QueryReadableSpan(value, targetEnd, targetMbi)) continue;
                    const bool targetObject = PointerLooksLikeObject(value);
                    const bool allocationStart = value == reinterpret_cast<std::uintptr_t>(targetMbi.BaseAddress);
                    const bool moderateBlock = targetMbi.RegionSize <= (16u * 1024u * 1024u);
                    const bool fromPriorityRoot = node.priority >= 2 || node.depth == 0;
                    if (!fromPriorityRoot && !targetObject && !allocationStart && !moderateBlock) continue;
                    const int childPriority = targetObject ? 2 : (fromPriorityRoot ? 1 : 0);
                    std::string classification = targetObject ? "object_pointer" : (allocationStart ? "allocation_base_pointer" : "readable_pointer");
                    const std::size_t child = enqueue(value, node.depth + 1, childPriority, node.sourceRoot, classification, node.id, offset);
                    edges << node.id << ',' << HexValue(node.address) << ',' << HexValue(offset) << ',' << HexValue(value) << ',';
                    if (child == static_cast<std::size_t>(-1)) edges << -1; else { edges << child; ++acceptedChildren; }
                    edges << ',' << classification << ',' << '"' << node.sourceRoot << '"' << "\r\n";
                }
                edges.flush();
            }

            ++processed;
            if ((processed % 64) == 0 || (priorityPending.empty() && normalPending.empty()))
            {
                progress << "processed_nodes=" << processed << " discovered_nodes=" << allNodes.size()
                         << " priority_pending=" << priorityPending.size() << " normal_pending=" << normalPending.size()
                         << " bytes=" << totalBytes << " elapsed_ms=" << elapsed << "\r\n" << std::flush;
            }
        }
        if (processed >= kMaximumNodes) stopReason = "node_limit";
        else if (totalBytes >= kMaximumTotalBytes) stopReason = "byte_limit";

        std::ostringstream manifest;
        manifest << "{\r\n"
                 << "  \"capture_version\": 146,\r\n"
                 << "  \"capture_type\": \"prioritized_live_object_graph\",\r\n"
                 << "  \"page\": " << JsonString(page) << ",\r\n"
                 << "  \"read_only\": true,\r\n"
                 << "  \"unknown_functions_called\": false,\r\n"
                 << "  \"process_id\": " << snapshot.processId << ",\r\n"
                 << "  \"module_base\": " << JsonString(HexValue(snapshot.atonpTrackerBase)) << ",\r\n"
                 << "  \"CATPTTabView\": " << JsonString(HexValue(accounts.tabView)) << ",\r\n"
                 << "  \"ITC_TradeInfo\": " << JsonString(HexValue(accounts.tradeInfo)) << ",\r\n"
                 << "  \"nodes_discovered\": " << allNodes.size() << ",\r\n"
                 << "  \"nodes_processed\": " << processed << ",\r\n"
                 << "  \"memory_bytes\": " << totalBytes << ",\r\n"
                 << "  \"stop_reason\": " << JsonString(stopReason) << ",\r\n"
                 << "  \"limits\": {\"max_nodes\":20000,\"max_depth\":5,\"max_total_bytes\":536870912,\"max_vtable_slots\":512,\"max_children_per_node\":256,\"max_runtime_ms\":600000},\r\n"
                 << "  \"files\": {\r\n"
                 << "    \"nodes\": " << JsonString(nodesPath) << ",\r\n"
                 << "    \"edges\": " << JsonString(edgesPath) << ",\r\n"
                 << "    \"vtables\": " << JsonString(vtablesPath) << ",\r\n"
                 << "    \"strings\": " << JsonString(stringsPath) << ",\r\n"
                 << "    \"containers\": " << JsonString(containersPath) << ",\r\n"
                 << "    \"memory\": " << JsonString(binaryPath) << ",\r\n"
                 << "    \"progress\": " << JsonString(progressPath) << ",\r\n"
                 << "    \"diagnostics\": " << JsonString(diagnosticsPath) << "\r\n"
                 << "  }\r\n"
                 << "}\r\n";
        const bool manifestOk = WriteUtf8File(manifestPath, manifest.str());
        progress << "status=" << (manifestOk ? "completed" : "manifest_failed") << "\r\n"
                 << "stop_reason=" << stopReason << "\r\nprocessed_nodes=" << processed << "\r\nbytes=" << totalBytes << "\r\n" << std::flush;
        diagnostics << "stop_reason=" << stopReason << "\r\nnodes_discovered=" << allNodes.size() << "\r\nnodes_processed=" << processed << "\r\n" << std::flush;

        std::ostringstream summary;
        summary << "{\"capture\":\"prioritized_live_object_graph\",\"version\":145,\"page\":" << JsonString(page) << ','
                << "\"read_only\":true,\"nodes_discovered\":" << allNodes.size() << ",\"nodes_processed\":" << processed << ','
                << "\"memory_bytes\":" << totalBytes << ",\"stop_reason\":" << JsonString(stopReason) << ','
                << "\"manifest_written\":" << (manifestOk ? "true" : "false") << ",\"manifest_path\":" << JsonString(manifestPath) << '}';
        summaryJson = summary.str();
        return manifestOk && processed > 0;
    }


    struct TargetedProbeTarget
    {
        std::string name;
        std::string chain;
        std::uintptr_t address = 0;
        std::size_t requestedBytes = 0;
    };

    bool AddResolvedPointerTarget(
        std::vector<TargetedProbeTarget>& targets,
        const std::string& name,
        const std::string& chain,
        std::uintptr_t pointerAddress,
        std::size_t requestedBytes)
    {
        std::uintptr_t value = 0;
        if (!pointerAddress || !SafeReadValue(reinterpret_cast<void*>(pointerAddress), value))
            return false;
        std::uintptr_t regionEnd = 0; MEMORY_BASIC_INFORMATION mbi{};
        if (!QueryReadableSpan(value, regionEnd, mbi))
            return false;
        targets.push_back({name, chain, value, requestedBytes});
        return true;
    }

    bool WriteTargetedStructureProbe(
        const Snapshot& snapshot,
        std::string& summaryJson,
        const char* requestedPage)
    {
        CreateDirectoryW(kOutputDirectory, nullptr);
        const std::string page = (requestedPage && *requestedPage) ? requestedPage : "unspecified";
        std::wstring pageWide(page.begin(), page.end());
        const std::wstring prefix = L"MC_V147_" + pageWide + L"_Targeted_Structure_Probe_";
        const std::wstring targetsPath = ReportPath((prefix + L"Targets").c_str(), snapshot.processId);
        const std::wstring stringsPath = ReportPath((prefix + L"Strings").c_str(), snapshot.processId);
        const std::wstring scalarsPath = ReportPath((prefix + L"Scalars").c_str(), snapshot.processId);
        const std::wstring pointersPath = ReportPath((prefix + L"Pointers").c_str(), snapshot.processId);
        const std::wstring diffsPath = ReportPath((prefix + L"Diffs").c_str(), snapshot.processId);
        const std::wstring diagnosticsPath = ReportPath((prefix + L"Diagnostics").c_str(), snapshot.processId);
        const std::wstring manifestPath = ReportPath((prefix + L"Manifest").c_str(), snapshot.processId);
        std::wstring memoryPath = ReportPath((prefix + L"Memory").c_str(), snapshot.processId);
        if (memoryPath.size() >= 4 && memoryPath.substr(memoryPath.size()-4) == L".txt")
            memoryPath.replace(memoryPath.size()-4, 4, L".bin");

        std::ofstream targetsFile(targetsPath, std::ios::binary | std::ios::trunc);
        std::ofstream stringsFile(stringsPath, std::ios::binary | std::ios::trunc);
        std::ofstream scalarsFile(scalarsPath, std::ios::binary | std::ios::trunc);
        std::ofstream pointersFile(pointersPath, std::ios::binary | std::ios::trunc);
        std::ofstream diffsFile(diffsPath, std::ios::binary | std::ios::trunc);
        std::ofstream diagnostics(diagnosticsPath, std::ios::binary | std::ios::trunc);
        std::ofstream memory(memoryPath, std::ios::binary | std::ios::trunc);
        if (!targetsFile || !stringsFile || !scalarsFile || !pointersFile || !diffsFile || !diagnostics || !memory)
        {
            summaryJson = "{\"capture\":\"targeted_structure_probe\",\"version\":146,\"error\":\"could_not_create_output_files\"}";
            return false;
        }

        targetsFile << "sample,target_id,name,chain,address,region_base,region_size,protection,dump_offset,dump_size,read_ok\r\n";
        stringsFile << "sample,target_id,target_name,address,offset,encoding,length,text_preview\r\n";
        scalarsFile << "sample,target_id,target_name,address,offset,kind,value\r\n";
        pointersFile << "sample,target_id,target_name,address,offset,value,readable,object_candidate,region_base,region_size,protection\r\n";
        diffsFile << "target_id,target_name,sample_a,sample_b,bytes_compared,changed_bytes,changed_qwords,first_changed_offset,last_changed_offset\r\n";
        diagnostics << "MC V147 targeted structure probe\r\nread_only=yes\r\nunknown_functions_called=no\r\npage=" << page << "\r\n";

        ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "accounts", kExtractAccountsRva);
        ExtractorAttempt positions = BuildExtractorProbe(snapshot, "open_positions", kExtractOpenPositionsRva);
        const std::uintptr_t tabView = accounts.tabView ? accounts.tabView : positions.tabView;
        const std::uintptr_t tradeInfo = accounts.tradeInfo ? accounts.tradeInfo : positions.tradeInfo;

        std::vector<TargetedProbeTarget> targets;
        auto addDirect = [&](const std::string& name, const std::string& chain, std::uintptr_t address, std::size_t bytes)
        {
            std::uintptr_t end=0; MEMORY_BASIC_INFORMATION mbi{};
            if (address && QueryReadableSpan(address, end, mbi)) targets.push_back({name,chain,address,bytes});
        };

        addDirect("CATPTTabView", "CATPTTabView", tabView, 0x4000);
        addDirect("ITC_TradeInfo", "ITC_TradeInfo", tradeInfo, 0x4000);

        if (page == "accounts" || page == "all")
        {
            AddResolvedPointerTarget(targets, "accounts_presenter", "CATPTTabView+0x1A70 -> ptr", tabView + 0x1A70, 1024 * 1024);
            AddResolvedPointerTarget(targets, "tradeinfo_b18", "ITC_TradeInfo+0xB18 -> ptr", tradeInfo + 0xB18, 512 * 1024);
        }
        if (page == "open_positions" || page == "all")
        {
            std::uintptr_t root98=0, root88=0;
            if (SafeReadValue(reinterpret_cast<void*>(tradeInfo + 0x98), root98))
            {
                addDirect("positions_root_98", "ITC_TradeInfo+0x98 -> ptr", root98, 1024 * 1024);
                AddResolvedPointerTarget(targets, "positions_symbols_950", "ITC_TradeInfo+0x98 -> ptr; +0x950 -> ptr", root98 + 0x950, 1024 * 1024);
                AddResolvedPointerTarget(targets, "positions_records_10E0", "ITC_TradeInfo+0x98 -> ptr; +0x10E0 -> ptr", root98 + 0x10E0, 1024 * 1024);
            }
            if (SafeReadValue(reinterpret_cast<void*>(tradeInfo + 0x88), root88))
            {
                addDirect("positions_root_88", "ITC_TradeInfo+0x88 -> ptr", root88, 512 * 1024);
                AddResolvedPointerTarget(targets, "positions_numeric_1070", "ITC_TradeInfo+0x88 -> ptr; +0x1070 -> ptr", root88 + 0x1070, 1024 * 1024);
            }
        }
        if (page == "logs" || page == "all")
        {
            std::uintptr_t a=0,b=0;
            if (SafeReadValue(reinterpret_cast<void*>(tabView + 0xC40), a))
            {
                addDirect("logs_root_c40", "CATPTTabView+0xC40 -> ptr", a, 512 * 1024);
                if (SafeReadValue(reinterpret_cast<void*>(a + 0xA40), b))
                {
                    addDirect("logs_second_a40", "CATPTTabView+0xC40 -> ptr; +0xA40 -> ptr", b, 512 * 1024);
                    AddResolvedPointerTarget(targets, "logs_candidate_e80", "CATPTTabView+0xC40 -> ptr; +0xA40 -> ptr; +0xE80 -> ptr", b + 0xE80, 1024 * 1024);
                }
            }
        }

        constexpr int kSamples = 3;
        constexpr DWORD kSampleDelayMs = 2000;
        constexpr std::size_t kHardTargetLimit = 1024 * 1024;
        std::vector<std::vector<std::vector<unsigned char>>> captures(kSamples);
        captures.assign(kSamples, std::vector<std::vector<unsigned char>>(targets.size()));
        std::uint64_t binaryOffset = 0;
        std::size_t successfulReads = 0;

        for (int sample=0; sample<kSamples; ++sample)
        {
            if (sample) Sleep(kSampleDelayMs);
            for (std::size_t tid=0; tid<targets.size(); ++tid)
            {
                const auto& target = targets[tid];
                std::uintptr_t regionEnd=0; MEMORY_BASIC_INFORMATION mbi{};
                bool queryOk = QueryReadableSpan(target.address, regionEnd, mbi);
                std::size_t dumpSize = 0;
                if (queryOk)
                    dumpSize = std::min<std::size_t>(std::min<std::size_t>(target.requestedBytes, kHardTargetLimit), static_cast<std::size_t>(regionEnd-target.address));
                auto& bytes = captures[sample][tid];
                bytes.assign(dumpSize, 0);
                const bool readOk = dumpSize && SafeReadBytes(reinterpret_cast<void*>(target.address), bytes.data(), dumpSize);
                if (readOk) ++successfulReads;
                const std::uintptr_t regionBase = queryOk ? reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) : 0;
                targetsFile << sample << ',' << tid << ",\"" << target.name << "\",\"" << target.chain << "\"," << HexValue(target.address) << ','
                            << HexValue(regionBase) << ',' << (queryOk ? mbi.RegionSize : 0) << ',' << (queryOk ? ProtectionName(mbi.Protect) : "unknown") << ','
                            << binaryOffset << ',' << dumpSize << ',' << (readOk ? "yes" : "no") << "\r\n";
                if (!readOk) continue;
                memory.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                binaryOffset += bytes.size();

                // Strings: preserve exact offsets and capture longer previews than V147.
                for (std::size_t i=0; i+5<bytes.size(); ++i)
                {
                    if (bytes[i] >= 32 && bytes[i] <= 126)
                    {
                        std::size_t j=i;
                        while (j<bytes.size() && bytes[j]>=32 && bytes[j]<=126 && j-i<512) ++j;
                        if (j-i>=5)
                        {
                            std::string text(reinterpret_cast<const char*>(bytes.data()+i), j-i);
                            for (char& c:text) if (c=='\"') c='\'';
                            stringsFile << sample << ',' << tid << ",\"" << target.name << "\"," << HexValue(target.address) << ',' << HexValue(i)
                                        << ",ascii," << (j-i) << ",\"" << text << "\"\r\n";
                            i=j;
                        }
                    }
                }
                for (std::size_t i=0; i+10<bytes.size(); i+=2)
                {
                    std::size_t j=i; std::string text;
                    while (j+1<bytes.size() && bytes[j]>=32 && bytes[j]<=126 && bytes[j+1]==0 && text.size()<512)
                    { text.push_back(static_cast<char>(bytes[j])); j+=2; }
                    if (text.size()>=5)
                    {
                        for (char& c:text) if (c=='\"') c='\'';
                        stringsFile << sample << ',' << tid << ",\"" << target.name << "\"," << HexValue(target.address) << ',' << HexValue(i)
                                    << ",utf16le," << text.size() << ",\"" << text << "\"\r\n";
                        i=j;
                    }
                }

                // Qword pointers and candidate object links.
                for (std::size_t off=0; off+8<=bytes.size(); off+=8)
                {
                    std::uintptr_t value=0; std::memcpy(&value, bytes.data()+off, 8);
                    std::uintptr_t pEnd=0; MEMORY_BASIC_INFORMATION pMbi{};
                    const bool readable = QueryReadableSpan(value, pEnd, pMbi);
                    if (readable)
                        pointersFile << sample << ',' << tid << ",\"" << target.name << "\"," << HexValue(target.address) << ',' << HexValue(off) << ',' << HexValue(value)
                                     << ",yes," << (PointerLooksLikeObject(value)?"yes":"no") << ',' << HexValue(reinterpret_cast<std::uintptr_t>(pMbi.BaseAddress)) << ','
                                     << pMbi.RegionSize << ',' << ProtectionName(pMbi.Protect) << "\r\n";
                }

                // Numeric candidates: aligned integers, float and double. Keep useful finite trading-scale values.
                for (std::size_t off=0; off+8<=bytes.size(); off+=4)
                {
                    std::int32_t i32=0; float f=0.0f; double d=0.0;
                    std::memcpy(&i32, bytes.data()+off, 4);
                    std::memcpy(&f, bytes.data()+off, 4);
                    if (i32 != 0 && i32 > -1000000000 && i32 < 1000000000)
                        scalarsFile << sample << ',' << tid << ",\"" << target.name << "\"," << HexValue(target.address) << ',' << HexValue(off) << ",int32," << i32 << "\r\n";
                    if (std::isfinite(f) && std::fabs(f)>=0.0001f && std::fabs(f)<=1.0e9f)
                        scalarsFile << sample << ',' << tid << ",\"" << target.name << "\"," << HexValue(target.address) << ',' << HexValue(off) << ",float," << std::setprecision(9) << f << "\r\n";
                    if ((off%8)==0)
                    {
                        std::memcpy(&d, bytes.data()+off, 8);
                        if (std::isfinite(d) && std::fabs(d)>=0.000001 && std::fabs(d)<=1.0e15)
                            scalarsFile << sample << ',' << tid << ",\"" << target.name << "\"," << HexValue(target.address) << ',' << HexValue(off) << ",double," << std::setprecision(17) << d << "\r\n";
                    }
                }
            }
            targetsFile.flush(); stringsFile.flush(); scalarsFile.flush(); pointersFile.flush(); memory.flush();
        }

        for (std::size_t tid=0; tid<targets.size(); ++tid)
        {
            for (int b=1; b<kSamples; ++b)
            {
                const auto& aBytes=captures[0][tid]; const auto& bBytes=captures[b][tid];
                const std::size_t n=std::min(aBytes.size(), bBytes.size());
                std::size_t changedBytes=0, changedQwords=0, first=n, last=0;
                for (std::size_t i=0;i<n;++i) if (aBytes[i]!=bBytes[i]) { ++changedBytes; if (first==n) first=i; last=i; }
                for (std::size_t i=0;i+8<=n;i+=8) if (std::memcmp(aBytes.data()+i,bBytes.data()+i,8)!=0) ++changedQwords;
                diffsFile << tid << ",\"" << targets[tid].name << "\",0," << b << ',' << n << ',' << changedBytes << ',' << changedQwords << ',';
                if (first==n) diffsFile << "none,none\r\n"; else diffsFile << HexValue(first) << ',' << HexValue(last) << "\r\n";
            }
        }

        std::ostringstream manifest;
        manifest << "{\r\n"
                 << "  \"capture_version\": 146,\r\n"
                 << "  \"capture_type\": \"targeted_structure_probe\",\r\n"
                 << "  \"page\": " << JsonString(page) << ",\r\n"
                 << "  \"read_only\": true,\r\n"
                 << "  \"unknown_functions_called\": false,\r\n"
                 << "  \"process_id\": " << snapshot.processId << ",\r\n"
                 << "  \"CATPTTabView\": " << JsonString(HexValue(tabView)) << ",\r\n"
                 << "  \"ITC_TradeInfo\": " << JsonString(HexValue(tradeInfo)) << ",\r\n"
                 << "  \"target_count\": " << targets.size() << ",\r\n"
                 << "  \"samples\": 3,\r\n"
                 << "  \"sample_delay_ms\": 2000,\r\n"
                 << "  \"successful_reads\": " << successfulReads << ",\r\n"
                 << "  \"memory_bytes\": " << binaryOffset << ",\r\n"
                 << "  \"files\": {\"targets\":" << JsonString(targetsPath) << ",\"strings\":" << JsonString(stringsPath)
                 << ",\"scalars\":" << JsonString(scalarsPath) << ",\"pointers\":" << JsonString(pointersPath)
                 << ",\"diffs\":" << JsonString(diffsPath) << ",\"memory\":" << JsonString(memoryPath)
                 << ",\"diagnostics\":" << JsonString(diagnosticsPath) << "}\r\n"
                 << "}\r\n";
        const bool manifestOk=WriteUtf8File(manifestPath, manifest.str());
        diagnostics << "target_count=" << targets.size() << "\r\nsuccessful_reads=" << successfulReads << "\r\nmemory_bytes=" << binaryOffset << "\r\n";
        std::ostringstream summary;
        summary << "{\"capture\":\"targeted_structure_probe\",\"version\":146,\"page\":" << JsonString(page)
                << ",\"target_count\":" << targets.size() << ",\"samples\":3,\"successful_reads\":" << successfulReads
                << ",\"memory_bytes\":" << binaryOffset << ",\"manifest_written\":" << (manifestOk?"true":"false")
                << ",\"manifest_path\":" << JsonString(manifestPath) << '}';
        summaryJson=summary.str();
        return manifestOk && successfulReads>0;
    }

    bool WriteResearchCaptureBundle(
        const Snapshot& snapshot,
        std::string& summaryJson)
    {
        CreateDirectoryW(kOutputDirectory, nullptr);
        std::vector<std::wstring> snapshotPaths;
        const bool snapshotOk = WriteSnapshotReports(snapshot, snapshotPaths);

        ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "accounts", kExtractAccountsRva);
        ExtractorAttempt positions = BuildExtractorProbe(snapshot, "open_positions", kExtractOpenPositionsRva);

        std::wstring contractPath;
        const bool contractOk = WriteContractAnalysisReport(snapshot, accounts, positions, contractPath);

        CallerXrefAnalysis callerAnalysis = BuildCallerXrefAnalysis(snapshot, accounts, positions);
        std::wstring callerPath;
        const bool callerOk = WriteCallerXrefAnalysisReport(
            snapshot, accounts, positions, callerAnalysis, callerPath);

        std::wstring anchorsPath;
        std::string anchorsSummary;
        const bool anchorsOk = WriteThreeTabAnchorReport(snapshot, anchorsPath, anchorsSummary);

        std::wstring pageMethodsPath;
        std::string pageMethodsSummary;
        const bool pageMethodsOk = WritePageVirtualMethodExplorerReport(
            snapshot, pageMethodsPath, pageMethodsSummary);

        std::wstring slot17Path;
        std::wstring slot17CsvPath;
        std::string slot17Summary;
        const bool slot17Ok = WriteSlot17DeepAnalysisReport(
            snapshot, slot17Path, slot17CsvPath, slot17Summary);

        std::wstring imagePath, regionPath, manifestPath;
        std::string imageDiagnostic;
        const bool imageOk = WriteLoadedModuleMemoryImage(
            snapshot, imagePath, regionPath, manifestPath, imageDiagnostic);

        const bool allOk = snapshotOk && contractOk && callerOk && anchorsOk &&
            pageMethodsOk && slot17Ok && imageOk;
        std::ostringstream json;
        json << "{\"capture\":\"research_bundle\",\"version\":143,"
             << "\"read_only\":true,\"live_function_calls\":false,"
             << "\"snapshot_ok\":" << (snapshotOk ? "true" : "false") << ','
             << "\"contract_ok\":" << (contractOk ? "true" : "false") << ','
             << "\"callers_ok\":" << (callerOk ? "true" : "false") << ','
             << "\"anchors_ok\":" << (anchorsOk ? "true" : "false") << ','
             << "\"page_methods_ok\":" << (pageMethodsOk ? "true" : "false") << ','
             << "\"slot17_ok\":" << (slot17Ok ? "true" : "false") << ','
             << "\"loaded_image_ok\":" << (imageOk ? "true" : "false") << ','
             << "\"image_diagnostic\":" << JsonString(imageDiagnostic) << ','
             << "\"manifest_path\":" << JsonString(manifestPath) << ','
             << "\"loaded_image_path\":" << JsonString(imagePath) << ','
             << "\"region_map_path\":" << JsonString(regionPath) << ','
             << "\"contract_path\":" << JsonString(contractPath) << ','
             << "\"caller_path\":" << JsonString(callerPath) << ','
             << "\"anchors_path\":" << JsonString(anchorsPath) << ','
             << "\"page_methods_path\":" << JsonString(pageMethodsPath) << ','
             << "\"slot17_path\":" << JsonString(slot17Path) << ','
             << "\"slot17_csv_path\":" << JsonString(slot17CsvPath) << "}";
        summaryJson = json.str();
        return allOk;
    }


    struct PageProbeMember
    {
        std::size_t memberOffset = 0;
        std::uintptr_t address = 0;
        std::uintptr_t vtable = 0;
        std::size_t readableBytes = 0;
        bool objectCandidate = false;
        std::string moduleName;
        std::uintptr_t vtableRva = 0;
    };

    std::string ModuleForAddress(const Snapshot& snapshot, std::uintptr_t address, std::uintptr_t& rva)
    {
        rva = 0;
        for (const auto& module : snapshot.modules)
        {
            if (address >= module.base && address < module.base + module.size)
            {
                rva = address - module.base;
                return WideToUtf8(module.name);
            }
        }
        return {};
    }


    std::string CsvEscapeWide(const std::wstring& value)
    {
        std::string utf8 = WideToUtf8(value);
        bool quote = utf8.find_first_of(",\r\n") != std::string::npos;
        std::string escaped;
        escaped.reserve(utf8.size() + 8);
        for (char ch : utf8)
        {
            if (ch == '"') escaped += "\"\"";
            else escaped += ch;
        }
        return quote ? ("\"" + escaped + "\"") : escaped;
    }

    bool WriteFlexGridTextReader(
        const Snapshot& snapshot,
        std::string& summaryJson,
        const char* requestedPage)
    {
        constexpr std::uintptr_t kFlexGridVtableRva = 0x1FD778;
        constexpr std::uintptr_t kGetTextRva = 0x135E40;
        constexpr std::size_t kGetTextSlot = 60;
        constexpr std::size_t kGridMemberOffset = 0x118;
        constexpr std::size_t kRowsOffset1 = 0xD20;
        constexpr std::size_t kRowsOffset2 = 0xD24;
        constexpr unsigned int kMaximumRows = 100;
        constexpr unsigned int kMaximumColumns = 32;
        constexpr unsigned int kCellBufferCharacters = 2048;

        CreateDirectoryW(kOutputDirectory, nullptr);
        const std::string page = (requestedPage && *requestedPage) ? requestedPage : "unspecified";
        std::wstring pageWide(page.begin(), page.end());
        const std::wstring prefix = L"MC_V150_" + pageWide + L"_FlexGrid_Text_Reader_";
        const std::wstring cellsPath = ReportPath((prefix + L"Cells").c_str(), snapshot.processId);
        const std::wstring matrixPath = ReportPath((prefix + L"Matrix").c_str(), snapshot.processId);
        const std::wstring diagnosticsPath = ReportPath((prefix + L"Diagnostics").c_str(), snapshot.processId);
        const std::wstring manifestPath = ReportPath((prefix + L"Manifest").c_str(), snapshot.processId);
        std::ofstream cells(cellsPath, std::ios::binary | std::ios::trunc);
        std::ofstream matrix(matrixPath, std::ios::binary | std::ios::trunc);
        std::ofstream diagnostics(diagnosticsPath, std::ios::binary | std::ios::trunc);
        if (!cells || !matrix || !diagnostics)
        {
            summaryJson = "{\"capture\":\"flexgrid_text_reader\",\"version\":150,\"error\":\"could_not_create_output_files\"}";
            return false;
        }

        diagnostics << "MC V150 FlexGrid Text Reader\r\n"
                    << "page=" << page << "\r\n"
                    << "controlled_internal_call=yes\r\n"
                    << "function=CFlexGridImpl::GetText\r\n"
                    << "grid_member_offset=0x118\r\n"
                    << "expected_vtable_rva=0x1FD778\r\n"
                    << "expected_GetText_rva=0x135E40\r\n"
                    << "GetText_slot=60\r\n";
        cells << "page,row,column,call_ok,seh_code,text\r\n";

        ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "accounts", kExtractAccountsRva);
        ExtractorAttempt positions = BuildExtractorProbe(snapshot, "open_positions", kExtractOpenPositionsRva);
        const std::uintptr_t tabView = accounts.tabView ? accounts.tabView : positions.tabView;
        if (!tabView || !snapshot.atonpTrackerModule)
        {
            summaryJson = "{\"capture\":\"flexgrid_text_reader\",\"version\":150,\"error\":\"required_anchor_not_found\"}";
            return false;
        }
        const std::uintptr_t moduleBase = reinterpret_cast<std::uintptr_t>(snapshot.atonpTrackerModule);
        const std::uintptr_t expectedVtable = moduleBase + kFlexGridVtableRva;
        const std::uintptr_t expectedGetText = moduleBase + kGetTextRva;

        struct PageRoot { const char* name; std::size_t offset; };
        std::vector<PageRoot> roots;
        if (page == "accounts" || page == "all") roots.push_back({"accounts", 0x58});
        if (page == "open_positions" || page == "all") roots.push_back({"open_positions", 0x68});
        if (page == "logs" || page == "all") roots.push_back({"logs", 0x80});
        if (roots.empty())
        {
            summaryJson = "{\"capture\":\"flexgrid_text_reader\",\"version\":150,\"error\":\"invalid_page\"}";
            return false;
        }

        std::size_t pagesSucceeded = 0, callsAttempted = 0, callsSucceeded = 0, nonEmptyCells = 0, sehFailures = 0;
        for (const auto& root : roots)
        {
            std::uintptr_t pageObject = 0, gridObject = 0, actualVtable = 0, actualGetText = 0;
            std::uint32_t rows1 = 0, rows2 = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(tabView + root.offset), pageObject) || !pageObject ||
                !SafeReadValue(reinterpret_cast<void*>(pageObject + kGridMemberOffset), gridObject) || !gridObject ||
                !SafeReadValue(reinterpret_cast<void*>(gridObject), actualVtable) ||
                !SafeReadValue(reinterpret_cast<void*>(actualVtable + kGetTextSlot * sizeof(std::uintptr_t)), actualGetText) ||
                !SafeReadValue(reinterpret_cast<void*>(gridObject + kRowsOffset1), rows1) ||
                !SafeReadValue(reinterpret_cast<void*>(gridObject + kRowsOffset2), rows2))
            {
                diagnostics << "page=" << root.name << " anchor_read_failed\r\n";
                continue;
            }

            diagnostics << "page=" << root.name
                        << " page_object=" << HexValue(pageObject)
                        << " grid_object=" << HexValue(gridObject)
                        << " actual_vtable=" << HexValue(actualVtable)
                        << " expected_vtable=" << HexValue(expectedVtable)
                        << " slot60=" << HexValue(actualGetText)
                        << " expected_GetText=" << HexValue(expectedGetText)
                        << " rows1=" << rows1 << " rows2=" << rows2 << "\r\n";

            const bool identityOk = actualVtable == expectedVtable && actualGetText == expectedGetText;
            const bool rowsOk = rows1 == rows2 && rows1 <= 100000;
            if (!identityOk || !rowsOk)
            {
                diagnostics << "page=" << root.name << " validation_failed identity_ok=" << (identityOk?"yes":"no")
                            << " rows_ok=" << (rowsOk?"yes":"no") << "\r\n";
                continue;
            }

            const unsigned int rowsToRead = std::min<unsigned int>(rows1, kMaximumRows);
            matrix << "PAGE," << root.name << ",reported_rows," << rows1 << "\r\n";
            matrix << "row";
            for (unsigned int column=0; column<kMaximumColumns; ++column) matrix << ",col_" << column;
            matrix << "\r\n";

            for (unsigned int row=0; row<rowsToRead; ++row)
            {
                matrix << row;
                for (unsigned int column=0; column<kMaximumColumns; ++column)
                {
                    wchar_t buffer[kCellBufferCharacters]{};
                    DWORD sehCode = 0;
                    ++callsAttempted;
                    const int callOk = MCBridge_CallFlexGridGetTextWithSeh(
                        reinterpret_cast<void*>(gridObject), reinterpret_cast<void*>(actualGetText),
                        row, column, buffer, kCellBufferCharacters, &sehCode);
                    if (callOk) ++callsSucceeded; else { ++sehFailures; }
                    std::wstring text(buffer);
                    if (!text.empty()) ++nonEmptyCells;
                    cells << root.name << ',' << row << ',' << column << ',' << (callOk?"yes":"no")
                          << ",0x" << std::hex << std::uppercase << sehCode << std::dec << ',' << CsvEscapeWide(text) << "\r\n";
                    matrix << ',' << CsvEscapeWide(text);
                    if (!callOk)
                    {
                        diagnostics << "page=" << root.name << " row=" << row << " column=" << column
                                    << " GetText_SEH=0x" << std::hex << std::uppercase << sehCode << std::dec << "\r\n";
                    }
                }
                matrix << "\r\n";
            }
            matrix << "\r\n";
            ++pagesSucceeded;
        }

        std::ostringstream manifest;
        manifest << "{\r\n"
                 << "  \"capture_version\": 150,\r\n"
                 << "  \"capture_type\": \"flexgrid_text_reader\",\r\n"
                 << "  \"page\": " << JsonString(page) << ",\r\n"
                 << "  \"controlled_internal_call\": true,\r\n"
                 << "  \"GetText_rva\": \"0x135E40\",\r\n"
                 << "  \"GetText_slot\": 60,\r\n"
                 << "  \"pages_succeeded\": " << pagesSucceeded << ",\r\n"
                 << "  \"calls_attempted\": " << callsAttempted << ",\r\n"
                 << "  \"calls_succeeded\": " << callsSucceeded << ",\r\n"
                 << "  \"non_empty_cells\": " << nonEmptyCells << ",\r\n"
                 << "  \"seh_failures\": " << sehFailures << ",\r\n"
                 << "  \"files\": {\"cells\":" << JsonString(cellsPath)
                 << ",\"matrix\":" << JsonString(matrixPath)
                 << ",\"diagnostics\":" << JsonString(diagnosticsPath) << "}\r\n"
                 << "}\r\n";
        const bool manifestOk = WriteUtf8File(manifestPath, manifest.str());
        std::ostringstream summary;
        summary << "{\"capture\":\"flexgrid_text_reader\",\"version\":150,\"page\":" << JsonString(page)
                << ",\"pages_succeeded\":" << pagesSucceeded << ",\"calls_attempted\":" << callsAttempted
                << ",\"calls_succeeded\":" << callsSucceeded << ",\"non_empty_cells\":" << nonEmptyCells
                << ",\"seh_failures\":" << sehFailures << ",\"manifest_written\":" << (manifestOk?"true":"false")
                << ",\"manifest_path\":" << JsonString(manifestPath) << '}';
        summaryJson = summary.str();
        return manifestOk && pagesSucceeded > 0 && callsSucceeded > 0;
    }

    bool WriteV151SingleCellProbe(
        const Snapshot& snapshot,
        std::string& summaryJson,
        const char* requestedPage)
    {
        constexpr std::uintptr_t kFlexGridVtableRva = 0x1FD778;
        constexpr std::uintptr_t kGetTextRva = 0x135E40;
        constexpr std::size_t kGetTextSlot = 60;
        constexpr std::size_t kGridMemberOffset = 0x118;
        constexpr std::size_t kRowsOffset1 = 0xD20;
        constexpr std::size_t kRowsOffset2 = 0xD24;
        constexpr unsigned int kBufferCharacters = 2048;

        CreateDirectoryW(kOutputDirectory, nullptr);
        const std::string page = (requestedPage && *requestedPage) ? requestedPage : "unspecified";
        std::wstring pageWide(page.begin(), page.end());
        const std::wstring prefix = L"MC_V151_" + pageWide + L"_Single_Cell_GetText_";
        const std::wstring diagnosticsPath = ReportPath((prefix + L"Diagnostics").c_str(), snapshot.processId);
        const std::wstring manifestPath = ReportPath((prefix + L"Manifest").c_str(), snapshot.processId);
        std::ofstream diagnostics(diagnosticsPath, std::ios::binary | std::ios::trunc);
        if (!diagnostics)
        {
            summaryJson = "{\"capture\":\"v151_single_cell_gettext\",\"version\":151,\"error\":\"could_not_create_diagnostics\"}";
            return false;
        }

        diagnostics << "MC V151 Single Cell GetText Probe\r\n"
                    << "page=" << page << "\r\n"
                    << "scope=exactly one GetText call per requested page\r\n"
                    << "cell=row_0,column_0\r\n"
                    << "grid_member_offset=0x118\r\n"
                    << "expected_vtable_rva=0x1FD778\r\n"
                    << "expected_GetText_rva=0x135E40\r\n"
                    << "GetText_slot=60\r\n";

        ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "accounts", kExtractAccountsRva);
        ExtractorAttempt positions = BuildExtractorProbe(snapshot, "open_positions", kExtractOpenPositionsRva);
        const std::uintptr_t tabView = accounts.tabView ? accounts.tabView : positions.tabView;
        if (!tabView || !snapshot.atonpTrackerModule)
        {
            diagnostics << "result=required_anchor_not_found\r\n";
            summaryJson = "{\"capture\":\"v151_single_cell_gettext\",\"version\":151,\"error\":\"required_anchor_not_found\"}";
            return false;
        }

        const std::uintptr_t moduleBase = reinterpret_cast<std::uintptr_t>(snapshot.atonpTrackerModule);
        const std::uintptr_t expectedVtable = moduleBase + kFlexGridVtableRva;
        const std::uintptr_t expectedGetText = moduleBase + kGetTextRva;
        struct PageRoot { const char* name; std::size_t offset; };
        std::vector<PageRoot> roots;
        if (page == "accounts" || page == "all") roots.push_back({"accounts", 0x58});
        if (page == "open_positions" || page == "all") roots.push_back({"open_positions", 0x68});
        if (page == "logs" || page == "all") roots.push_back({"logs", 0x80});
        if (roots.empty())
        {
            diagnostics << "result=invalid_page\r\n";
            summaryJson = "{\"capture\":\"v151_single_cell_gettext\",\"version\":151,\"error\":\"invalid_page\"}";
            return false;
        }

        std::size_t pagesValidated = 0, callsAttempted = 0, callsSucceeded = 0, nonEmpty = 0, sehFailures = 0;
        for (const auto& root : roots)
        {
            std::uintptr_t pageObject = 0, gridObject = 0, actualVtable = 0, actualGetText = 0;
            std::uint32_t rows1 = 0, rows2 = 0;
            const bool anchorsOk =
                SafeReadValue(reinterpret_cast<void*>(tabView + root.offset), pageObject) && pageObject &&
                SafeReadValue(reinterpret_cast<void*>(pageObject + kGridMemberOffset), gridObject) && gridObject &&
                SafeReadValue(reinterpret_cast<void*>(gridObject), actualVtable) &&
                SafeReadValue(reinterpret_cast<void*>(actualVtable + kGetTextSlot * sizeof(std::uintptr_t)), actualGetText) &&
                SafeReadValue(reinterpret_cast<void*>(gridObject + kRowsOffset1), rows1) &&
                SafeReadValue(reinterpret_cast<void*>(gridObject + kRowsOffset2), rows2);

            diagnostics << "page=" << root.name << "\r\n"
                        << "page_object=" << HexValue(pageObject) << "\r\n"
                        << "grid_object=" << HexValue(gridObject) << "\r\n"
                        << "actual_vtable=" << HexValue(actualVtable) << "\r\n"
                        << "expected_vtable=" << HexValue(expectedVtable) << "\r\n"
                        << "actual_slot60=" << HexValue(actualGetText) << "\r\n"
                        << "expected_GetText=" << HexValue(expectedGetText) << "\r\n"
                        << "rows1=" << rows1 << "\r\n"
                        << "rows2=" << rows2 << "\r\n";

            if (!anchorsOk)
            {
                diagnostics << "validation=anchor_read_failed\r\n\r\n";
                continue;
            }
            const bool identityOk = actualVtable == expectedVtable && actualGetText == expectedGetText;
            const bool rowsOk = rows1 == rows2 && rows1 > 0 && rows1 <= 100000;
            diagnostics << "identity_ok=" << (identityOk ? "yes" : "no") << "\r\n"
                        << "rows_ok=" << (rowsOk ? "yes" : "no") << "\r\n";
            if (!identityOk || !rowsOk)
            {
                diagnostics << "call_attempted=no\r\n\r\n";
                continue;
            }

            ++pagesValidated;
            wchar_t buffer[kBufferCharacters]{};
            DWORD sehCode = 0;
            ++callsAttempted;
            const int callOk = MCBridge_CallFlexGridGetTextWithSeh(
                reinterpret_cast<void*>(gridObject), reinterpret_cast<void*>(actualGetText),
                0, 0, buffer, kBufferCharacters, &sehCode);
            if (callOk) ++callsSucceeded; else { ++sehFailures; }
            const std::wstring text(buffer);
            if (!text.empty()) ++nonEmpty;
            diagnostics << "call_attempted=yes\r\n"
                        << "call_ok=" << (callOk ? "yes" : "no") << "\r\n"
                        << "seh_code=0x" << std::hex << std::uppercase << sehCode << std::dec << "\r\n"
                        << "text_utf8=" << WideToUtf8(text) << "\r\n\r\n";
        }

        std::ostringstream manifest;
        manifest << "{\r\n"
                 << "  \"capture_version\": 151,\r\n"
                 << "  \"capture_type\": \"single_cell_gettext\",\r\n"
                 << "  \"page\": " << JsonString(page) << ",\r\n"
                 << "  \"row\": 0,\r\n"
                 << "  \"column\": 0,\r\n"
                 << "  \"pages_validated\": " << pagesValidated << ",\r\n"
                 << "  \"calls_attempted\": " << callsAttempted << ",\r\n"
                 << "  \"calls_succeeded\": " << callsSucceeded << ",\r\n"
                 << "  \"non_empty_results\": " << nonEmpty << ",\r\n"
                 << "  \"seh_failures\": " << sehFailures << ",\r\n"
                 << "  \"diagnostics\": " << JsonString(diagnosticsPath) << "\r\n"
                 << "}\r\n";
        const bool manifestOk = WriteUtf8File(manifestPath, manifest.str());
        std::ostringstream summary;
        summary << "{\"capture\":\"v151_single_cell_gettext\",\"version\":151,\"page\":" << JsonString(page)
                << ",\"pages_validated\":" << pagesValidated << ",\"calls_attempted\":" << callsAttempted
                << ",\"calls_succeeded\":" << callsSucceeded << ",\"non_empty_results\":" << nonEmpty
                << ",\"seh_failures\":" << sehFailures << ",\"manifest_written\":" << (manifestOk ? "true" : "false")
                << ",\"manifest_path\":" << JsonString(manifestPath) << '}';
        summaryJson = summary.str();
        return manifestOk && pagesValidated > 0 && callsSucceeded > 0;
    }

    bool WriteV152CoordinateMap(
        const Snapshot& snapshot,
        std::string& summaryJson,
        const char* requestedPage)
    {
        constexpr std::uintptr_t kFlexGridVtableRva = 0x1FD778;
        constexpr std::uintptr_t kGetTextRva = 0x135E40;
        constexpr std::size_t kGetTextSlot = 60;
        constexpr std::size_t kGridMemberOffset = 0x118;
        constexpr std::size_t kRowsOffset1 = 0xD20;
        constexpr std::size_t kRowsOffset2 = 0xD24;
        constexpr unsigned int kRowsToProbe = 10;
        constexpr unsigned int kColumnsToProbe = 16;
        constexpr unsigned int kBufferCharacters = 2048;

        CreateDirectoryW(kOutputDirectory, nullptr);
        const std::string page = (requestedPage && *requestedPage) ? requestedPage : "unspecified";
        std::wstring pageWide(page.begin(), page.end());
        const std::wstring prefix = L"MC_V152_" + pageWide + L"_Coordinate_Map_";
        const std::wstring cellsPath = ReportPath((prefix + L"Cells").c_str(), snapshot.processId);
        const std::wstring matrixPath = ReportPath((prefix + L"Matrix").c_str(), snapshot.processId);
        const std::wstring diagnosticsPath = ReportPath((prefix + L"Diagnostics").c_str(), snapshot.processId);
        const std::wstring manifestPath = ReportPath((prefix + L"Manifest").c_str(), snapshot.processId);
        std::ofstream cells(cellsPath, std::ios::binary | std::ios::trunc);
        std::ofstream matrix(matrixPath, std::ios::binary | std::ios::trunc);
        std::ofstream diagnostics(diagnosticsPath, std::ios::binary | std::ios::trunc);
        if (!cells || !matrix || !diagnostics)
        {
            summaryJson = "{\"capture\":\"v152_coordinate_map\",\"version\":152,\"error\":\"could_not_create_output_files\"}";
            return false;
        }

        diagnostics << "MC V152 Guarded FlexGrid Coordinate Map\r\n"
                    << "page=" << page << "\r\n"
                    << "rows_probed=0..9\r\ncolumns_probed=0..15\r\n"
                    << "grid_member_offset=0x118\r\n"
                    << "expected_vtable_rva=0x1FD778\r\n"
                    << "expected_GetText_rva=0x135E40\r\n"
                    << "GetText_slot=60\r\n";
        cells << "page,row,column,call_ok,seh_code,text_length,text\r\n";

        ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "accounts", kExtractAccountsRva);
        ExtractorAttempt positions = BuildExtractorProbe(snapshot, "open_positions", kExtractOpenPositionsRva);
        const std::uintptr_t tabView = accounts.tabView ? accounts.tabView : positions.tabView;
        if (!tabView || !snapshot.atonpTrackerModule)
        {
            summaryJson = "{\"capture\":\"v152_coordinate_map\",\"version\":152,\"error\":\"required_anchor_not_found\"}";
            return false;
        }

        const std::uintptr_t moduleBase = reinterpret_cast<std::uintptr_t>(snapshot.atonpTrackerModule);
        const std::uintptr_t expectedVtable = moduleBase + kFlexGridVtableRva;
        const std::uintptr_t expectedGetText = moduleBase + kGetTextRva;
        struct PageRoot { const char* name; std::size_t offset; };
        std::vector<PageRoot> roots;
        if (page == "accounts" || page == "all") roots.push_back({"accounts", 0x58});
        if (page == "open_positions" || page == "all") roots.push_back({"open_positions", 0x68});
        if (page == "logs" || page == "all") roots.push_back({"logs", 0x80});
        if (roots.empty())
        {
            summaryJson = "{\"capture\":\"v152_coordinate_map\",\"version\":152,\"error\":\"invalid_page\"}";
            return false;
        }

        std::size_t pagesValidated = 0, callsAttempted = 0, callsSucceeded = 0;
        std::size_t nonEmpty = 0, sehFailures = 0;
        for (const auto& root : roots)
        {
            std::uintptr_t pageObject = 0, gridObject = 0, actualVtable = 0, actualGetText = 0;
            std::uint32_t rows1 = 0, rows2 = 0;
            const bool anchorsOk =
                SafeReadValue(reinterpret_cast<void*>(tabView + root.offset), pageObject) && pageObject &&
                SafeReadValue(reinterpret_cast<void*>(pageObject + kGridMemberOffset), gridObject) && gridObject &&
                SafeReadValue(reinterpret_cast<void*>(gridObject), actualVtable) &&
                SafeReadValue(reinterpret_cast<void*>(actualVtable + kGetTextSlot * sizeof(std::uintptr_t)), actualGetText) &&
                SafeReadValue(reinterpret_cast<void*>(gridObject + kRowsOffset1), rows1) &&
                SafeReadValue(reinterpret_cast<void*>(gridObject + kRowsOffset2), rows2);

            diagnostics << "page=" << root.name << "\r\n"
                        << "page_object=" << HexValue(pageObject) << "\r\n"
                        << "grid_object=" << HexValue(gridObject) << "\r\n"
                        << "actual_vtable=" << HexValue(actualVtable) << "\r\n"
                        << "expected_vtable=" << HexValue(expectedVtable) << "\r\n"
                        << "actual_slot60=" << HexValue(actualGetText) << "\r\n"
                        << "expected_GetText=" << HexValue(expectedGetText) << "\r\n"
                        << "reported_rows_1=" << rows1 << "\r\n"
                        << "reported_rows_2=" << rows2 << "\r\n";

            const bool identityOk = anchorsOk && actualVtable == expectedVtable && actualGetText == expectedGetText;
            const bool rowsOk = anchorsOk && rows1 == rows2 && rows1 > 0 && rows1 <= 100000;
            diagnostics << "anchors_ok=" << (anchorsOk ? "yes" : "no") << "\r\n"
                        << "identity_ok=" << (identityOk ? "yes" : "no") << "\r\n"
                        << "rows_ok=" << (rowsOk ? "yes" : "no") << "\r\n";
            if (!identityOk || !rowsOk)
            {
                diagnostics << "probe_started=no\r\n\r\n";
                continue;
            }

            ++pagesValidated;
            matrix << "PAGE," << root.name << ",reported_rows," << rows1 << "\r\n";
            matrix << "row";
            for (unsigned int column = 0; column < kColumnsToProbe; ++column)
                matrix << ",col_" << column;
            matrix << "\r\n";

            std::size_t pageNonEmpty = 0, pageSeh = 0;
            for (unsigned int row = 0; row < kRowsToProbe; ++row)
            {
                matrix << row;
                for (unsigned int column = 0; column < kColumnsToProbe; ++column)
                {
                    wchar_t buffer[kBufferCharacters]{};
                    DWORD sehCode = 0;
                    ++callsAttempted;
                    const int callOk = MCBridge_CallFlexGridGetTextWithSeh(
                        reinterpret_cast<void*>(gridObject), reinterpret_cast<void*>(actualGetText),
                        row, column, buffer, kBufferCharacters, &sehCode);
                    if (callOk) ++callsSucceeded;
                    else { ++sehFailures; ++pageSeh; }
                    const std::wstring text(buffer);
                    if (!text.empty()) { ++nonEmpty; ++pageNonEmpty; }
                    cells << root.name << ',' << row << ',' << column << ',' << (callOk ? "yes" : "no")
                          << ",0x" << std::hex << std::uppercase << sehCode << std::dec
                          << ',' << text.size() << ',' << CsvEscapeWide(text) << "\r\n";
                    matrix << ',' << CsvEscapeWide(text);
                    if (!callOk)
                        diagnostics << "GetText_SEH row=" << row << " column=" << column
                                    << " code=0x" << std::hex << std::uppercase << sehCode << std::dec << "\r\n";
                }
                matrix << "\r\n";
            }
            matrix << "\r\n";
            diagnostics << "probe_started=yes\r\n"
                        << "calls_for_page=" << (kRowsToProbe * kColumnsToProbe) << "\r\n"
                        << "non_empty_for_page=" << pageNonEmpty << "\r\n"
                        << "seh_failures_for_page=" << pageSeh << "\r\n\r\n";
        }

        std::ostringstream manifest;
        manifest << "{\r\n"
                 << "  \"capture_version\": 152,\r\n"
                 << "  \"capture_type\": \"guarded_coordinate_map\",\r\n"
                 << "  \"page\": " << JsonString(page) << ",\r\n"
                 << "  \"row_range\": \"0..9\",\r\n"
                 << "  \"column_range\": \"0..15\",\r\n"
                 << "  \"pages_validated\": " << pagesValidated << ",\r\n"
                 << "  \"calls_attempted\": " << callsAttempted << ",\r\n"
                 << "  \"calls_succeeded\": " << callsSucceeded << ",\r\n"
                 << "  \"non_empty_results\": " << nonEmpty << ",\r\n"
                 << "  \"seh_failures\": " << sehFailures << ",\r\n"
                 << "  \"files\": {\"cells\":" << JsonString(cellsPath)
                 << ",\"matrix\":" << JsonString(matrixPath)
                 << ",\"diagnostics\":" << JsonString(diagnosticsPath) << "}\r\n"
                 << "}\r\n";
        const bool manifestOk = WriteUtf8File(manifestPath, manifest.str());
        std::ostringstream summary;
        summary << "{\"capture\":\"v152_coordinate_map\",\"version\":152,\"page\":" << JsonString(page)
                << ",\"pages_validated\":" << pagesValidated << ",\"calls_attempted\":" << callsAttempted
                << ",\"calls_succeeded\":" << callsSucceeded << ",\"non_empty_results\":" << nonEmpty
                << ",\"seh_failures\":" << sehFailures << ",\"manifest_written\":" << (manifestOk ? "true" : "false")
                << ",\"manifest_path\":" << JsonString(manifestPath) << '}';
        summaryJson = summary.str();
        return manifestOk && pagesValidated > 0 && callsSucceeded > 0;
    }

    bool WriteGridInterfaceLocator(
        const Snapshot& snapshot,
        std::string& summaryJson,
        const char* requestedPage)
    {
        CreateDirectoryW(kOutputDirectory, nullptr);
        const std::string page = (requestedPage && *requestedPage) ? requestedPage : "unspecified";
        std::wstring pageWide(page.begin(), page.end());
        const std::wstring prefix = L"MC_V149_" + pageWide + L"_Grid_Interface_Locator_";
        const std::wstring objectsPath = ReportPath((prefix + L"Objects").c_str(), snapshot.processId);
        const std::wstring vtablesPath = ReportPath((prefix + L"Vtables").c_str(), snapshot.processId);
        const std::wstring diagnosticsPath = ReportPath((prefix + L"Diagnostics").c_str(), snapshot.processId);
        const std::wstring manifestPath = ReportPath((prefix + L"Manifest").c_str(), snapshot.processId);
        std::ofstream objects(objectsPath, std::ios::binary | std::ios::trunc);
        std::ofstream vtables(vtablesPath, std::ios::binary | std::ios::trunc);
        std::ofstream diagnostics(diagnosticsPath, std::ios::binary | std::ios::trunc);
        if (!objects || !vtables || !diagnostics)
        {
            summaryJson = "{\"capture\":\"grid_interface_locator\",\"version\":149,\"error\":\"could_not_create_output_files\"}";
            return false;
        }
        diagnostics << "MC V149 Grid Interface Locator\r\n"
                    << "goal=locate the live grid object used by Accounts, Open Positions and Logs\r\n"
                    << "read_only=yes\r\nunknown_functions_called=no\r\nGetText_called=no\r\npage=" << page << "\r\n";
        objects << "page,depth,parent_offset,object,vtable,vtable_module,vtable_rva,allocation_base,region_size,protect,executable_slots,score\r\n";
        vtables << "page,object,vtable,slot,method,method_module,method_rva,executable\r\n";

        ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "accounts", kExtractAccountsRva);
        ExtractorAttempt positions = BuildExtractorProbe(snapshot, "open_positions", kExtractOpenPositionsRva);
        const std::uintptr_t tabView = accounts.tabView ? accounts.tabView : positions.tabView;
        if (!tabView)
        {
            summaryJson = "{\"capture\":\"grid_interface_locator\",\"version\":149,\"error\":\"CATPTTabView_not_found\"}";
            return false;
        }
        struct PageRoot { const char* name; std::size_t offset; };
        std::vector<PageRoot> roots;
        if (page == "accounts" || page == "all") roots.push_back({"accounts", 0x58});
        if (page == "open_positions" || page == "all") roots.push_back({"open_positions", 0x68});
        if (page == "logs" || page == "all") roots.push_back({"logs", 0x80});
        if (roots.empty())
        {
            summaryJson = "{\"capture\":\"grid_interface_locator\",\"version\":149,\"error\":\"invalid_page\"}";
            return false;
        }

        std::size_t objectCount = 0, vtableSlotCount = 0, highScoreCount = 0;
        std::set<std::uintptr_t> globalSeen;
        for (const auto& root : roots)
        {
            std::uintptr_t pageObject = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(tabView + root.offset), pageObject) || !pageObject)
            {
                diagnostics << "page=" << root.name << " page_pointer_read_failed\r\n";
                continue;
            }
            struct Work { std::uintptr_t object; int depth; std::size_t parentOffset; };
            std::vector<Work> queue{{pageObject,0,root.offset}};
            for (std::size_t qi=0; qi<queue.size() && qi<512; ++qi)
            {
                const Work work=queue[qi];
                if (!globalSeen.insert(work.object).second) continue;
                std::uintptr_t vtable=0;
                if (!SafeReadValue(reinterpret_cast<void*>(work.object), vtable)) continue;
                std::uintptr_t vtRva=0;
                const std::string vtModule=ModuleForAddress(snapshot,vtable,vtRva);
                if (vtModule.empty()) continue;
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(reinterpret_cast<void*>(work.object), &mbi, sizeof(mbi))) continue;
                std::size_t executableSlots=0;
                for (std::size_t slot=0; slot<64; ++slot)
                {
                    std::uintptr_t method=0;
                    if (!SafeReadValue(reinterpret_cast<void*>(vtable+slot*sizeof(void*)),method) || !method) break;
                    std::uintptr_t methodRva=0;
                    const std::string methodModule=ModuleForAddress(snapshot,method,methodRva);
                    const bool executable=MemoryRangeHasProtection(reinterpret_cast<void*>(method),1,true);
                    if (executable) ++executableSlots;
                    vtables << root.name << ',' << HexValue(work.object) << ',' << HexValue(vtable) << ',' << slot << ','
                            << HexValue(method) << ",\"" << methodModule << "\"," << HexValue(methodRva) << ',' << (executable?"yes":"no") << "\r\n";
                    ++vtableSlotCount;
                    if (!executable && slot>2) break;
                }
                int score=0;
                if (vtModule.find("ATOnPTracker")!=std::string::npos) score+=50;
                if (executableSlots>=8) score+=20;
                if (executableSlots>=20) score+=15;
                if (work.depth==1) score+=10;
                if (work.depth==2) score+=5;
                if (score>=70) ++highScoreCount;
                objects << root.name << ',' << work.depth << ',' << HexValue(work.parentOffset) << ',' << HexValue(work.object) << ','
                        << HexValue(vtable) << ",\"" << vtModule << "\"," << HexValue(vtRva) << ','
                        << HexValue(reinterpret_cast<std::uintptr_t>(mbi.AllocationBase)) << ',' << mbi.RegionSize << ',' << HexValue(mbi.Protect)
                        << ',' << executableSlots << ',' << score << "\r\n";
                ++objectCount;

                if (work.depth < 2)
                {
                    constexpr std::size_t kScanBytes=0x1000;
                    for (std::size_t off=sizeof(void*); off+sizeof(void*)<=kScanBytes; off+=sizeof(void*))
                    {
                        std::uintptr_t child=0, childVtable=0;
                        if (!SafeReadValue(reinterpret_cast<void*>(work.object+off),child) || !child || child==work.object) continue;
                        if (!SafeReadValue(reinterpret_cast<void*>(child),childVtable)) continue;
                        std::uintptr_t dummy=0;
                        const std::string childModule=ModuleForAddress(snapshot,childVtable,dummy);
                        if (childModule.find("ATOnPTracker")!=std::string::npos)
                            queue.push_back({child,work.depth+1,off});
                    }
                }
            }
        }
        std::ostringstream manifest;
        manifest << "{\r\n"
                 << "  \"capture_version\": 149,\r\n"
                 << "  \"capture_type\": \"grid_interface_locator\",\r\n"
                 << "  \"final_goal\": \"read Accounts, Open Positions and Logs for Watchdog status and situation reports\",\r\n"
                 << "  \"page\": " << JsonString(page) << ",\r\n"
                 << "  \"read_only\": true,\r\n"
                 << "  \"unknown_functions_called\": false,\r\n"
                 << "  \"GetText_called\": false,\r\n"
                 << "  \"objects\": " << objectCount << ",\r\n"
                 << "  \"vtable_slots\": " << vtableSlotCount << ",\r\n"
                 << "  \"high_score_objects\": " << highScoreCount << ",\r\n"
                 << "  \"files\": {\"objects\":" << JsonString(objectsPath) << ",\"vtables\":" << JsonString(vtablesPath)
                 << ",\"diagnostics\":" << JsonString(diagnosticsPath) << "}\r\n"
                 << "}\r\n";
        const bool manifestOk=WriteUtf8File(manifestPath,manifest.str());
        std::ostringstream summary;
        summary << "{\"capture\":\"grid_interface_locator\",\"version\":149,\"page\":" << JsonString(page)
                << ",\"read_only\":true,\"GetText_called\":false,\"objects\":" << objectCount
                << ",\"vtable_slots\":" << vtableSlotCount << ",\"high_score_objects\":" << highScoreCount
                << ",\"manifest_written\":" << (manifestOk?"true":"false") << ",\"manifest_path\":" << JsonString(manifestPath) << '}';
        summaryJson=summary.str();
        return manifestOk && objectCount>0;
    }

    bool WritePageObjectStructureProbe(
        const Snapshot& snapshot,
        std::string& summaryJson,
        const char* requestedPage)
    {
        CreateDirectoryW(kOutputDirectory, nullptr);
        const std::string page = (requestedPage && *requestedPage) ? requestedPage : "unspecified";
        std::wstring pageWide(page.begin(), page.end());
        const std::wstring prefix = L"MC_V148_" + pageWide + L"_FlexGrid_Row_Layout_Probe_";
        const std::wstring rootsPath = ReportPath((prefix + L"Page_Roots").c_str(), snapshot.processId);
        const std::wstring regionsPath = ReportPath((prefix + L"Ranked_Regions").c_str(), snapshot.processId);
        const std::wstring stringsPath = ReportPath((prefix + L"Strings").c_str(), snapshot.processId);
        const std::wstring vectorsPath = ReportPath((prefix + L"Vector_Candidates").c_str(), snapshot.processId);
        const std::wstring stridesPath = ReportPath((prefix + L"Stride_Candidates").c_str(), snapshot.processId);
        const std::wstring diffsPath = ReportPath((prefix + L"Diffs").c_str(), snapshot.processId);
        const std::wstring diagnosticsPath = ReportPath((prefix + L"Diagnostics").c_str(), snapshot.processId);
        const std::wstring manifestPath = ReportPath((prefix + L"Manifest").c_str(), snapshot.processId);
        std::wstring memoryPath = ReportPath((prefix + L"Memory").c_str(), snapshot.processId);
        if (memoryPath.size() >= 4 && memoryPath.substr(memoryPath.size() - 4) == L".txt")
            memoryPath.replace(memoryPath.size() - 4, 4, L".bin");

        std::ofstream rootsOut(rootsPath, std::ios::binary | std::ios::trunc);
        std::ofstream regionsOut(regionsPath, std::ios::binary | std::ios::trunc);
        std::ofstream stringsOut(stringsPath, std::ios::binary | std::ios::trunc);
        std::ofstream vectorsOut(vectorsPath, std::ios::binary | std::ios::trunc);
        std::ofstream stridesOut(stridesPath, std::ios::binary | std::ios::trunc);
        std::ofstream diffsOut(diffsPath, std::ios::binary | std::ios::trunc);
        std::ofstream diagnostics(diagnosticsPath, std::ios::binary | std::ios::trunc);
        std::ofstream memory(memoryPath, std::ios::binary | std::ios::trunc);
        if (!rootsOut || !regionsOut || !stringsOut || !vectorsOut || !stridesOut ||
            !diffsOut || !diagnostics || !memory)
        {
            summaryJson = "{\"capture\":\"flexgrid_row_layout_probe\",\"version\":148,\"error\":\"could_not_create_output_files\"}";
            return false;
        }

        rootsOut << "sample,page,page_member_offset,page_object,vtable,vtable_module,vtable_rva,read_ok\r\n";
        regionsOut << "sample,page,member_offset,address,region_base,region_size,protection,captured_bytes,header_hits,string_count,pointer_density_per_mille,changed_bytes,score,memory_offset\r\n";
        stringsOut << "sample,page,member_offset,address,offset,encoding,length,known_header,text_preview\r\n";
        vectorsOut << "sample,page,owner,owner_member_offset,triplet_offset,begin,end,capacity,used_bytes,capacity_bytes,element_size,element_count,readable,score\r\n";
        stridesOut << "sample,page,member_offset,address,stride,records_checked,records_with_text,records_with_pointer,text_ratio_per_mille,pointer_ratio_per_mille,score\r\n";
        diffsOut << "page,member_offset,sample_a,sample_b,bytes_compared,changed_bytes,first_changed_offset,last_changed_offset\r\n";
        diagnostics << "MC V148 FlexGrid Row Layout Probe\r\n"
                    << "goal=identify Accounts/Open Positions/Logs model, row and cell storage for Watchdog reports\r\n"
                    << "read_only=yes\r\nunknown_functions_called=no\r\npage=" << page << "\r\n";

        ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "accounts", kExtractAccountsRva);
        ExtractorAttempt positions = BuildExtractorProbe(snapshot, "open_positions", kExtractOpenPositionsRva);
        const std::uintptr_t tabView = accounts.tabView ? accounts.tabView : positions.tabView;
        if (!tabView)
        {
            summaryJson = "{\"capture\":\"flexgrid_row_layout_probe\",\"version\":148,\"error\":\"CATPTTabView_not_found\"}";
            return false;
        }

        struct PageRoot { const char* name; std::size_t offset; std::uintptr_t expectedVtableRva; };
        std::vector<PageRoot> roots;
        if (page == "accounts" || page == "all") roots.push_back({"accounts", 0x58, 0x1DB7E8});
        if (page == "open_positions" || page == "all") roots.push_back({"open_positions", 0x68, 0x1E15C8});
        if (page == "logs" || page == "all") roots.push_back({"logs", 0x80, 0x1F0450});
        if (roots.empty())
        {
            summaryJson = "{\"capture\":\"flexgrid_row_layout_probe\",\"version\":148,\"error\":\"invalid_page\"}";
            return false;
        }

        constexpr int kSamples = 3;
        constexpr DWORD kSampleDelayMs = 2000;
        constexpr std::size_t kPageObjectBytes = 0x1000;
        constexpr std::size_t kRegionCaptureBytes = 0x40000; // 256 KiB: broad enough for rows, bounded for safety.
        constexpr std::size_t kMaximumMembersPerPage = 192;
        const std::size_t candidateStrides[] = { 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 160, 192, 224, 256 };
        const std::size_t candidateElementSizes[] = { 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 160, 192, 224, 256 };

        struct Capture
        {
            std::string page;
            std::size_t memberOffset = 0;
            std::uintptr_t address = 0;
            std::vector<unsigned char> bytes;
        };
        std::map<std::string, std::vector<Capture>> captures;
        std::uint64_t binaryOffset = 0;
        std::size_t successfulPages = 0;
        std::size_t successfulRegions = 0;
        std::size_t vectorCandidates = 0;
        std::size_t strideCandidates = 0;

        auto isPrintableAscii = [](unsigned char c) { return c >= 32 && c <= 126; };
        auto isKnownHeader = [&](const std::string& text, const std::string& pageName)
        {
            static const char* common[] = { "Account", "Profile", "Strategy", "Instrument", "Symbol", "Quantity", "Price", "Date/Time", "Category", "Message", "Status", "Currency", "P/L", "Open P/L", "Market Value" };
            for (const char* item : common) if (text.find(item) != std::string::npos) return true;
            if (pageName == "accounts" && (text.find("Cash") != std::string::npos || text.find("Equity") != std::string::npos || text.find("Buying Power") != std::string::npos)) return true;
            if (pageName == "open_positions" && (text.find("Average Price") != std::string::npos || text.find("Position") != std::string::npos)) return true;
            if (pageName == "logs" && (text.find("Event") != std::string::npos || text.find("Error") != std::string::npos)) return true;
            return false;
        };
        auto containsTextNear = [&](const std::vector<unsigned char>& bytes, std::size_t offset, std::size_t length)
        {
            const std::size_t end = std::min(bytes.size(), offset + length);
            std::size_t run = 0;
            for (std::size_t i = offset; i < end; ++i)
            {
                if (isPrintableAscii(bytes[i])) { if (++run >= 4) return true; }
                else run = 0;
            }
            run = 0;
            for (std::size_t i = offset; i + 1 < end; i += 2)
            {
                if (isPrintableAscii(bytes[i]) && bytes[i + 1] == 0) { if (++run >= 4) return true; }
                else run = 0;
            }
            return false;
        };
        auto pointerDensity = [&](const std::vector<unsigned char>& bytes)
        {
            if (bytes.size() < sizeof(std::uintptr_t)) return std::size_t(0);
            std::size_t pointers = 0, slots = 0;
            const std::size_t scanBytes = std::min<std::size_t>(bytes.size(), 0x8000);
            for (std::size_t off = 0; off + sizeof(std::uintptr_t) <= scanBytes; off += sizeof(std::uintptr_t))
            {
                std::uintptr_t value = 0; std::memcpy(&value, bytes.data() + off, sizeof(value));
                ++slots;
                std::uintptr_t end = 0; MEMORY_BASIC_INFORMATION mbi{};
                if (QueryReadableSpan(value, end, mbi)) ++pointers;
            }
            return slots ? (pointers * 1000 / slots) : 0;
        };

        auto emitStrings = [&](int sample, const std::string& pageName, std::size_t memberOffset,
                               std::uintptr_t address, const std::vector<unsigned char>& bytes,
                               std::size_t& stringCount, std::size_t& headerHits)
        {
            for (std::size_t i = 0; i + 5 < bytes.size(); ++i)
            {
                if (!isPrintableAscii(bytes[i])) continue;
                std::size_t j = i;
                while (j < bytes.size() && isPrintableAscii(bytes[j]) && j - i < 512) ++j;
                if (j - i >= 5)
                {
                    std::string text(reinterpret_cast<const char*>(bytes.data() + i), j - i);
                    for (char& c : text) if (c == '"') c = '\'';
                    const bool header = isKnownHeader(text, pageName);
                    ++stringCount; if (header) ++headerHits;
                    stringsOut << sample << ",\"" << pageName << "\"," << HexValue(memberOffset) << ',' << HexValue(address)
                               << ',' << HexValue(i) << ",ascii," << (j - i) << ',' << (header ? "yes" : "no") << ",\"" << text << "\"\r\n";
                    i = j;
                }
            }
            for (std::size_t i = 0; i + 10 < bytes.size(); i += 2)
            {
                std::size_t j = i; std::string text;
                while (j + 1 < bytes.size() && isPrintableAscii(bytes[j]) && bytes[j + 1] == 0 && text.size() < 512)
                { text.push_back(static_cast<char>(bytes[j])); j += 2; }
                if (text.size() >= 5)
                {
                    for (char& c : text) if (c == '"') c = '\'';
                    const bool header = isKnownHeader(text, pageName);
                    ++stringCount; if (header) ++headerHits;
                    stringsOut << sample << ",\"" << pageName << "\"," << HexValue(memberOffset) << ',' << HexValue(address)
                               << ',' << HexValue(i) << ",utf16le," << text.size() << ',' << (header ? "yes" : "no") << ",\"" << text << "\"\r\n";
                    i = j;
                }
            }
        };

        auto scanVectorTriplets = [&](int sample, const std::string& pageName, const char* owner,
                                      std::size_t ownerMemberOffset, const std::vector<unsigned char>& bytes)
        {
            const std::size_t vectorScanBytes = std::min<std::size_t>(bytes.size(), 0x10000);
            for (std::size_t off = 0; off + 3 * sizeof(std::uintptr_t) <= vectorScanBytes; off += sizeof(std::uintptr_t))
            {
                std::uintptr_t begin = 0, end = 0, capacity = 0;
                std::memcpy(&begin, bytes.data() + off, sizeof(begin));
                std::memcpy(&end, bytes.data() + off + sizeof(begin), sizeof(end));
                std::memcpy(&capacity, bytes.data() + off + 2 * sizeof(begin), sizeof(capacity));
                if (!begin || begin > end || end > capacity || capacity - begin > 0x4000000ull) continue;
                std::uintptr_t readableEnd = 0; MEMORY_BASIC_INFORMATION mbi{};
                const bool readable = QueryReadableSpan(begin, readableEnd, mbi) && end <= readableEnd;
                if (!readable) continue;
                const std::size_t used = static_cast<std::size_t>(end - begin);
                if (used == 0 || used > 0x1000000) continue;
                for (std::size_t elementSize : candidateElementSizes)
                {
                    if (used % elementSize != 0) continue;
                    const std::size_t count = used / elementSize;
                    if (count == 0 || count > 100000) continue;
                    int score = 20;
                    if (count >= 2) score += 10;
                    if (count <= 5000) score += 5;
                    if (elementSize >= 24) score += 5;
                    vectorsOut << sample << ",\"" << pageName << "\",\"" << owner << "\"," << HexValue(ownerMemberOffset)
                               << ',' << HexValue(off) << ',' << HexValue(begin) << ',' << HexValue(end) << ',' << HexValue(capacity)
                               << ',' << used << ',' << (capacity - begin) << ',' << elementSize << ',' << count << ",yes," << score << "\r\n";
                    ++vectorCandidates;
                }
            }
        };

        for (int sample = 0; sample < kSamples; ++sample)
        {
            if (sample) Sleep(kSampleDelayMs);
            for (const auto& root : roots)
            {
                std::uintptr_t pageObject = 0;
                if (!SafeReadValue(reinterpret_cast<void*>(tabView + root.offset), pageObject))
                {
                    diagnostics << "sample=" << sample << " page=" << root.name << " failed_to_read_page_pointer\r\n";
                    continue;
                }
                std::uintptr_t pageEnd = 0; MEMORY_BASIC_INFORMATION pageMbi{};
                if (!QueryReadableSpan(pageObject, pageEnd, pageMbi)) continue;
                const std::size_t objectSize = std::min<std::size_t>(kPageObjectBytes, static_cast<std::size_t>(pageEnd - pageObject));
                std::vector<unsigned char> objectBytes(objectSize, 0);
                if (!objectSize || !SafeReadBytes(reinterpret_cast<void*>(pageObject), objectBytes.data(), objectSize)) continue;
                ++successfulPages;
                std::uintptr_t vtable = 0; std::memcpy(&vtable, objectBytes.data(), sizeof(vtable));
                std::uintptr_t vtableRva = 0; const std::string vtableModule = ModuleForAddress(snapshot, vtable, vtableRva);
                rootsOut << sample << ",\"" << root.name << "\"," << HexValue(root.offset) << ',' << HexValue(pageObject) << ','
                         << HexValue(vtable) << ",\"" << vtableModule << "\"," << HexValue(vtableRva) << ",yes\r\n";
                scanVectorTriplets(sample, root.name, "page_object", 0, objectBytes);

                std::set<std::uintptr_t> seenAddresses;
                std::size_t memberCount = 0;
                for (std::size_t off = 0; off + sizeof(std::uintptr_t) <= objectBytes.size() && memberCount < kMaximumMembersPerPage; off += sizeof(std::uintptr_t))
                {
                    std::uintptr_t value = 0; std::memcpy(&value, objectBytes.data() + off, sizeof(value));
                    std::uintptr_t regionEnd = 0; MEMORY_BASIC_INFORMATION mbi{};
                    if (!QueryReadableSpan(value, regionEnd, mbi) || !seenAddresses.insert(value).second) continue;
                    ++memberCount;
                    const std::size_t captureSize = std::min<std::size_t>(kRegionCaptureBytes, static_cast<std::size_t>(regionEnd - value));
                    if (captureSize < 32) continue;
                    std::vector<unsigned char> bytes(captureSize, 0);
                    if (!SafeReadBytes(reinterpret_cast<void*>(value), bytes.data(), captureSize)) continue;
                    ++successfulRegions;

                    const std::string key = std::string(root.name) + ":" + std::to_string(off);
                    std::size_t changedBytes = 0;
                    const auto previous = captures.find(key);
                    if (previous != captures.end() && !previous->second.empty())
                    {
                        const auto& first = previous->second.front().bytes;
                        const std::size_t n = std::min(first.size(), bytes.size());
                        for (std::size_t i = 0; i < n; ++i) if (first[i] != bytes[i]) ++changedBytes;
                    }
                    std::size_t stringCount = 0, headerHits = 0;
                    emitStrings(sample, root.name, off, value, bytes, stringCount, headerHits);
                    const std::size_t density = pointerDensity(bytes);
                    int score = static_cast<int>(headerHits * 30 + std::min<std::size_t>(stringCount, 40) * 2);
                    if (density >= 50 && density <= 800) score += 15;
                    if (changedBytes > 0) score += 20;
                    if (changedBytes > 0 && changedBytes < bytes.size() / 4) score += 15;

                    regionsOut << sample << ",\"" << root.name << "\"," << HexValue(off) << ',' << HexValue(value) << ','
                               << HexValue(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)) << ',' << mbi.RegionSize << ',' << ProtectionName(mbi.Protect)
                               << ',' << captureSize << ',' << headerHits << ',' << stringCount << ',' << density << ',' << changedBytes << ',' << score << ',' << binaryOffset << "\r\n";
                    memory.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    binaryOffset += bytes.size();
                    captures[key].push_back({ root.name, off, value, bytes });
                    scanVectorTriplets(sample, root.name, "direct_member_region", off, bytes);

                    for (std::size_t stride : candidateStrides)
                    {
                        const std::size_t records = std::min<std::size_t>(bytes.size() / stride, 64);
                        if (records < 3) continue;
                        std::size_t textRecords = 0, pointerRecords = 0;
                        for (std::size_t record = 0; record < records; ++record)
                        {
                            const std::size_t base = record * stride;
                            if (containsTextNear(bytes, base, stride)) ++textRecords;
                            bool hasPointer = false;
                            const std::size_t pointerWindowEnd = std::min<std::size_t>(std::min(bytes.size(), base + stride), base + 4 * sizeof(std::uintptr_t));
                            for (std::size_t p = base; p + sizeof(std::uintptr_t) <= pointerWindowEnd; p += sizeof(std::uintptr_t))
                            {
                                std::uintptr_t candidate = 0; std::memcpy(&candidate, bytes.data() + p, sizeof(candidate));
                                std::uintptr_t candidateEnd = 0; MEMORY_BASIC_INFORMATION candidateMbi{};
                                if (QueryReadableSpan(candidate, candidateEnd, candidateMbi)) { hasPointer = true; break; }
                            }
                            if (hasPointer) ++pointerRecords;
                        }
                        const std::size_t textRatio = textRecords * 1000 / records;
                        const std::size_t pointerRatio = pointerRecords * 1000 / records;
                        int strideScore = static_cast<int>(textRatio / 20 + pointerRatio / 25);
                        if (textRecords >= 2 && textRecords < records) strideScore += 15;
                        if (pointerRecords >= 2 && pointerRecords < records) strideScore += 10;
                        if (strideScore >= 25)
                        {
                            stridesOut << sample << ",\"" << root.name << "\"," << HexValue(off) << ',' << HexValue(value) << ',' << stride << ','
                                       << records << ',' << textRecords << ',' << pointerRecords << ',' << textRatio << ',' << pointerRatio << ',' << strideScore << "\r\n";
                            ++strideCandidates;
                        }
                    }
                }
                diagnostics << "sample=" << sample << " page=" << root.name << " page_object=" << HexValue(pageObject)
                            << " vtable_rva=" << HexValue(vtableRva) << " expected=" << HexValue(root.expectedVtableRva)
                            << " unique_readable_members=" << memberCount << "\r\n";
            }
            rootsOut.flush(); regionsOut.flush(); stringsOut.flush(); vectorsOut.flush(); stridesOut.flush(); memory.flush(); diagnostics.flush();
        }

        for (const auto& item : captures)
        {
            if (item.second.size() < 2) continue;
            const Capture& firstCapture = item.second.front();
            for (std::size_t sampleIndex = 1; sampleIndex < item.second.size(); ++sampleIndex)
            {
                const Capture& other = item.second[sampleIndex];
                const std::size_t n = std::min(firstCapture.bytes.size(), other.bytes.size());
                std::size_t changed = 0, first = n, last = 0;
                for (std::size_t i = 0; i < n; ++i)
                    if (firstCapture.bytes[i] != other.bytes[i]) { ++changed; if (first == n) first = i; last = i; }
                diffsOut << '"' << firstCapture.page << "\"," << HexValue(firstCapture.memberOffset) << ",0," << sampleIndex << ',' << n << ',' << changed << ',';
                if (first == n) diffsOut << "none,none\r\n"; else diffsOut << HexValue(first) << ',' << HexValue(last) << "\r\n";
            }
        }

        std::ostringstream manifest;
        manifest << "{\r\n"
                 << "  \"capture_version\": 148,\r\n"
                 << "  \"capture_type\": \"flexgrid_row_layout_probe\",\r\n"
                 << "  \"final_goal\": \"read Accounts, Open Positions and Logs for Watchdog status and situation reports\",\r\n"
                 << "  \"page\": " << JsonString(page) << ",\r\n"
                 << "  \"read_only\": true,\r\n"
                 << "  \"unknown_functions_called\": false,\r\n"
                 << "  \"process_id\": " << snapshot.processId << ",\r\n"
                 << "  \"CATPTTabView\": " << JsonString(HexValue(tabView)) << ",\r\n"
                 << "  \"page_object_offsets\": {\"accounts\":\"0x58\",\"open_positions\":\"0x68\",\"logs\":\"0x80\"},\r\n"
                 << "  \"samples\": 3,\r\n"
                 << "  \"successful_page_reads\": " << successfulPages << ",\r\n"
                 << "  \"successful_region_reads\": " << successfulRegions << ",\r\n"
                 << "  \"vector_candidates\": " << vectorCandidates << ",\r\n"
                 << "  \"stride_candidates\": " << strideCandidates << ",\r\n"
                 << "  \"memory_bytes\": " << binaryOffset << ",\r\n"
                 << "  \"files\": {\"page_roots\":" << JsonString(rootsPath) << ",\"ranked_regions\":" << JsonString(regionsPath)
                 << ",\"strings\":" << JsonString(stringsPath) << ",\"vectors\":" << JsonString(vectorsPath)
                 << ",\"strides\":" << JsonString(stridesPath) << ",\"diffs\":" << JsonString(diffsPath)
                 << ",\"memory\":" << JsonString(memoryPath) << ",\"diagnostics\":" << JsonString(diagnosticsPath) << "}\r\n"
                 << "}\r\n";
        const bool manifestOk = WriteUtf8File(manifestPath, manifest.str());
        std::ostringstream summary;
        summary << "{\"capture\":\"flexgrid_row_layout_probe\",\"version\":148,\"page\":" << JsonString(page)
                << ",\"read_only\":true,\"successful_page_reads\":" << successfulPages
                << ",\"successful_region_reads\":" << successfulRegions
                << ",\"vector_candidates\":" << vectorCandidates
                << ",\"stride_candidates\":" << strideCandidates
                << ",\"memory_bytes\":" << binaryOffset
                << ",\"manifest_written\":" << (manifestOk ? "true" : "false")
                << ",\"manifest_path\":" << JsonString(manifestPath) << '}';
        summaryJson = summary.str();
        return manifestOk && successfulPages > 0 && successfulRegions > 0;
    }


    struct V153GridSectionResult
    {
        std::string name;
        bool ok = false;
        std::wstring diagnostic;
        std::uint32_t reportedRows = 0;
        std::size_t callsAttempted = 0;
        std::size_t callsSucceeded = 0;
        std::size_t sehFailures = 0;
        std::vector<std::vector<std::wstring>> rows;
    };

    class V153GridReadLockGuard
    {
    public:
        V153GridReadLockGuard() { AcquireSRWLockExclusive(&g_gridReadLock); }
        ~V153GridReadLockGuard() { ReleaseSRWLockExclusive(&g_gridReadLock); }
        V153GridReadLockGuard(const V153GridReadLockGuard&) = delete;
        V153GridReadLockGuard& operator=(const V153GridReadLockGuard&) = delete;
    };

    std::string V153EscapeFieldUtf8(const std::string& value)
    {
        std::string result;
        result.reserve(value.size() + 16);
        for (unsigned char ch : value)
        {
            switch (ch)
            {
            case '\\': result += "\\\\"; break;
            case '\t': result += "\\t"; break;
            case '\r': result += "\\r"; break;
            case '\n': result += "\\n"; break;
            default: result.push_back(static_cast<char>(ch)); break;
            }
        }
        return result;
    }

    std::string V153EscapeField(const std::wstring& value)
    {
        return V153EscapeFieldUtf8(WideToUtf8(value));
    }

    std::string V153UtcText(const SYSTEMTIME& time)
    {
        std::ostringstream out;
        out << std::setfill('0')
            << std::setw(4) << time.wYear << '-'
            << std::setw(2) << time.wMonth << '-'
            << std::setw(2) << time.wDay << 'T'
            << std::setw(2) << time.wHour << ':'
            << std::setw(2) << time.wMinute << ':'
            << std::setw(2) << time.wSecond << '.'
            << std::setw(3) << time.wMilliseconds << 'Z';
        return out.str();
    }

    bool V153RowIsEmpty(const std::vector<std::wstring>& row)
    {
        for (const auto& value : row)
        {
            if (!value.empty())
                return false;
        }
        return true;
    }

    V153GridSectionResult ReadV153GridSection(
        const Snapshot& snapshot,
        const std::vector<RttiVtableRecord>& flexGridRttiVtables,
        std::uintptr_t tabView,
        const TrackerCompatibilityProfile& profile,
        const char* name,
        std::size_t pageOffset,
        unsigned int firstColumn,
        unsigned int columnCount,
        unsigned int maxRowsToScan,
        unsigned int maxRowsToReturn,
        unsigned int emptyRowsToStop)
    {
        struct FlexGridIdentity
        {
            std::uintptr_t vtableRva;
            std::uintptr_t getTextRva;
            const wchar_t* buildLabel;
        };
        constexpr FlexGridIdentity kEmbeddedFlexGridIdentities[] =
        {
            { 0x1FD778, 0x135E40, L"legacy validated build" },
            { 0x1FD6F8, 0x136230, L"MultiCharts build 2026-08-01" }
        };
        constexpr unsigned int kBufferCharacters = 2048;

        V153GridSectionResult result;
        result.name = name ? name : "unknown";

        if (!snapshot.atonpTrackerModule)
        {
            result.diagnostic = L"ATOnPTracker.dll is not loaded.";
            return result;
        }
        if (!profile.matched)
        {
            result.diagnostic = L"Tracker compatibility UNKNOWN: " + profile.diagnostic;
            return result;
        }
        if (pageOffset == 0)
        {
            result.diagnostic = L"This Tracker profile does not define the optional page offset.";
            return result;
        }
        if (!tabView)
        {
            result.diagnostic = L"CATPTTabView object was not found for Tracker profile " + profile.name;
            return result;
        }

        const std::uintptr_t moduleBase = reinterpret_cast<std::uintptr_t>(snapshot.atonpTrackerModule);
        std::wstring identityLabel;

        std::uintptr_t pageObject = 0;
        std::uintptr_t gridObject = 0;
        std::uintptr_t actualVtable = 0;
        std::uintptr_t actualGetText = 0;
        std::uint32_t rows1 = 0;
        std::uint32_t rows2 = 0;

        const bool anchorsOk =
            SafeReadValue(reinterpret_cast<void*>(tabView + pageOffset), pageObject) && pageObject &&
            SafeReadValue(reinterpret_cast<void*>(pageObject + profile.gridMemberOffset), gridObject) && gridObject &&
            SafeReadValue(reinterpret_cast<void*>(gridObject), actualVtable) &&
            SafeReadValue(reinterpret_cast<void*>(actualVtable + profile.getTextSlot * sizeof(std::uintptr_t)), actualGetText) &&
            SafeReadValue(reinterpret_cast<void*>(gridObject + profile.rowsOffset1), rows1) &&
            SafeReadValue(reinterpret_cast<void*>(gridObject + profile.rowsOffset2), rows2);

        if (!anchorsOk)
        {
            std::wostringstream diagnostic;
            diagnostic << L"Tracker profile layout read failed for " << profile.name
                       << L" at page_offset=0x" << std::hex << std::uppercase << pageOffset
                       << L" grid_member_offset=0x" << profile.gridMemberOffset
                       << L" rows_offsets=(0x" << profile.rowsOffset1 << L",0x" << profile.rowsOffset2 << L")";
            result.diagnostic = diagnostic.str();
            return result;
        }

        if (profile.externalVerified)
        {
            const std::uintptr_t expectedVtable = moduleBase + profile.flexGridVtableRva;
            const std::uintptr_t expectedGetText = moduleBase + profile.getTextRva;
            if (actualVtable != expectedVtable || actualGetText != expectedGetText)
            {
                std::wostringstream diagnostic;
                diagnostic << L"Verified Tracker profile identity mismatch: actual_vtable_rva=0x"
                           << std::hex << std::uppercase << (actualVtable - moduleBase)
                           << L" expected_vtable_rva=0x" << profile.flexGridVtableRva
                           << L" actual_gettext_rva=0x" << (actualGetText - moduleBase)
                           << L" expected_gettext_rva=0x" << profile.getTextRva;
                result.diagnostic = diagnostic.str();
                return result;
            }
            identityLabel = L"verified-profile:" + profile.name;
        }
        else
        {
            const FlexGridIdentity* matchedIdentity = nullptr;
            for (const auto& identity : kEmbeddedFlexGridIdentities)
            {
                if (actualVtable == moduleBase + identity.vtableRva &&
                    actualGetText == moduleBase + identity.getTextRva)
                {
                    matchedIdentity = &identity;
                    break;
                }
            }

            if (matchedIdentity)
            {
                identityLabel = std::wstring(L"embedded-known:") + matchedIdentity->buildLabel;
            }
            else if (profile.allowAdaptiveFlexGridIdentity)
            {
                // The embedded validated fallback preserves the adaptive RTTI path.
                // Externally verified profiles never use this path: they must match
                // their exact configured identity pair.
                const bool rttiMatched = std::any_of(
                    flexGridRttiVtables.begin(), flexGridRttiVtables.end(),
                    [actualVtable](const RttiVtableRecord& item) { return item.vtable == actualVtable; });
                const bool vtableInsideModule = ModuleContains(snapshot, actualVtable, sizeof(std::uintptr_t));
                const bool getTextInsideModule = ModuleContains(snapshot, actualGetText, 1);
                const bool getTextExecutable =
                    getTextInsideModule && MemoryRangeHasProtection(reinterpret_cast<void*>(actualGetText), 1, true);

                std::size_t executableSlots = 0;
                constexpr std::size_t kVtableSlotsToValidate = 64;
                for (std::size_t slot = 0; slot < kVtableSlotsToValidate; ++slot)
                {
                    std::uintptr_t method = 0;
                    if (!SafeReadValue(reinterpret_cast<void*>(actualVtable + slot * sizeof(std::uintptr_t)), method))
                        continue;
                    if (ModuleContains(snapshot, method, 1) &&
                        MemoryRangeHasProtection(reinterpret_cast<void*>(method), 1, true))
                    {
                        ++executableSlots;
                    }
                }

                constexpr std::size_t kMinimumExecutableSlots = 32;
                if (rttiMatched && vtableInsideModule && getTextExecutable &&
                    executableSlots >= kMinimumExecutableSlots)
                {
                    identityLabel = L"embedded-adaptive:RTTI CFlexGridImpl + executable configured slot";
                }
                else
                {
                    std::wostringstream diagnostic;
                    diagnostic << L"Unsupported embedded CFlexGridImpl identity: actual_vtable_rva=0x"
                               << std::hex << std::uppercase << (actualVtable - moduleBase)
                               << L" actual_gettext_rva=0x" << (actualGetText - moduleBase)
                               << L" rtti_match=" << (rttiMatched ? L"yes" : L"no")
                               << L" vtable_in_module=" << (vtableInsideModule ? L"yes" : L"no")
                               << L" gettext_executable=" << (getTextExecutable ? L"yes" : L"no")
                               << L" executable_slots=" << std::dec << executableSlots
                               << L"/" << kVtableSlotsToValidate;
                    result.diagnostic = diagnostic.str();
                    return result;
                }
            }
            else
            {
                result.diagnostic = L"Tracker profile does not authorize an adaptive FlexGrid identity.";
                return result;
            }
        }

        if (rows1 != rows2 || rows1 == 0 || rows1 > 100000)
        {
            result.diagnostic = L"FlexGrid mirrored row-capacity fields were inconsistent";
            return result;
        }

        result.reportedRows = rows1;
        const unsigned int scanLimit =
            (std::min)(maxRowsToScan, static_cast<unsigned int>(rows1));
        unsigned int consecutiveEmptyRows = 0;
        bool sawNonEmptyRow = false;

        for (unsigned int rowIndex = 0; rowIndex < scanLimit; ++rowIndex)
        {
            std::vector<std::wstring> row;
            row.reserve(columnCount);
            bool rowCallFailed = false;

            for (unsigned int columnOffset = 0; columnOffset < columnCount; ++columnOffset)
            {
                wchar_t buffer[kBufferCharacters]{};
                DWORD sehCode = 0;
                ++result.callsAttempted;
                const int callOk = MCBridge_CallFlexGridGetTextWithSeh(
                    reinterpret_cast<void*>(gridObject),
                    reinterpret_cast<void*>(actualGetText),
                    rowIndex,
                    firstColumn + columnOffset,
                    buffer,
                    kBufferCharacters,
                    &sehCode);

                if (!callOk)
                {
                    ++result.sehFailures;
                    std::wostringstream diagnostic;
                    diagnostic << L"GetText failed at row=" << rowIndex
                               << L" column=" << (firstColumn + columnOffset)
                               << L" seh=0x" << std::hex << std::uppercase << sehCode;
                    result.diagnostic = diagnostic.str();
                    rowCallFailed = true;
                    break;
                }

                ++result.callsSucceeded;
                row.emplace_back(buffer);
            }

            if (rowCallFailed)
                return result;

            if (V153RowIsEmpty(row))
            {
                ++consecutiveEmptyRows;
                if (consecutiveEmptyRows >= emptyRowsToStop)
                    break;
                continue;
            }

            sawNonEmptyRow = true;
            consecutiveEmptyRows = 0;
            result.rows.push_back(std::move(row));
            if (maxRowsToReturn > 0 && result.rows.size() >= maxRowsToReturn)
                break;
        }

        result.ok = result.sehFailures == 0;
        if (result.ok)
        {
            std::wostringstream diagnostic;
            diagnostic << L"grid validated; profile=" << profile.name
                       << L"; identity=" << identityLabel
                       << L"; rows_returned=" << result.rows.size()
                       << L"; calls=" << result.callsSucceeded
                       << L"; reported_capacity=" << result.reportedRows;
            if (!sawNonEmptyRow)
                diagnostic << L"; no non-empty rows";
            result.diagnostic = diagnostic.str();
        }
        return result;
    }


    bool TryParsePositionResearchNumber(const std::wstring& source, double& value)
    {
        std::wstring compact;
        compact.reserve(source.size());
        for (wchar_t ch : source)
        {
            if ((ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'+' || ch == L'.' || ch == L',')
                compact.push_back(ch);
        }
        if (compact.empty())
            return false;

        const std::size_t dot = compact.find_last_of(L'.');
        const std::size_t comma = compact.find_last_of(L',');
        std::size_t decimal = std::wstring::npos;
        if (dot != std::wstring::npos && comma != std::wstring::npos)
            decimal = std::max(dot, comma);
        else if (dot != std::wstring::npos)
            decimal = dot;
        else if (comma != std::wstring::npos)
            decimal = comma;

        std::wstring normalized;
        normalized.reserve(compact.size());
        for (std::size_t i = 0; i < compact.size(); ++i)
        {
            const wchar_t ch = compact[i];
            if (ch == L'.' || ch == L',')
            {
                if (i == decimal)
                    normalized.push_back(L'.');
                continue;
            }
            normalized.push_back(ch);
        }

        wchar_t* end = nullptr;
        errno = 0;
        const double parsed = std::wcstod(normalized.c_str(), &end);
        if (errno == ERANGE || end == normalized.c_str() || *end != L'\0' || !std::isfinite(parsed))
            return false;
        value = parsed;
        return true;
    }

    std::wstring PositionResearchCurrencyHint(const std::wstring& source)
    {
        std::wstring upper = source;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towupper(ch)); });
        static const wchar_t* codes[] =
        {
            L"EUR", L"USD", L"SEK", L"DKK", L"NOK", L"GBP", L"CHF", L"JPY", L"CAD", L"AUD", L"NZD", L"HKD", L"SGD"
        };
        for (const wchar_t* code : codes)
        {
            if (upper.find(code) != std::wstring::npos)
                return code;
        }
        if (source.find(L'\x20AC') != std::wstring::npos)
            return L"EUR";
        if (source.find(L'$') != std::wstring::npos)
            return L"DOLLAR_SYMBOL_AMBIGUOUS";
        if (source.find(L'\x00A3') != std::wstring::npos)
            return L"GBP";
        return L"UNKNOWN";
    }

    std::wstring ReadShortUtf16Candidate(std::uintptr_t address)
    {
        if (!address)
            return L"";
        std::uintptr_t end = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!QueryReadableSpan(address, end, mbi))
            return L"";

        constexpr std::size_t kMaxCharacters = 32;
        const std::size_t availableCharacters = std::min<std::size_t>(
            kMaxCharacters,
            static_cast<std::size_t>((end - address) / sizeof(wchar_t)));
        if (availableCharacters == 0)
            return L"";

        wchar_t buffer[kMaxCharacters + 1]{};
        if (!SafeReadBytes(reinterpret_cast<const void*>(address), buffer, availableCharacters * sizeof(wchar_t)))
            return L"";
        std::size_t length = 0;
        for (; length < availableCharacters; ++length)
        {
            const wchar_t ch = buffer[length];
            if (ch == L'\0')
                break;
            if (!(iswalnum(ch) || iswspace(ch) || ch == L'/' || ch == L'-' || ch == L'_' || ch == L':' || ch == L'.' || ch == L'@' || ch == L'$' || ch == L'\x20AC' || ch == L'\x00A3'))
                return L"";
        }
        if (length == 0 || length == availableCharacters)
            return L"";
        return std::wstring(buffer, length);
    }

    std::string ReadShortAsciiCandidate(std::uintptr_t address)
    {
        if (!address)
            return {};
        std::uintptr_t end = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!QueryReadableSpan(address, end, mbi))
            return {};
        constexpr std::size_t kMaxCharacters = 32;
        const std::size_t availableCharacters = std::min<std::size_t>(kMaxCharacters, static_cast<std::size_t>(end - address));
        if (availableCharacters == 0)
            return {};
        char buffer[kMaxCharacters + 1]{};
        if (!SafeReadBytes(reinterpret_cast<const void*>(address), buffer, availableCharacters))
            return {};
        std::size_t length = 0;
        for (; length < availableCharacters; ++length)
        {
            const unsigned char ch = static_cast<unsigned char>(buffer[length]);
            if (ch == 0)
                break;
            if (ch < 32 || ch > 126)
                return {};
        }
        if (length == 0 || length == availableCharacters)
            return {};
        return std::string(buffer, length);
    }

    struct PositionResearchRegion
    {
        std::uintptr_t base = 0;
        std::size_t size = 0;
        DWORD protect = 0;
        DWORD type = 0;
        std::string source;
    };

    struct PositionResearchRow
    {
        std::size_t rowIndex = 0;
        const std::vector<std::wstring>* fields = nullptr;
        bool quantityOk = false;
        bool averageOk = false;
        bool openPlOk = false;
        long long quantityAbs = 0;
        double averagePrice = 0.0;
        double displayedOpenPl = 0.0;
    };

    struct PositionResearchCandidate
    {
        std::uintptr_t averageAddress = 0;
        std::uintptr_t regionBase = 0;
        std::size_t regionSize = 0;
        std::string regionSource;
        std::vector<long long> quantityI32Offsets;
        std::vector<long long> quantityI64Offsets;
        std::vector<long long> openPlOffsets;
        int score = 0;
    };

    struct PositionResearchScanStats
    {
        std::size_t regionsConsidered = 0;
        std::size_t regionsRead = 0;
        std::uint64_t bytesRead = 0;
        std::uint64_t valuesChecked = 0;
        bool byteLimitReached = false;
        bool runtimeLimitReached = false;
    };

    bool PositionResearchRegionIsDataCandidate(const MEMORY_BASIC_INFORMATION& mbi)
    {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD basic = mbi.Protect & 0xFFu;
        const bool readable = basic == PAGE_READONLY || basic == PAGE_READWRITE || basic == PAGE_WRITECOPY;
        if (!readable)
            return false;
        return mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED;
    }

    bool AddPositionResearchRegion(
        std::vector<PositionResearchRegion>& regions,
        std::map<std::uintptr_t, std::size_t>& regionIndex,
        std::uintptr_t address,
        const std::string& source)
    {
        std::uintptr_t regionEnd = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!QueryReadableSpan(address, regionEnd, mbi) || !PositionResearchRegionIsDataCandidate(mbi))
            return false;

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        auto found = regionIndex.find(base);
        if (found != regionIndex.end())
        {
            PositionResearchRegion& existing = regions[found->second];
            if (existing.source.find(source) == std::string::npos)
                existing.source += "|" + source;
            return true;
        }

        PositionResearchRegion region;
        region.base = base;
        region.size = mbi.RegionSize;
        region.protect = mbi.Protect;
        region.type = mbi.Type;
        region.source = source;
        regionIndex[base] = regions.size();
        regions.push_back(region);
        return true;
    }

    void DiscoverPositionResearchRegions(
        const std::vector<std::pair<std::uintptr_t, std::string>>& seeds,
        std::vector<PositionResearchRegion>& regions,
        std::size_t& pointerNodesScanned,
        const std::chrono::steady_clock::time_point& deadline)
    {
        regions.clear();
        pointerNodesScanned = 0;

        std::map<std::uintptr_t, std::size_t> regionIndex;
        std::set<std::uintptr_t> queued;
        struct Pending
        {
            std::uintptr_t address = 0;
            std::size_t depth = 0;
            std::string source;
        };
        std::deque<Pending> pending;

        auto enqueue = [&](std::uintptr_t address, std::size_t depth, const std::string& source)
        {
            if (!address || address < 0x10000 || queued.size() >= 4096)
                return;
            if (!queued.insert(address).second)
                return;
            pending.push_back({ address, depth, source });
        };

        for (const auto& seed : seeds)
            enqueue(seed.first, 0, seed.second);

        constexpr std::size_t kMaximumDepth = 3;
        constexpr std::size_t kMaximumNodes = 4096;
        constexpr std::size_t kMaximumBytesPerNode = 64u * 1024u;
        constexpr std::size_t kMaximumChildrenPerNode = 128;
        unsigned char* nodeBuffer = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, kMaximumBytesPerNode, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!nodeBuffer)
            return;

        while (!pending.empty() && pointerNodesScanned < kMaximumNodes &&
               std::chrono::steady_clock::now() < deadline)
        {
            Pending node = pending.front();
            pending.pop_front();

            std::uintptr_t regionEnd = 0;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!QueryReadableSpan(node.address, regionEnd, mbi))
                continue;

            AddPositionResearchRegion(regions, regionIndex, node.address, node.source);

            const std::size_t available = static_cast<std::size_t>(regionEnd - node.address);
            const std::size_t bytesToInspect = (std::min)(kMaximumBytesPerNode, available);
            if (bytesToInspect < sizeof(std::uintptr_t))
                continue;

            if (!SafeReadBytes(reinterpret_cast<void*>(node.address), nodeBuffer, bytesToInspect))
                continue;

            ++pointerNodesScanned;
            std::size_t children = 0;
            for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= bytesToInspect; offset += sizeof(std::uintptr_t))
            {
                std::uintptr_t value = 0;
                std::memcpy(&value, nodeBuffer + offset, sizeof(value));
                if (!value || value < 0x10000)
                    continue;

                std::uintptr_t targetEnd = 0;
                MEMORY_BASIC_INFORMATION targetMbi{};
                if (!QueryReadableSpan(value, targetEnd, targetMbi) ||
                    !PositionResearchRegionIsDataCandidate(targetMbi))
                    continue;

                std::ostringstream edgeSource;
                edgeSource << node.source << "+ptr@" << HexValue(offset);
                AddPositionResearchRegion(regions, regionIndex, value, edgeSource.str());

                if (node.depth < kMaximumDepth && children < kMaximumChildrenPerNode)
                {
                    enqueue(value, node.depth + 1, edgeSource.str());
                    ++children;
                }
            }
        }
        VirtualFree(nodeBuffer, 0, MEM_RELEASE);
    }

    std::vector<PositionResearchRegion> EnumerateFallbackPositionResearchRegions(
        const std::map<std::uintptr_t, std::size_t>& alreadyKnown)
    {
        std::vector<PositionResearchRegion> result;

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
        const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);

        while (cursor < maximum)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
            {
                cursor += 0x1000;
                continue;
            }

            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::uintptr_t next = base + mbi.RegionSize;
            if (next <= cursor)
                break;

            if (PositionResearchRegionIsDataCandidate(mbi) &&
                alreadyKnown.find(base) == alreadyKnown.end() &&
                mbi.RegionSize >= 0x1000)
            {
                PositionResearchRegion region;
                region.base = base;
                region.size = mbi.RegionSize;
                region.protect = mbi.Protect;
                region.type = mbi.Type;
                region.source = "process_fallback";
                result.push_back(region);
            }

            cursor = next;
        }

        std::stable_sort(
            result.begin(),
            result.end(),
            [](const PositionResearchRegion& a, const PositionResearchRegion& b)
            {
                // Smaller committed data regions are more likely to be object/container
                // allocations and are cheaper to inspect. Large heaps remain eligible.
                if (a.size != b.size)
                    return a.size < b.size;
                return a.base < b.base;
            });

        return result;
    }

    void FindPositionResearchNeighborhood(
        const PositionResearchRow& row,
        std::uintptr_t averageAddress,
        PositionResearchCandidate& candidate)
    {
        candidate.averageAddress = averageAddress;
        constexpr long long kWindow = 0x180;

        for (long long delta = -kWindow; delta <= kWindow; delta += 4)
        {
            const std::uintptr_t address = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(averageAddress) + delta);
            std::int32_t value32 = 0;
            if (SafeReadValue(reinterpret_cast<void*>(address), value32))
            {
                const long long magnitude = value32 < 0
                    ? -static_cast<long long>(value32)
                    : static_cast<long long>(value32);
                if (magnitude == row.quantityAbs)
                    candidate.quantityI32Offsets.push_back(delta);
            }
        }

        for (long long delta = -kWindow; delta <= kWindow; delta += 8)
        {
            const std::uintptr_t address = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(averageAddress) + delta);

            std::int64_t value64 = 0;
            if (SafeReadValue(reinterpret_cast<void*>(address), value64))
            {
                const unsigned long long magnitude =
                    value64 < 0
                    ? static_cast<unsigned long long>(-(value64 + 1)) + 1ull
                    : static_cast<unsigned long long>(value64);
                if (magnitude == static_cast<unsigned long long>(row.quantityAbs))
                    candidate.quantityI64Offsets.push_back(delta);
            }

            if (row.openPlOk)
            {
                double value = 0.0;
                if (SafeReadValue(reinterpret_cast<void*>(address), value) && std::isfinite(value))
                {
                    const double tolerance = (std::max)(0.011, std::fabs(row.displayedOpenPl) * 1.0e-7);
                    if (std::fabs(value - row.displayedOpenPl) <= tolerance)
                        candidate.openPlOffsets.push_back(delta);
                }
            }
        }

        if (!candidate.quantityI32Offsets.empty() || !candidate.quantityI64Offsets.empty())
            candidate.score += 10;
        if (!candidate.openPlOffsets.empty())
            candidate.score += 8;
        if (!candidate.quantityI32Offsets.empty() && !candidate.quantityI64Offsets.empty())
            candidate.score += 1;
    }

    void ScanPositionResearchRegions(
        const std::vector<PositionResearchRegion>& regions,
        const std::vector<PositionResearchRow>& parsedRows,
        std::vector<std::vector<PositionResearchCandidate>>& rowCandidates,
        std::vector<std::set<std::uintptr_t>>& seenAverageAddresses,
        std::uint64_t byteBudget,
        const std::chrono::steady_clock::time_point& deadline,
        PositionResearchScanStats& stats)
    {
        constexpr std::size_t kChunkBytes = 4u * 1024u * 1024u;
        constexpr std::size_t kMaximumRegionBytes = 128u * 1024u * 1024u;
        constexpr std::size_t kMaximumCandidatesPerRow = 32;
        unsigned char* scanBuffer = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, kChunkBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!scanBuffer)
            return;

        for (const PositionResearchRegion& region : regions)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                stats.runtimeLimitReached = true;
                break;
            }
            if (stats.bytesRead >= byteBudget)
            {
                stats.byteLimitReached = true;
                break;
            }

            ++stats.regionsConsidered;
            const std::size_t regionLimit = (std::min)(region.size, kMaximumRegionBytes);
            std::size_t regionOffset = 0;
            bool regionReadAny = false;

            while (regionOffset < regionLimit)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    stats.runtimeLimitReached = true;
                    break;
                }
                if (stats.bytesRead >= byteBudget)
                {
                    stats.byteLimitReached = true;
                    break;
                }

                const std::uint64_t remainingBudget = byteBudget - stats.bytesRead;
                std::size_t bytesToRead = (std::min)(
                    kChunkBytes,
                    regionLimit - regionOffset);
                bytesToRead = static_cast<std::size_t>(
                    (std::min)(static_cast<std::uint64_t>(bytesToRead), remainingBudget));
                if (bytesToRead < sizeof(double))
                    break;

                const std::uintptr_t chunkAddress = region.base + regionOffset;
                if (!SafeReadBytes(reinterpret_cast<void*>(chunkAddress), scanBuffer, bytesToRead))
                {
                    regionOffset += bytesToRead;
                    continue;
                }

                regionReadAny = true;
                stats.bytesRead += bytesToRead;

                const std::size_t firstAligned =
                    static_cast<std::size_t>((8u - (chunkAddress & 7u)) & 7u);
                for (std::size_t offset = firstAligned;
                     offset + sizeof(double) <= bytesToRead;
                     offset += 8)
                {
                    double value = 0.0;
                    std::memcpy(&value, scanBuffer + offset, sizeof(value));
                    ++stats.valuesChecked;
                    if (!std::isfinite(value))
                        continue;

                    for (const PositionResearchRow& row : parsedRows)
                    {
                        if (!row.averageOk ||
                            rowCandidates[row.rowIndex].size() >= kMaximumCandidatesPerRow)
                            continue;

                        const double tolerance =
                            (std::max)(0.00051, std::fabs(row.averagePrice) * 1.0e-8);
                        if (std::fabs(value - row.averagePrice) > tolerance)
                            continue;

                        const std::uintptr_t averageAddress = chunkAddress + offset;
                        if (!seenAverageAddresses[row.rowIndex].insert(averageAddress).second)
                            continue;

                        PositionResearchCandidate candidate;
                        candidate.regionBase = region.base;
                        candidate.regionSize = region.size;
                        candidate.regionSource = region.source;
                        FindPositionResearchNeighborhood(row, averageAddress, candidate);

                        // Average Price alone is too weak. Require Quantity nearby before
                        // retaining the candidate; Open P/L match raises its confidence.
                        if (candidate.quantityI32Offsets.empty() &&
                            candidate.quantityI64Offsets.empty())
                            continue;

                        rowCandidates[row.rowIndex].push_back(candidate);
                    }
                }

                regionOffset += bytesToRead;
            }

            if (regionReadAny)
                ++stats.regionsRead;
            if (stats.runtimeLimitReached || stats.byteLimitReached)
                break;
        }
        VirtualFree(scanBuffer, 0, MEM_RELEASE);
    }

    struct PositionResearchSignatureEvidence
    {
        std::string signature;
        std::string sectionName;
        std::uintptr_t stringVa = 0;
        std::uintptr_t stringRva = 0;
        std::vector<std::uintptr_t> rawRipReferences;
    };

    struct R13CodePatternEvidence
    {
        std::uintptr_t instructionVa = 0;
        std::uintptr_t targetVa = 0;
        long long fieldOffset = 0;
        std::string kind;
    };

    struct R13StaticCodeWindow
    {
        std::uintptr_t referenceVa = 0;
        std::uintptr_t beginVa = 0;
        std::vector<unsigned char> bytes;
        std::vector<R13CodePatternEvidence> patterns;
    };

    bool FindAsciiSignatureInPeSections(
        const Snapshot& snapshot,
        const std::vector<PeSectionAnalysis>& sections,
        const std::string& signature,
        PositionResearchSignatureEvidence& evidence)
    {
        evidence = PositionResearchSignatureEvidence{};
        evidence.signature = signature;

        for (const PeSectionAnalysis& section : sections)
        {
            if (!section.readable || section.endVa <= section.beginVa)
                continue;

            std::uintptr_t cursor = section.beginVa;
            while (cursor < section.endVa)
            {
                std::uintptr_t readableEnd = 0;
                MEMORY_BASIC_INFORMATION mbi{};
                if (!QueryReadableSpan(cursor, readableEnd, mbi))
                {
                    cursor += 0x1000;
                    continue;
                }

                const std::uintptr_t spanEnd = (std::min)(readableEnd, section.endVa);
                if (spanEnd <= cursor)
                    break;
                const std::size_t span = static_cast<std::size_t>(spanEnd - cursor);
                std::vector<unsigned char> bytes(span, 0);
                if (SafeReadBytes(reinterpret_cast<void*>(cursor), bytes.data(), bytes.size()))
                {
                    const unsigned char* begin =
                        reinterpret_cast<const unsigned char*>(signature.data());
                    const unsigned char* end = begin + signature.size();
                    const auto found = std::search(bytes.begin(), bytes.end(), begin, end);
                    if (found != bytes.end())
                    {
                        evidence.sectionName = section.name;
                        evidence.stringVa = cursor +
                            static_cast<std::uintptr_t>(std::distance(bytes.begin(), found));
                        evidence.stringRva = evidence.stringVa - snapshot.atonpTrackerBase;
                        break;
                    }
                }
                cursor = spanEnd;
            }
            if (evidence.stringVa)
                break;
        }

        if (!evidence.stringVa)
            return false;

        // Record raw RIP-relative references as diagnostics only. This is not a
        // disassembler and does not promote a reference to a callable function.
        for (const PeSectionAnalysis& section : sections)
        {
            if (!section.executable || section.endVa <= section.beginVa)
                continue;
            const std::size_t sectionBytes =
                static_cast<std::size_t>(section.endVa - section.beginVa);
            std::vector<unsigned char> bytes(sectionBytes, 0);
            if (!SafeReadBytes(
                    reinterpret_cast<void*>(section.beginVa),
                    bytes.data(),
                    bytes.size()))
                continue;

            for (std::size_t i = 0; i + 7 <= bytes.size(); ++i)
            {
                const unsigned char rex = bytes[i];
                const unsigned char opcode = bytes[i + 1];
                const unsigned char modrm = bytes[i + 2];
                if (rex < 0x40 || rex > 0x4F)
                    continue;
                if (opcode != 0x8D && opcode != 0x8B)
                    continue;
                if ((modrm & 0xC7u) != 0x05u)
                    continue;

                std::int32_t displacement = 0;
                std::memcpy(&displacement, bytes.data() + i + 3, sizeof(displacement));
                const std::uintptr_t instruction = section.beginVa + i;
                const std::uintptr_t target = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(instruction + 7) + displacement);
                if (target == evidence.stringVa)
                {
                    evidence.rawRipReferences.push_back(instruction);
                    if (evidence.rawRipReferences.size() >= 16)
                        return true;
                }
            }
        }

        return true;
    }

    bool R13ReadStaticCodeWindow(
        const std::vector<PeSectionAnalysis>& sections,
        std::uintptr_t referenceVa,
        R13StaticCodeWindow& window)
    {
        window = R13StaticCodeWindow{};
        window.referenceVa = referenceVa;
        const PeSectionAnalysis* executable = nullptr;
        for (const PeSectionAnalysis& section : sections)
        {
            if (section.executable && referenceVa >= section.beginVa &&
                referenceVa < section.endVa)
            {
                executable = &section;
                break;
            }
        }
        if (!executable)
            return false;

        window.beginVa = (std::max)(
            executable->beginVa,
            referenceVa > 0x180 ? referenceVa - 0x180 : executable->beginVa);
        const std::uintptr_t endVa = (std::min)(
            executable->endVa, referenceVa + 0x280);
        if (endVa <= window.beginVa)
            return false;
        window.bytes.resize(static_cast<std::size_t>(endVa - window.beginVa));
        if (!SafeReadBytes(
                reinterpret_cast<void*>(window.beginVa),
                window.bytes.data(), window.bytes.size()))
        {
            window.bytes.clear();
            return false;
        }

        auto addField = [&](std::size_t offset, std::size_t displacementAt,
                            bool displacement32, const char* kind)
        {
            long long displacement = 0;
            if (displacement32)
            {
                std::int32_t value = 0;
                std::memcpy(&value, window.bytes.data() + displacementAt, sizeof(value));
                displacement = value;
            }
            else
            {
                displacement = static_cast<std::int8_t>(window.bytes[displacementAt]);
            }
            R13CodePatternEvidence item;
            item.instructionVa = window.beginVa + offset;
            item.fieldOffset = displacement;
            item.kind = kind;
            window.patterns.push_back(item);
        };
        auto addTarget = [&](std::size_t offset, std::size_t displacementAt,
                             std::size_t instructionLength, const char* kind)
        {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, window.bytes.data() + displacementAt, sizeof(displacement));
            R13CodePatternEvidence item;
            item.instructionVa = window.beginVa + offset;
            item.targetVa = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(item.instructionVa + instructionLength) + displacement);
            item.kind = kind;
            window.patterns.push_back(item);
        };

        // These are deliberately conservative byte-pattern candidates, not a claim
        // of complete instruction decoding. The raw bytes remain the primary evidence.
        for (std::size_t i = 0; i < window.bytes.size(); ++i)
        {
            const std::size_t remaining = window.bytes.size() - i;
            const unsigned char* p = window.bytes.data() + i;
            if (remaining >= 5 && (p[0] == 0xE8 || p[0] == 0xE9))
                addTarget(i, i + 1, 5, p[0] == 0xE8 ? "relative_call_candidate" : "relative_jump_candidate");

            if (remaining >= 4 && p[0] == 0x48 &&
                (p[1] == 0x8B || p[1] == 0x8D) && (p[2] & 0xC7u) == 0x41u)
                addField(i, i + 3, false, p[1] == 0x8B ? "this_qword_load_candidate" : "this_address_candidate");
            if (remaining >= 7 && p[0] == 0x48 &&
                (p[1] == 0x8B || p[1] == 0x8D) && (p[2] & 0xC7u) == 0x81u)
                addField(i, i + 3, true, p[1] == 0x8B ? "this_qword_load_candidate" : "this_address_candidate");
            if (remaining >= 3 && p[0] == 0x8B && (p[1] & 0xC7u) == 0x41u)
                addField(i, i + 2, false, "this_dword_load_candidate");
            if (remaining >= 6 && p[0] == 0x8B && (p[1] & 0xC7u) == 0x81u)
                addField(i, i + 2, true, "this_dword_load_candidate");
            if (remaining >= 4 && p[0] == 0x0F &&
                (p[1] == 0xB6 || p[1] == 0xB7) && (p[2] & 0xC7u) == 0x41u)
                addField(i, i + 3, false, p[1] == 0xB6 ? "this_byte_load_candidate" : "this_word_load_candidate");
            if (remaining >= 7 && p[0] == 0x0F &&
                (p[1] == 0xB6 || p[1] == 0xB7) && (p[2] & 0xC7u) == 0x81u)
                addField(i, i + 3, true, p[1] == 0xB6 ? "this_byte_load_candidate" : "this_word_load_candidate");
            if (remaining >= 5 && (p[0] == 0xF2 || p[0] == 0xF3) &&
                p[1] == 0x0F && p[2] == 0x10 && (p[3] & 0xC7u) == 0x41u)
                addField(i, i + 4, false, p[0] == 0xF2 ? "this_scalar_double_load_candidate" : "this_scalar_float_load_candidate");
            if (remaining >= 8 && (p[0] == 0xF2 || p[0] == 0xF3) &&
                p[1] == 0x0F && p[2] == 0x10 && (p[3] & 0xC7u) == 0x81u)
                addField(i, i + 4, true, p[0] == 0xF2 ? "this_scalar_double_load_candidate" : "this_scalar_float_load_candidate");

            if (remaining >= 7 && p[0] >= 0x40 && p[0] <= 0x4F &&
                (p[1] == 0x8B || p[1] == 0x8D) && (p[2] & 0xC7u) == 0x05u)
                addTarget(i, i + 3, 7, "rip_relative_target_candidate");
        }
        return true;
    }

    struct R15DispatchSlotEvidence
    {
        std::string methodName;
        std::size_t expectedSlotOffset = 0;
        std::vector<std::pair<std::uintptr_t, std::size_t>> virtualCalls;
        bool expectedSlotFound = false;
    };

    enum class R15AbiClassification
    {
        Rejected,
        Unknown,
        Plausible,
        Compatible
    };

    const char* R15AbiClassificationText(R15AbiClassification value)
    {
        switch (value)
        {
        case R15AbiClassification::Rejected: return "REJECTED";
        case R15AbiClassification::Plausible: return "PLAUSIBLE";
        case R15AbiClassification::Compatible: return "COMPATIBLE";
        default: return "UNKNOWN";
        }
    }

    struct R15TargetCodeEvidence
    {
        std::uintptr_t originalTargetVa = 0;
        std::uintptr_t effectiveTargetVa = 0;
        std::size_t thunkDepth = 0;
        std::string moduleName;
        std::uintptr_t moduleBase = 0;
        std::uintptr_t targetRva = 0;
        bool boundaryFound = false;
        std::uintptr_t functionBeginVa = 0;
        std::uintptr_t functionEndVa = 0;
        std::uintptr_t unwindInfoVa = 0;
        std::uintptr_t windowBeginVa = 0;
        std::vector<unsigned char> bytes;
        bool rdxObserved = false;
        bool directOutputWrite = false;
        bool rdxForwardedOrSaved = false;
        bool callObserved = false;
        bool trivialThisGetter = false;
        bool earlyLeafReturnWithoutRdx = false;
        R15AbiClassification classification = R15AbiClassification::Unknown;
        std::string diagnostic;
    };

    struct R15InterfaceSlotEvidence
    {
        std::size_t slotOffset = 0;
        std::string semanticName;
        std::uintptr_t targetVa = 0;
        std::string targetModule;
        std::uintptr_t targetModuleBase = 0;
        std::uintptr_t targetRva = 0;
        bool executable = false;
        R15AbiClassification abiClassification = R15AbiClassification::Unknown;
    };

    struct R15InterfaceCandidate
    {
        std::uintptr_t referenceFieldVa = 0;
        std::uintptr_t interfaceVa = 0;
        std::uintptr_t vtableVa = 0;
        std::uintptr_t vtableModuleBase = 0;
        std::uintptr_t vtableRva = 0;
        std::uintptr_t regionBase = 0;
        std::size_t regionSize = 0;
        std::string regionSource;
        std::string vtableModule;
        std::vector<R15InterfaceSlotEvidence> slots;
        std::set<std::uintptr_t> interfaceInstances;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> referenceSamples;
        std::size_t referenceCount = 0;
        std::size_t uniqueTargets = 0;
        std::size_t compatibleSlots = 0;
        std::size_t plausibleSlots = 0;
        std::size_t unknownSlots = 0;
        std::size_t rejectedSlots = 0;
        bool abiCandidate = false;
        int score = 0;
    };

    struct R15InterfaceSearchStats
    {
        std::size_t regionsConsidered = 0;
        std::size_t regionsRead = 0;
        std::uint64_t bytesRead = 0;
        std::uint64_t alignedValuesChecked = 0;
        std::size_t pointerShapedValues = 0;
        std::size_t objectPointersRead = 0;
        std::size_t vtablesInKnownModules = 0;
        std::size_t uniqueVtablesExamined = 0;
        std::size_t fullDispatchVtables = 0;
        std::size_t duplicateVtableReferences = 0;
        std::size_t uniqueTargetsAnalyzed = 0;
        std::size_t compatibleTargets = 0;
        std::size_t plausibleTargets = 0;
        std::size_t unknownTargets = 0;
        std::size_t rejectedTargets = 0;
        bool allocationFailed = false;
        bool byteLimitReached = false;
        bool runtimeLimitReached = false;
        bool uniqueVtableLimitReached = false;
        bool targetAnalysisLimitReached = false;
    };

    const std::pair<std::size_t, const char*> kR15DispatchSlots[] =
    {
        { 0x48, "AveragePrice" },
        { 0x58, "OpenPL" },
        { 0x60, "PriceScaleCode" },
        { 0x70, "CurrencyCode_or_CurrencyLetter" },
        { 0x88, "CurrencyLetterRPL" },
        { 0x90, "RealizedPL" }
    };

    R15DispatchSlotEvidence R15AnalyzeDispatchCalls(
        const std::string& methodName,
        std::size_t expectedSlotOffset,
        const std::vector<R13StaticCodeWindow>& windows)
    {
        R15DispatchSlotEvidence result;
        result.methodName = methodName;
        result.expectedSlotOffset = expectedSlotOffset;
        std::set<std::pair<std::uintptr_t, std::size_t>> unique;
        for (const R13StaticCodeWindow& window : windows)
        {
            for (std::size_t i = 0; i < window.bytes.size(); ++i)
            {
                const std::size_t remaining = window.bytes.size() - i;
                const unsigned char* p = window.bytes.data() + i;
                std::size_t slotOffset = static_cast<std::size_t>(-1);
                if (remaining >= 3 && p[0] == 0xFF && p[1] == 0x50)
                    slotOffset = p[2];
                else if (remaining >= 6 && p[0] == 0xFF && p[1] == 0x90)
                {
                    std::uint32_t displacement = 0;
                    std::memcpy(&displacement, p + 2, sizeof(displacement));
                    slotOffset = displacement;
                }
                else if (remaining >= 4 && p[0] >= 0x40 && p[0] <= 0x4F &&
                         p[1] == 0x8B && (p[2] & 0xC7u) == 0x40u)
                {
                    // mov register,[rax+disp8], followed later by call register
                    slotOffset = p[3];
                }
                else if (remaining >= 7 && p[0] >= 0x40 && p[0] <= 0x4F &&
                         p[1] == 0x8B && (p[2] & 0xC7u) == 0x80u)
                {
                    // mov register,[rax+disp32], followed later by call register
                    std::uint32_t displacement = 0;
                    std::memcpy(&displacement, p + 3, sizeof(displacement));
                    slotOffset = displacement;
                }
                if (slotOffset == static_cast<std::size_t>(-1) || slotOffset > 0x400)
                    continue;
                const auto item = std::make_pair(window.beginVa + i, slotOffset);
                if (unique.insert(item).second)
                    result.virtualCalls.push_back(item);
                if (slotOffset == expectedSlotOffset)
                    result.expectedSlotFound = true;
            }
        }
        return result;
    }

    const ModuleRecord* R15ModuleForAddress(
        const Snapshot& snapshot,
        std::uintptr_t address,
        std::size_t bytes = 1)
    {
        if (!bytes)
            return nullptr;
        for (const ModuleRecord& module : snapshot.modules)
        {
            const std::uintptr_t end = module.base + module.size;
            const std::uintptr_t requestedEnd = address + bytes;
            if (end >= module.base && requestedEnd >= address &&
                address >= module.base && requestedEnd <= end)
                return &module;
        }
        return nullptr;
    }

    std::string R15ModuleDisplayName(const ModuleRecord* module)
    {
        if (!module)
            return "unknown";
        const std::wstring display = module->name.empty()
            ? BaseName(module->path) : module->name;
        return WideToUtf8(display);
    }

    std::string R15SummarizeRegionSource(const std::string& source)
    {
        constexpr std::size_t kMaximumCharacters = 384;
        if (source.size() <= kMaximumCharacters)
            return source;
        const std::size_t pathCount = static_cast<std::size_t>(
            1 + std::count(source.begin(), source.end(), '|'));
        std::ostringstream out;
        out << source.substr(0, kMaximumCharacters)
            << "...[truncated paths=" << pathCount
            << " original_chars=" << source.size() << ']';
        return out.str();
    }

    bool R15ResolveRuntimeBoundary(
        std::uintptr_t targetVa,
        R15TargetCodeEvidence& evidence)
    {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
            static_cast<DWORD64>(targetVa), &imageBase, nullptr);
        if (!runtimeFunction || !imageBase)
            return false;
        const std::uintptr_t beginVa = static_cast<std::uintptr_t>(
            imageBase + runtimeFunction->BeginAddress);
        const std::uintptr_t endVa = static_cast<std::uintptr_t>(
            imageBase + runtimeFunction->EndAddress);
        if (endVa <= beginVa || targetVa < beginVa || targetVa >= endVa)
            return false;
        evidence.boundaryFound = true;
        evidence.functionBeginVa = beginVa;
        evidence.functionEndVa = endVa;
        evidence.unwindInfoVa = static_cast<std::uintptr_t>(
            imageBase + runtimeFunction->UnwindData);
        return true;
    }

    std::uintptr_t R15ResolveDirectThunk(
        std::uintptr_t originalTargetVa,
        std::size_t& depth)
    {
        depth = 0;
        std::uintptr_t current = originalTargetVa;
        std::set<std::uintptr_t> visited;
        for (std::size_t hop = 0; hop < 4 && visited.insert(current).second; ++hop)
        {
            unsigned char bytes[16]{};
            if (!SafeReadBytes(reinterpret_cast<void*>(current), bytes, sizeof(bytes)))
                break;
            std::uintptr_t next = 0;
            if (bytes[0] == 0xE9)
            {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, bytes + 1, sizeof(displacement));
                next = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(current + 5) + displacement);
            }
            else if (bytes[0] == 0xEB)
            {
                const std::int8_t displacement = static_cast<std::int8_t>(bytes[1]);
                next = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(current + 2) + displacement);
            }
            else if (bytes[0] == 0xFF && bytes[1] == 0x25)
            {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, bytes + 2, sizeof(displacement));
                const std::uintptr_t pointerVa = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(current + 6) + displacement);
                SafeReadValue(reinterpret_cast<void*>(pointerVa), next);
            }
            else if (bytes[0] == 0x48 && bytes[1] == 0xFF && bytes[2] == 0x25)
            {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, bytes + 3, sizeof(displacement));
                const std::uintptr_t pointerVa = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(current + 7) + displacement);
                SafeReadValue(reinterpret_cast<void*>(pointerVa), next);
            }
            if (!next || !MemoryRangeHasProtection(reinterpret_cast<void*>(next), 1, true))
                break;
            current = next;
            ++depth;
        }
        return current;
    }

    bool R15LooksLikeTrivialThisGetter(const std::vector<unsigned char>& bytes)
    {
        std::size_t i = 0;
        if (bytes.size() >= 4 && bytes[0] == 0xF3 && bytes[1] == 0x0F &&
            bytes[2] == 0x1E && bytes[3] == 0xFA)
            i = 4;
        if (i < bytes.size() && bytes[i] >= 0x40 && bytes[i] <= 0x4F)
        {
            if ((bytes[i] & 0x01u) != 0)
                return false;
            ++i;
        }
        if (i + 2 >= bytes.size())
            return false;

        std::size_t instructionEnd = 0;
        if (bytes[i] == 0x8B)
        {
            const unsigned char modrm = bytes[i + 1];
            const unsigned int mod = modrm >> 6;
            const unsigned int rm = modrm & 7u;
            if (rm != 1u || (mod != 1u && mod != 2u))
                return false;
            instructionEnd = i + 2 + (mod == 1u ? 1u : 4u);
        }
        else if (i + 3 < bytes.size() && bytes[i] == 0x0F &&
                 (bytes[i + 1] == 0xB6 || bytes[i + 1] == 0xB7))
        {
            const unsigned char modrm = bytes[i + 2];
            const unsigned int mod = modrm >> 6;
            const unsigned int rm = modrm & 7u;
            if (rm != 1u || (mod != 1u && mod != 2u))
                return false;
            instructionEnd = i + 3 + (mod == 1u ? 1u : 4u);
        }
        return instructionEnd < bytes.size() && bytes[instructionEnd] == 0xC3;
    }

    void R15AnalyzeOutputPointerAbi(R15TargetCodeEvidence& evidence)
    {
        const std::size_t limit = (std::min)(
            static_cast<std::size_t>(256), evidence.bytes.size());
        evidence.trivialThisGetter = R15LooksLikeTrivialThisGetter(evidence.bytes);
        std::size_t firstReturn = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < limit; ++i)
        {
            if (evidence.bytes[i] == 0xC3 && firstReturn == static_cast<std::size_t>(-1))
                firstReturn = i;
            if (evidence.bytes[i] == 0xE8)
                evidence.callObserved = true;
            if (i + 1 < limit && evidence.bytes[i] == 0xFF &&
                ((evidence.bytes[i + 1] >> 3) & 7u) == 2u)
                evidence.callObserved = true;

            std::size_t opcode = i;
            unsigned char rex = 0;
            if (evidence.bytes[opcode] >= 0x40 && evidence.bytes[opcode] <= 0x4F)
            {
                rex = evidence.bytes[opcode];
                ++opcode;
            }
            if (opcode + 1 >= limit)
                continue;

            const unsigned char operation = evidence.bytes[opcode];
            if (operation == 0x8B || operation == 0x89 || operation == 0x88 ||
                operation == 0x8D || operation == 0x85 || operation == 0x84 ||
                operation == 0xC6 || operation == 0xC7)
            {
                const unsigned char modrm = evidence.bytes[opcode + 1];
                const unsigned int mod = modrm >> 6;
                const unsigned int reg = (modrm >> 3) & 7u;
                const unsigned int rm = modrm & 7u;
                const bool rmIsRdx = rm == 2u && (rex & 0x01u) == 0;
                const bool regIsRdx = reg == 2u && (rex & 0x04u) == 0;
                bool rdxRead = false;
                if (operation == 0x8B || operation == 0x8D)
                    rdxRead = rmIsRdx;
                else if (operation == 0x89 || operation == 0x88)
                    rdxRead = regIsRdx || (mod != 3u && rmIsRdx);
                else if (operation == 0x85 || operation == 0x84)
                    rdxRead = rmIsRdx || regIsRdx;
                else if (operation == 0xC6 || operation == 0xC7)
                    rdxRead = mod != 3u && rmIsRdx;
                if (rdxRead)
                    evidence.rdxObserved = true;
                if (mod != 3u && rmIsRdx &&
                    (operation == 0x89 || operation == 0x88 ||
                     operation == 0xC6 || operation == 0xC7))
                    evidence.directOutputWrite = true;
                if ((operation == 0x8B || operation == 0x8D) && rmIsRdx)
                    evidence.rdxForwardedOrSaved = true;
                if ((operation == 0x89 || operation == 0x88) && regIsRdx)
                    evidence.rdxForwardedOrSaved = true;
            }

            std::size_t twoByte = opcode;
            if ((evidence.bytes[twoByte] == 0xF2 || evidence.bytes[twoByte] == 0xF3 ||
                 evidence.bytes[twoByte] == 0x66) && twoByte + 1 < limit)
                ++twoByte;
            if (twoByte + 2 < limit && evidence.bytes[twoByte] == 0x0F &&
                (evidence.bytes[twoByte + 1] == 0x11 ||
                 evidence.bytes[twoByte + 1] == 0x7F))
            {
                const unsigned char modrm = evidence.bytes[twoByte + 2];
                if ((modrm >> 6) != 3u && (modrm & 7u) == 2u)
                {
                    evidence.rdxObserved = true;
                    evidence.directOutputWrite = true;
                }
            }
        }

        evidence.earlyLeafReturnWithoutRdx =
            firstReturn != static_cast<std::size_t>(-1) && firstReturn <= 32 &&
            !evidence.rdxObserved && !evidence.callObserved;
        if (evidence.trivialThisGetter)
        {
            evidence.classification = R15AbiClassification::Rejected;
            evidence.diagnostic = "trivial this-field getter ignores the required RDX output pointer";
        }
        else if (evidence.earlyLeafReturnWithoutRdx)
        {
            evidence.classification = R15AbiClassification::Rejected;
            evidence.diagnostic = "early leaf return has no RDX output-pointer evidence";
        }
        else if (evidence.directOutputWrite)
        {
            evidence.classification = R15AbiClassification::Compatible;
            evidence.diagnostic = "direct write through an RDX-based output address observed";
        }
        else if (evidence.rdxObserved &&
                 (evidence.rdxForwardedOrSaved || evidence.callObserved))
        {
            evidence.classification = R15AbiClassification::Plausible;
            evidence.diagnostic = "RDX is preserved or forwarded on a call-capable path";
        }
        else
        {
            evidence.classification = R15AbiClassification::Unknown;
            evidence.diagnostic = evidence.bytes.empty()
                ? "target code could not be read"
                : "no decisive output-pointer evidence in the bounded raw window";
        }
    }

    R15TargetCodeEvidence R15AnalyzeTargetCode(
        const Snapshot& snapshot,
        std::uintptr_t targetVa)
    {
        R15TargetCodeEvidence evidence;
        evidence.originalTargetVa = targetVa;
        evidence.effectiveTargetVa = R15ResolveDirectThunk(targetVa, evidence.thunkDepth);
        const ModuleRecord* module = R15ModuleForAddress(
            snapshot, evidence.effectiveTargetVa);
        evidence.moduleName = R15ModuleDisplayName(module);
        if (module)
        {
            evidence.moduleBase = module->base;
            evidence.targetRva = evidence.effectiveTargetVa - module->base;
        }
        R15ResolveRuntimeBoundary(evidence.effectiveTargetVa, evidence);
        evidence.windowBeginVa = evidence.boundaryFound
            ? evidence.functionBeginVa : evidence.effectiveTargetVa;
        std::size_t byteCount = 256;
        if (evidence.boundaryFound)
            byteCount = static_cast<std::size_t>((std::min)(
                static_cast<std::uintptr_t>(384),
                evidence.functionEndVa - evidence.windowBeginVa));
        if (module)
            byteCount = static_cast<std::size_t>((std::min)(
                static_cast<std::uintptr_t>(byteCount),
                module->base + module->size - evidence.windowBeginVa));
        if (byteCount)
        {
            evidence.bytes.resize(byteCount);
            if (!SafeReadBytes(
                    reinterpret_cast<void*>(evidence.windowBeginVa),
                    evidence.bytes.data(), evidence.bytes.size()))
                evidence.bytes.clear();
        }
        R15AnalyzeOutputPointerAbi(evidence);
        return evidence;
    }

    int R15VtableModuleBonus(const std::string& moduleName)
    {
        if (_stricmp(moduleName.c_str(), "ATCenterProxy.dll") == 0) return 100;
        if (_stricmp(moduleName.c_str(), "ATCenterCommon.dll") == 0) return 80;
        if (_stricmp(moduleName.c_str(), "Objects.dll") == 0) return 60;
        if (_stricmp(moduleName.c_str(), "ATOnPTracker.dll") == 0) return 20;
        return 0;
    }

    void R15ApplyAbiEvidenceAndSort(
        std::vector<R15InterfaceCandidate>& candidates,
        const std::map<std::uintptr_t, R15TargetCodeEvidence>& targetEvidence)
    {
        for (R15InterfaceCandidate& candidate : candidates)
        {
            candidate.compatibleSlots = 0;
            candidate.plausibleSlots = 0;
            candidate.unknownSlots = 0;
            candidate.rejectedSlots = 0;
            bool currencySlotsHaveAbiEvidence = true;
            for (R15InterfaceSlotEvidence& slot : candidate.slots)
            {
                const auto found = targetEvidence.find(slot.targetVa);
                slot.abiClassification = found == targetEvidence.end()
                    ? R15AbiClassification::Unknown : found->second.classification;
                switch (slot.abiClassification)
                {
                case R15AbiClassification::Compatible: ++candidate.compatibleSlots; break;
                case R15AbiClassification::Plausible: ++candidate.plausibleSlots; break;
                case R15AbiClassification::Rejected: ++candidate.rejectedSlots; break;
                default: ++candidate.unknownSlots; break;
                }
                if ((slot.slotOffset == 0x70 || slot.slotOffset == 0x88) &&
                    slot.abiClassification != R15AbiClassification::Compatible &&
                    slot.abiClassification != R15AbiClassification::Plausible)
                    currencySlotsHaveAbiEvidence = false;
            }
            candidate.abiCandidate = candidate.rejectedSlots == 0 &&
                currencySlotsHaveAbiEvidence &&
                candidate.compatibleSlots + candidate.plausibleSlots >= 4;
            candidate.score = R15VtableModuleBonus(candidate.vtableModule) +
                static_cast<int>(candidate.compatibleSlots * 40) +
                static_cast<int>(candidate.plausibleSlots * 25) +
                static_cast<int>(candidate.unknownSlots * 2) -
                static_cast<int>(candidate.rejectedSlots * 80) +
                static_cast<int>(candidate.uniqueTargets * 3) +
                static_cast<int>((std::min)(
                    static_cast<std::size_t>(20), candidate.interfaceInstances.size()));
        }
        std::stable_sort(
            candidates.begin(), candidates.end(),
            [](const R15InterfaceCandidate& a, const R15InterfaceCandidate& b)
            {
                if (a.abiCandidate != b.abiCandidate) return a.abiCandidate > b.abiCandidate;
                if (a.score != b.score) return a.score > b.score;
                if (a.rejectedSlots != b.rejectedSlots) return a.rejectedSlots < b.rejectedSlots;
                return a.vtableVa < b.vtableVa;
            });
    }

    void R15FindUniqueInterfaceVtables(
        const Snapshot& snapshot,
        const std::vector<PositionResearchRegion>& regions,
        std::vector<R15InterfaceCandidate>& candidates,
        R15InterfaceSearchStats& stats)
    {
        candidates.clear();
        stats = R15InterfaceSearchStats{};
        constexpr std::uint64_t kByteLimit = 512ull * 1024ull * 1024ull;
        constexpr std::size_t kChunkBytes = 4u * 1024u * 1024u;
        constexpr std::size_t kUniqueVtableLimit = 512;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::map<std::uintptr_t, std::size_t> retainedVtableIndexes;
        std::set<std::uintptr_t> rejectedVtables;
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        const std::uintptr_t maximumUserAddress =
            reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
        unsigned char* buffer = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, kChunkBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!buffer)
        {
            stats.allocationFailed = true;
            return;
        }

        for (const PositionResearchRegion& region : regions)
        {
            ++stats.regionsConsidered;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                stats.runtimeLimitReached = true;
                break;
            }
            if (stats.bytesRead >= kByteLimit)
            {
                stats.byteLimitReached = true;
                break;
            }
            bool regionRead = false;
            for (std::size_t regionOffset = 0; regionOffset < region.size;)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    stats.runtimeLimitReached = true;
                    break;
                }
                const std::size_t remaining = region.size - regionOffset;
                std::size_t bytesToRead = (std::min)(kChunkBytes, remaining);
                bytesToRead = static_cast<std::size_t>((std::min)(
                    static_cast<std::uint64_t>(bytesToRead), kByteLimit - stats.bytesRead));
                if (bytesToRead < sizeof(std::uintptr_t))
                {
                    stats.byteLimitReached = true;
                    break;
                }
                const std::uintptr_t chunkVa = region.base + regionOffset;
                if (!SafeReadBytes(reinterpret_cast<void*>(chunkVa), buffer, bytesToRead))
                {
                    regionOffset += bytesToRead;
                    continue;
                }
                regionRead = true;
                stats.bytesRead += bytesToRead;
                for (std::size_t offset = 0;
                     offset + sizeof(std::uintptr_t) <= bytesToRead;
                     offset += sizeof(std::uintptr_t))
                {
                    ++stats.alignedValuesChecked;
                    std::uintptr_t interfaceVa = 0;
                    std::memcpy(&interfaceVa, buffer + offset, sizeof(interfaceVa));
                    if (interfaceVa < 0x10000 || interfaceVa > maximumUserAddress ||
                        (interfaceVa & (sizeof(std::uintptr_t) - 1)) != 0)
                        continue;
                    ++stats.pointerShapedValues;

                    std::uintptr_t vtableVa = 0;
                    if (!SafeReadValue(reinterpret_cast<void*>(interfaceVa), vtableVa))
                        continue;
                    ++stats.objectPointersRead;
                    if (vtableVa < 0x10000 || vtableVa > maximumUserAddress ||
                        !R15ModuleForAddress(snapshot, vtableVa, 0x98))
                        continue;
                    ++stats.vtablesInKnownModules;

                    const auto retained = retainedVtableIndexes.find(vtableVa);
                    if (retained != retainedVtableIndexes.end())
                    {
                        R15InterfaceCandidate& candidate = candidates[retained->second];
                        ++candidate.referenceCount;
                        candidate.interfaceInstances.insert(interfaceVa);
                        if (candidate.referenceSamples.size() < 8)
                            candidate.referenceSamples.push_back(
                                {chunkVa + offset, interfaceVa});
                        ++stats.duplicateVtableReferences;
                        continue;
                    }
                    if (rejectedVtables.find(vtableVa) != rejectedVtables.end())
                        continue;
                    ++stats.uniqueVtablesExamined;

                    R15InterfaceCandidate candidate;
                    candidate.referenceFieldVa = chunkVa + offset;
                    candidate.interfaceVa = interfaceVa;
                    candidate.vtableVa = vtableVa;
                    candidate.regionBase = region.base;
                    candidate.regionSize = region.size;
                    candidate.regionSource = R15SummarizeRegionSource(region.source);
                    const ModuleRecord* vtableModule = R15ModuleForAddress(snapshot, vtableVa);
                    candidate.vtableModule = R15ModuleDisplayName(vtableModule);
                    if (vtableModule)
                    {
                        candidate.vtableModuleBase = vtableModule->base;
                        candidate.vtableRva = vtableVa - vtableModule->base;
                    }
                    candidate.referenceCount = 1;
                    candidate.interfaceInstances.insert(interfaceVa);
                    candidate.referenceSamples.push_back({chunkVa + offset, interfaceVa});
                    std::set<std::uintptr_t> uniqueTargets;
                    bool fullShape = true;
                    for (const auto& required : kR15DispatchSlots)
                    {
                        R15InterfaceSlotEvidence slot;
                        slot.slotOffset = required.first;
                        slot.semanticName = required.second;
                        if (!SafeReadValue(
                                reinterpret_cast<void*>(vtableVa + slot.slotOffset),
                                slot.targetVa) ||
                            !MemoryRangeHasProtection(
                                reinterpret_cast<void*>(slot.targetVa), 1, true))
                        {
                            fullShape = false;
                            break;
                        }
                        slot.executable = true;
                        const ModuleRecord* targetModule = R15ModuleForAddress(
                            snapshot, slot.targetVa);
                        slot.targetModule = R15ModuleDisplayName(targetModule);
                        if (targetModule)
                        {
                            slot.targetModuleBase = targetModule->base;
                            slot.targetRva = slot.targetVa - targetModule->base;
                        }
                        uniqueTargets.insert(slot.targetVa);
                        candidate.slots.push_back(slot);
                    }
                    if (!fullShape || candidate.slots.size() !=
                        sizeof(kR15DispatchSlots) / sizeof(kR15DispatchSlots[0]))
                    {
                        rejectedVtables.insert(vtableVa);
                        continue;
                    }
                    ++stats.fullDispatchVtables;
                    candidate.uniqueTargets = uniqueTargets.size();
                    if (candidate.uniqueTargets < 4)
                    {
                        rejectedVtables.insert(vtableVa);
                        continue;
                    }
                    if (candidates.size() >= kUniqueVtableLimit)
                    {
                        stats.uniqueVtableLimitReached = true;
                        rejectedVtables.insert(vtableVa);
                        continue;
                    }
                    retainedVtableIndexes[vtableVa] = candidates.size();
                    candidates.push_back(std::move(candidate));
                }
                regionOffset += bytesToRead;
            }
            if (regionRead)
                ++stats.regionsRead;
            if (stats.runtimeLimitReached || stats.byteLimitReached)
                break;
        }
        VirtualFree(buffer, 0, MEM_RELEASE);
    }

    std::string PositionResearchOffsetText(long long offset)
    {
        std::ostringstream out;
        if (offset < 0)
            out << '-';
        else
            out << '+';
        out << HexValue(static_cast<std::uintptr_t>(offset < 0 ? -offset : offset));
        return out.str();
    }

    std::string PositionResearchJoinOffsets(const std::vector<long long>& offsets)
    {
        if (offsets.empty())
            return "none";
        std::ostringstream out;
        for (std::size_t i = 0; i < offsets.size(); ++i)
        {
            if (i)
                out << ',';
            out << PositionResearchOffsetText(offsets[i]);
        }
        return out.str();
    }

    void AppendPositionResearchNeighborhood(
        std::ostringstream& out,
        const PositionResearchRow& row,
        const PositionResearchCandidate& candidate)
    {
        out << "    quantity_i32_offsets_from_average="
            << PositionResearchJoinOffsets(candidate.quantityI32Offsets) << "\r\n"
            << "    quantity_i64_offsets_from_average="
            << PositionResearchJoinOffsets(candidate.quantityI64Offsets) << "\r\n"
            << "    displayed_open_pl_offsets_from_average="
            << PositionResearchJoinOffsets(candidate.openPlOffsets) << "\r\n";

        out << "    small_integer_fields_near_average:\r\n";
        std::size_t smallIntegerCount = 0;
        for (long long delta = -0x400; delta <= 0x800; delta += 4)
        {
            const std::uintptr_t address = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(candidate.averageAddress) + delta);
            std::int32_t value = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(address), value))
                continue;
            if (value <= 0 || value > 64)
                continue;
            ++smallIntegerCount;
            out << "      offset=" << PositionResearchOffsetText(delta)
                << " value=" << value << "\r\n";
        }
        if (!smallIntegerCount)
            out << "      none\r\n";

        out << "    nearby_pointer_strings:\r\n";
        std::size_t pointerStringCount = 0;
        for (long long delta = -0x800; delta <= 0x1000; delta += 8)
        {
            const std::uintptr_t fieldAddress = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(candidate.averageAddress) + delta);
            std::uintptr_t pointerValue = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(fieldAddress), pointerValue) || !pointerValue)
                continue;

            std::wstring directWide = ReadShortUtf16Candidate(pointerValue);
            std::string directAscii = ReadShortAsciiCandidate(pointerValue);
            std::uintptr_t indirect = 0;
            std::wstring indirectWide;
            std::string indirectAscii;
            std::uintptr_t indirect2 = 0;
            std::wstring indirect2Wide;
            std::string indirect2Ascii;

            if (SafeReadValue(reinterpret_cast<void*>(pointerValue), indirect) && indirect)
            {
                indirectWide = ReadShortUtf16Candidate(indirect);
                indirectAscii = ReadShortAsciiCandidate(indirect);
                if (SafeReadValue(reinterpret_cast<void*>(indirect), indirect2) && indirect2)
                {
                    indirect2Wide = ReadShortUtf16Candidate(indirect2);
                    indirect2Ascii = ReadShortAsciiCandidate(indirect2);
                }
            }

            if (directWide.empty() && directAscii.empty() &&
                indirectWide.empty() && indirectAscii.empty() &&
                indirect2Wide.empty() && indirect2Ascii.empty())
                continue;

            ++pointerStringCount;
            out << "      offset=" << PositionResearchOffsetText(delta)
                << " raw=" << HexValue(pointerValue);
            if (!directWide.empty())
                out << " direct_utf16=" << V153EscapeField(directWide)
                    << " direct_currency_hint=" << V153EscapeField(PositionResearchCurrencyHint(directWide));
            if (!directAscii.empty())
            {
                const std::wstring wide(directAscii.begin(), directAscii.end());
                out << " direct_ascii=" << V153EscapeFieldUtf8(directAscii)
                    << " direct_ascii_currency_hint=" << V153EscapeField(PositionResearchCurrencyHint(wide));
            }
            if (!indirectWide.empty())
                out << " indirect_utf16=" << V153EscapeField(indirectWide)
                    << " indirect_currency_hint=" << V153EscapeField(PositionResearchCurrencyHint(indirectWide));
            if (!indirectAscii.empty())
            {
                const std::wstring wide(indirectAscii.begin(), indirectAscii.end());
                out << " indirect_ascii=" << V153EscapeFieldUtf8(indirectAscii)
                    << " indirect_ascii_currency_hint=" << V153EscapeField(PositionResearchCurrencyHint(wide));
            }
            if (!indirect2Wide.empty())
                out << " indirect2_utf16=" << V153EscapeField(indirect2Wide)
                    << " indirect2_currency_hint=" << V153EscapeField(PositionResearchCurrencyHint(indirect2Wide));
            if (!indirect2Ascii.empty())
            {
                const std::wstring wide(indirect2Ascii.begin(), indirect2Ascii.end());
                out << " indirect2_ascii=" << V153EscapeFieldUtf8(indirect2Ascii)
                    << " indirect2_ascii_currency_hint=" << V153EscapeField(PositionResearchCurrencyHint(wide));
            }
            out << "\r\n";
        }
        if (!pointerStringCount)
            out << "      none\r\n";

        out << "    nearby_double_fields:\r\n";
        std::size_t doubleCount = 0;
        for (long long delta = -0x400; delta <= 0x800; delta += 8)
        {
            const std::uintptr_t address = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(candidate.averageAddress) + delta);
            double value = 0.0;
            if (!SafeReadValue(reinterpret_cast<void*>(address), value) ||
                !std::isfinite(value) ||
                std::fabs(value) > 1.0e12 ||
                (std::fabs(value) < 1.0e-12 && value != 0.0))
                continue;

            ++doubleCount;
            out << "      offset=" << PositionResearchOffsetText(delta)
                << " value=" << std::setprecision(17) << value;
            const double averageTolerance =
                (std::max)(0.00051, std::fabs(row.averagePrice) * 1.0e-8);
            if (std::fabs(value - row.averagePrice) <= averageTolerance)
                out << " tag=AVERAGE_PRICE_MATCH";
            if (row.openPlOk)
            {
                const double pnlTolerance =
                    (std::max)(0.011, std::fabs(row.displayedOpenPl) * 1.0e-7);
                if (std::fabs(value - row.displayedOpenPl) <= pnlTolerance)
                    out << " tag=DISPLAYED_OPEN_PL_MATCH";
            }
            out << "\r\n";
        }
        if (!doubleCount)
            out << "      none\r\n";
    }

    struct R6ValidatedPositionTable
    {
        std::uintptr_t firstRecord = 0;
        std::uintptr_t firstAverage = 0;
        std::uintptr_t regionBase = 0;
        std::size_t regionSize = 0;
        std::string regionSource;
        std::size_t rowsMatched = 0;
        std::size_t openPlRowsMatched = 0;
    };

    struct R6OwnerBackReference
    {
        std::uintptr_t fieldAddress = 0;
        std::uintptr_t pointerValue = 0;
        std::size_t rowIndex = 0;
        std::uintptr_t regionBase = 0;
        std::string regionSource;
    };

    bool R6NumberMatches(double actual, double expected, double minimumTolerance)
    {
        if (!std::isfinite(actual))
            return false;
        const double tolerance = (std::max)(minimumTolerance, std::fabs(expected) * 1.0e-7);
        return std::fabs(actual - expected) <= tolerance;
    }

    bool R6ValidateStrideTable(
        const std::vector<PositionResearchRow>& rows,
        const PositionResearchCandidate& firstCandidate,
        R6ValidatedPositionTable& table)
    {
        if (rows.empty() || firstCandidate.averageAddress < 8)
            return false;

        R6ValidatedPositionTable candidate;
        candidate.firstAverage = firstCandidate.averageAddress;
        candidate.firstRecord = firstCandidate.averageAddress - 0x20;
        candidate.regionBase = firstCandidate.regionBase;
        candidate.regionSize = firstCandidate.regionSize;
        candidate.regionSource = firstCandidate.regionSource;

        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const PositionResearchRow& row = rows[i];
            if (!row.quantityOk || !row.averageOk)
                return false;

            const std::uintptr_t averageAddress = candidate.firstAverage + i * 0x30;
            std::int32_t quantity = 0;
            double average = 0.0;
            double openPl = 0.0;
            if (!SafeReadValue(reinterpret_cast<void*>(averageAddress - 0x08), quantity) ||
                !SafeReadValue(reinterpret_cast<void*>(averageAddress), average) ||
                !SafeReadValue(reinterpret_cast<void*>(averageAddress + 0x08), openPl))
                return false;

            const long long quantityMagnitude = quantity < 0
                ? -static_cast<long long>(quantity)
                : static_cast<long long>(quantity);
            if (quantityMagnitude != row.quantityAbs ||
                !R6NumberMatches(average, row.averagePrice, 0.00051))
                return false;

            ++candidate.rowsMatched;
            if (row.openPlOk && R6NumberMatches(openPl, row.displayedOpenPl, 0.011))
                ++candidate.openPlRowsMatched;
        }

        if (candidate.rowsMatched != rows.size() ||
            candidate.openPlRowsMatched != rows.size())
            return false;
        table = candidate;
        return true;
    }

    std::vector<R6ValidatedPositionTable> R6FindValidatedTables(
        const std::vector<PositionResearchRow>& rows,
        const std::vector<std::vector<PositionResearchCandidate>>& rowCandidates)
    {
        std::vector<R6ValidatedPositionTable> tables;
        if (rows.empty() || rows.front().rowIndex >= rowCandidates.size())
            return tables;

        std::set<std::uintptr_t> seen;
        for (const PositionResearchCandidate& candidate : rowCandidates[rows.front().rowIndex])
        {
            R6ValidatedPositionTable table;
            if (R6ValidateStrideTable(rows, candidate, table) &&
                seen.insert(table.firstAverage).second)
                tables.push_back(table);
        }
        return tables;
    }

    std::vector<R6ValidatedPositionTable> R7SearchValidatedTablesProcessWide(
        const std::vector<PositionResearchRow>& rows,
        PositionResearchScanStats& stats,
        std::size_t& regionsEnumerated)
    {
        std::vector<R6ValidatedPositionTable> tables;
        regionsEnumerated = 0;
        if (rows.empty() || !rows.front().averageOk)
            return tables;

        std::map<std::uintptr_t, std::size_t> noExcludedRegions;
        const std::vector<PositionResearchRegion> regions =
            EnumerateFallbackPositionResearchRegions(noExcludedRegions);
        regionsEnumerated = regions.size();
        std::set<std::uintptr_t> seenFirstAverages;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(150);
        constexpr std::uint64_t kByteBudget = 3ull * 1024ull * 1024ull * 1024ull;
        constexpr std::size_t kChunkBytes = 4u * 1024u * 1024u;
        constexpr std::size_t kMaximumTables = 8;
        unsigned char* tableScanBuffer = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, kChunkBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!tableScanBuffer)
            return tables;

        for (const PositionResearchRegion& region : regions)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                stats.runtimeLimitReached = true;
                break;
            }
            if (stats.bytesRead >= kByteBudget)
            {
                stats.byteLimitReached = true;
                break;
            }

            ++stats.regionsConsidered;
            bool readAny = false;
            for (std::size_t regionOffset = 0; regionOffset < region.size; )
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    stats.runtimeLimitReached = true;
                    break;
                }
                const std::uint64_t remainingBudget = kByteBudget - stats.bytesRead;
                std::size_t bytesToRead = (std::min)(kChunkBytes, region.size - regionOffset);
                bytesToRead = static_cast<std::size_t>((std::min)(
                    remainingBudget, static_cast<std::uint64_t>(bytesToRead)));
                if (bytesToRead < sizeof(double))
                    break;

                const std::uintptr_t chunkAddress = region.base + regionOffset;
                if (!SafeReadBytes(reinterpret_cast<void*>(chunkAddress), tableScanBuffer, bytesToRead))
                {
                    regionOffset += bytesToRead;
                    continue;
                }
                readAny = true;
                stats.bytesRead += bytesToRead;
                const std::size_t firstAligned = static_cast<std::size_t>((8u - (chunkAddress & 7u)) & 7u);
                for (std::size_t offset = firstAligned; offset + sizeof(double) <= bytesToRead; offset += 8)
                {
                    double value = 0.0;
                    std::memcpy(&value, tableScanBuffer + offset, sizeof(value));
                    ++stats.valuesChecked;
                    if (!R6NumberMatches(value, rows.front().averagePrice, 0.00051))
                        continue;

                    const std::uintptr_t averageAddress = chunkAddress + offset;
                    if (!seenFirstAverages.insert(averageAddress).second)
                        continue;
                    PositionResearchCandidate candidate;
                    candidate.averageAddress = averageAddress;
                    candidate.regionBase = region.base;
                    candidate.regionSize = region.size;
                    candidate.regionSource = "R7_process_wide_table_search";
                    R6ValidatedPositionTable table;
                    if (R6ValidateStrideTable(rows, candidate, table))
                    {
                        tables.push_back(table);
                        if (tables.size() >= kMaximumTables)
                            break;
                    }
                }
                regionOffset += bytesToRead;
                if (tables.size() >= kMaximumTables)
                    break;
            }
            if (readAny)
                ++stats.regionsRead;
            if (stats.runtimeLimitReached || stats.byteLimitReached || tables.size() >= kMaximumTables)
                break;
        }
        VirtualFree(tableScanBuffer, 0, MEM_RELEASE);
        return tables;
    }

    struct R12CleanBackReferenceStats
    {
        std::size_t rawHits = 0;
        std::size_t excludedRegions = 0;
        std::size_t excludedRegionsEncountered = 0;
        std::size_t rawCapacityDrops = 0;
        std::size_t postScanValueMismatches = 0;
        std::size_t postScanUnreadable = 0;
        std::size_t duplicatePairsRemoved = 0;
        std::size_t historicalSelfRecordsRejected = 0;
        std::size_t selfRecordShapeMatches = 0;
        std::size_t cleanCandidateRegions = 0;
        std::size_t fullRowCoverageRegions = 0;
        std::size_t currentResearchAddressesSupplied = 0;
        std::size_t currentResearchRegionsExcluded = 0;
        std::size_t verifiedHits = 0;
    };

    bool R12AddExcludedRegion(
        std::set<std::uintptr_t>& excludedBases,
        const void* address)
    {
        if (!address)
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        return excludedBases.insert(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)).second;
    }

    void R12AddExcludedAllocation(
        std::set<std::uintptr_t>& excludedBases,
        const void* address)
    {
        MEMORY_BASIC_INFORMATION origin{};
        if (!address || VirtualQuery(address, &origin, sizeof(origin)) != sizeof(origin) ||
            !origin.AllocationBase)
            return;

        std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(origin.AllocationBase);
        const void* allocationBase = origin.AllocationBase;
        for (;;)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                mbi.AllocationBase != allocationBase || !mbi.RegionSize)
                break;
            excludedBases.insert(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress));
            const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (next <= cursor)
                break;
            cursor = next;
        }
    }

    bool R12IsKnownMcstResearchSource(const std::string& value)
    {
        return value.find("process_fallback") != std::string::npos ||
            value.find("R7_process_wide_table_search") != std::string::npos ||
            value.find("object_graph") != std::string::npos ||
            value.find("validated_stride") != std::string::npos ||
            value.find("independent_quantity_average") != std::string::npos;
    }

    bool R12IsHistoricalMcstBackReference(
        std::uintptr_t hitAddress,
        std::size_t expectedRow,
        std::string& sourceEvidence)
    {
        sourceEvidence.clear();
        std::size_t storedRow = static_cast<std::size_t>(-1);
        std::uintptr_t storedRegionBase = 0;
        std::uintptr_t sourcePointer = 0;
        if (!SafeReadValue(reinterpret_cast<void*>(hitAddress + 0x08), storedRow) ||
            !SafeReadValue(reinterpret_cast<void*>(hitAddress + 0x10), storedRegionBase) ||
            !SafeReadValue(reinterpret_cast<void*>(hitAddress + 0x18), sourcePointer) ||
            storedRow != expectedRow || !storedRegionBase ||
            (storedRegionBase & 0xFFFu) != 0 || !sourcePointer)
            return false;

        sourceEvidence = ReadShortAsciiCandidate(sourcePointer);
        if (!R12IsKnownMcstResearchSource(sourceEvidence))
            return false;

        return true;
    }

    void R12FindCleanTargetBackReferences(
        const std::map<std::uintptr_t, std::size_t>& targetRows,
        const std::vector<const void*>& currentResearchAddresses,
        std::vector<R6OwnerBackReference>& references,
        std::vector<R6OwnerBackReference>& rejectedSelfRecords,
        PositionResearchScanStats& stats,
        R12CleanBackReferenceStats& cleanStats)
    {
        references.clear();
        rejectedSelfRecords.clear();
        cleanStats = R12CleanBackReferenceStats{};
        if (targetRows.empty())
            return;

        struct RawHit
        {
            std::uintptr_t fieldAddress;
            std::uintptr_t pointerValue;
            std::size_t rowIndex;
            std::uintptr_t regionBase;
        };
        constexpr std::size_t kMaximumRawHits = 16384;
        constexpr std::size_t kChunkBytes = 4u * 1024u * 1024u;
        RawHit* rawHits = static_cast<RawHit*>(VirtualAlloc(
            nullptr, kMaximumRawHits * sizeof(RawHit), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        unsigned char* scanBuffer = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, kChunkBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!rawHits || !scanBuffer)
        {
            if (rawHits) VirtualFree(rawHits, 0, MEM_RELEASE);
            if (scanBuffer) VirtualFree(scanBuffer, 0, MEM_RELEASE);
            return;
        }

        std::set<std::uintptr_t> excludedBases;
        R12AddExcludedRegion(excludedBases, rawHits);
        R12AddExcludedRegion(excludedBases, scanBuffer);
        R12AddExcludedRegion(excludedBases, &targetRows);
        R12AddExcludedRegion(excludedBases, &references);
        R12AddExcludedRegion(excludedBases, &rejectedSelfRecords);
        R12AddExcludedRegion(excludedBases, &stats);
        R12AddExcludedRegion(excludedBases, &cleanStats);
        R12AddExcludedRegion(excludedBases, &currentResearchAddresses);
        if (!currentResearchAddresses.empty())
            R12AddExcludedRegion(excludedBases, currentResearchAddresses.data());
        cleanStats.currentResearchAddressesSupplied = currentResearchAddresses.size();
        for (const void* address : currentResearchAddresses)
        {
            if (R12AddExcludedRegion(excludedBases, address))
                ++cleanStats.currentResearchRegionsExcluded;
        }
        int stackMarker = 0;
        R12AddExcludedAllocation(excludedBases, &stackMarker);
        for (const auto& target : targetRows)
            R12AddExcludedRegion(excludedBases, &target);

        std::map<std::uintptr_t, std::size_t> noExcludedRegions;
        const std::vector<PositionResearchRegion> regions =
            EnumerateFallbackPositionResearchRegions(noExcludedRegions);
        R12AddExcludedRegion(excludedBases, &regions);
        if (!regions.empty())
            R12AddExcludedRegion(excludedBases, regions.data());
        cleanStats.excludedRegions = excludedBases.size();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        constexpr std::uint64_t kByteBudget = 3ull * 1024ull * 1024ull * 1024ull;
        std::size_t rawHitCount = 0;

        for (const PositionResearchRegion& region : regions)
        {
            if (excludedBases.find(region.base) != excludedBases.end())
            {
                ++cleanStats.excludedRegionsEncountered;
                continue;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                stats.runtimeLimitReached = true;
                break;
            }
            if (stats.bytesRead >= kByteBudget)
            {
                stats.byteLimitReached = true;
                break;
            }
            ++stats.regionsConsidered;
            bool readAny = false;
            for (std::size_t regionOffset = 0; regionOffset < region.size; )
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    stats.runtimeLimitReached = true;
                    break;
                }
                const std::uint64_t remainingBudget = kByteBudget - stats.bytesRead;
                std::size_t bytesToRead = (std::min)(kChunkBytes, region.size - regionOffset);
                bytesToRead = static_cast<std::size_t>((std::min)(
                    remainingBudget, static_cast<std::uint64_t>(bytesToRead)));
                if (bytesToRead < sizeof(std::uintptr_t))
                    break;
                const std::uintptr_t chunkAddress = region.base + regionOffset;
                if (!SafeReadBytes(reinterpret_cast<void*>(chunkAddress), scanBuffer, bytesToRead))
                {
                    regionOffset += bytesToRead;
                    continue;
                }
                readAny = true;
                stats.bytesRead += bytesToRead;
                const std::size_t firstAligned = static_cast<std::size_t>(
                    (sizeof(std::uintptr_t) - (chunkAddress & (sizeof(std::uintptr_t) - 1))) &
                    (sizeof(std::uintptr_t) - 1));
                for (std::size_t offset = firstAligned;
                     offset + sizeof(std::uintptr_t) <= bytesToRead;
                     offset += sizeof(std::uintptr_t))
                {
                    std::uintptr_t value = 0;
                    std::memcpy(&value, scanBuffer + offset, sizeof(value));
                    ++stats.valuesChecked;
                    const auto target = targetRows.find(value);
                    if (target == targetRows.end())
                        continue;
                    const std::uintptr_t fieldAddress = chunkAddress + offset;
                    if (rawHitCount < kMaximumRawHits)
                    {
                        rawHits[rawHitCount++] = { fieldAddress, value, target->second, region.base };
                    }
                    else
                    {
                        ++cleanStats.rawCapacityDrops;
                    }
                }
                regionOffset += bytesToRead;
            }
            if (readAny)
                ++stats.regionsRead;
            if (stats.runtimeLimitReached || stats.byteLimitReached)
                break;
        }

        cleanStats.rawHits = rawHitCount;
        std::set<std::pair<std::uintptr_t, std::uintptr_t>> verifiedPairs;
        references.reserve(rawHitCount);
        for (std::size_t i = 0; i < rawHitCount; ++i)
        {
            const RawHit& raw = rawHits[i];
            std::uintptr_t currentValue = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(raw.fieldAddress), currentValue))
            {
                ++cleanStats.postScanUnreadable;
                continue;
            }
            if (currentValue != raw.pointerValue)
            {
                ++cleanStats.postScanValueMismatches;
                continue;
            }
            std::string historicalSource;
            if (R12IsHistoricalMcstBackReference(raw.fieldAddress, raw.rowIndex, historicalSource))
            {
                ++cleanStats.selfRecordShapeMatches;
                ++cleanStats.historicalSelfRecordsRejected;
                rejectedSelfRecords.push_back({
                    raw.fieldAddress,
                    raw.pointerValue,
                    raw.rowIndex,
                    raw.regionBase,
                    "historical_mcst_self_record:" + historicalSource });
                continue;
            }
            if (!verifiedPairs.insert({ raw.fieldAddress, raw.pointerValue }).second)
            {
                ++cleanStats.duplicatePairsRemoved;
                continue;
            }
            references.push_back({
                raw.fieldAddress,
                raw.pointerValue,
                raw.rowIndex,
                raw.regionBase,
                "process_fallback_verified_after_scan" });
        }
        cleanStats.verifiedHits = references.size();
        std::map<std::uintptr_t, std::set<std::size_t>> rowsByRegion;
        for (const R6OwnerBackReference& reference : references)
            rowsByRegion[reference.regionBase].insert(reference.rowIndex);
        cleanStats.cleanCandidateRegions = rowsByRegion.size();
        for (const auto& regionRows : rowsByRegion)
        {
            if (regionRows.second.size() == targetRows.size())
                ++cleanStats.fullRowCoverageRegions;
        }
        std::stable_sort(references.begin(), references.end(),
            [](const R6OwnerBackReference& a, const R6OwnerBackReference& b)
            {
                if (a.fieldAddress != b.fieldAddress)
                    return a.fieldAddress < b.fieldAddress;
                return a.pointerValue < b.pointerValue;
            });
        VirtualFree(scanBuffer, 0, MEM_RELEASE);
        VirtualFree(rawHits, 0, MEM_RELEASE);
    }

    void R6FindUniqueOwnerBackReferences(
        const R6ValidatedPositionTable& table,
        const std::vector<PositionResearchRow>& rows,
        std::vector<R6OwnerBackReference>& references,
        PositionResearchScanStats& stats)
    {
        references.clear();
        std::map<std::uintptr_t, std::size_t> noExcludedRegions;
        const std::vector<PositionResearchRegion> regions =
            EnumerateFallbackPositionResearchRegions(noExcludedRegions);
        std::set<std::pair<std::uintptr_t, std::uintptr_t>> uniquePairs;
        std::set<std::size_t> referencedRows;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        constexpr std::uint64_t kByteBudget = 1024ull * 1024ull * 1024ull;
        constexpr std::size_t kMaximumUniqueReferences = 1024;
        constexpr std::size_t kChunkBytes = 4u * 1024u * 1024u;

        for (const PositionResearchRegion& region : regions)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                stats.runtimeLimitReached = true;
                break;
            }
            if (stats.bytesRead >= kByteBudget || references.size() >= kMaximumUniqueReferences)
            {
                stats.byteLimitReached = stats.bytesRead >= kByteBudget;
                break;
            }
            ++stats.regionsConsidered;
            bool readAny = false;
            for (std::size_t regionOffset = 0; regionOffset < region.size; )
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    stats.runtimeLimitReached = true;
                    break;
                }
                const std::uint64_t remainingBudget = kByteBudget - stats.bytesRead;
                std::size_t bytesToRead = (std::min)(kChunkBytes, region.size - regionOffset);
                bytesToRead = static_cast<std::size_t>((std::min)(remainingBudget, static_cast<std::uint64_t>(bytesToRead)));
                if (bytesToRead < sizeof(std::uintptr_t))
                    break;

                const std::uintptr_t chunkAddress = region.base + regionOffset;
                std::vector<unsigned char> bytes(bytesToRead);
                if (!SafeReadBytes(reinterpret_cast<void*>(chunkAddress), bytes.data(), bytes.size()))
                {
                    regionOffset += bytesToRead;
                    continue;
                }
                readAny = true;
                stats.bytesRead += bytes.size();
                const std::size_t firstAligned = static_cast<std::size_t>(
                    (sizeof(std::uintptr_t) - (chunkAddress & (sizeof(std::uintptr_t) - 1))) &
                    (sizeof(std::uintptr_t) - 1));
                for (std::size_t offset = firstAligned;
                     offset + sizeof(std::uintptr_t) <= bytes.size();
                     offset += sizeof(std::uintptr_t))
                {
                    std::uintptr_t value = 0;
                    std::memcpy(&value, bytes.data() + offset, sizeof(value));
                    ++stats.valuesChecked;
                    if (value < table.firstAverage)
                        continue;
                    const std::uintptr_t delta = value - table.firstAverage;
                    if ((delta % 0x30) != 0)
                        continue;
                    const std::size_t rowIndex = static_cast<std::size_t>(delta / 0x30);
                    if (rowIndex >= rows.size())
                        continue;

                    const std::uintptr_t fieldAddress = chunkAddress + offset;
                    if (!uniquePairs.insert({ fieldAddress, value }).second)
                        continue;
                    references.push_back({ fieldAddress, value, rowIndex, region.base, region.source });
                    referencedRows.insert(rowIndex);
                    if (references.size() >= kMaximumUniqueReferences)
                        break;
                }
                regionOffset += bytesToRead;
                if (references.size() >= kMaximumUniqueReferences)
                    break;
            }
            if (readAny)
                ++stats.regionsRead;
            if (stats.runtimeLimitReached || stats.byteLimitReached ||
                references.size() >= kMaximumUniqueReferences)
                break;
        }

        std::stable_sort(references.begin(), references.end(),
            [](const R6OwnerBackReference& a, const R6OwnerBackReference& b)
            {
                if (a.fieldAddress != b.fieldAddress)
                    return a.fieldAddress < b.fieldAddress;
                return a.pointerValue < b.pointerValue;
            });
    }

    void R6AppendOwnerRecordNeighborhood(
        std::ostringstream& out,
        const R6OwnerBackReference& reference,
        std::uintptr_t familyAnchor)
    {
        const std::uintptr_t distance = reference.fieldAddress - familyAnchor;
        out << "OWNER_RECORD field_address=" << HexValue(reference.fieldAddress)
            << " pointer_value=" << HexValue(reference.pointerValue)
            << " row=" << reference.rowIndex
            << " family_anchor=" << HexValue(familyAnchor)
            << " delta_from_anchor=" << HexValue(distance)
            << " stride_0x90_slot=";
        if ((distance % 0x90) == 0)
            out << (distance / 0x90);
        else
            out << "NOT_ALIGNED";
        out << " region_base=" << HexValue(reference.regionBase)
            << " region_source=" << V153EscapeFieldUtf8(reference.regionSource) << "\r\n";

        for (long long delta = -0x400; delta <= 0x800; delta += 8)
        {
            const std::uintptr_t address = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(reference.fieldAddress) + delta);
            std::uintptr_t value = 0;
            if (!SafeReadValue(reinterpret_cast<void*>(address), value))
                continue;
            const std::wstring wide = ReadShortUtf16Candidate(value);
            const std::string ascii = ReadShortAsciiCandidate(value);
            if (value == reference.pointerValue || !wide.empty() || !ascii.empty())
            {
                out << "  field_offset=" << PositionResearchOffsetText(delta)
                    << " raw=" << HexValue(value);
                if (value == reference.pointerValue)
                    out << " tag=POSITION_ROW_AVERAGE_POINTER";
                if (!wide.empty())
                    out << " utf16=" << V153EscapeField(wide)
                        << " currency_hint=" << V153EscapeField(PositionResearchCurrencyHint(wide));
                if (!ascii.empty())
                {
                    const std::wstring asciiWide(ascii.begin(), ascii.end());
                    out << " ascii=" << V153EscapeFieldUtf8(ascii)
                        << " currency_hint=" << V153EscapeField(PositionResearchCurrencyHint(asciiWide));
                }
                out << "\r\n";
            }
        }
    }

    void R12AppendRawReferenceLayout(
        std::ostringstream& out,
        const R6OwnerBackReference& reference)
    {
        out << "RAW_LAYOUT field_address=" << HexValue(reference.fieldAddress)
            << " row=" << reference.rowIndex << "\r\n";
        for (long long delta = -0x10; delta <= 0x40; delta += 8)
        {
            const std::uintptr_t address = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(reference.fieldAddress) + delta);
            std::uintptr_t value = 0;
            out << "  qword_offset=" << PositionResearchOffsetText(delta)
                << " address=" << HexValue(address);
            if (!SafeReadValue(reinterpret_cast<void*>(address), value))
            {
                out << " status=UNREADABLE\r\n";
                continue;
            }
            out << " raw=" << HexValue(value);
            if (value == reference.pointerValue)
                out << " tag=POSITION_ROW_AVERAGE_POINTER";
            if (delta == 0x08 && value == reference.rowIndex)
                out << " tag=POSSIBLE_R6_ROW_INDEX";
            if (delta == 0x10 && value && (value & 0xFFFu) == 0)
                out << " tag=POSSIBLE_R6_REGION_BASE";
            const std::string ascii = ReadShortAsciiCandidate(value);
            const std::wstring wide = ReadShortUtf16Candidate(value);
            if (!ascii.empty())
                out << " ascii=" << V153EscapeFieldUtf8(ascii);
            if (!wide.empty())
                out << " utf16=" << V153EscapeField(wide)
                    << " currency_hint=" << V153EscapeField(PositionResearchCurrencyHint(wide));
            out << "\r\n";
        }
    }

    bool WritePositionCurrencyDirectResearchR15(
        const Snapshot& snapshot,
        std::string& summaryJson)
    {
        V153GridReadLockGuard lock;
        CreateDirectoryW(kOutputDirectory, nullptr);
        const std::wstring reportPath =
            ReportPath(L"MCST_Position_Currency_Dynamic", snapshot.processId);
        const std::wstring checkpointPath =
            ReportPath(L"MCST_Position_Currency_R15_Checkpoint", snapshot.processId);
        auto writeR15PhaseCheckpoint = [&](const char* phase, const std::string& evidence)
        {
            std::ostringstream phaseOut;
            phaseOut << "MCST POSITION CURRENCY R15 CHECKPOINT\r\n"
                     << "research_build=1.114-R15\r\n"
                     << "bridge_version=" << kBridgeVersion << "\r\n"
                     << "read_only=yes\r\n"
                     << "phase_status=RUNNING\r\n"
                     << "current_phase=" << phase << "\r\n"
                     << evidence;
            WriteUtf8File(checkpointPath, phaseOut.str());
        };
        constexpr DWORD kResearchAtonpTimestamp = 0x6A5694FBu;
        constexpr DWORD kResearchAtonpImageSize = 3534848u;

        if (snapshot.atonpTrackerPeTimestamp != kResearchAtonpTimestamp ||
            snapshot.atonpTrackerSize != kResearchAtonpImageSize)
        {
            std::ostringstream mismatch;
            mismatch << "MCST POSITION CURRENCY DYNAMIC RESEARCH\r\n"
                     << "=======================================\r\n"
                     << "research_build=1.114-R15\r\n"
                     << "status=BLOCKED_FINGERPRINT_MISMATCH\r\n"
                     << "expected_atonptracker_pe_timestamp=" << HexValue(kResearchAtonpTimestamp) << "\r\n"
                     << "expected_atonptracker_image_size=" << kResearchAtonpImageSize << "\r\n"
                     << "actual_atonptracker_pe_timestamp=" << HexValue(snapshot.atonpTrackerPeTimestamp) << "\r\n"
                     << "actual_atonptracker_image_size=" << snapshot.atonpTrackerSize << "\r\n"
                     << "reason=R15 interface ABI research is fingerprint-scoped and is not reused on a different ATOnPTracker build.\r\n";
            const bool mismatchWritten = WriteUtf8File(reportPath, mismatch.str());
            std::ostringstream mismatchSummary;
            mismatchSummary << "{\"capture\":\"position_currency_dynamic_research\",\"version\":170,"
                            << "\"blocked\":true,\"reason\":\"fingerprint_mismatch\","
                            << "\"report_written\":" << (mismatchWritten ? "true" : "false") << ','
                            << "\"report_path\":" << JsonString(reportPath) << '}';
            summaryJson = mismatchSummary.str();
            return false;
        }

        TrackerCompatibilityProfile trackerProfile =
            ResolveExternalTrackerCompatibilityProfile(snapshot);
        if (!trackerProfile.matched)
        {
            TrackerCompatibilityProfile embedded =
                EmbeddedLegacyTrackerCompatibilityProfile(snapshot);
            if (embedded.matched)
                trackerProfile = embedded;
        }

        std::string tabViewDiagnostic;
        const std::uintptr_t tabView = trackerProfile.matched
            ? FindTabViewObject(snapshot, tabViewDiagnostic, &trackerProfile)
            : 0;
        std::string flexGridRttiDiagnostic;
        const std::vector<RttiVtableRecord> flexGridRttiVtables =
            trackerProfile.matched
            ? ResolveRttiVtables(
                snapshot,
                ".?AVCFlexGridImpl@implementation@UILayer@@",
                flexGridRttiDiagnostic)
            : std::vector<RttiVtableRecord>{};

        V153GridSectionResult positions = ReadV153GridSection(
            snapshot,
            flexGridRttiVtables,
            tabView,
            trackerProfile,
            "open_positions",
            trackerProfile.openPositionsPageOffset,
            1,
            8,
            1000,
            1000,
            3);

        ExtractorAttempt positionProbe =
            BuildExtractorProbe(snapshot, "position_currency_dynamic", kExtractOpenPositionsRva);
        const std::uintptr_t tradeInfo = positionProbe.tradeInfo;
        std::uintptr_t root98 = 0;
        std::uintptr_t root88 = 0;
        if (tradeInfo)
        {
            SafeReadValue(reinterpret_cast<void*>(tradeInfo + 0x98), root98);
            SafeReadValue(reinterpret_cast<void*>(tradeInfo + 0x88), root88);
        }

        std::uintptr_t pageObject = 0;
        std::uintptr_t gridObject = 0;
        if (tabView && trackerProfile.matched)
        {
            SafeReadValue(
                reinterpret_cast<void*>(tabView + trackerProfile.openPositionsPageOffset),
                pageObject);
            if (pageObject)
            {
                SafeReadValue(
                    reinterpret_cast<void*>(pageObject + trackerProfile.gridMemberOffset),
                    gridObject);
            }
        }

        std::vector<PositionResearchRow> parsedRows;
        parsedRows.reserve(positions.rows.size());
        for (std::size_t rowIndex = 0; rowIndex < positions.rows.size(); ++rowIndex)
        {
            const auto& row = positions.rows[rowIndex];
            if (row.size() < 8)
                continue;

            PositionResearchRow parsed;
            parsed.rowIndex = rowIndex;
            parsed.fields = &row;
            double quantity = 0.0;
            parsed.quantityOk =
                TryParsePositionResearchNumber(row[4], quantity);
            parsed.averageOk =
                TryParsePositionResearchNumber(row[5], parsed.averagePrice);
            parsed.openPlOk =
                TryParsePositionResearchNumber(row[6], parsed.displayedOpenPl);
            if (parsed.quantityOk)
            {
                const long long signedQuantity =
                    static_cast<long long>(std::llround(quantity));
                parsed.quantityAbs =
                    signedQuantity < 0 ? -signedQuantity : signedQuantity;
            }
            if (parsed.quantityOk && parsed.averageOk && parsed.quantityAbs > 0)
                parsedRows.push_back(parsed);
        }

        std::vector<std::pair<std::uintptr_t, std::string>> seeds;
        if (tradeInfo) seeds.push_back({ tradeInfo, "ITC_TradeInfo" });
        if (root98) seeds.push_back({ root98, "ITC_TradeInfo+0x98" });
        if (root88) seeds.push_back({ root88, "ITC_TradeInfo+0x88" });
        if (tabView) seeds.push_back({ tabView, "CATPTTabView" });
        if (pageObject) seeds.push_back({ pageObject, "OpenPositionsPage" });
        if (gridObject) seeds.push_back({ gridObject, "OpenPositionsGrid" });

        {
            std::ostringstream evidence;
            evidence << "visible_rows=" << positions.rows.size() << "\r\n"
                     << "parsed_rows=" << parsedRows.size() << "\r\n";
            writeR15PhaseCheckpoint("independent_record_discovery", evidence.str());
        }

        std::vector<PositionResearchRegion> graphRegions;
        std::size_t pointerNodesScanned = 0;
        DiscoverPositionResearchRegions(
            seeds,
            graphRegions,
            pointerNodesScanned,
            std::chrono::steady_clock::now() + std::chrono::seconds(8));

        std::vector<std::vector<PositionResearchCandidate>> rowCandidates(
            positions.rows.size());
        std::vector<std::set<std::uintptr_t>> seenAverageAddresses(
            positions.rows.size());

        const auto scanStart = std::chrono::steady_clock::now();
        const auto deadline = scanStart + std::chrono::seconds(75);

        PositionResearchScanStats graphStats;
        ScanPositionResearchRegions(
            graphRegions,
            parsedRows,
            rowCandidates,
            seenAverageAddresses,
            256ull * 1024ull * 1024ull,
            deadline,
            graphStats);

        std::size_t rowsWithGraphCandidates = 0;
        for (const PositionResearchRow& row : parsedRows)
        {
            if (!rowCandidates[row.rowIndex].empty())
                ++rowsWithGraphCandidates;
        }

        std::map<std::uintptr_t, std::size_t> graphRegionIndex;
        for (std::size_t i = 0; i < graphRegions.size(); ++i)
            graphRegionIndex[graphRegions[i].base] = i;

        PositionResearchScanStats fallbackStats;
        std::vector<PositionResearchRegion> fallbackRegions;
        if (rowsWithGraphCandidates < parsedRows.size() &&
            std::chrono::steady_clock::now() < deadline)
        {
            fallbackRegions =
                EnumerateFallbackPositionResearchRegions(graphRegionIndex);
            ScanPositionResearchRegions(
                fallbackRegions,
                parsedRows,
                rowCandidates,
                seenAverageAddresses,
                768ull * 1024ull * 1024ull,
                deadline,
                fallbackStats);
        }

        for (const PositionResearchRow& row : parsedRows)
        {
            auto& candidates = rowCandidates[row.rowIndex];
            std::stable_sort(
                candidates.begin(),
                candidates.end(),
                [](const PositionResearchCandidate& a, const PositionResearchCandidate& b)
                {
                    if (a.score != b.score)
                        return a.score > b.score;
                    if (a.openPlOffsets.size() != b.openPlOffsets.size())
                        return a.openPlOffsets.size() > b.openPlOffsets.size();
                    return a.averageAddress < b.averageAddress;
                });
            if (candidates.size() > 8)
                candidates.resize(8);
        }

        PositionResearchScanStats tableSearchStats;
        std::size_t tableSearchRegionsEnumerated = 0;
        writeR15PhaseCheckpoint(
            "broad_process_wide_stride_table_search",
            "byte_budget=3221225472\r\nruntime_limit_seconds=150\r\n");
        const std::vector<R6ValidatedPositionTable> validatedTables =
            R7SearchValidatedTablesProcessWide(
                parsedRows, tableSearchStats, tableSearchRegionsEnumerated);

        std::map<std::uintptr_t, std::size_t> ownerTargets;
        std::string ownerTargetSource;
        if (!validatedTables.empty())
        {
            ownerTargetSource = "validated_stride_0x30_table";
            for (std::size_t i = 0; i < parsedRows.size(); ++i)
                ownerTargets[validatedTables.front().firstAverage + i * 0x30] = i;
        }
        else
        {
            ownerTargetSource = "independent_quantity_average_open_pl_records";
            for (const PositionResearchRow& row : parsedRows)
            {
                if (row.rowIndex < rowCandidates.size() &&
                    !rowCandidates[row.rowIndex].empty())
                {
                    ownerTargets[rowCandidates[row.rowIndex].front().averageAddress] = row.rowIndex;
                }
            }
        }
        std::vector<R6OwnerBackReference> ownerReferences;
        std::vector<R6OwnerBackReference> rejectedSelfRecords;
        PositionResearchScanStats ownerReferenceStats;
        R12CleanBackReferenceStats cleanBackReferenceStats;
        std::vector<const void*> currentResearchAddresses;
        currentResearchAddresses.reserve(256);
        auto addResearchObject = [&](const void* address)
        {
            if (address)
                currentResearchAddresses.push_back(address);
        };
        addResearchObject(&parsedRows);
        if (!parsedRows.empty()) addResearchObject(parsedRows.data());
        addResearchObject(&rowCandidates);
        if (!rowCandidates.empty()) addResearchObject(rowCandidates.data());
        for (const auto& candidates : rowCandidates)
        {
            addResearchObject(&candidates);
            if (!candidates.empty()) addResearchObject(candidates.data());
            for (const PositionResearchCandidate& candidate : candidates)
            {
                addResearchObject(&candidate);
                addResearchObject(candidate.regionSource.data());
                if (!candidate.quantityI32Offsets.empty()) addResearchObject(candidate.quantityI32Offsets.data());
                if (!candidate.quantityI64Offsets.empty()) addResearchObject(candidate.quantityI64Offsets.data());
                if (!candidate.openPlOffsets.empty()) addResearchObject(candidate.openPlOffsets.data());
            }
        }
        addResearchObject(&seenAverageAddresses);
        if (!seenAverageAddresses.empty()) addResearchObject(seenAverageAddresses.data());
        for (const auto& addresses : seenAverageAddresses)
        {
            addResearchObject(&addresses);
            for (const std::uintptr_t& address : addresses)
                addResearchObject(&address);
        }
        addResearchObject(&graphRegions);
        if (!graphRegions.empty()) addResearchObject(graphRegions.data());
        for (const PositionResearchRegion& region : graphRegions)
            addResearchObject(region.source.data());
        addResearchObject(&fallbackRegions);
        if (!fallbackRegions.empty()) addResearchObject(fallbackRegions.data());
        for (const PositionResearchRegion& region : fallbackRegions)
            addResearchObject(region.source.data());
        addResearchObject(&validatedTables);
        if (!validatedTables.empty()) addResearchObject(validatedTables.data());
        for (const R6ValidatedPositionTable& table : validatedTables)
            addResearchObject(table.regionSource.data());
        addResearchObject(&graphRegionIndex);
        for (const auto& entry : graphRegionIndex)
            addResearchObject(&entry);
        addResearchObject(&seeds);
        if (!seeds.empty()) addResearchObject(seeds.data());
        for (const auto& seed : seeds)
            addResearchObject(seed.second.data());
        {
            std::ostringstream evidence;
            evidence << "validated_stride_tables=" << validatedTables.size() << "\r\n"
                     << "table_search_regions_enumerated=" << tableSearchRegionsEnumerated << "\r\n"
                     << "table_search_bytes_read=" << tableSearchStats.bytesRead << "\r\n"
                     << "owner_target_source=" << ownerTargetSource << "\r\n"
                     << "owner_target_count=" << ownerTargets.size() << "\r\n"
                     << "current_research_addresses=" << currentResearchAddresses.size() << "\r\n"
                     << "byte_budget=3221225472\r\n"
                     << "runtime_limit_seconds=120\r\n";
            writeR15PhaseCheckpoint("prepare_static_method_analysis", evidence.str());
        }
        // R15 intentionally stops repeating the process-wide owner-pointer search.
        // R12 proved that its survivors were MCST research containers. Preserve the
        // earlier numeric/table evidence, then spend the next capture on static code.
        {
            std::ostringstream evidence;
            evidence << "unique_owner_back_references=" << ownerReferences.size() << "\r\n"
                     << "raw_hits=" << cleanBackReferenceStats.rawHits << "\r\n"
                     << "excluded_regions=" << cleanBackReferenceStats.excludedRegions << "\r\n"
                     << "historical_self_records_rejected=" << cleanBackReferenceStats.historicalSelfRecordsRejected << "\r\n"
                     << "current_research_regions_excluded=" << cleanBackReferenceStats.currentResearchRegionsExcluded << "\r\n"
                     << "genuine_candidates=" << cleanBackReferenceStats.verifiedHits << "\r\n"
                     << "post_scan_value_mismatches=" << cleanBackReferenceStats.postScanValueMismatches << "\r\n";
            writeR15PhaseCheckpoint("static_method_code_windows_and_final_report", evidence.str());
        }

        std::vector<PeSectionAnalysis> sections;
        std::string peSectionDiagnostic;
        const bool sectionsParsed =
            ParsePeSections(snapshot, sections, peSectionDiagnostic);

        const char* methodSignatures[] =
        {
            "ATOnPTracker::COpenPositionInfoExtractor::AveragePrice",
            "ATOnPTracker::COpenPositionInfoExtractor::OpenPL",
            "ATOnPTracker::COpenPositionInfoExtractor::RealizedPL",
            "ATOnPTracker::COpenPositionInfoExtractor::CurrencyCode",
            "ATOnPTracker::COpenPositionInfoExtractor::CurrencyLetter",
            "ATOnPTracker::COpenPositionInfoExtractor::CurrencyLetterRPL",
            "ATOnPTracker::COpenPositionInfoExtractor::PriceScaleCode"
        };
        std::vector<PositionResearchSignatureEvidence> signatureEvidence;
        std::vector<std::vector<R13StaticCodeWindow>> staticCodeEvidence;
        for (const char* signature : methodSignatures)
        {
            PositionResearchSignatureEvidence evidence;
            if (sectionsParsed)
                FindAsciiSignatureInPeSections(
                    snapshot, sections, signature, evidence);
            else
                evidence.signature = signature;
            signatureEvidence.push_back(evidence);
            std::vector<R13StaticCodeWindow> methodWindows;
            for (std::uintptr_t reference : evidence.rawRipReferences)
            {
                R13StaticCodeWindow window;
                if (R13ReadStaticCodeWindow(sections, reference, window))
                    methodWindows.push_back(std::move(window));
            }
            staticCodeEvidence.push_back(std::move(methodWindows));
        }

        const std::size_t expectedDispatchOffsets[] =
        {
            0x48, // AveragePrice
            0x58, // OpenPL
            0x90, // RealizedPL
            0x70, // CurrencyCode
            0x70, // CurrencyLetter
            0x88, // CurrencyLetterRPL
            0x60  // PriceScaleCode
        };
        std::vector<R15DispatchSlotEvidence> dispatchEvidence;
        std::size_t verifiedDispatchMethods = 0;
        for (std::size_t methodIndex = 0;
             methodIndex < signatureEvidence.size(); ++methodIndex)
        {
            R15DispatchSlotEvidence dispatch = R15AnalyzeDispatchCalls(
                signatureEvidence[methodIndex].signature,
                expectedDispatchOffsets[methodIndex],
                staticCodeEvidence[methodIndex]);
            if (dispatch.expectedSlotFound)
                ++verifiedDispatchMethods;
            dispatchEvidence.push_back(std::move(dispatch));
        }

        writeR15PhaseCheckpoint(
            "unique_interface_vtable_and_abi_search",
            "search_scope=known_anchor_graph_regions\r\n"
            "byte_limit=536870912\r\nruntime_limit_seconds=30\r\n"
            "unique_vtable_limit=512\r\ntarget_code_limit=2048\r\n");
        std::vector<R15InterfaceCandidate> interfaceCandidates;
        R15InterfaceSearchStats interfaceSearchStats;
        R15FindUniqueInterfaceVtables(
            snapshot, graphRegions, interfaceCandidates, interfaceSearchStats);

        std::map<std::uintptr_t, std::set<std::string>> targetSemantics;
        for (const R15InterfaceCandidate& candidate : interfaceCandidates)
            for (const R15InterfaceSlotEvidence& slot : candidate.slots)
                targetSemantics[slot.targetVa].insert(slot.semanticName);

        constexpr std::size_t kTargetCodeLimit = 2048;
        std::map<std::uintptr_t, R15TargetCodeEvidence> targetCodeEvidence;
        for (const auto& target : targetSemantics)
        {
            if (targetCodeEvidence.size() >= kTargetCodeLimit)
            {
                interfaceSearchStats.targetAnalysisLimitReached = true;
                break;
            }
            R15TargetCodeEvidence evidence = R15AnalyzeTargetCode(
                snapshot, target.first);
            switch (evidence.classification)
            {
            case R15AbiClassification::Compatible:
                ++interfaceSearchStats.compatibleTargets;
                break;
            case R15AbiClassification::Plausible:
                ++interfaceSearchStats.plausibleTargets;
                break;
            case R15AbiClassification::Rejected:
                ++interfaceSearchStats.rejectedTargets;
                break;
            default:
                ++interfaceSearchStats.unknownTargets;
                break;
            }
            targetCodeEvidence[target.first] = std::move(evidence);
        }
        interfaceSearchStats.uniqueTargetsAnalyzed = targetCodeEvidence.size();
        R15ApplyAbiEvidenceAndSort(interfaceCandidates, targetCodeEvidence);
        const std::size_t abiCandidateCount = static_cast<std::size_t>(std::count_if(
            interfaceCandidates.begin(), interfaceCandidates.end(),
            [](const R15InterfaceCandidate& candidate) { return candidate.abiCandidate; }));

        std::ostringstream out;
        out << "MCST POSITION CURRENCY DYNAMIC RESEARCH\r\n"
            << "=======================================\r\n"
            << "research_build=1.114-R15\r\n"
            << "bridge_version=" << kBridgeVersion << "\r\n"
            << "bridge_protocol=" << mcbridge::kProtocolVersion << "\r\n"
            << "read_only=yes\r\n"
            << "unknown_functions_called=no\r\n"
            << "purpose=group live interface objects by unique vtable and test every required target for the extractor RDX output-pointer ABI\r\n"
            << "process_id=" << snapshot.processId << "\r\n"
            << "atonptracker_pe_timestamp=" << HexValue(snapshot.atonpTrackerPeTimestamp) << "\r\n"
            << "atonptracker_image_size=" << snapshot.atonpTrackerSize << "\r\n"
            << "tracker_profile=" << V153EscapeField(trackerProfile.name) << "\r\n"
            << "tracker_profile_mode=" << V153EscapeField(trackerProfile.mode) << "\r\n"
            << "open_positions_grid_ok=" << (positions.ok ? "yes" : "no") << "\r\n"
            << "open_positions_grid_diagnostic=" << V153EscapeField(positions.diagnostic) << "\r\n"
            << "ITC_TradeInfo=" << HexValue(tradeInfo) << "\r\n"
            << "positions_root_98=" << HexValue(root98) << "\r\n"
            << "positions_root_88=" << HexValue(root88) << "\r\n"
            << "open_positions_page_object=" << HexValue(pageObject) << "\r\n"
            << "open_positions_grid_object=" << HexValue(gridObject) << "\r\n"
            << "parsed_reference_rows=" << parsedRows.size() << "\r\n"
            << "pointer_graph_nodes_scanned=" << pointerNodesScanned << "\r\n"
            << "pointer_graph_regions_discovered=" << graphRegions.size() << "\r\n"
            << "fallback_regions_enumerated=" << fallbackRegions.size() << "\r\n"
            << "graph_scan_regions_considered=" << graphStats.regionsConsidered << "\r\n"
            << "graph_scan_regions_read=" << graphStats.regionsRead << "\r\n"
            << "graph_scan_bytes_read=" << graphStats.bytesRead << "\r\n"
            << "graph_scan_values_checked=" << graphStats.valuesChecked << "\r\n"
            << "fallback_scan_regions_considered=" << fallbackStats.regionsConsidered << "\r\n"
            << "fallback_scan_regions_read=" << fallbackStats.regionsRead << "\r\n"
            << "fallback_scan_bytes_read=" << fallbackStats.bytesRead << "\r\n"
            << "fallback_scan_values_checked=" << fallbackStats.valuesChecked << "\r\n"
            << "scan_runtime_limit_reached="
            << ((graphStats.runtimeLimitReached || fallbackStats.runtimeLimitReached) ? "yes" : "no") << "\r\n"
            << "scan_byte_limit_reached="
            << ((graphStats.byteLimitReached || fallbackStats.byteLimitReached) ? "yes" : "no") << "\r\n\r\n";

        out << "STATIC EXTRACTOR EVIDENCE\r\n"
            << "-------------------------\r\n"
            << "pe_section_parse=" << (sectionsParsed ? "ok" : "failed")
            << " diagnostic=" << V153EscapeFieldUtf8(peSectionDiagnostic) << "\r\n";
        for (const PositionResearchSignatureEvidence& evidence : signatureEvidence)
        {
            out << evidence.signature
                << " diagnostic_string_rva=" << HexValue(evidence.stringRva)
                << " section=" << (evidence.sectionName.empty() ? "NOT_FOUND" : evidence.sectionName)
                << " raw_rip_reference_count=" << evidence.rawRipReferences.size();
            if (!evidence.rawRipReferences.empty())
            {
                out << " raw_rip_reference_rvas=";
                for (std::size_t i = 0; i < evidence.rawRipReferences.size(); ++i)
                {
                    if (i) out << ',';
                    out << HexValue(evidence.rawRipReferences[i] - snapshot.atonpTrackerBase);
                }
            }
            out << "\r\n";
        }
        out << "NOTE: diagnostic strings and raw RIP references prove static presence only. "
               "R15 does not call these unknown internal extractor functions.\r\n\r\n";

        out << "R15 VERIFIED EXTRACTOR VTABLE DISPATCH\r\n"
            << "--------------------------------------\r\n"
            << "verification=raw_call_[rax+offset]_or_load_[rax+offset]_then_indirect_call_opcode_scan\r\n"
            << "verified_methods=" << verifiedDispatchMethods << "\r\n"
            << "expected_methods=" << dispatchEvidence.size() << "\r\n";
        for (const R15DispatchSlotEvidence& dispatch : dispatchEvidence)
        {
            out << "METHOD " << dispatch.methodName
                << " expected_vtable_slot=" << HexValue(dispatch.expectedSlotOffset)
                << " expected_slot_found=" << (dispatch.expectedSlotFound ? "yes" : "no")
                << " dispatch_access_count=" << dispatch.virtualCalls.size() << "\r\n";
            for (const auto& call : dispatch.virtualCalls)
                out << "  DISPATCH_ACCESS instruction_rva="
                    << HexValue(call.first - snapshot.atonpTrackerBase)
                    << " vtable_slot=" << HexValue(call.second) << "\r\n";
        }
        out << "NOTE: CurrencyCode and CurrencyLetter are independently verified at the shared +0x70 slot; CurrencyLetterRPL is verified at +0x88.\r\n\r\n";

        out << "R15 UNIQUE INTERFACE VTABLE AND ABI SEARCH\r\n"
            << "------------------------------------------\r\n"
            << "search_scope=known_anchor_graph_regions\r\n"
            << "search_unit=unique_vtable_not_object_instance\r\n"
            << "fallback_process_wide_search=no\r\n"
            << "required_executable_slots=0x48,0x58,0x60,0x70,0x88,0x90\r\n"
            << "minimum_unique_targets=4\r\n"
            << "abi_requirement=callee must consume, preserve, forward, or write through the RDX output pointer\r\n"
            << "abi_analysis=bounded raw x64 heuristic plus direct-thunk following\r\n"
            << "regions_considered=" << interfaceSearchStats.regionsConsidered << "\r\n"
            << "regions_read=" << interfaceSearchStats.regionsRead << "\r\n"
            << "bytes_read=" << interfaceSearchStats.bytesRead << "\r\n"
            << "aligned_values_checked=" << interfaceSearchStats.alignedValuesChecked << "\r\n"
            << "pointer_shaped_values=" << interfaceSearchStats.pointerShapedValues << "\r\n"
            << "object_pointers_read=" << interfaceSearchStats.objectPointersRead << "\r\n"
            << "vtables_in_known_modules=" << interfaceSearchStats.vtablesInKnownModules << "\r\n"
            << "unique_vtables_examined=" << interfaceSearchStats.uniqueVtablesExamined << "\r\n"
            << "full_dispatch_vtables=" << interfaceSearchStats.fullDispatchVtables << "\r\n"
            << "retained_unique_vtables=" << interfaceCandidates.size() << "\r\n"
            << "duplicate_vtable_references_grouped=" << interfaceSearchStats.duplicateVtableReferences << "\r\n"
            << "abi_candidate_vtables=" << abiCandidateCount << "\r\n"
            << "unique_targets_total=" << targetSemantics.size() << "\r\n"
            << "unique_targets_analyzed=" << interfaceSearchStats.uniqueTargetsAnalyzed << "\r\n"
            << "compatible_targets=" << interfaceSearchStats.compatibleTargets << "\r\n"
            << "plausible_targets=" << interfaceSearchStats.plausibleTargets << "\r\n"
            << "unknown_targets=" << interfaceSearchStats.unknownTargets << "\r\n"
            << "rejected_targets=" << interfaceSearchStats.rejectedTargets << "\r\n"
            << "allocation_failed=" << (interfaceSearchStats.allocationFailed ? "yes" : "no") << "\r\n"
            << "byte_limit_reached=" << (interfaceSearchStats.byteLimitReached ? "yes" : "no") << "\r\n"
            << "runtime_limit_reached=" << (interfaceSearchStats.runtimeLimitReached ? "yes" : "no") << "\r\n"
            << "unique_vtable_limit_reached=" << (interfaceSearchStats.uniqueVtableLimitReached ? "yes" : "no") << "\r\n"
            << "target_analysis_limit_reached=" << (interfaceSearchStats.targetAnalysisLimitReached ? "yes" : "no") << "\r\n";
        for (std::size_t candidateIndex = 0;
             candidateIndex < interfaceCandidates.size(); ++candidateIndex)
        {
            const R15InterfaceCandidate& candidate = interfaceCandidates[candidateIndex];
            out << "VTABLE_CANDIDATE " << candidateIndex
                << " score=" << candidate.score
                << " abi_candidate=" << (candidate.abiCandidate ? "yes" : "no")
                << " reference_field=" << HexValue(candidate.referenceFieldVa)
                << " reference_offset_in_region="
                << HexValue(candidate.referenceFieldVa - candidate.regionBase)
                << " representative_interface_object=" << HexValue(candidate.interfaceVa)
                << " vtable=" << HexValue(candidate.vtableVa)
                << " vtable_module=" << V153EscapeFieldUtf8(candidate.vtableModule)
                << " vtable_rva=" << HexValue(candidate.vtableRva)
                << " region_base=" << HexValue(candidate.regionBase)
                << " region_size=" << candidate.regionSize
                << " reference_count=" << candidate.referenceCount
                << " unique_interface_instances=" << candidate.interfaceInstances.size()
                << " unique_targets=" << candidate.uniqueTargets
                << " compatible_slots=" << candidate.compatibleSlots
                << " plausible_slots=" << candidate.plausibleSlots
                << " unknown_slots=" << candidate.unknownSlots
                << " rejected_slots=" << candidate.rejectedSlots
                << " region_source=" << V153EscapeFieldUtf8(candidate.regionSource)
                << "\r\n";
            for (const R15InterfaceSlotEvidence& slot : candidate.slots)
            {
                out << "  SLOT offset=" << HexValue(slot.slotOffset)
                    << " semantic=" << slot.semanticName
                    << " target=" << HexValue(slot.targetVa)
                    << " target_module=" << V153EscapeFieldUtf8(slot.targetModule)
                    << " target_rva=" << HexValue(slot.targetRva)
                    << " abi=" << R15AbiClassificationText(slot.abiClassification)
                    << "\r\n";
            }
            for (const auto& sample : candidate.referenceSamples)
                out << "  REFERENCE_SAMPLE field=" << HexValue(sample.first)
                    << " interface_object=" << HexValue(sample.second) << "\r\n";
        }
        if (interfaceCandidates.empty())
            out << "none\r\n";
        out << "\r\n";

        out << "R15 ALL-MODULE VTABLE TARGET ABI WINDOWS\r\n"
            << "----------------------------------------\r\n"
            << "unique_targets=" << targetSemantics.size() << "\r\n"
            << "captured_code_windows=" << targetCodeEvidence.size() << "\r\n"
            << "direct_thunk_hop_limit=4\r\n"
            << "maximum_function_window_bytes=384\r\n"
            << "classification_is_heuristic=yes\r\n";
        for (const auto& target : targetCodeEvidence)
        {
            const R15TargetCodeEvidence& evidence = target.second;
            const ModuleRecord* originalModule = R15ModuleForAddress(snapshot, target.first);
            out << "TARGET original_va=" << HexValue(target.first)
                << " original_module=" << V153EscapeFieldUtf8(
                    R15ModuleDisplayName(originalModule));
            if (originalModule)
                out << " original_rva=" << HexValue(target.first - originalModule->base);
            out << " effective_va=" << HexValue(evidence.effectiveTargetVa)
                << " thunk_depth=" << evidence.thunkDepth
                << " effective_module=" << V153EscapeFieldUtf8(evidence.moduleName)
                << " effective_rva=" << HexValue(evidence.targetRva)
                << " abi=" << R15AbiClassificationText(evidence.classification)
                << " semantics=";
            bool firstSemantic = true;
            for (const std::string& semantic : targetSemantics[target.first])
            {
                if (!firstSemantic) out << ',';
                firstSemantic = false;
                out << semantic;
            }
            out << " runtime_boundary=" << (evidence.boundaryFound ? "yes" : "no")
                << " rdx_observed=" << (evidence.rdxObserved ? "yes" : "no")
                << " direct_output_write=" << (evidence.directOutputWrite ? "yes" : "no")
                << " rdx_forwarded_or_saved=" << (evidence.rdxForwardedOrSaved ? "yes" : "no")
                << " call_observed=" << (evidence.callObserved ? "yes" : "no")
                << " trivial_this_getter=" << (evidence.trivialThisGetter ? "yes" : "no")
                << " early_leaf_return_without_rdx="
                << (evidence.earlyLeafReturnWithoutRdx ? "yes" : "no")
                << " diagnostic=" << V153EscapeFieldUtf8(evidence.diagnostic);
            if (evidence.boundaryFound && evidence.moduleBase)
                out << " function_begin_rva="
                    << HexValue(evidence.functionBeginVa - evidence.moduleBase)
                    << " function_end_rva="
                    << HexValue(evidence.functionEndVa - evidence.moduleBase)
                    << " unwind_info_rva="
                    << HexValue(evidence.unwindInfoVa - evidence.moduleBase);
            out << " window_begin_rva="
                << HexValue(evidence.moduleBase
                    ? evidence.windowBeginVa - evidence.moduleBase : evidence.windowBeginVa)
                << " effective_target_offset_in_window="
                << (evidence.effectiveTargetVa >= evidence.windowBeginVa
                    ? evidence.effectiveTargetVa - evidence.windowBeginVa : 0)
                << " byte_count=" << evidence.bytes.size() << "\r\n";
            for (std::size_t offset = 0; offset < evidence.bytes.size(); offset += 16)
            {
                out << "  RAW rva="
                    << HexValue(evidence.moduleBase
                        ? evidence.windowBeginVa + offset - evidence.moduleBase
                        : evidence.windowBeginVa + offset)
                    << " bytes=";
                const std::size_t count = (std::min)(
                    static_cast<std::size_t>(16), evidence.bytes.size() - offset);
                for (std::size_t byteIndex = 0; byteIndex < count; ++byteIndex)
                {
                    if (byteIndex) out << ' ';
                    out << std::hex << std::uppercase << std::setfill('0')
                        << std::setw(2)
                        << static_cast<unsigned int>(evidence.bytes[offset + byteIndex])
                        << std::dec;
                }
                out << "\r\n";
            }
        }
        if (targetCodeEvidence.empty())
            out << "none\r\n";
        out << "\r\n";

        out << "R15 STATIC METHOD CODE WINDOWS\r\n"
            << "------------------------------\r\n"
            << "analysis_kind=bounded_raw_x64_byte_pattern_analysis\r\n"
            << "window_before_reference=0x180\r\n"
            << "window_after_reference=0x280\r\n"
            << "pattern_candidates_are_disassembler_verified=no\r\n"
            << "general_owner_pointer_search_executed=no\r\n"
            << "unknown_functions_called=no\r\n\r\n";
        std::map<std::string, std::map<long long, std::size_t>> methodFieldFrequencies;
        for (std::size_t methodIndex = 0; methodIndex < signatureEvidence.size(); ++methodIndex)
        {
            const PositionResearchSignatureEvidence& evidence = signatureEvidence[methodIndex];
            out << "METHOD " << evidence.signature << "\r\n"
                << "  code_window_count=" << staticCodeEvidence[methodIndex].size() << "\r\n";
            for (std::size_t windowIndex = 0;
                 windowIndex < staticCodeEvidence[methodIndex].size(); ++windowIndex)
            {
                const R13StaticCodeWindow& window = staticCodeEvidence[methodIndex][windowIndex];
                out << "  WINDOW " << windowIndex
                    << " reference_rva=" << HexValue(window.referenceVa - snapshot.atonpTrackerBase)
                    << " begin_rva=" << HexValue(window.beginVa - snapshot.atonpTrackerBase)
                    << " byte_count=" << window.bytes.size() << "\r\n";
                for (std::size_t offset = 0; offset < window.bytes.size(); offset += 16)
                {
                    out << "    RAW rva="
                        << HexValue(window.beginVa + offset - snapshot.atonpTrackerBase)
                        << " bytes=";
                    const std::size_t count = (std::min)(
                        static_cast<std::size_t>(16), window.bytes.size() - offset);
                    for (std::size_t byteIndex = 0; byteIndex < count; ++byteIndex)
                    {
                        if (byteIndex) out << ' ';
                        out << std::hex << std::uppercase << std::setfill('0')
                            << std::setw(2)
                            << static_cast<unsigned int>(window.bytes[offset + byteIndex])
                            << std::dec;
                    }
                    out << "\r\n";
                }
                for (const R13CodePatternEvidence& pattern : window.patterns)
                {
                    out << "    PATTERN kind=" << pattern.kind
                        << " instruction_rva="
                        << HexValue(pattern.instructionVa - snapshot.atonpTrackerBase);
                    if (pattern.kind.find("this_") == 0)
                    {
                        out << " field_offset=" << PositionResearchOffsetText(pattern.fieldOffset);
                        ++methodFieldFrequencies[evidence.signature][pattern.fieldOffset];
                    }
                    if (pattern.targetVa)
                        out << " target_rva="
                            << HexValue(pattern.targetVa - snapshot.atonpTrackerBase);
                    out << "\r\n";
                }
            }
            out << "\r\n";
        }

        out << "R15 CROSS-METHOD THIS-OFFSET SUMMARY\r\n"
            << "------------------------------------\r\n";
        for (const auto& method : methodFieldFrequencies)
        {
            out << method.first << "\r\n";
            for (const auto& frequency : method.second)
                out << "  field_offset=" << PositionResearchOffsetText(frequency.first)
                    << " pattern_occurrences=" << frequency.second << "\r\n";
        }
        if (methodFieldFrequencies.empty())
            out << "none\r\n";
        out << "NOTE: offsets are byte-pattern candidates. Compare repeated offsets across AveragePrice, OpenPL, CurrencyCode, CurrencyLetter, and CurrencyLetterRPL before promoting any object layout.\r\n\r\n";

        out << "DYNAMIC ROW CORRELATION\r\n"
            << "-----------------------\r\n"
            << "R15 retains the earlier numeric row correlation only as context and skips the disproven general owner-pointer pass. "
               "The independent route requires visible Quantity and Average Price; displayed Open P/L raises confidence.\r\n"
            << "No fixed record base, +0x60/+0x68/+0x70 layout, or +0x10E0 container pointer is assumed.\r\n\r\n";

        std::size_t rowsWithCandidates = 0;
        std::size_t totalCandidates = 0;
        std::map<long long, std::size_t> quantityI32OffsetFrequency;
        std::map<long long, std::size_t> quantityI64OffsetFrequency;
        std::map<long long, std::size_t> openPlOffsetFrequency;

        for (std::size_t rowIndex = 0; rowIndex < positions.rows.size(); ++rowIndex)
        {
            const auto& row = positions.rows[rowIndex];
            if (row.size() < 8)
                continue;

            out << "ROW " << rowIndex << "\r\n"
                << "  profile=" << V153EscapeField(row[0]) << "\r\n"
                << "  account=" << V153EscapeField(row[1]) << "\r\n"
                << "  symbol=" << V153EscapeField(row[2]) << "\r\n"
                << "  side=" << V153EscapeField(row[3]) << "\r\n"
                << "  quantity_text=" << V153EscapeField(row[4]) << "\r\n"
                << "  average_price_text=" << V153EscapeField(row[5]) << "\r\n"
                << "  open_pl_text=" << V153EscapeField(row[6]) << "\r\n"
                << "  open_pl_currency_hint="
                << V153EscapeField(PositionResearchCurrencyHint(row[6])) << "\r\n"
                << "  last_update=" << V153EscapeField(row[7]) << "\r\n";

            const auto& candidates = rowCandidates[rowIndex];
            out << "  candidate_count=" << candidates.size() << "\r\n";
            if (!candidates.empty())
            {
                ++rowsWithCandidates;
                totalCandidates += candidates.size();
            }

            const PositionResearchRow* parsed = nullptr;
            for (const PositionResearchRow& candidateRow : parsedRows)
            {
                if (candidateRow.rowIndex == rowIndex)
                {
                    parsed = &candidateRow;
                    break;
                }
            }

            if (!parsed)
            {
                out << "  candidate_status=reference row could not be parsed safely\r\n\r\n";
                continue;
            }

            for (std::size_t candidateIndex = 0;
                 candidateIndex < candidates.size();
                 ++candidateIndex)
            {
                const PositionResearchCandidate& candidate =
                    candidates[candidateIndex];
                out << "  CANDIDATE " << candidateIndex
                    << " score=" << candidate.score
                    << " average_address=" << HexValue(candidate.averageAddress)
                    << " region_base=" << HexValue(candidate.regionBase)
                    << " region_size=" << candidate.regionSize
                    << " region_source=" << V153EscapeFieldUtf8(candidate.regionSource)
                    << "\r\n";

                for (long long offset : candidate.quantityI32Offsets)
                    ++quantityI32OffsetFrequency[offset];
                for (long long offset : candidate.quantityI64Offsets)
                    ++quantityI64OffsetFrequency[offset];
                for (long long offset : candidate.openPlOffsets)
                    ++openPlOffsetFrequency[offset];

                AppendPositionResearchNeighborhood(out, *parsed, candidate);
            }

            if (candidates.empty())
            {
                out << "  candidate_status=no Quantity+AveragePrice co-location found within R12 scan limits\r\n";
            }
            out << "\r\n";
        }

        auto appendFrequency = [&](const char* label, const std::map<long long, std::size_t>& frequencies)
        {
            out << label << "\r\n";
            if (frequencies.empty())
            {
                out << "  none\r\n";
                return;
            }
            std::vector<std::pair<long long, std::size_t>> ordered(
                frequencies.begin(), frequencies.end());
            std::stable_sort(
                ordered.begin(),
                ordered.end(),
                [](const auto& a, const auto& b)
                {
                    if (a.second != b.second)
                        return a.second > b.second;
                    return a.first < b.first;
                });
            const std::size_t count = (std::min)(ordered.size(), static_cast<std::size_t>(16));
            for (std::size_t i = 0; i < count; ++i)
            {
                out << "  offset=" << PositionResearchOffsetText(ordered[i].first)
                    << " candidate_occurrences=" << ordered[i].second << "\r\n";
            }
        };

        out << "CROSS-ROW RELATIVE OFFSET EVIDENCE\r\n"
            << "----------------------------------\r\n";
        appendFrequency("quantity_i32_offsets:", quantityI32OffsetFrequency);
        appendFrequency("quantity_i64_offsets:", quantityI64OffsetFrequency);
        appendFrequency("displayed_open_pl_offsets:", openPlOffsetFrequency);
        out << "\r\n";

        out << "VALIDATED STRIDE-0x30 TABLES\r\n"
            << "----------------------------\r\n";
        out << "process_wide_regions_enumerated=" << tableSearchRegionsEnumerated << "\r\n"
            << "process_wide_regions_read=" << tableSearchStats.regionsRead << "\r\n"
            << "process_wide_bytes_read=" << tableSearchStats.bytesRead << "\r\n"
            << "process_wide_values_checked=" << tableSearchStats.valuesChecked << "\r\n"
            << "process_wide_runtime_limit_reached="
            << (tableSearchStats.runtimeLimitReached ? "yes" : "no") << "\r\n"
            << "process_wide_byte_limit_reached="
            << (tableSearchStats.byteLimitReached ? "yes" : "no") << "\r\n";
        if (validatedTables.empty())
        {
            out << "none\r\n\r\n";
        }
        else
        {
            for (std::size_t i = 0; i < validatedTables.size(); ++i)
            {
                const R6ValidatedPositionTable& table = validatedTables[i];
                out << "TABLE " << i
                    << " first_record=" << HexValue(table.firstRecord)
                    << " first_average=" << HexValue(table.firstAverage)
                    << " rows_matched=" << table.rowsMatched
                    << " open_pl_rows_matched=" << table.openPlRowsMatched
                    << " region_base=" << HexValue(table.regionBase)
                    << " region_size=" << table.regionSize
                    << " region_source=" << V153EscapeFieldUtf8(table.regionSource)
                    << "\r\n";
            }
            out << "\r\n";
        }

        std::set<std::size_t> referencedRows;
        for (const R6OwnerBackReference& reference : ownerReferences)
            referencedRows.insert(reference.rowIndex);

        out << "DEDUPLICATED TABLE OWNERSHIP BACK-REFERENCES\r\n"
            << "--------------------------------------------\r\n"
            << "deduplication_key=field_address+pointer_value\r\n"
            << "target_source=" << ownerTargetSource << "\r\n"
            << "target_count=" << ownerTargets.size() << "\r\n"
            << "unique_reference_count=" << ownerReferences.size() << "\r\n"
            << "referenced_row_count=" << referencedRows.size() << "\r\n"
            << "pointer_scan_regions_read=" << ownerReferenceStats.regionsRead << "\r\n"
            << "pointer_scan_bytes_read=" << ownerReferenceStats.bytesRead << "\r\n"
            << "pointer_scan_runtime_limit_reached="
            << (ownerReferenceStats.runtimeLimitReached ? "yes" : "no") << "\r\n";
        for (std::size_t i = 0; i < ownerReferences.size(); ++i)
        {
            const R6OwnerBackReference& reference = ownerReferences[i];
            out << "REFERENCE " << i
                << " field_address=" << HexValue(reference.fieldAddress)
                << " pointer_value=" << HexValue(reference.pointerValue)
                << " row=" << reference.rowIndex
                << " region_base=" << HexValue(reference.regionBase)
                << " region_source=" << V153EscapeFieldUtf8(reference.regionSource)
                << "\r\n";
        }
        out << "\r\n";

        out << "R12 HISTORICAL SELF-RECORD REJECTION AND VERIFICATION\r\n"
            << "------------------------------------------------------\r\n"
            << "scan_result_storage=fixed_VirtualAlloc_array\r\n"
            << "scan_buffer_storage=fixed_VirtualAlloc_buffer\r\n"
            << "result_vector_mutated_during_scan=no\r\n"
            << "target_container_regions_excluded=yes\r\n"
            << "scan_storage_regions_excluded=yes\r\n"
            << "current_stack_region_excluded=yes\r\n"
            << "preexisting_research_containers_excluded=yes\r\n"
            << "graph_discovery_buffer=fixed_VirtualAlloc_then_MEM_RELEASE\r\n"
            << "candidate_scan_buffer=fixed_VirtualAlloc_then_MEM_RELEASE\r\n"
            << "table_scan_buffer=fixed_VirtualAlloc_then_MEM_RELEASE\r\n"
            << "prior_scan_copy_regions_rejected=yes\r\n"
            << "post_scan_direct_memory_revalidation=yes\r\n"
            << "raw_hits=" << cleanBackReferenceStats.rawHits << "\r\n"
            << "raw_capacity_drops=" << cleanBackReferenceStats.rawCapacityDrops << "\r\n"
            << "excluded_region_count=" << cleanBackReferenceStats.excludedRegions << "\r\n"
            << "excluded_regions_encountered=" << cleanBackReferenceStats.excludedRegionsEncountered << "\r\n"
            << "post_scan_unreadable=" << cleanBackReferenceStats.postScanUnreadable << "\r\n"
            << "post_scan_value_mismatches=" << cleanBackReferenceStats.postScanValueMismatches << "\r\n"
            << "duplicate_pairs_removed=" << cleanBackReferenceStats.duplicatePairsRemoved << "\r\n"
            << "self_record_shape_matches=" << cleanBackReferenceStats.selfRecordShapeMatches << "\r\n"
            << "historical_self_records_rejected=" << cleanBackReferenceStats.historicalSelfRecordsRejected << "\r\n"
            << "current_research_addresses_supplied=" << cleanBackReferenceStats.currentResearchAddressesSupplied << "\r\n"
            << "current_research_regions_excluded=" << cleanBackReferenceStats.currentResearchRegionsExcluded << "\r\n"
            << "current_research_structures_rejected=" << cleanBackReferenceStats.currentResearchRegionsExcluded << "\r\n"
            << "clean_candidate_regions=" << cleanBackReferenceStats.cleanCandidateRegions << "\r\n"
            << "full_row_coverage_regions=" << cleanBackReferenceStats.fullRowCoverageRegions << "\r\n"
            << "genuine_candidates=" << cleanBackReferenceStats.verifiedHits << "\r\n"
            << "verified_hits=" << cleanBackReferenceStats.verifiedHits << "\r\n"
            << "NOTE: R12 rejects historical R6OwnerBackReference records only when row, region-base, and a known MCST research-source string form the complete self-record signature.\r\n\r\n";

        out << "REJECTED HISTORICAL MCST SELF-RECORDS\r\n"
            << "-------------------------------------\r\n";
        for (const R6OwnerBackReference& rejected : rejectedSelfRecords)
        {
            out << "SELF_RECORD field_address=" << HexValue(rejected.fieldAddress)
                << " pointer_value=" << HexValue(rejected.pointerValue)
                << " row=" << rejected.rowIndex
                << " region_base=" << HexValue(rejected.regionBase)
                << " evidence=" << V153EscapeFieldUtf8(rejected.regionSource) << "\r\n";
        }
        if (rejectedSelfRecords.empty())
            out << "none\r\n";
        out << "\r\n";

        out << "CLEAN CANDIDATE REGION COVERAGE\r\n"
            << "-------------------------------\r\n";
        std::map<std::uintptr_t, std::vector<const R6OwnerBackReference*>> cleanReferencesByRegion;
        for (const R6OwnerBackReference& reference : ownerReferences)
            cleanReferencesByRegion[reference.regionBase].push_back(&reference);
        for (const auto& regionEntry : cleanReferencesByRegion)
        {
            std::set<std::size_t> rows;
            for (const R6OwnerBackReference* reference : regionEntry.second)
                rows.insert(reference->rowIndex);
            out << "CANDIDATE_REGION base=" << HexValue(regionEntry.first)
                << " reference_count=" << regionEntry.second.size()
                << " distinct_rows=" << rows.size()
                << " full_row_coverage=" << (rows.size() == ownerTargets.size() ? "yes" : "no")
                << " rows=";
            bool firstRow = true;
            for (std::size_t row : rows)
            {
                if (!firstRow) out << ',';
                firstRow = false;
                out << row;
            }
            out << "\r\n";
        }
        if (cleanReferencesByRegion.empty())
            out << "none\r\n";
        out << "\r\n";

        out << "VERIFIED REFERENCE NEIGHBORHOODS\r\n"
            << "--------------------------------\r\n";
        for (const R6OwnerBackReference& reference : ownerReferences)
        {
            R12AppendRawReferenceLayout(out, reference);
            R6AppendOwnerRecordNeighborhood(out, reference, reference.fieldAddress);
        }
        out << "\r\n";

        out << "SUMMARY\r\n"
            << "-------\r\n"
            << "visible_rows=" << positions.rows.size() << "\r\n"
            << "parsed_reference_rows=" << parsedRows.size() << "\r\n"
            << "rows_with_dynamic_candidates=" << rowsWithCandidates << "\r\n"
            << "total_retained_candidates=" << totalCandidates << "\r\n"
            << "validated_stride_tables=" << validatedTables.size() << "\r\n"
            << "unique_owner_back_references=" << ownerReferences.size() << "\r\n"
            << "owner_rows_referenced=" << referencedRows.size() << "\r\n"
            << "raw_owner_back_reference_hits=" << cleanBackReferenceStats.rawHits << "\r\n"
            << "verified_owner_back_references=" << cleanBackReferenceStats.verifiedHits << "\r\n"
            << "historical_self_records_rejected=" << cleanBackReferenceStats.historicalSelfRecordsRejected << "\r\n"
            << "current_research_regions_excluded=" << cleanBackReferenceStats.currentResearchRegionsExcluded << "\r\n"
            << "clean_candidate_regions=" << cleanBackReferenceStats.cleanCandidateRegions << "\r\n"
            << "full_row_coverage_regions=" << cleanBackReferenceStats.fullRowCoverageRegions << "\r\n"
            << "verified_dispatch_methods=" << verifiedDispatchMethods << "\r\n"
            << "interface_unique_vtables=" << interfaceCandidates.size() << "\r\n"
            << "interface_abi_candidate_vtables=" << abiCandidateCount << "\r\n"
            << "unique_vtable_targets=" << targetSemantics.size() << "\r\n"
            << "unique_vtable_targets_analyzed=" << targetCodeEvidence.size() << "\r\n"
            << "research_interpretation=R15 ranks interface vtables by the required RDX output-pointer ABI. Promote native-currency or P/L-currency fields only after a retained interface is tied back to the visible position rows and repeated captures remain stable across at least two instrument currencies. "
               "UNKNOWN remains the required production result when that proof is absent.\r\n";

        const bool written = WriteUtf8File(reportPath, out.str());
        std::ostringstream checkpoint;
        std::size_t methodsWithCodeWindows = 0;
        for (const auto& windows : staticCodeEvidence)
            if (!windows.empty()) ++methodsWithCodeWindows;
        const bool researchComplete =
            sectionsParsed && methodsWithCodeWindows == signatureEvidence.size() &&
            verifiedDispatchMethods == signatureEvidence.size() &&
            positions.ok && !interfaceSearchStats.allocationFailed &&
            !interfaceSearchStats.byteLimitReached &&
            !interfaceSearchStats.runtimeLimitReached &&
            !interfaceSearchStats.uniqueVtableLimitReached &&
            !interfaceSearchStats.targetAnalysisLimitReached &&
            targetCodeEvidence.size() == targetSemantics.size();
        checkpoint << "MCST POSITION CURRENCY R15 CHECKPOINT\r\n"
                   << "research_build=1.114-R15\r\n"
                   << "bridge_version=" << kBridgeVersion << "\r\n"
                   << "read_only=yes\r\n"
                   << "phase_status="
                   << (!written ? "WRITE_FAILED" : (researchComplete ? "OK" : "PARTIAL")) << "\r\n"
                   << "visible_rows=" << positions.rows.size() << "\r\n"
                   << "parsed_rows=" << parsedRows.size() << "\r\n"
                   << "static_methods_expected=" << signatureEvidence.size() << "\r\n"
                   << "static_methods_with_code_windows=" << methodsWithCodeWindows << "\r\n"
                   << "verified_dispatch_methods=" << verifiedDispatchMethods << "\r\n"
                   << "expected_dispatch_methods=" << dispatchEvidence.size() << "\r\n"
                   << "interface_search_regions_read=" << interfaceSearchStats.regionsRead << "\r\n"
                   << "interface_search_bytes_read=" << interfaceSearchStats.bytesRead << "\r\n"
                   << "interface_unique_vtables_examined=" << interfaceSearchStats.uniqueVtablesExamined << "\r\n"
                   << "interface_full_dispatch_vtables=" << interfaceSearchStats.fullDispatchVtables << "\r\n"
                   << "interface_retained_unique_vtables=" << interfaceCandidates.size() << "\r\n"
                   << "interface_abi_candidate_vtables=" << abiCandidateCount << "\r\n"
                   << "interface_allocation_failed=" << (interfaceSearchStats.allocationFailed ? "yes" : "no") << "\r\n"
                   << "interface_byte_limit_reached=" << (interfaceSearchStats.byteLimitReached ? "yes" : "no") << "\r\n"
                   << "interface_runtime_limit_reached=" << (interfaceSearchStats.runtimeLimitReached ? "yes" : "no") << "\r\n"
                   << "interface_unique_vtable_limit_reached=" << (interfaceSearchStats.uniqueVtableLimitReached ? "yes" : "no") << "\r\n"
                   << "interface_target_analysis_limit_reached=" << (interfaceSearchStats.targetAnalysisLimitReached ? "yes" : "no") << "\r\n"
                   << "unique_vtable_targets=" << targetSemantics.size() << "\r\n"
                   << "vtable_targets_analyzed=" << targetCodeEvidence.size() << "\r\n"
                   << "vtable_targets_compatible=" << interfaceSearchStats.compatibleTargets << "\r\n"
                   << "vtable_targets_plausible=" << interfaceSearchStats.plausibleTargets << "\r\n"
                   << "vtable_targets_unknown=" << interfaceSearchStats.unknownTargets << "\r\n"
                   << "vtable_targets_rejected=" << interfaceSearchStats.rejectedTargets << "\r\n"
                   << "general_owner_pointer_search_executed=no\r\n"
                   << "validated_stride_tables=" << validatedTables.size() << "\r\n"
                   << "table_search_regions_enumerated=" << tableSearchRegionsEnumerated << "\r\n"
                   << "table_search_bytes_read=" << tableSearchStats.bytesRead << "\r\n"
                   << "table_search_runtime_limit_reached="
                   << (tableSearchStats.runtimeLimitReached ? "yes" : "no") << "\r\n"
                   << "owner_target_source=" << ownerTargetSource << "\r\n"
                   << "unique_owner_back_references=" << ownerReferences.size() << "\r\n"
                   << "owner_rows_referenced=" << referencedRows.size() << "\r\n"
                   << "raw_owner_back_reference_hits=" << cleanBackReferenceStats.rawHits << "\r\n"
                   << "verified_owner_back_references=" << cleanBackReferenceStats.verifiedHits << "\r\n"
                   << "historical_self_records_rejected=" << cleanBackReferenceStats.historicalSelfRecordsRejected << "\r\n"
                   << "current_research_addresses_supplied=" << cleanBackReferenceStats.currentResearchAddressesSupplied << "\r\n"
                   << "current_research_regions_excluded=" << cleanBackReferenceStats.currentResearchRegionsExcluded << "\r\n"
                   << "current_research_structures_rejected=" << cleanBackReferenceStats.currentResearchRegionsExcluded << "\r\n"
                   << "clean_candidate_regions=" << cleanBackReferenceStats.cleanCandidateRegions << "\r\n"
                   << "full_row_coverage_regions=" << cleanBackReferenceStats.fullRowCoverageRegions << "\r\n"
                   << "excluded_regions_encountered=" << cleanBackReferenceStats.excludedRegionsEncountered << "\r\n"
                   << "post_scan_value_mismatches=" << cleanBackReferenceStats.postScanValueMismatches << "\r\n"
                   << "owner_scan_runtime_limit_reached="
                   << (ownerReferenceStats.runtimeLimitReached ? "yes" : "no") << "\r\n"
                   << "unknown_functions_called=no\r\n"
                   << "report_written=" << (written ? "yes" : "no") << "\r\n";
        const bool checkpointWritten = WriteUtf8File(checkpointPath, checkpoint.str());
        std::ostringstream summary;
        summary << "{\"capture\":\"position_currency_dynamic_research\",\"version\":170,"
                << "\"read_only\":true,\"unknown_functions_called\":false,"
                << "\"visible_rows\":" << positions.rows.size() << ','
                << "\"parsed_reference_rows\":" << parsedRows.size() << ','
                << "\"static_methods_expected\":" << signatureEvidence.size() << ','
                << "\"static_methods_with_code_windows\":" << methodsWithCodeWindows << ','
                << "\"verified_dispatch_methods\":" << verifiedDispatchMethods << ','
                << "\"expected_dispatch_methods\":" << dispatchEvidence.size() << ','
                << "\"interface_search_bytes\":" << interfaceSearchStats.bytesRead << ','
                << "\"interface_unique_vtables_examined\":" << interfaceSearchStats.uniqueVtablesExamined << ','
                << "\"interface_full_dispatch_vtables\":" << interfaceSearchStats.fullDispatchVtables << ','
                << "\"interface_retained_unique_vtables\":" << interfaceCandidates.size() << ','
                << "\"interface_abi_candidate_vtables\":" << abiCandidateCount << ','
                << "\"unique_vtable_targets\":" << targetSemantics.size() << ','
                << "\"vtable_targets_analyzed\":" << targetCodeEvidence.size() << ','
                << "\"vtable_targets_compatible\":" << interfaceSearchStats.compatibleTargets << ','
                << "\"vtable_targets_plausible\":" << interfaceSearchStats.plausibleTargets << ','
                << "\"vtable_targets_unknown\":" << interfaceSearchStats.unknownTargets << ','
                << "\"vtable_targets_rejected\":" << interfaceSearchStats.rejectedTargets << ','
                << "\"interface_search_complete\":" << (researchComplete ? "true" : "false") << ','
                << "\"general_owner_pointer_search_executed\":false,"
                << "\"rows_with_dynamic_candidates\":" << rowsWithCandidates << ','
                << "\"total_retained_candidates\":" << totalCandidates << ','
                << "\"validated_stride_tables\":" << validatedTables.size() << ','
                << "\"table_search_bytes\":" << tableSearchStats.bytesRead << ','
                << "\"owner_target_source\":" << JsonString(ownerTargetSource) << ','
                << "\"unique_owner_back_references\":" << ownerReferences.size() << ','
                << "\"owner_rows_referenced\":" << referencedRows.size() << ','
                << "\"raw_owner_back_reference_hits\":" << cleanBackReferenceStats.rawHits << ','
                << "\"verified_owner_back_references\":" << cleanBackReferenceStats.verifiedHits << ','
                << "\"historical_self_records_rejected\":" << cleanBackReferenceStats.historicalSelfRecordsRejected << ','
                << "\"current_research_regions_excluded\":" << cleanBackReferenceStats.currentResearchRegionsExcluded << ','
                << "\"current_research_structures_rejected\":" << cleanBackReferenceStats.currentResearchRegionsExcluded << ','
                << "\"clean_candidate_regions\":" << cleanBackReferenceStats.cleanCandidateRegions << ','
                << "\"full_row_coverage_regions\":" << cleanBackReferenceStats.fullRowCoverageRegions << ','
                << "\"checkpoint_written\":" << (checkpointWritten ? "true" : "false") << ','
                << "\"graph_scan_bytes\":" << graphStats.bytesRead << ','
                << "\"fallback_scan_bytes\":" << fallbackStats.bytesRead << ','
                << "\"report_written\":" << (written ? "true" : "false") << ','
                << "\"report_path\":" << JsonString(reportPath) << '}';
        summaryJson = summary.str();

        // A completed research capture is useful even if no candidate was found;
        // the report then records scan coverage and the negative result.
        return written && positions.ok && !positions.rows.empty();
    }

    struct R16MsvcWstringEvidence
    {
        bool metadataReadable = false;
        bool layoutValid = false;
        bool terminatorValid = false;
        bool inlineStorage = false;
        bool empty = false;
        bool strictCurrencyCode = false;
        std::size_t size = 0;
        std::size_t capacity = 0;
        std::uintptr_t dataAddress = 0;
        std::wstring value;
        std::string diagnostic;
    };

    R16MsvcWstringEvidence R16ReadMsvcWstring(
        std::uintptr_t object,
        std::size_t fieldOffset)
    {
        R16MsvcWstringEvidence result;
        const std::uintptr_t stringObject = object + fieldOffset;
        if (stringObject < object ||
            !SafeReadValue(reinterpret_cast<void*>(stringObject + 0x10), result.size) ||
            !SafeReadValue(reinterpret_cast<void*>(stringObject + 0x18), result.capacity))
        {
            result.diagnostic = "wstring metadata unreadable";
            return result;
        }
        result.metadataReadable = true;

        // MSVC x64 std::wstring has a 16-byte small-string buffer, followed by
        // size and capacity. Currency fields are intentionally bounded much
        // more tightly than a general-purpose string reader.
        constexpr std::size_t kInlineCapacity = 7;
        constexpr std::size_t kMaximumCapacity = 1024;
        constexpr std::size_t kMaximumCurrencyCharacters = 8;
        if (result.capacity > kMaximumCapacity || result.size > result.capacity ||
            result.size > kMaximumCurrencyCharacters)
        {
            result.diagnostic = "wstring size/capacity outside the bounded currency layout";
            return result;
        }

        result.inlineStorage = result.capacity <= kInlineCapacity;
        result.dataAddress = stringObject;
        if (!result.inlineStorage &&
            !SafeReadValue(reinterpret_cast<void*>(stringObject), result.dataAddress))
        {
            result.diagnostic = "wstring heap pointer unreadable";
            return result;
        }
        if (!result.dataAddress)
        {
            result.diagnostic = "wstring data pointer is null";
            return result;
        }

        wchar_t characters[kMaximumCurrencyCharacters + 1]{};
        const std::size_t charactersToRead = result.size + 1;
        if (!SafeReadBytes(
                reinterpret_cast<void*>(result.dataAddress),
                characters,
                charactersToRead * sizeof(wchar_t)))
        {
            result.diagnostic = "wstring character buffer unreadable";
            return result;
        }
        if (characters[result.size] != L'\0')
        {
            result.diagnostic = "wstring is missing its bounded terminator";
            return result;
        }
        result.terminatorValid = true;
        result.value.assign(characters, result.size);
        result.empty = result.value.empty();

        bool printable = true;
        for (wchar_t ch : result.value)
        {
            if (!iswprint(ch) || iswspace(ch))
            {
                printable = false;
                break;
            }
        }
        if (!printable)
        {
            result.diagnostic = "wstring contains non-printable currency characters";
            return result;
        }

        result.layoutValid = true;
        result.strictCurrencyCode = result.value.size() == 3;
        for (wchar_t ch : result.value)
        {
            if (ch < L'A' || ch > L'Z')
                result.strictCurrencyCode = false;
        }
        result.diagnostic = result.empty ? "valid empty wstring" :
            (result.strictCurrencyCode ? "valid ISO-like currency code" :
             "valid bounded wstring but not a strict three-letter currency code");
        return result;
    }

    struct R16PositionObjectEvidence
    {
        std::uintptr_t object = 0;
        std::uintptr_t vtable = 0;
        std::uintptr_t regionBase = 0;
        std::size_t regionSize = 0;
        std::string discoverySource;
        std::size_t pointerReferenceCount = 0;
        bool directVtableHit = false;
        bool vtableStable = false;
        bool numericReadable = false;
        bool numericPlausible = false;
        std::int32_t quantity = 0;
        double averagePrice = 0.0;
        double openPl = 0.0;
        double realizedPl = 0.0;
        R16MsvcWstringEvidence primaryCurrency;
        R16MsvcWstringEvidence rplCurrency;
    };

    R16PositionObjectEvidence R16ReadPositionObject(
        std::uintptr_t object,
        std::uintptr_t expectedVtable,
        const PositionResearchRegion& region,
        const std::string& discoverySource,
        bool directVtableHit)
    {
        R16PositionObjectEvidence result;
        result.object = object;
        result.regionBase = region.base;
        result.regionSize = region.size;
        result.discoverySource = discoverySource;
        result.directVtableHit = directVtableHit;
        result.vtableStable =
            SafeReadValue(reinterpret_cast<void*>(object), result.vtable) &&
            result.vtable == expectedVtable;
        if (!result.vtableStable)
            return result;

        const bool quantityOk = SafeReadValue(
            reinterpret_cast<void*>(object + 0x1A8), result.quantity);
        const bool averageOk = SafeReadValue(
            reinterpret_cast<void*>(object + 0x1B0), result.averagePrice);
        const bool openPlOk = SafeReadValue(
            reinterpret_cast<void*>(object + 0x1B8), result.openPl);
        const bool realizedOk = SafeReadValue(
            reinterpret_cast<void*>(object + 0x1C8), result.realizedPl);
        result.numericReadable = quantityOk && averageOk && openPlOk && realizedOk;
        if (result.numericReadable)
        {
            const long long absoluteQuantity = result.quantity < 0
                ? -static_cast<long long>(result.quantity)
                : static_cast<long long>(result.quantity);
            result.numericPlausible = absoluteQuantity > 0 && absoluteQuantity <= 1000000000ll &&
                std::isfinite(result.averagePrice) && result.averagePrice > 0.0 &&
                result.averagePrice < 1000000000.0 &&
                std::isfinite(result.openPl) && std::fabs(result.openPl) < 1.0e15 &&
                std::isfinite(result.realizedPl) && std::fabs(result.realizedPl) < 1.0e15;
        }
        result.primaryCurrency = R16ReadMsvcWstring(object, 0x308);
        result.rplCurrency = R16ReadMsvcWstring(object, 0x328);
        return result;
    }

    const ModuleRecord* R16FindModule(
        const Snapshot& snapshot,
        const wchar_t* moduleName)
    {
        for (const ModuleRecord& module : snapshot.modules)
        {
            const std::wstring display = module.name.empty()
                ? BaseName(module.path) : module.name;
            if (_wcsicmp(display.c_str(), moduleName) == 0)
                return &module;
        }
        return nullptr;
    }

    bool R16BytesMatch(
        std::uintptr_t address,
        const unsigned char* expected,
        std::size_t expectedSize)
    {
        if (!expected || expectedSize == 0)
            return false;
        std::vector<unsigned char> actual(expectedSize);
        return SafeReadBytes(reinterpret_cast<void*>(address), actual.data(), actual.size()) &&
            std::equal(actual.begin(), actual.end(), expected);
    }

    struct R16FingerprintSlot
    {
        std::size_t slotOffset = 0;
        const char* semantic = nullptr;
        std::uintptr_t expectedRva = 0;
        std::uintptr_t actualTarget = 0;
        std::uintptr_t actualRva = 0;
        bool match = false;
    };

    struct R16FingerprintEvidence
    {
        const ModuleRecord* module = nullptr;
        std::uintptr_t expectedVtable = 0;
        bool moduleSizeMatches = false;
        bool vtableReadable = false;
        bool slotsMatch = false;
        bool codeSignaturesMatch = false;
        bool ok = false;
        std::uintptr_t lastUpdateTarget = 0;
        std::uintptr_t lastUpdateTargetRva = 0;
        std::vector<R16FingerprintSlot> slots;
        std::string diagnostic;
    };

    R16FingerprintEvidence R16VerifyPositionInterfaceFingerprint(
        const Snapshot& snapshot)
    {
        R16FingerprintEvidence result;
        constexpr DWORD kExpectedAtCenterProxySize = 7303168u;
        constexpr std::uintptr_t kVtableRva = 0x44D518;
        struct ExpectedSlot
        {
            std::size_t offset;
            const char* semantic;
            std::uintptr_t rva;
        };
        constexpr ExpectedSlot expectedSlots[] =
        {
            { 0x40, "Quantity", 0x1DE610 },
            { 0x48, "AveragePrice", 0x1DE720 },
            { 0x58, "OpenPL", 0x1DE840 },
            { 0x60, "same-interface +0x60 (not PriceScaleCode)", 0x69480 },
            { 0x70, "CurrencyCode_or_CurrencyLetter", 0x1E02F0 },
            { 0x88, "CurrencyLetterRPL", 0x1E0570 },
            { 0x90, "RealizedPL", 0x1E09B0 }
        };

        result.module = R16FindModule(snapshot, L"ATCenterProxy.dll");
        if (!result.module)
        {
            result.diagnostic = "ATCenterProxy.dll is not loaded";
            return result;
        }
        result.moduleSizeMatches = result.module->size == kExpectedAtCenterProxySize;
        result.expectedVtable = result.module->base + kVtableRva;
        result.vtableReadable =
            result.expectedVtable >= result.module->base &&
            result.expectedVtable + 0xA0 >= result.expectedVtable &&
            result.expectedVtable + 0xA0 <= result.module->base + result.module->size &&
            MemoryRangeHasProtection(reinterpret_cast<void*>(result.expectedVtable), 0xA0, false);

        bool allSlotsMatch = result.vtableReadable;
        for (const ExpectedSlot& expected : expectedSlots)
        {
            R16FingerprintSlot slot;
            slot.slotOffset = expected.offset;
            slot.semantic = expected.semantic;
            slot.expectedRva = expected.rva;
            if (result.vtableReadable && SafeReadValue(
                    reinterpret_cast<void*>(result.expectedVtable + expected.offset),
                    slot.actualTarget))
            {
                if (slot.actualTarget >= result.module->base)
                    slot.actualRva = slot.actualTarget - result.module->base;
                slot.match = slot.actualTarget == result.module->base + expected.rva;
            }
            allSlotsMatch = allSlotsMatch && slot.match;
            result.slots.push_back(slot);
        }
        result.slotsMatch = allSlotsMatch;
        if (result.vtableReadable && SafeReadValue(
                reinterpret_cast<void*>(result.expectedVtable + 0x98),
                result.lastUpdateTarget) &&
            result.lastUpdateTarget >= result.module->base)
        {
            result.lastUpdateTargetRva = result.lastUpdateTarget - result.module->base;
        }

        static const unsigned char quantitySignature[] =
        {
            0x48,0x8B,0xC4,0x48,0x81,0xEC,0x18,0x01,0x00,0x00,
            0x48,0xC7,0x40,0xB8,0xFE,0xFF,0xFF,0xFF,0x48,0x85,
            0xD2,0x74,0x48,0x8B,0x81,0xA8,0x01,0x00,0x00,0x89,0x02
        };
        static const unsigned char averageSignature[] =
        {
            0x48,0x8B,0xC4,0x48,0x81,0xEC,0x18,0x01,0x00,0x00,
            0x48,0xC7,0x40,0xB8,0xFE,0xFF,0xFF,0xFF,0x48,0x85,
            0xD2,0x74,0x4C,0xF2,0x0F,0x10,0x81,0xB0,0x01,0x00,0x00,0xF2,
            0x0F,0x11,0x02
        };
        static const unsigned char openPlSignature[] =
        {
            0x48,0x8B,0xC4,0x48,0x81,0xEC,0x18,0x01,0x00,0x00,
            0x48,0xC7,0x40,0xB8,0xFE,0xFF,0xFF,0xFF,0x48,0x85,
            0xD2,0x74,0x4C,0xF2,0x0F,0x10,0x81,0xB8,0x01,0x00,0x00,0xF2,
            0x0F,0x11,0x02
        };
        static const unsigned char primaryCurrencySignature[] =
        {
            0x48,0x8B,0xC4,0x57,0x41,0x56,0x41,0x57,
            0x48,0x81,0xEC,0xF0,0x02,0x00,0x00,0x48
        };
        static const unsigned char rplCurrencySignature[] =
        {
            0x48,0x8B,0xC4,0x57,0x41,0x56,0x41,0x57,
            0x48,0x81,0xEC,0x00,0x03,0x00,0x00,0x48
        };
        static const unsigned char realizedSignature[] =
        {
            0x48,0x8B,0xC4,0x48,0x81,0xEC,0x18,0x01,0x00,0x00,
            0x48,0xC7,0x40,0xB8,0xFE,0xFF,0xFF,0xFF,0x48,0x85,
            0xD2,0x74,0x4C,0xF2,0x0F,0x10,0x81,0xC8,0x01,0x00,0x00,0xF2,
            0x0F,0x11,0x02
        };
        result.codeSignaturesMatch =
            R16BytesMatch(result.module->base + 0x1DE610, quantitySignature, sizeof(quantitySignature)) &&
            R16BytesMatch(result.module->base + 0x1DE720, averageSignature, sizeof(averageSignature)) &&
            R16BytesMatch(result.module->base + 0x1DE840, openPlSignature, sizeof(openPlSignature)) &&
            R16BytesMatch(result.module->base + 0x1E02F0, primaryCurrencySignature, sizeof(primaryCurrencySignature)) &&
            R16BytesMatch(result.module->base + 0x1E0570, rplCurrencySignature, sizeof(rplCurrencySignature)) &&
            R16BytesMatch(result.module->base + 0x1E09B0, realizedSignature, sizeof(realizedSignature));
        result.ok = result.moduleSizeMatches && result.vtableReadable &&
            result.slotsMatch && result.codeSignaturesMatch;
        result.diagnostic = result.ok
            ? "ATCenterProxy position-interface vtable, targets, and field-access signatures match R15 evidence"
            : "ATCenterProxy position-interface fingerprint mismatch; R16 refuses layout reads";
        return result;
    }

    struct R16ScanStats
    {
        std::size_t regionsConsidered = 0;
        std::size_t regionsRead = 0;
        std::uint64_t bytesRead = 0;
        std::uint64_t alignedValuesChecked = 0;
        std::size_t directVtableMatches = 0;
        std::size_t pointerReferenceMatches = 0;
        bool allocationFailed = false;
        bool byteLimitReached = false;
        bool runtimeLimitReached = false;
        bool objectLimitReached = false;
    };

    void R16ScanDirectVtableValues(
        const std::vector<PositionResearchRegion>& regions,
        std::uintptr_t expectedVtable,
        std::uint64_t byteLimit,
        const std::chrono::steady_clock::time_point& deadline,
        const std::string& scope,
        std::map<std::uintptr_t, R16PositionObjectEvidence>& objects,
        R16ScanStats& stats)
    {
        constexpr std::size_t kChunkBytes = 4u * 1024u * 1024u;
        constexpr std::size_t kObjectLimit = 4096;
        unsigned char* buffer = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, kChunkBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!buffer)
        {
            stats.allocationFailed = true;
            return;
        }

        for (const PositionResearchRegion& region : regions)
        {
            ++stats.regionsConsidered;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                stats.runtimeLimitReached = true;
                break;
            }
            bool regionRead = false;
            for (std::size_t regionOffset = 0; regionOffset < region.size;)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    stats.runtimeLimitReached = true;
                    break;
                }
                if (stats.bytesRead >= byteLimit)
                {
                    stats.byteLimitReached = true;
                    break;
                }
                std::size_t bytesToRead = (std::min)(kChunkBytes, region.size - regionOffset);
                bytesToRead = static_cast<std::size_t>((std::min)(
                    static_cast<std::uint64_t>(bytesToRead), byteLimit - stats.bytesRead));
                if (bytesToRead < sizeof(std::uintptr_t))
                {
                    stats.byteLimitReached = true;
                    break;
                }
                const std::uintptr_t chunkAddress = region.base + regionOffset;
                if (!SafeReadBytes(reinterpret_cast<void*>(chunkAddress), buffer, bytesToRead))
                {
                    regionOffset += bytesToRead;
                    continue;
                }
                regionRead = true;
                stats.bytesRead += bytesToRead;
                for (std::size_t offset = 0;
                     offset + sizeof(std::uintptr_t) <= bytesToRead;
                     offset += sizeof(std::uintptr_t))
                {
                    ++stats.alignedValuesChecked;
                    std::uintptr_t value = 0;
                    std::memcpy(&value, buffer + offset, sizeof(value));
                    if (value != expectedVtable)
                        continue;
                    ++stats.directVtableMatches;
                    const std::uintptr_t object = chunkAddress + offset;
                    if (objects.find(object) != objects.end())
                        continue;
                    if (objects.size() >= kObjectLimit)
                    {
                        stats.objectLimitReached = true;
                        break;
                    }
                    const std::string source = scope + ":" +
                        R15SummarizeRegionSource(region.source);
                    objects.emplace(object, R16ReadPositionObject(
                        object, expectedVtable, region, source, true));
                }
                regionOffset += bytesToRead;
                if (stats.objectLimitReached)
                    break;
            }
            if (regionRead)
                ++stats.regionsRead;
            if (stats.runtimeLimitReached || stats.byteLimitReached || stats.objectLimitReached)
                break;
        }
        VirtualFree(buffer, 0, MEM_RELEASE);
    }

    void R16ScanPointerReferences(
        const std::vector<PositionResearchRegion>& regions,
        std::uintptr_t expectedVtable,
        std::uint64_t byteLimit,
        const std::chrono::steady_clock::time_point& deadline,
        std::map<std::uintptr_t, R16PositionObjectEvidence>& objects,
        R16ScanStats& stats)
    {
        constexpr std::size_t kChunkBytes = 4u * 1024u * 1024u;
        constexpr std::size_t kObjectLimit = 4096;
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        const std::uintptr_t maximumUserAddress =
            reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
        unsigned char* buffer = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, kChunkBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!buffer)
        {
            stats.allocationFailed = true;
            return;
        }

        for (const PositionResearchRegion& region : regions)
        {
            ++stats.regionsConsidered;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                stats.runtimeLimitReached = true;
                break;
            }
            bool regionRead = false;
            for (std::size_t regionOffset = 0; regionOffset < region.size;)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    stats.runtimeLimitReached = true;
                    break;
                }
                if (stats.bytesRead >= byteLimit)
                {
                    stats.byteLimitReached = true;
                    break;
                }
                std::size_t bytesToRead = (std::min)(kChunkBytes, region.size - regionOffset);
                bytesToRead = static_cast<std::size_t>((std::min)(
                    static_cast<std::uint64_t>(bytesToRead), byteLimit - stats.bytesRead));
                if (bytesToRead < sizeof(std::uintptr_t))
                {
                    stats.byteLimitReached = true;
                    break;
                }
                const std::uintptr_t chunkAddress = region.base + regionOffset;
                if (!SafeReadBytes(reinterpret_cast<void*>(chunkAddress), buffer, bytesToRead))
                {
                    regionOffset += bytesToRead;
                    continue;
                }
                regionRead = true;
                stats.bytesRead += bytesToRead;
                for (std::size_t offset = 0;
                     offset + sizeof(std::uintptr_t) <= bytesToRead;
                     offset += sizeof(std::uintptr_t))
                {
                    ++stats.alignedValuesChecked;
                    std::uintptr_t object = 0;
                    std::memcpy(&object, buffer + offset, sizeof(object));
                    if (object < 0x10000 || object > maximumUserAddress ||
                        (object & (sizeof(std::uintptr_t) - 1)) != 0)
                        continue;
                    auto known = objects.find(object);
                    if (known != objects.end())
                    {
                        ++known->second.pointerReferenceCount;
                        ++stats.pointerReferenceMatches;
                        continue;
                    }
                    std::uintptr_t vtable = 0;
                    if (!SafeReadValue(reinterpret_cast<void*>(object), vtable) ||
                        vtable != expectedVtable)
                        continue;
                    ++stats.pointerReferenceMatches;
                    if (objects.size() >= kObjectLimit)
                    {
                        stats.objectLimitReached = true;
                        break;
                    }
                    PositionResearchRegion objectRegion;
                    std::uintptr_t objectRegionEnd = 0;
                    MEMORY_BASIC_INFORMATION objectMbi{};
                    if (QueryReadableSpan(object, objectRegionEnd, objectMbi))
                    {
                        objectRegion.base = reinterpret_cast<std::uintptr_t>(objectMbi.BaseAddress);
                        objectRegion.size = objectMbi.RegionSize;
                        objectRegion.protect = objectMbi.Protect;
                        objectRegion.type = objectMbi.Type;
                        objectRegion.source = "object reached by graph pointer";
                    }
                    R16PositionObjectEvidence evidence = R16ReadPositionObject(
                        object,
                        expectedVtable,
                        objectRegion,
                        "known_anchor_graph:pointer_reference",
                        false);
                    evidence.pointerReferenceCount = 1;
                    objects.emplace(object, std::move(evidence));
                }
                regionOffset += bytesToRead;
                if (stats.objectLimitReached)
                    break;
            }
            if (regionRead)
                ++stats.regionsRead;
            if (stats.runtimeLimitReached || stats.byteLimitReached || stats.objectLimitReached)
                break;
        }
        VirtualFree(buffer, 0, MEM_RELEASE);
    }

    std::size_t R16PlausibleObjectCount(
        const std::map<std::uintptr_t, R16PositionObjectEvidence>& objects)
    {
        return static_cast<std::size_t>(std::count_if(
            objects.begin(), objects.end(),
            [](const auto& item) { return item.second.numericPlausible; }));
    }

    struct R16RowMatch
    {
        const PositionResearchRow* row = nullptr;
        std::vector<std::uintptr_t> candidates;
        std::uintptr_t matchedObject = 0;
        std::string status = "NO_MATCH";
    };

    std::vector<R16RowMatch> R16MatchRowsToObjects(
        const std::vector<PositionResearchRow>& rows,
        const std::map<std::uintptr_t, R16PositionObjectEvidence>& objects)
    {
        std::vector<R16RowMatch> matches;
        matches.reserve(rows.size());
        for (const PositionResearchRow& row : rows)
        {
            R16RowMatch match;
            match.row = &row;
            std::vector<std::uintptr_t> anchoredCandidates;
            const double averageTolerance = (std::max)(
                0.0011, std::fabs(row.averagePrice) * 1.0e-8);
            for (const auto& item : objects)
            {
                const R16PositionObjectEvidence& object = item.second;
                if (!object.numericPlausible)
                    continue;
                const long long absoluteQuantity = object.quantity < 0
                    ? -static_cast<long long>(object.quantity)
                    : static_cast<long long>(object.quantity);
                if (absoluteQuantity == row.quantityAbs &&
                    std::fabs(object.averagePrice - row.averagePrice) <= averageTolerance)
                {
                    match.candidates.push_back(item.first);
                    if (object.pointerReferenceCount > 0)
                        anchoredCandidates.push_back(item.first);
                }
            }
            if (anchoredCandidates.size() == 1)
            {
                match.matchedObject = anchoredCandidates.front();
                match.status = "UNIQUE_ANCHORED_QUANTITY_AVERAGE";
            }
            else if (anchoredCandidates.size() > 1)
            {
                match.candidates = std::move(anchoredCandidates);
                match.status = "AMBIGUOUS_ANCHORED_QUANTITY_AVERAGE";
            }
            else if (match.candidates.size() == 1)
            {
                match.matchedObject = match.candidates.front();
                match.status = "UNIQUE_QUANTITY_AVERAGE";
            }
            else if (!match.candidates.empty())
            {
                match.status = "AMBIGUOUS_QUANTITY_AVERAGE";
            }
            matches.push_back(std::move(match));
        }

        std::map<std::uintptr_t, std::size_t> useCounts;
        for (const R16RowMatch& match : matches)
            if (match.matchedObject) ++useCounts[match.matchedObject];
        for (R16RowMatch& match : matches)
        {
            if (match.matchedObject && useCounts[match.matchedObject] != 1)
            {
                match.status = "CONFLICT_OBJECT_MATCHED_TO_MULTIPLE_ROWS";
                match.matchedObject = 0;
            }
        }
        return matches;
    }

    std::size_t R16UniqueMatchedRowCount(
        const std::vector<PositionResearchRow>& rows,
        const std::map<std::uintptr_t, R16PositionObjectEvidence>& objects)
    {
        const std::vector<R16RowMatch> matches =
            R16MatchRowsToObjects(rows, objects);
        return static_cast<std::size_t>(std::count_if(
            matches.begin(), matches.end(),
            [](const R16RowMatch& match) { return match.matchedObject != 0; }));
    }

    std::wstring R16EffectiveCurrency(const R16PositionObjectEvidence& object)
    {
        return object.rplCurrency.layoutValid && !object.rplCurrency.empty
            ? object.rplCurrency.value : object.primaryCurrency.value;
    }

    bool WritePositionCurrencyDirectResearch(
        const Snapshot& snapshot,
        std::string& summaryJson)
    {
        V153GridReadLockGuard lock;
        CreateDirectoryW(kOutputDirectory, nullptr);
        const std::wstring reportPath =
            ReportPath(L"MCST_Position_Currency_Dynamic", snapshot.processId);
        const std::wstring checkpointPath =
            ReportPath(L"MCST_Position_Currency_R16_Checkpoint", snapshot.processId);
        auto writeCheckpoint = [&](const char* phase, const std::string& evidence)
        {
            std::ostringstream checkpoint;
            checkpoint << "MCST POSITION CURRENCY R16 CHECKPOINT\r\n"
                       << "research_build=1.114-R16\r\n"
                       << "bridge_version=" << kBridgeVersion << "\r\n"
                       << "read_only=yes\r\n"
                       << "unknown_functions_called=no\r\n"
                       << "phase_status=RUNNING\r\n"
                       << "current_phase=" << phase << "\r\n"
                       << evidence;
            WriteUtf8File(checkpointPath, checkpoint.str());
        };

        constexpr DWORD kResearchAtonpTimestamp = 0x6A5694FBu;
        constexpr DWORD kResearchAtonpImageSize = 3534848u;
        if (snapshot.atonpTrackerPeTimestamp != kResearchAtonpTimestamp ||
            snapshot.atonpTrackerSize != kResearchAtonpImageSize)
        {
            std::ostringstream blocked;
            blocked << "MCST POSITION CURRENCY R16 TARGETED VERIFICATION\r\n"
                    << "=================================================\r\n"
                    << "research_build=1.114-R16\r\n"
                    << "bridge_version=" << kBridgeVersion << "\r\n"
                    << "status=BLOCKED_ATONPTRACKER_FINGERPRINT_MISMATCH\r\n"
                    << "expected_atonptracker_pe_timestamp=" << HexValue(kResearchAtonpTimestamp) << "\r\n"
                    << "expected_atonptracker_image_size=" << kResearchAtonpImageSize << "\r\n"
                    << "actual_atonptracker_pe_timestamp=" << HexValue(snapshot.atonpTrackerPeTimestamp) << "\r\n"
                    << "actual_atonptracker_image_size=" << snapshot.atonpTrackerSize << "\r\n"
                    << "read_only=yes\r\nunknown_functions_called=no\r\n";
            const bool written = WriteUtf8File(reportPath, blocked.str());
            writeCheckpoint("blocked_fingerprint", "phase_status=BLOCKED\r\n");
            std::ostringstream summary;
            summary << "{\"capture\":\"position_currency_r16\",\"version\":171,"
                    << "\"blocked\":true,\"reason\":\"atonptracker_fingerprint_mismatch\","
                    << "\"report_written\":" << (written ? "true" : "false") << ','
                    << "\"report_path\":" << JsonString(reportPath) << '}';
            summaryJson = summary.str();
            return false;
        }

        const R16FingerprintEvidence fingerprint =
            R16VerifyPositionInterfaceFingerprint(snapshot);
        {
            std::ostringstream evidence;
            evidence << "atcenterproxy_found=" << (fingerprint.module ? "yes" : "no") << "\r\n"
                     << "module_size_match=" << (fingerprint.moduleSizeMatches ? "yes" : "no") << "\r\n"
                     << "vtable_readable=" << (fingerprint.vtableReadable ? "yes" : "no") << "\r\n"
                     << "slot_targets_match=" << (fingerprint.slotsMatch ? "yes" : "no") << "\r\n"
                     << "code_signatures_match=" << (fingerprint.codeSignaturesMatch ? "yes" : "no") << "\r\n";
            writeCheckpoint("verify_position_interface_fingerprint", evidence.str());
        }
        if (!fingerprint.ok)
        {
            std::ostringstream blocked;
            blocked << "MCST POSITION CURRENCY R16 TARGETED VERIFICATION\r\n"
                    << "=================================================\r\n"
                    << "research_build=1.114-R16\r\n"
                    << "bridge_version=" << kBridgeVersion << "\r\n"
                    << "status=BLOCKED_ATCENTERPROXY_FINGERPRINT_MISMATCH\r\n"
                    << "diagnostic=" << V153EscapeFieldUtf8(fingerprint.diagnostic) << "\r\n"
                    << "module_found=" << (fingerprint.module ? "yes" : "no") << "\r\n"
                    << "module_size_match=" << (fingerprint.moduleSizeMatches ? "yes" : "no") << "\r\n"
                    << "vtable_readable=" << (fingerprint.vtableReadable ? "yes" : "no") << "\r\n"
                    << "slot_targets_match=" << (fingerprint.slotsMatch ? "yes" : "no") << "\r\n"
                    << "code_signatures_match=" << (fingerprint.codeSignaturesMatch ? "yes" : "no") << "\r\n";
            for (const R16FingerprintSlot& slot : fingerprint.slots)
            {
                blocked << "SLOT offset=" << HexValue(slot.slotOffset)
                        << " semantic=" << slot.semantic
                        << " expected_rva=" << HexValue(slot.expectedRva)
                        << " actual_rva=" << HexValue(slot.actualRva)
                        << " match=" << (slot.match ? "yes" : "no") << "\r\n";
            }
            blocked << "read_only=yes\r\nunknown_functions_called=no\r\n";
            const bool written = WriteUtf8File(reportPath, blocked.str());
            std::ostringstream finalCheckpoint;
            finalCheckpoint << "MCST POSITION CURRENCY R16 CHECKPOINT\r\n"
                            << "research_build=1.114-R16\r\nbridge_version=" << kBridgeVersion << "\r\n"
                            << "read_only=yes\r\nunknown_functions_called=no\r\n"
                            << "phase_status=BLOCKED\r\ncurrent_phase=verify_position_interface_fingerprint\r\n"
                            << "report_written=" << (written ? "yes" : "no") << "\r\n";
            WriteUtf8File(checkpointPath, finalCheckpoint.str());
            std::ostringstream summary;
            summary << "{\"capture\":\"position_currency_r16\",\"version\":171,"
                    << "\"blocked\":true,\"reason\":\"atcenterproxy_fingerprint_mismatch\","
                    << "\"report_written\":" << (written ? "true" : "false") << ','
                    << "\"report_path\":" << JsonString(reportPath) << '}';
            summaryJson = summary.str();
            return false;
        }

        TrackerCompatibilityProfile trackerProfile =
            ResolveExternalTrackerCompatibilityProfile(snapshot);
        if (!trackerProfile.matched)
        {
            TrackerCompatibilityProfile embedded =
                EmbeddedLegacyTrackerCompatibilityProfile(snapshot);
            if (embedded.matched)
                trackerProfile = embedded;
        }
        std::string tabViewDiagnostic;
        const std::uintptr_t tabView = trackerProfile.matched
            ? FindTabViewObject(snapshot, tabViewDiagnostic, &trackerProfile) : 0;
        std::string flexGridRttiDiagnostic;
        const std::vector<RttiVtableRecord> flexGridRttiVtables = trackerProfile.matched
            ? ResolveRttiVtables(
                snapshot,
                ".?AVCFlexGridImpl@implementation@UILayer@@",
                flexGridRttiDiagnostic)
            : std::vector<RttiVtableRecord>{};
        V153GridSectionResult positions = ReadV153GridSection(
            snapshot,
            flexGridRttiVtables,
            tabView,
            trackerProfile,
            "open_positions",
            trackerProfile.openPositionsPageOffset,
            1,
            8,
            1000,
            1000,
            3);

        std::vector<PositionResearchRow> parsedRows;
        parsedRows.reserve(positions.rows.size());
        for (std::size_t rowIndex = 0; rowIndex < positions.rows.size(); ++rowIndex)
        {
            const auto& fields = positions.rows[rowIndex];
            if (fields.size() < 8)
                continue;
            PositionResearchRow row;
            row.rowIndex = rowIndex;
            row.fields = &fields;
            double quantity = 0.0;
            row.quantityOk = TryParsePositionResearchNumber(fields[4], quantity);
            row.averageOk = TryParsePositionResearchNumber(fields[5], row.averagePrice);
            row.openPlOk = TryParsePositionResearchNumber(fields[6], row.displayedOpenPl);
            if (row.quantityOk)
            {
                const long long signedQuantity = static_cast<long long>(std::llround(quantity));
                row.quantityAbs = signedQuantity < 0 ? -signedQuantity : signedQuantity;
            }
            if (row.quantityOk && row.averageOk && row.quantityAbs > 0)
                parsedRows.push_back(row);
        }
        {
            std::ostringstream evidence;
            evidence << "grid_ok=" << (positions.ok ? "yes" : "no") << "\r\n"
                     << "visible_rows=" << positions.rows.size() << "\r\n"
                     << "parsed_rows=" << parsedRows.size() << "\r\n";
            writeCheckpoint("read_open_positions_reference", evidence.str());
        }

        ExtractorAttempt positionProbe =
            BuildExtractorProbe(snapshot, "position_currency_r16", kExtractOpenPositionsRva);
        const std::uintptr_t tradeInfo = positionProbe.tradeInfo;
        std::uintptr_t root98 = 0;
        std::uintptr_t root88 = 0;
        if (tradeInfo)
        {
            SafeReadValue(reinterpret_cast<void*>(tradeInfo + 0x98), root98);
            SafeReadValue(reinterpret_cast<void*>(tradeInfo + 0x88), root88);
        }
        std::uintptr_t pageObject = 0;
        std::uintptr_t gridObject = 0;
        if (tabView && trackerProfile.matched)
        {
            SafeReadValue(
                reinterpret_cast<void*>(tabView + trackerProfile.openPositionsPageOffset),
                pageObject);
            if (pageObject)
                SafeReadValue(
                    reinterpret_cast<void*>(pageObject + trackerProfile.gridMemberOffset),
                    gridObject);
        }
        std::vector<std::pair<std::uintptr_t, std::string>> seeds;
        if (tradeInfo) seeds.push_back({ tradeInfo, "ITC_TradeInfo" });
        if (root98) seeds.push_back({ root98, "ITC_TradeInfo+0x98" });
        if (root88) seeds.push_back({ root88, "ITC_TradeInfo+0x88" });
        if (tabView) seeds.push_back({ tabView, "CATPTTabView" });
        if (pageObject) seeds.push_back({ pageObject, "OpenPositionsPage" });
        if (gridObject) seeds.push_back({ gridObject, "OpenPositionsGrid" });

        std::vector<PositionResearchRegion> graphRegions;
        std::size_t pointerNodesScanned = 0;
        DiscoverPositionResearchRegions(
            seeds,
            graphRegions,
            pointerNodesScanned,
            std::chrono::steady_clock::now() + std::chrono::seconds(10));
        std::map<std::uintptr_t, R16PositionObjectEvidence> objects;
        R16ScanStats graphDirectStats;
        R16ScanDirectVtableValues(
            graphRegions,
            fingerprint.expectedVtable,
            512ull * 1024ull * 1024ull,
            std::chrono::steady_clock::now() + std::chrono::seconds(30),
            "known_anchor_graph_direct",
            objects,
            graphDirectStats);
        {
            std::ostringstream evidence;
            evidence << "pointer_nodes_scanned=" << pointerNodesScanned << "\r\n"
                     << "graph_regions=" << graphRegions.size() << "\r\n"
                     << "bytes_read=" << graphDirectStats.bytesRead << "\r\n"
                     << "direct_vtable_matches=" << graphDirectStats.directVtableMatches << "\r\n"
                     << "unique_objects=" << objects.size() << "\r\n"
                     << "plausible_objects=" << R16PlausibleObjectCount(objects) << "\r\n";
            writeCheckpoint("scan_anchor_graph_for_exact_vtable", evidence.str());
        }

        R16ScanStats graphPointerStats;
        bool pointerReferenceScanExecuted = false;
        if (R16UniqueMatchedRowCount(parsedRows, objects) < parsedRows.size())
        {
            pointerReferenceScanExecuted = true;
            R16ScanPointerReferences(
                graphRegions,
                fingerprint.expectedVtable,
                512ull * 1024ull * 1024ull,
                std::chrono::steady_clock::now() + std::chrono::seconds(45),
                objects,
                graphPointerStats);
            std::ostringstream evidence;
            evidence << "bytes_read=" << graphPointerStats.bytesRead << "\r\n"
                     << "pointer_reference_matches=" << graphPointerStats.pointerReferenceMatches << "\r\n"
                     << "unique_objects=" << objects.size() << "\r\n"
                     << "plausible_objects=" << R16PlausibleObjectCount(objects) << "\r\n";
            writeCheckpoint("scan_anchor_graph_pointer_references", evidence.str());
        }

        R16ScanStats fallbackStats;
        bool fallbackScanExecuted = false;
        std::size_t fallbackRegionCount = 0;
        if (R16UniqueMatchedRowCount(parsedRows, objects) < parsedRows.size())
        {
            fallbackScanExecuted = true;
            std::map<std::uintptr_t, std::size_t> excludedGraphRegions;
            for (std::size_t index = 0; index < graphRegions.size(); ++index)
                excludedGraphRegions[graphRegions[index].base] = index;
            const std::vector<PositionResearchRegion> fallbackRegions =
                EnumerateFallbackPositionResearchRegions(excludedGraphRegions);
            fallbackRegionCount = fallbackRegions.size();
            R16ScanDirectVtableValues(
                fallbackRegions,
                fingerprint.expectedVtable,
                2ull * 1024ull * 1024ull * 1024ull,
                std::chrono::steady_clock::now() + std::chrono::seconds(90),
                "bounded_process_fallback_direct",
                objects,
                fallbackStats);
            std::ostringstream evidence;
            evidence << "fallback_regions=" << fallbackRegionCount << "\r\n"
                     << "bytes_read=" << fallbackStats.bytesRead << "\r\n"
                     << "direct_vtable_matches=" << fallbackStats.directVtableMatches << "\r\n"
                     << "unique_objects=" << objects.size() << "\r\n"
                     << "plausible_objects=" << R16PlausibleObjectCount(objects) << "\r\n";
            writeCheckpoint("bounded_process_fallback_exact_vtable", evidence.str());
        }

        const std::vector<R16RowMatch> rowMatches =
            R16MatchRowsToObjects(parsedRows, objects);
        std::size_t matchedRows = 0;
        std::size_t currencyDecodedRows = 0;
        std::set<std::uintptr_t> matchedObjects;
        for (const R16RowMatch& match : rowMatches)
        {
            if (!match.matchedObject)
                continue;
            ++matchedRows;
            matchedObjects.insert(match.matchedObject);
            const auto found = objects.find(match.matchedObject);
            if (found == objects.end())
                continue;
            const R16PositionObjectEvidence& object = found->second;
            const bool primaryOk = object.primaryCurrency.layoutValid &&
                object.primaryCurrency.strictCurrencyCode;
            const bool rplOk = object.rplCurrency.layoutValid &&
                (object.rplCurrency.empty || object.rplCurrency.strictCurrencyCode);
            if (primaryOk && rplOk)
                ++currencyDecodedRows;
        }

        const bool verificationComplete = positions.ok && !parsedRows.empty() &&
            parsedRows.size() == positions.rows.size() &&
            matchedRows == parsedRows.size() &&
            matchedObjects.size() == parsedRows.size() &&
            currencyDecodedRows == parsedRows.size();

        std::ostringstream out;
        out << std::setprecision(15)
            << "MCST POSITION CURRENCY R16 TARGETED VERIFICATION\r\n"
            << "=================================================\r\n"
            << "research_build=1.114-R16\r\n"
            << "bridge_version=" << kBridgeVersion << "\r\n"
            << "bridge_protocol=" << mcbridge::kProtocolVersion << "\r\n"
            << "read_only=yes\r\n"
            << "unknown_functions_called=no\r\n"
            << "process_memory_writes=no\r\n"
            << "purpose=verify the R15 ATCenterProxy position interface against visible rows and decode its two bounded currency wstrings\r\n"
            << "process_id=" << snapshot.processId << "\r\n"
            << "verification_status=" << (verificationComplete ? "COMPLETE" : "PARTIAL") << "\r\n\r\n";

        out << "R16 POSITION INTERFACE FINGERPRINT\r\n"
            << "----------------------------------\r\n"
            << "module=ATCenterProxy.dll\r\n"
            << "module_base=" << HexValue(fingerprint.module->base) << "\r\n"
            << "module_size=" << fingerprint.module->size << "\r\n"
            << "module_size_match=" << (fingerprint.moduleSizeMatches ? "yes" : "no") << "\r\n"
            << "vtable=" << HexValue(fingerprint.expectedVtable) << "\r\n"
            << "vtable_rva=0x44D518\r\n"
            << "slot_targets_match=" << (fingerprint.slotsMatch ? "yes" : "no") << "\r\n"
            << "code_signatures_match=" << (fingerprint.codeSignaturesMatch ? "yes" : "no") << "\r\n"
            << "last_update_slot_0x98_target=" << HexValue(fingerprint.lastUpdateTarget) << "\r\n"
            << "last_update_slot_0x98_target_rva=" << HexValue(fingerprint.lastUpdateTargetRva) << "\r\n";
        for (const R16FingerprintSlot& slot : fingerprint.slots)
        {
            out << "SLOT offset=" << HexValue(slot.slotOffset)
                << " semantic=" << slot.semantic
                << " expected_rva=" << HexValue(slot.expectedRva)
                << " actual_rva=" << HexValue(slot.actualRva)
                << " match=" << (slot.match ? "yes" : "no") << "\r\n";
        }
        out << "NOTE: PriceScaleCode first obtains a different interface. The position interface's own +0x60 slot is fingerprint evidence only and is not interpreted as PriceScaleCode.\r\n\r\n";

        out << "OPEN POSITIONS REFERENCE\r\n"
            << "------------------------\r\n"
            << "grid_ok=" << (positions.ok ? "yes" : "no") << "\r\n"
            << "grid_diagnostic=" << V153EscapeField(positions.diagnostic) << "\r\n"
            << "visible_rows=" << positions.rows.size() << "\r\n"
            << "parsed_rows=" << parsedRows.size() << "\r\n";
        for (const PositionResearchRow& row : parsedRows)
        {
            const auto& fields = *row.fields;
            out << "REFERENCE_ROW row=" << row.rowIndex
                << " profile=" << V153EscapeField(fields[0])
                << " account=" << V153EscapeField(fields[1])
                << " symbol=" << V153EscapeField(fields[2])
                << " side=" << V153EscapeField(fields[3])
                << " quantity_text=" << V153EscapeField(fields[4])
                << " average_price_text=" << V153EscapeField(fields[5])
                << " open_pl_text=" << V153EscapeField(fields[6])
                << " last_update=" << V153EscapeField(fields[7]) << "\r\n";
        }
        out << "\r\n";

        out << "TARGETED OBJECT SEARCH COVERAGE\r\n"
            << "-------------------------------\r\n"
            << "pointer_graph_nodes_scanned=" << pointerNodesScanned << "\r\n"
            << "pointer_graph_regions=" << graphRegions.size() << "\r\n"
            << "graph_direct_regions_read=" << graphDirectStats.regionsRead << "\r\n"
            << "graph_direct_bytes_read=" << graphDirectStats.bytesRead << "\r\n"
            << "graph_direct_vtable_matches=" << graphDirectStats.directVtableMatches << "\r\n"
            << "graph_direct_runtime_limit_reached=" << (graphDirectStats.runtimeLimitReached ? "yes" : "no") << "\r\n"
            << "pointer_reference_scan_executed=" << (pointerReferenceScanExecuted ? "yes" : "no") << "\r\n"
            << "pointer_reference_bytes_read=" << graphPointerStats.bytesRead << "\r\n"
            << "pointer_reference_matches=" << graphPointerStats.pointerReferenceMatches << "\r\n"
            << "pointer_reference_runtime_limit_reached=" << (graphPointerStats.runtimeLimitReached ? "yes" : "no") << "\r\n"
            << "fallback_scan_executed=" << (fallbackScanExecuted ? "yes" : "no") << "\r\n"
            << "fallback_regions=" << fallbackRegionCount << "\r\n"
            << "fallback_bytes_read=" << fallbackStats.bytesRead << "\r\n"
            << "fallback_vtable_matches=" << fallbackStats.directVtableMatches << "\r\n"
            << "fallback_runtime_limit_reached=" << (fallbackStats.runtimeLimitReached ? "yes" : "no") << "\r\n"
            << "unique_exact_vtable_objects=" << objects.size() << "\r\n"
            << "numeric_plausible_objects=" << R16PlausibleObjectCount(objects) << "\r\n\r\n";

        out << "PER-ROW OBJECT AND CURRENCY VERIFICATION\r\n"
            << "----------------------------------------\r\n";
        for (const R16RowMatch& match : rowMatches)
        {
            const auto& fields = *match.row->fields;
            out << "ROW_MATCH row=" << match.row->rowIndex
                << " symbol=" << V153EscapeField(fields[2])
                << " status=" << match.status
                << " candidate_count=" << match.candidates.size()
                << " matched_object=" << HexValue(match.matchedObject) << "\r\n";
            for (std::uintptr_t candidateAddress : match.candidates)
            {
                const R16PositionObjectEvidence& object = objects.at(candidateAddress);
                out << "  CANDIDATE object=" << HexValue(candidateAddress)
                    << " quantity=" << object.quantity
                    << " average_price=" << object.averagePrice
                    << " average_delta=" << (object.averagePrice - match.row->averagePrice)
                    << " open_pl=" << object.openPl;
                if (match.row->openPlOk)
                    out << " displayed_open_pl_delta=" << (object.openPl - match.row->displayedOpenPl);
                out << "\r\n";
            }
            if (!match.matchedObject)
                continue;
            const R16PositionObjectEvidence& object = objects.at(match.matchedObject);
            out << "  VERIFIED_FIELDS quantity_offset=0x1A8 quantity=" << object.quantity
                << " average_price_offset=0x1B0 average_price=" << object.averagePrice
                << " open_pl_offset=0x1B8 open_pl=" << object.openPl
                << " realized_pl_offset=0x1C8 realized_pl=" << object.realizedPl << "\r\n"
                << "  PRIMARY_CURRENCY offset=0x308 value=" << V153EscapeField(object.primaryCurrency.value)
                << " layout_valid=" << (object.primaryCurrency.layoutValid ? "yes" : "no")
                << " strict_code=" << (object.primaryCurrency.strictCurrencyCode ? "yes" : "no")
                << " size=" << object.primaryCurrency.size
                << " capacity=" << object.primaryCurrency.capacity
                << " storage=" << (object.primaryCurrency.inlineStorage ? "inline" : "heap")
                << " diagnostic=" << V153EscapeFieldUtf8(object.primaryCurrency.diagnostic) << "\r\n"
                << "  RPL_CURRENCY offset=0x328 value=" << V153EscapeField(object.rplCurrency.value)
                << " layout_valid=" << (object.rplCurrency.layoutValid ? "yes" : "no")
                << " strict_code=" << (object.rplCurrency.strictCurrencyCode ? "yes" : "no")
                << " empty=" << (object.rplCurrency.empty ? "yes" : "no")
                << " size=" << object.rplCurrency.size
                << " capacity=" << object.rplCurrency.capacity
                << " storage=" << (object.rplCurrency.inlineStorage ? "inline" : "heap")
                << " diagnostic=" << V153EscapeFieldUtf8(object.rplCurrency.diagnostic) << "\r\n"
                << "  EFFECTIVE_RPL_CURRENCY value=" << V153EscapeField(R16EffectiveCurrency(object))
                << " rule=RPL_CURRENCY_when_nonempty_otherwise_PRIMARY_CURRENCY\r\n";
        }
        out << "\r\n";

        out << "ALL EXACT-VTABLE OBJECTS\r\n"
            << "------------------------\r\n";
        for (const auto& item : objects)
        {
            const R16PositionObjectEvidence& object = item.second;
            out << "OBJECT address=" << HexValue(object.object)
                << " vtable_stable=" << (object.vtableStable ? "yes" : "no")
                << " numeric_readable=" << (object.numericReadable ? "yes" : "no")
                << " numeric_plausible=" << (object.numericPlausible ? "yes" : "no")
                << " direct_vtable_hit=" << (object.directVtableHit ? "yes" : "no")
                << " pointer_reference_count=" << object.pointerReferenceCount
                << " region_base=" << HexValue(object.regionBase)
                << " region_size=" << object.regionSize
                << " source=" << V153EscapeFieldUtf8(object.discoverySource)
                << " quantity=" << object.quantity
                << " average_price=" << object.averagePrice
                << " open_pl=" << object.openPl
                << " realized_pl=" << object.realizedPl
                << " primary_currency=" << V153EscapeField(object.primaryCurrency.value)
                << " rpl_currency=" << V153EscapeField(object.rplCurrency.value)
                << " effective_rpl_currency=" << V153EscapeField(R16EffectiveCurrency(object))
                << "\r\n";
        }
        if (objects.empty())
            out << "none\r\n";
        out << "\r\n"
            << "R16 RESULT\r\n"
            << "----------\r\n"
            << "matched_rows=" << matchedRows << "\r\n"
            << "matched_unique_objects=" << matchedObjects.size() << "\r\n"
            << "currency_decoded_rows=" << currencyDecodedRows << "\r\n"
            << "verification_complete=" << (verificationComplete ? "yes" : "no") << "\r\n"
            << "interpretation=+0x308 is the primary position-currency string used by CurrencyCode/CurrencyLetter. +0x328 is the CurrencyLetterRPL override; an empty override falls back to +0x308. Production promotion still requires repeat stability and at least one capture with a different native instrument currency.\r\n";

        const bool written = WriteUtf8File(reportPath, out.str());
        std::ostringstream finalCheckpoint;
        finalCheckpoint << "MCST POSITION CURRENCY R16 CHECKPOINT\r\n"
                        << "research_build=1.114-R16\r\n"
                        << "bridge_version=" << kBridgeVersion << "\r\n"
                        << "read_only=yes\r\nunknown_functions_called=no\r\n"
                        << "phase_status=" << (!written ? "WRITE_FAILED" :
                            (verificationComplete ? "OK" : "PARTIAL")) << "\r\n"
                        << "visible_rows=" << positions.rows.size() << "\r\n"
                        << "parsed_rows=" << parsedRows.size() << "\r\n"
                        << "exact_vtable_objects=" << objects.size() << "\r\n"
                        << "numeric_plausible_objects=" << R16PlausibleObjectCount(objects) << "\r\n"
                        << "matched_rows=" << matchedRows << "\r\n"
                        << "matched_unique_objects=" << matchedObjects.size() << "\r\n"
                        << "currency_decoded_rows=" << currencyDecodedRows << "\r\n"
                        << "pointer_reference_scan_executed=" << (pointerReferenceScanExecuted ? "yes" : "no") << "\r\n"
                        << "fallback_scan_executed=" << (fallbackScanExecuted ? "yes" : "no") << "\r\n"
                        << "verification_complete=" << (verificationComplete ? "yes" : "no") << "\r\n"
                        << "report_written=" << (written ? "yes" : "no") << "\r\n";
        const bool checkpointWritten = WriteUtf8File(
            checkpointPath, finalCheckpoint.str());
        std::ostringstream summary;
        summary << "{\"capture\":\"position_currency_r16\",\"version\":171,"
                << "\"read_only\":true,\"unknown_functions_called\":false,"
                << "\"visible_rows\":" << positions.rows.size() << ','
                << "\"parsed_rows\":" << parsedRows.size() << ','
                << "\"exact_vtable_objects\":" << objects.size() << ','
                << "\"numeric_plausible_objects\":" << R16PlausibleObjectCount(objects) << ','
                << "\"matched_rows\":" << matchedRows << ','
                << "\"currency_decoded_rows\":" << currencyDecodedRows << ','
                << "\"verification_complete\":" << (verificationComplete ? "true" : "false") << ','
                << "\"checkpoint_written\":" << (checkpointWritten ? "true" : "false") << ','
                << "\"report_written\":" << (written ? "true" : "false") << ','
                << "\"report_path\":" << JsonString(reportPath) << '}';
        summaryJson = summary.str();
        return written && positions.ok && !positions.rows.empty();
    }


    void AppendV153Section(std::ostringstream& out, const V153GridSectionResult& section, unsigned int columnCount)
    {
        out << "SECTION\t" << section.name
            << '\t' << (section.ok ? "OK" : "FAIL")
            << '\t' << columnCount
            << '\t' << section.rows.size()
            << '\t' << section.reportedRows
            << '\t' << section.callsAttempted
            << '\t' << section.callsSucceeded
            << '\t' << section.sehFailures
            << '\t' << V153EscapeField(section.diagnostic)
            << "\n";

        for (const auto& row : section.rows)
        {
            out << "ROW\t" << section.name;
            for (const auto& value : row)
                out << '\t' << V153EscapeField(value);
            out << "\n";
        }
        out << "ENDSECTION\t" << section.name << "\n";
    }

    bool BuildV153StatusReportSnapshot(
        const Snapshot& snapshot,
        std::string& payload,
        bool forceFreshScan,
        bool& tabViewFound,
        bool& compatibilityMatched,
        std::size_t& pagesRead,
        std::size_t& sehFailures,
        ULONGLONG recoveryTimeBudgetMs = 900,
        std::size_t recoveryByteBudget = 16ull * 1024ull * 1024ull,
        const char* recoveryTier = "fast",
        std::string* tabViewDiagnosticOutput = nullptr)
    {
        V153GridReadLockGuard lock;

        TrackerCompatibilityProfile trackerProfile = ResolveExternalTrackerCompatibilityProfile(snapshot);
        if (!trackerProfile.matched)
        {
            TrackerCompatibilityProfile embedded = EmbeddedLegacyTrackerCompatibilityProfile(snapshot);
            if (embedded.matched)
            {
                trackerProfile = embedded;
            }
        }
        std::string tabViewDiagnostic;
        const std::uintptr_t tabView = trackerProfile.matched
            ? FindTabViewObject(
                snapshot,
                tabViewDiagnostic,
                &trackerProfile,
                forceFreshScan,
                recoveryTimeBudgetMs,
                recoveryByteBudget,
                recoveryTier)
            : 0;
        tabViewFound = tabView != 0;

        std::string flexGridRttiDiagnostic;
        const std::vector<RttiVtableRecord> flexGridRttiVtables = trackerProfile.matched
            ? ResolveRttiVtables(
                snapshot, ".?AVCFlexGridImpl@implementation@UILayer@@", flexGridRttiDiagnostic)
            : std::vector<RttiVtableRecord>{};

        if (!trackerProfile.matched && tabViewDiagnostic.empty())
            tabViewDiagnostic = "Tracker reader blocked before CATPTTabView lookup because no compatibility profile is authorized.";
        if (tabViewDiagnosticOutput)
            *tabViewDiagnosticOutput = tabViewDiagnostic;
        compatibilityMatched = trackerProfile.matched;

        V153GridSectionResult accounts = ReadV153GridSection(
            snapshot, flexGridRttiVtables, tabView, trackerProfile, "accounts",
            trackerProfile.accountsPageOffset, 1, 12, 100, 100, 3);
        V153GridSectionResult positions = ReadV153GridSection(
            snapshot, flexGridRttiVtables, tabView, trackerProfile, "open_positions",
            trackerProfile.openPositionsPageOffset, 1, 8, 1000, 1000, 3);
        V153GridSectionResult positionHistory = ReadV153GridSection(
            snapshot, flexGridRttiVtables, tabView, trackerProfile, "position_history",
            trackerProfile.positionHistoryPageOffset, 1, 8, 5000, 5000, 3);
        if (positionHistory.ok && positionHistory.rows.size() >= 5000)
        {
            positionHistory.ok = false;
            positionHistory.diagnostic +=
                L"; capture limit reached; current-month Realized P/L total withheld";
        }
        V153GridSectionResult monitoringLogs = ReadV153GridSection(
            snapshot, flexGridRttiVtables, tabView, trackerProfile, "recent_logs",
            trackerProfile.logsPageOffset, 1, 6, 200, 200, 3);
        V153GridSectionResult logs = monitoringLogs;
        logs.name = "recent_logs";
        if (logs.rows.size() > 10)
            logs.rows.resize(10);
        monitoringLogs.name = "monitoring_logs";
        logs.diagnostic += L"; display_rows=" + std::to_wstring(logs.rows.size()) +
            L"; monitoring_rows=" + std::to_wstring(monitoringLogs.rows.size());

        const std::size_t pagesOk =
            static_cast<std::size_t>(accounts.ok) +
            static_cast<std::size_t>(positions.ok) +
            static_cast<std::size_t>(monitoringLogs.ok);
        const std::size_t totalSeh = accounts.sehFailures + positions.sehFailures + monitoringLogs.sehFailures;
        pagesRead = pagesOk;
        sehFailures = totalSeh;

        std::ostringstream out;
        out << "MC_TRACKER_STATUS_V1\n";
        out << "META\tbridge_version\t" << kBridgeVersion << "\n";
        out << "META\tprotocol_version\t" << mcbridge::kProtocolVersion << "\n";
        out << "META\tprocess_id\t" << snapshot.processId << "\n";
        out << "META\tcaptured_utc\t" << V153UtcText(snapshot.capturedUtc) << "\n";
        out << "META\ttracker_found\t" << (snapshot.trackerFound ? "true" : "false") << "\n";
        out << "META\ttracker_same_process\t" << (snapshot.trackerInSameProcess ? "true" : "false") << "\n";
        out << "META\tatonptracker_loaded\t" << (snapshot.atonpTrackerBase ? "true" : "false") << "\n";
        out << "META\tatonptracker_pe_timestamp\t" << snapshot.atonpTrackerPeTimestamp << "\n";
        out << "META\tatonptracker_image_size\t" << snapshot.atonpTrackerSize << "\n";
        out << "META\ttracker_compatibility_matched\t" << (trackerProfile.matched ? "true" : "false") << "\n";
        out << "META\ttracker_compatibility_mode\t" << V153EscapeField(trackerProfile.mode) << "\n";
        out << "META\ttracker_compatibility_profile\t" << V153EscapeField(trackerProfile.name) << "\n";
        out << "META\ttracker_compatibility_source\t" << V153EscapeField(trackerProfile.source) << "\n";
        out << "META\ttracker_compatibility_diagnostic\t" << V153EscapeField(trackerProfile.diagnostic) << "\n";
        out << "META\ttabview_diagnostic\t" << V153EscapeFieldUtf8(tabViewDiagnostic) << "\n";
        out << "META\tflexgrid_identity_mode\t"
            << (trackerProfile.externalVerified ? "exact_verified_profile" : "embedded_known_or_adaptive_rtti") << "\n";
        out << "META\tflexgrid_rtti_diagnostic\t" << V153EscapeFieldUtf8(flexGridRttiDiagnostic) << "\n";
        AppendV153Section(out, accounts, 12);
        AppendV153Section(out, positions, 8);
        AppendV153Section(out, positionHistory, 8);
        AppendV153Section(out, logs, 6);
        AppendV153Section(out, monitoringLogs, 6);
        out << "SUMMARY\tpages_ok\t" << pagesOk
            << "\tpages_failed\t" << (3 - pagesOk)
            << "\tseh_failures\t" << totalSeh << "\n";
        out << "END\n";
        payload = out.str();
        return pagesOk > 0 && totalSeh == 0;
    }

    void AppendV178RecoveryMetadata(
        std::string& payload,
        bool attempted,
        bool tabViewFound,
        std::size_t pagesRead,
        std::size_t sehFailures,
        const std::string& explicitResult = {})
    {
        const std::size_t markerEnd = payload.find('\n');
        if (markerEnd == std::string::npos)
            return;

        std::ostringstream metadata;
        metadata << "META\trecovery_attempted\t" << (attempted ? "true" : "false") << "\n";
        const std::string result = !explicitResult.empty() ? explicitResult :
            (!attempted ? "not_needed" :
                (tabViewFound && pagesRead == 3 && sehFailures == 0 ? "complete" : "incomplete"));
        metadata << "META\trecovery_result\t" << V153EscapeFieldUtf8(result) << "\n";
        payload.insert(markerEnd + 1, metadata.str());
    }

    bool BuildV178StatusReportSnapshotWithRecovery(const Snapshot& initialSnapshot, std::string& payload)
    {
        bool tabViewFound = false;
        bool compatibilityMatched = false;
        std::size_t pagesRead = 0;
        std::size_t sehFailures = 0;
        const bool initialBuilt = BuildV153StatusReportSnapshot(
            initialSnapshot,
            payload,
            false,
            tabViewFound,
            compatibilityMatched,
            pagesRead,
            sehFailures);

        // A complete snapshot needs no extra work. An unmatched/unknown build is
        // deliberately not retried because compatibility authorization is absent.
        const bool recoveryEligible =
            initialSnapshot.trackerFound &&
            initialSnapshot.trackerInSameProcess &&
            initialSnapshot.atonpTrackerBase != 0 &&
            compatibilityMatched &&
            pagesRead < 3;
        if (!recoveryEligible)
        {
            if (pagesRead == 3 && sehFailures == 0)
            {
                g_tabViewRecoveryModeActive.store(false);
                g_tabViewPersistentFailureActive.store(false);
                g_tabViewRecoveryFailureStreak.store(0);
            }
            AppendV178RecoveryMetadata(payload, false, tabViewFound, pagesRead, sehFailures);
            return initialBuilt;
        }

        g_tabViewPersistentFailureActive.store(true);

        // One production refresh can issue several Watchdog retries. Do not turn
        // a persistent failure into repeated process scans; one recovery scan is
        // allowed per 30-second window and ordinary snapshot reads continue.
        if (!TryBeginTabViewRecovery())
        {
            AppendV178RecoveryMetadata(
                payload,
                false,
                tabViewFound,
                pagesRead,
                sehFailures,
                "cooldown; failure_streak=" +
                    std::to_string(g_tabViewRecoveryFailureStreak.load()));
            return initialBuilt;
        }

        TabViewRecoveryModeScope recoveryModeScope;

        const unsigned int failureStreak = g_tabViewRecoveryFailureStreak.load();
        const ULONGLONG recoveryStarted = GetTickCount64();
        const mcst::TrackerRecoveryPolicyDecision tier =
            mcst::SelectTrackerRecoveryPolicy({
                failureStreak,
                recoveryStarted,
                g_lastExpandedTabViewRecoveryTick.load(),
                g_lastWideTabViewRecoveryTick.load()
            });
        if (tier.stampWideTick)
            g_lastWideTabViewRecoveryTick.store(recoveryStarted);
        if (tier.stampExpandedTick)
            g_lastExpandedTabViewRecoveryTick.store(recoveryStarted);

        {
            std::ostringstream trace;
            trace << "tier=" << tier.name
                  << " failure_streak=" << failureStreak
                  << " time_budget_ms=" << tier.timeBudgetMs
                  << " byte_budget=" << tier.byteBudget;
            AppendExecutionTrace("tabview_recovery_start", trace.str());
        }

        // Release of V153GridReadLockGuard is guaranteed before this bounded pause
        // and the second pass. The retry only refreshes Bridge-local discovery and
        // performs another read-only capture; it does not manipulate Tracker UI.
        Sleep(250);
        const Snapshot freshSnapshot = CaptureSnapshot();
        bool freshTabViewFound = false;
        bool freshCompatibilityMatched = false;
        std::size_t freshPagesRead = 0;
        std::size_t freshSehFailures = 0;
        std::string recoveryDiagnostic;
        const bool recovered = BuildV153StatusReportSnapshot(
            freshSnapshot,
            payload,
            true,
            freshTabViewFound,
            freshCompatibilityMatched,
            freshPagesRead,
            freshSehFailures,
            tier.timeBudgetMs,
            tier.byteBudget,
            tier.name,
            &recoveryDiagnostic);

        const bool completeRecovery = recovered && freshCompatibilityMatched &&
            freshTabViewFound && freshPagesRead == 3 && freshSehFailures == 0;
        unsigned int resultingFailureStreak = 0;
        if (completeRecovery)
        {
            g_tabViewPersistentFailureActive.store(false);
            g_tabViewRecoveryFailureStreak.store(0);
        }
        else
        {
            resultingFailureStreak = g_tabViewRecoveryFailureStreak.fetch_add(1) + 1;
        }

        std::ostringstream result;
        result << "tier=" << tier.name
               << "; " << (completeRecovery ? "complete" : "incomplete")
               << "; failure_streak=" << resultingFailureStreak;
        if (!recoveryDiagnostic.empty())
            result << "; " << recoveryDiagnostic;
        AppendV178RecoveryMetadata(
            payload,
            true,
            freshTabViewFound,
            freshPagesRead,
            freshSehFailures,
            result.str());
        AppendExecutionTrace(
            completeRecovery ? "tabview_recovery_complete" : "tabview_recovery_incomplete",
            result.str());
        return completeRecovery;
    }


    mcbridge::Status ProcessRequest(
        const mcbridge::MessageHeader& request,
        const std::string& requestPayload,
        std::string& responsePayload)
    {
        (void)requestPayload;
        const auto command = static_cast<mcbridge::Command>(request.command);
        Snapshot snapshot = CaptureSnapshot();

        switch (command)
        {
        case mcbridge::Command::Ping:
            responsePayload = StatusJson(snapshot);
            return mcbridge::Status::Ok;

        case mcbridge::Command::GetStatus:
            responsePayload = StatusJson(snapshot);
            return mcbridge::Status::Ok;

        case mcbridge::Command::GetTrackerMap:
            responsePayload = TrackerMapJson(snapshot);
            return snapshot.trackerFound ? mcbridge::Status::Ok : mcbridge::Status::TrackerNotFound;

        case mcbridge::Command::TakeLocalSnapshot:
        {
            std::vector<std::wstring> paths;
            const bool written = WriteSnapshotReports(snapshot, paths);
            std::ostringstream out;
            out << '{';
            AppendStatusJson(out, snapshot);
            out << ",\"reports_written\":" << (written ? "true" : "false") << ",\"report_paths\":[";
            for (std::size_t index = 0; index < paths.size(); ++index)
            {
                if (index) out << ',';
                out << JsonString(paths[index]);
            }
            out << "]}";
            responsePayload = out.str();
            if (!snapshot.trackerFound)
                return mcbridge::Status::TrackerNotFound;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::ProbeAccountsExtractor:
        {
            ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "extract_accounts", kExtractAccountsRva);
            AcquireSRWLockExclusive(&g_extractorLock);
            g_accountsAttempt = accounts;
            ReleaseSRWLockExclusive(&g_extractorLock);
            responsePayload = ExtractorAttemptJson(accounts);
            return accounts.functionValid && accounts.tabViewValid && accounts.tradeInfoValid
                ? mcbridge::Status::Ok : mcbridge::Status::ExtractorContractNotResolved;
        }

        case mcbridge::Command::ProbeOpenPositionsExtractor:
        {
            ExtractorAttempt positions = BuildExtractorProbe(snapshot, "extract_open_positions", kExtractOpenPositionsRva);
            AcquireSRWLockExclusive(&g_extractorLock);
            g_openPositionsAttempt = positions;
            ReleaseSRWLockExclusive(&g_extractorLock);
            responsePayload = ExtractorAttemptJson(positions);
            return positions.functionValid && positions.tabViewValid && positions.tradeInfoValid
                ? mcbridge::Status::Ok : mcbridge::Status::ExtractorContractNotResolved;
        }

        case mcbridge::Command::RunBothExtractorCalls:
        {
            ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "extract_accounts", kExtractAccountsRva);
            ExtractorAttempt positions = BuildExtractorProbe(snapshot, "extract_open_positions", kExtractOpenPositionsRva);
            accounts.diagnostic += "; V148 refused live execution to protect MultiCharts";
            positions.diagnostic += "; V148 refused live execution to protect MultiCharts";
            StoreExtractorAttempts(accounts, positions);
            responsePayload = "{\"error\":\"live_extractor_calls_disabled_in_v148\",\"accounts\":" +
                ExtractorAttemptJson(accounts) + ",\"open_positions\":" + ExtractorAttemptJson(positions) + "}";
            return mcbridge::Status::ExperimentalCallsDisabled;
        }

        case mcbridge::Command::AnalyzeExtractorContracts:
        {
            ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "extract_accounts", kExtractAccountsRva);
            ExtractorAttempt positions = BuildExtractorProbe(snapshot, "extract_open_positions", kExtractOpenPositionsRva);
            StoreExtractorAttempts(accounts, positions);
            std::wstring reportPath;
            const bool written = WriteContractAnalysisReport(snapshot, accounts, positions, reportPath);
            responsePayload = "{\"accounts\":" + ExtractorContractAnalysisJson(snapshot, accounts) +
                ",\"open_positions\":" + ExtractorContractAnalysisJson(snapshot, positions) +
                ",\"report_written\":" + (written ? std::string("true") : std::string("false")) +
                ",\"report_path\":" + JsonString(reportPath) + "}";
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::AnalyzeExtractorCallers:
        {
            ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "extract_accounts", kExtractAccountsRva);
            ExtractorAttempt positions = BuildExtractorProbe(snapshot, "extract_open_positions", kExtractOpenPositionsRva);
            accounts.diagnostic += "; V148 caller/xref analysis only; candidate is never called";
            positions.diagnostic += "; V148 caller/xref analysis only; candidate is never called";
            StoreExtractorAttempts(accounts, positions);
            const CallerXrefAnalysis analysis = BuildCallerXrefAnalysis(snapshot, accounts, positions);
            std::wstring reportPath;
            const bool written = WriteCallerXrefAnalysisReport(
                snapshot, accounts, positions, analysis, reportPath);
            responsePayload = CallerXrefSummaryJson(snapshot, analysis, reportPath, written);
            if (!accounts.tabViewValid || !analysis.runtimeTable.valid ||
                !analysis.accountsBoundary.enclosingFound ||
                !analysis.positionsBoundary.enclosingFound)
            {
                return mcbridge::Status::ExtractorContractNotResolved;
            }
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }


        case mcbridge::Command::AnalyzeThreeTabAnchors:
        {
            std::wstring reportPath;
            std::string summary;
            const bool written = WriteThreeTabAnchorReport(snapshot, reportPath, summary);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::AnalyzePageVirtualMethods:
        {
            std::wstring reportPath;
            std::string summary;
            const bool written = WritePageVirtualMethodExplorerReport(snapshot, reportPath, summary);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::AnalyzeSlot17Deep:
        {
            std::wstring reportPath;
            std::wstring csvPath;
            std::string summary;
            const bool written = WriteSlot17DeepAnalysisReport(
                snapshot, reportPath, csvPath, summary);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::CaptureResearchBundle:
        {
            std::string summary;
            const bool written = WriteResearchCaptureBundle(snapshot, summary);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::CaptureLiveObjectGraph:
        {
            std::string summary;
            const bool written = WriteLiveObjectGraphCapture(snapshot, summary, "unspecified");
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::CaptureLiveObjectGraphAccounts:
        case mcbridge::Command::CaptureLiveObjectGraphOpenPositions:
        case mcbridge::Command::CaptureLiveObjectGraphLogs:
        {
            const char* pageName = command == mcbridge::Command::CaptureLiveObjectGraphAccounts ? "accounts" :
                (command == mcbridge::Command::CaptureLiveObjectGraphOpenPositions ? "open_positions" : "logs");
            std::string summary;
            const bool written = WriteLiveObjectGraphCapture(snapshot, summary, pageName);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::CaptureTargetedProbeAccounts:
        case mcbridge::Command::CaptureTargetedProbeOpenPositions:
        case mcbridge::Command::CaptureTargetedProbeLogs:
        case mcbridge::Command::CaptureTargetedProbeAll:
        {
            const char* pageName = command == mcbridge::Command::CaptureTargetedProbeAccounts ? "accounts" :
                (command == mcbridge::Command::CaptureTargetedProbeOpenPositions ? "open_positions" :
                (command == mcbridge::Command::CaptureTargetedProbeLogs ? "logs" : "all"));
            std::string summary;
            const bool written = WriteTargetedStructureProbe(snapshot, summary, pageName);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::CaptureFlexGridTextReaderAccounts:
        case mcbridge::Command::CaptureFlexGridTextReaderOpenPositions:
        case mcbridge::Command::CaptureFlexGridTextReaderLogs:
        case mcbridge::Command::CaptureFlexGridTextReaderAll:
        {
            const char* pageName = command == mcbridge::Command::CaptureFlexGridTextReaderAccounts ? "accounts" :
                (command == mcbridge::Command::CaptureFlexGridTextReaderOpenPositions ? "open_positions" :
                (command == mcbridge::Command::CaptureFlexGridTextReaderLogs ? "logs" : "all"));
            std::string summary;
            const bool written = WriteFlexGridTextReader(snapshot, summary, pageName);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::ExtractorCallFailed;
        }

        case mcbridge::Command::CapturePositionCurrencyDirectResearch:
        {
            std::string summary;
            const bool written = WritePositionCurrencyDirectResearch(snapshot, summary);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::ExtractorCallFailed;
        }

        case mcbridge::Command::GetStatusReportSnapshot:
        {
            const bool built = BuildV178StatusReportSnapshotWithRecovery(snapshot, responsePayload);
            return built ? mcbridge::Status::Ok : mcbridge::Status::ExtractorCallFailed;
        }

        case mcbridge::Command::CaptureV152CoordinateMapAccounts:
        case mcbridge::Command::CaptureV152CoordinateMapOpenPositions:
        case mcbridge::Command::CaptureV152CoordinateMapLogs:
        case mcbridge::Command::CaptureV152CoordinateMapAll:
        {
            const char* pageName = command == mcbridge::Command::CaptureV152CoordinateMapAccounts ? "accounts" :
                (command == mcbridge::Command::CaptureV152CoordinateMapOpenPositions ? "open_positions" :
                (command == mcbridge::Command::CaptureV152CoordinateMapLogs ? "logs" : "all"));
            std::string summary;
            const bool written = WriteV152CoordinateMap(snapshot, summary, pageName);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::ExtractorCallFailed;
        }

        case mcbridge::Command::CaptureV151SingleCellAccounts:
        case mcbridge::Command::CaptureV151SingleCellOpenPositions:
        case mcbridge::Command::CaptureV151SingleCellLogs:
        case mcbridge::Command::CaptureV151SingleCellAll:
        {
            const char* pageName = command == mcbridge::Command::CaptureV151SingleCellAccounts ? "accounts" :
                (command == mcbridge::Command::CaptureV151SingleCellOpenPositions ? "open_positions" :
                (command == mcbridge::Command::CaptureV151SingleCellLogs ? "logs" : "all"));
            std::string summary;
            const bool written = WriteV151SingleCellProbe(snapshot, summary, pageName);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::ExtractorCallFailed;
        }

        case mcbridge::Command::CaptureGridInterfaceLocatorAccounts:
        case mcbridge::Command::CaptureGridInterfaceLocatorOpenPositions:
        case mcbridge::Command::CaptureGridInterfaceLocatorLogs:
        case mcbridge::Command::CaptureGridInterfaceLocatorAll:
        {
            const char* pageName = command == mcbridge::Command::CaptureGridInterfaceLocatorAccounts ? "accounts" :
                (command == mcbridge::Command::CaptureGridInterfaceLocatorOpenPositions ? "open_positions" :
                (command == mcbridge::Command::CaptureGridInterfaceLocatorLogs ? "logs" : "all"));
            std::string summary;
            const bool written = WriteGridInterfaceLocator(snapshot, summary, pageName);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::CapturePageObjectProbeAccounts:
        case mcbridge::Command::CapturePageObjectProbeOpenPositions:
        case mcbridge::Command::CapturePageObjectProbeLogs:
        case mcbridge::Command::CapturePageObjectProbeAll:
        {
            const char* pageName = command == mcbridge::Command::CapturePageObjectProbeAccounts ? "accounts" :
                (command == mcbridge::Command::CapturePageObjectProbeOpenPositions ? "open_positions" :
                (command == mcbridge::Command::CapturePageObjectProbeLogs ? "logs" : "all"));
            std::string summary;
            const bool written = WritePageObjectStructureProbe(snapshot, summary, pageName);
            responsePayload = summary;
            return written ? mcbridge::Status::Ok : mcbridge::Status::InternalError;
        }

        case mcbridge::Command::TestUiDispatch:
        {
            ExtractorAttempt accounts = BuildExtractorProbe(snapshot, "ui_noop_accounts", kExtractAccountsRva);
            ExtractorAttempt positions = BuildExtractorProbe(snapshot, "ui_noop_positions", kExtractOpenPositionsRva);
            UiDispatchDiagnostics diagnostics;
            const bool completed = DispatchBothExtractorsOnTrackerThread(snapshot, accounts, positions, diagnostics);
            StoreExtractorAttempts(accounts, positions);
            responsePayload = "{\"no_op_only\":true,\"ui_dispatch\":" +
                UiDispatchDiagnosticsJson(diagnostics) + "}";
            return completed ? mcbridge::Status::Ok : mcbridge::Status::ExtractorCallFailed;
        }

        case mcbridge::Command::GetExtractorStatus:
            responsePayload = CurrentExtractorStatusJson();
            return mcbridge::Status::Ok;

        default:
            responsePayload = "{\"error\":\"unsupported_command\"}";
            return mcbridge::Status::UnsupportedCommand;
        }
    }

    void HandleClient(HANDLE pipe)
    {
        for (;;)
        {
            if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0)
                break;

            mcbridge::MessageHeader request{};
            if (!ReadExact(pipe, &request, sizeof(request)))
                break;

            if (request.magic != mcbridge::kMagic)
            {
                SendResponse(pipe, request, mcbridge::Status::InvalidHeader,
                    "{\"error\":\"invalid_magic\"}");
                break;
            }
            if (request.protocolVersion != mcbridge::kProtocolVersion)
            {
                SendResponse(pipe, request, mcbridge::Status::UnsupportedProtocol,
                    "{\"error\":\"unsupported_protocol\"}");
                break;
            }
            if (request.payloadBytes > mcbridge::kMaximumPayloadBytes)
            {
                SendResponse(pipe, request, mcbridge::Status::PayloadTooLarge,
                    "{\"error\":\"payload_too_large\"}");
                break;
            }

            std::string requestPayload(request.payloadBytes, '\0');
            if (request.payloadBytes > 0 && !ReadExact(pipe, requestPayload.data(), request.payloadBytes))
                break;

            std::string responsePayload;
            mcbridge::Status status = mcbridge::Status::InternalError;
            try
            {
                status = ProcessRequest(request, requestPayload, responsePayload);
            }
            catch (...)
            {
                responsePayload = "{\"error\":\"unhandled_bridge_exception\"}";
                status = mcbridge::Status::InternalError;
            }

            const bool responseWritten = SendResponse(pipe, request, status, responsePayload);
            if (static_cast<mcbridge::Command>(request.command) == mcbridge::Command::RunBothExtractorCalls)
            {
                AcquireSRWLockExclusive(&g_uiDispatchLock);
                g_uiDispatchDiagnostics.pipeResponseWritten = responseWritten;
                ReleaseSRWLockExclusive(&g_uiDispatchLock);
            }
            if (!responseWritten)
                break;
        }
    }

    DWORD WINAPI BridgeClientThread(void* parameter)
    {
        HANDLE pipe = static_cast<HANDLE>(parameter);
        AppendExecutionTrace("pipe_client_thread_start");
        try
        {
            HandleClient(pipe);
        }
        catch (...)
        {
            AppendExecutionTrace("pipe_client_thread_cpp_exception");
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        AppendExecutionTrace("pipe_client_thread_exit");
        return 0;
    }

    DWORD WINAPI BridgeWorker(void*)
    {
        g_workerThreadId = GetCurrentThreadId();
        const HRESULT comInitializeHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInitialized = SUCCEEDED(comInitializeHr);
        g_state.store(RuntimeState::Running);
        AppendExecutionTrace("pipe_listener_started");

        try
        {
            Snapshot startup = CaptureSnapshot();
            std::vector<std::wstring> ignored;
            WriteSnapshotReports(startup, ignored);
        }
        catch (...) {}

        while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0)
        {
            HANDLE pipe = CreateNamedPipeW(
                kPipeName,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                kPipeBufferBytes,
                kPipeBufferBytes,
                5000,
                nullptr);

            if (pipe == INVALID_HANDLE_VALUE)
            {
                const DWORD error = GetLastError();
                g_lastError.store(error);
                std::ostringstream trace; trace << "error=" << error;
                AppendExecutionTrace("pipe_create_failed", trace.str());
                Sleep(100);
                continue;
            }

            BOOL connected = ConnectNamedPipe(pipe, nullptr);
            if (!connected && GetLastError() == ERROR_PIPE_CONNECTED)
                connected = TRUE;

            if (!connected || WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(pipe);
                continue;
            }

            HANDLE clientThread = CreateThread(nullptr, 0, BridgeClientThread, pipe, 0, nullptr);
            if (clientThread)
            {
                CloseHandle(clientThread);
                // The listener immediately creates the next pipe instance. This keeps
                // the service name available while the current client remains connected.
                continue;
            }

            const DWORD error = GetLastError();
            std::ostringstream trace; trace << "error=" << error;
            AppendExecutionTrace("pipe_client_thread_create_failed", trace.str());
            BridgeClientThread(pipe);
        }

        AppendExecutionTrace("pipe_listener_stopped");
        if (g_state.load() != RuntimeState::Failed)
            g_state.store(RuntimeState::Stopped);
        if (comInitialized)
            CoUninitialize();
        return 0;
    }

    void WakePipeServer()
    {
        HANDLE pipe = CreateFileW(
            kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pipe);
    }

    void CloseBridgeHandles()
    {
        if (g_workerThread)
        {
            CloseHandle(g_workerThread);
            g_workerThread = nullptr;
        }
        if (g_stopEvent)
        {
            CloseHandle(g_stopEvent);
            g_stopEvent = nullptr;
        }
        if (g_singletonMutex)
        {
            CloseHandle(g_singletonMutex);
            g_singletonMutex = nullptr;
        }
        if (g_uiDispatchEvent)
        {
            CloseHandle(g_uiDispatchEvent);
            g_uiDispatchEvent = nullptr;
        }
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr)
    {
        // Do not wait while holding the loader lock. A normal explicit shutdown uses
        // MCBridge_Shutdown; FreeLibrary fallback only signals the worker.
        if (g_stopEvent)
            SetEvent(g_stopEvent);
    }
    return TRUE;
}

int __stdcall MCBridge_Initialize()
{
    RuntimeState expected = RuntimeState::Stopped;
    if (!g_state.compare_exchange_strong(expected, RuntimeState::Starting))
    {
        if (expected == RuntimeState::Running || expected == RuntimeState::Starting)
            return MC_BRIDGE_ALREADY_INITIALIZED;
        return MC_BRIDGE_ERROR;
    }

    g_initializeThreadId = GetCurrentThreadId();
    g_lastHeartbeatTick.store(GetTickCount64());

    const std::wstring processPath = ProcessPath();
    if (Lower(BaseName(processPath)) != L"multicharts64.exe")
    {
        g_state.store(RuntimeState::Failed);
        return MC_BRIDGE_NOT_MULTICHARTS_PROCESS;
    }

    g_singletonMutex = CreateMutexW(nullptr, FALSE, kSingletonMutexName);
    if (!g_singletonMutex)
    {
        g_lastError.store(GetLastError());
        g_state.store(RuntimeState::Failed);
        return MC_BRIDGE_ERROR;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(g_singletonMutex);
        g_singletonMutex = nullptr;
        g_state.store(RuntimeState::Failed);
        return MC_BRIDGE_ANOTHER_PROCESS_OWNS_SINGLETON;
    }


    // Keep one explicit module reference for the process lifetime. This prevents
    // PowerLanguage from unloading the DLL while the bridge worker thread still
    // executes inside it. The process releases the reference automatically on exit.
    if (!g_selfReference && g_module)
    {
        std::vector<wchar_t> modulePath(32768, L'\0');
        const DWORD modulePathLength = GetModuleFileNameW(
            g_module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (modulePathLength > 0 && modulePathLength < modulePath.size())
            g_selfReference = LoadLibraryW(modulePath.data());
    }
    if (!g_selfReference)
    {
        g_lastError.store(GetLastError());
        CloseBridgeHandles();
        g_state.store(RuntimeState::Failed);
        return MC_BRIDGE_SELF_REFERENCE_FAILED;
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent)
    {
        g_lastError.store(GetLastError());
        CloseBridgeHandles();
        g_state.store(RuntimeState::Failed);
        return MC_BRIDGE_ERROR;
    }

    g_workerThread = CreateThread(nullptr, 0, BridgeWorker, nullptr, 0, &g_workerThreadId);
    if (!g_workerThread)
    {
        g_lastError.store(GetLastError());
        CloseBridgeHandles();
        g_state.store(RuntimeState::Failed);
        return MC_BRIDGE_THREAD_CREATE_FAILED;
    }

    // Give the worker a short chance to publish its initial state, without making
    // PowerLanguage wait for Tracker availability.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (g_state.load() == RuntimeState::Running || g_state.load() == RuntimeState::Failed)
            break;
        Sleep(10);
    }

    return LastSnapshotHasTracker()
        ? MC_BRIDGE_OK
        : MC_BRIDGE_STARTED_WAITING_FOR_TRACKER;
}

int __stdcall MCBridge_Heartbeat()
{
    g_lastHeartbeatTick.store(GetTickCount64());
    const RuntimeState state = g_state.load();
    if (state == RuntimeState::Running)
        return LastSnapshotHasTracker()
            ? MC_BRIDGE_OK
            : MC_BRIDGE_STARTED_WAITING_FOR_TRACKER;
    if (state == RuntimeState::Starting)
        return MC_BRIDGE_STARTED_WAITING_FOR_TRACKER;
    return MC_BRIDGE_NOT_INITIALIZED;
}

int __stdcall MCBridge_GetState()
{
    return static_cast<int>(g_state.load());
}

int __stdcall MCBridge_Shutdown()
{
    const RuntimeState state = g_state.load();
    if (state == RuntimeState::Stopped)
        return MC_BRIDGE_NOT_INITIALIZED;

    g_state.store(RuntimeState::Stopping);
    if (g_stopEvent)
        SetEvent(g_stopEvent);
    if (g_workerThread)
        CancelSynchronousIo(g_workerThread);
    WakePipeServer();

    if (g_workerThread)
    {
        const DWORD wait = WaitForSingleObject(g_workerThread, 3000);
        if (wait != WAIT_OBJECT_0)
            return MC_BRIDGE_SHUTDOWN_TIMEOUT;
    }

    CloseBridgeHandles();
    g_state.store(RuntimeState::Stopped);
    return MC_BRIDGE_OK;
}

int __stdcall MCBridge_GetVersion()
{
    return kBridgeVersion;
}
