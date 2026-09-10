#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include "AutoTradingReader.h"
#include "CompatibilityManager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <unordered_map>
#include <cwctype>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct ModuleRange
    {
        std::wstring name;
        ULONG_PTR base = 0;
        SIZE_T size = 0;
    };

    struct StrategyObjectCount
    {
        bool chartingFound = false;
        bool profileMatched = false;
        int objects = 0;
        int active = 0;
        int readFailures = 0;
        std::wstring compatibilityProfile;
        std::wstring compatibilitySource;
        std::wstring compatibilityDiagnostic;
    };

    std::mutex g_cacheMutex;
    AutoTradingReadResult g_cachedResult;
    std::chrono::steady_clock::time_point g_lastAttempt{};
    bool g_haveCachedResult = false;

    std::wstring LowerCopy(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    std::wstring WindowText(HWND window)
    {
        const int length = GetWindowTextLengthW(window);
        if (length <= 0)
            return L"";
        std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1u);
        GetWindowTextW(window, buffer.data(), static_cast<int>(buffer.size()));
        return buffer.data();
    }

    bool IsMultiChartsMainWindow(HWND window)
    {
        if (!IsWindowVisible(window))
            return false;

        const std::wstring title = LowerCopy(WindowText(window));
        wchar_t className[512]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        const std::wstring windowClass = LowerCopy(className);

        if (title.find(L"quote manager") != std::wstring::npos ||
            title.find(L"portfolio trader") != std::wstring::npos ||
            title.find(L"order and position tracker") != std::wstring::npos)
        {
            return false;
        }

        return title.find(L"multicharts") != std::wstring::npos ||
               windowClass.find(L"atl_mcmdimainframe") != std::wstring::npos;
    }

    std::set<DWORD> CollectMultiChartsProcessIds()
    {
        std::set<DWORD> processIds;

        // Primary route: identify MultiCharts main-frame windows. Do not require
        // visibility here; a minimized or temporarily hidden MC instance must
        // still remain part of AutoTrading monitoring.
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            const std::wstring title = LowerCopy(WindowText(window));
            wchar_t className[512]{};
            GetClassNameW(window, className, static_cast<int>(std::size(className)));
            const std::wstring windowClass = LowerCopy(className);

            if (title.find(L"quote manager") != std::wstring::npos ||
                title.find(L"portfolio trader") != std::wstring::npos ||
                title.find(L"order and position tracker") != std::wstring::npos)
                return TRUE;

            if (title.find(L"multicharts") == std::wstring::npos &&
                windowClass.find(L"atl_mcmdimainframe") == std::wstring::npos)
                return TRUE;

            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId != 0)
                reinterpret_cast<std::set<DWORD>*>(parameter)->insert(processId);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&processIds));

        // Recovery route: enumerate MultiCharts executables as well. This covers
        // an instance whose main window title/class is temporarily unavailable.
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snapshot, &entry))
            {
                do
                {
                    const std::wstring executable = LowerCopy(entry.szExeFile);
                    if (executable.find(L"multicharts") != std::wstring::npos)
                        processIds.insert(entry.th32ProcessID);
                } while (Process32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }

        return processIds;
    }

    std::vector<ModuleRange> EnumerateModules(DWORD processId)
    {
        std::vector<ModuleRange> modules;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snapshot == INVALID_HANDLE_VALUE)
            return modules;

        MODULEENTRY32W moduleEntry{};
        moduleEntry.dwSize = sizeof(moduleEntry);
        if (Module32FirstW(snapshot, &moduleEntry))
        {
            do
            {
                ModuleRange module;
                module.name = moduleEntry.szModule;
                module.base = reinterpret_cast<ULONG_PTR>(moduleEntry.modBaseAddr);
                module.size = moduleEntry.modBaseSize;
                modules.push_back(std::move(module));
            } while (Module32NextW(snapshot, &moduleEntry));
        }
        CloseHandle(snapshot);
        return modules;
    }

    bool ReadRemotePeTimestamp(HANDLE process, ULONG_PTR moduleBase, DWORD& timestamp)
    {
        IMAGE_DOS_HEADER dosHeader{};
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(moduleBase),
            &dosHeader, sizeof(dosHeader), &bytesRead) || bytesRead != sizeof(dosHeader) ||
            dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
        {
            return false;
        }

        DWORD peSignature = 0;
        if (!ReadProcessMemory(process,
            reinterpret_cast<LPCVOID>(moduleBase + static_cast<ULONG_PTR>(dosHeader.e_lfanew)),
            &peSignature, sizeof(peSignature), &bytesRead) ||
            bytesRead != sizeof(peSignature) || peSignature != IMAGE_NT_SIGNATURE)
        {
            return false;
        }

        IMAGE_FILE_HEADER fileHeader{};
        const ULONG_PTR fileHeaderAddress = moduleBase + static_cast<ULONG_PTR>(dosHeader.e_lfanew) + sizeof(DWORD);
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(fileHeaderAddress),
            &fileHeader, sizeof(fileHeader), &bytesRead) || bytesRead != sizeof(fileHeader))
        {
            return false;
        }

        timestamp = fileHeader.TimeDateStamp;
        return true;
    }

    bool IsReadableProtection(DWORD protection)
    {
        if ((protection & PAGE_GUARD) != 0)
            return false;
        const DWORD base = protection & 0xFFu;
        return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
               base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
               base == PAGE_EXECUTE_WRITECOPY;
    }

    StrategyObjectCount CountStrategyObjects(DWORD processId)
    {
        StrategyObjectCount result;
        constexpr SIZE_T kReadChunk = 1024u * 1024u;

        const auto modules = EnumerateModules(processId);
        const ModuleRange* charting = nullptr;
        for (const auto& module : modules)
        {
            if (LowerCopy(module.name) == L"charting.dll")
            {
                charting = &module;
                break;
            }
        }
        if (charting == nullptr)
            return result;

        result.chartingFound = true;
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (process == nullptr)
        {
            ++result.readFailures;
            return result;
        }

        DWORD peTimestamp = 0;
        if (!ReadRemotePeTimestamp(process, charting->base, peTimestamp))
        {
            ++result.readFailures;
            result.compatibilityDiagnostic = L"Charting.dll PE timestamp could not be read.";
            CloseHandle(process);
            return result;
        }

        const CompatibilityProfile profile = ResolveCompatibilityProfile(
            peTimestamp, static_cast<unsigned long long>(charting->size));
        result.profileMatched = profile.matched;
        result.compatibilityProfile = profile.name;
        result.compatibilitySource = profile.source;
        result.compatibilityDiagnostic = profile.diagnostic;
        if (!profile.matched)
        {
            CloseHandle(process);
            return result;
        }

        const ULONG_PTR wantedVtable = charting->base + profile.strategyVtableRva;
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        ULONG_PTR address = reinterpret_cast<ULONG_PTR>(systemInfo.lpMinimumApplicationAddress);
        const ULONG_PTR maximum = reinterpret_cast<ULONG_PTR>(systemInfo.lpMaximumApplicationAddress);
        std::vector<unsigned char> bytes;

        while (address < maximum)
        {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0)
                break;

            const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(memory.BaseAddress);
            const SIZE_T regionSize = memory.RegionSize;
            const bool candidate = memory.State == MEM_COMMIT &&
                IsReadableProtection(memory.Protect) &&
                (memory.Type == MEM_PRIVATE || memory.Type == MEM_MAPPED);

            if (candidate)
            {
                for (SIZE_T offset = 0; offset < regionSize; offset += kReadChunk)
                {
                    const SIZE_T requested = (std::min)(kReadChunk, regionSize - offset);
                    bytes.resize(requested);
                    SIZE_T received = 0;
                    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + offset), bytes.data(), requested, &received) ||
                        received < sizeof(ULONG_PTR))
                    {
                        continue;
                    }

                    for (SIZE_T index = 0; index + sizeof(ULONG_PTR) <= received; index += sizeof(ULONG_PTR))
                    {
                        ULONG_PTR value = 0;
                        std::memcpy(&value, bytes.data() + index, sizeof(value));
                        if (value != wantedVtable)
                            continue;

                        const ULONG_PTR objectBase = base + offset + index;
                        unsigned char state = 0xFF;
                        SIZE_T stateBytes = 0;
                        if (ReadProcessMemory(process,
                            reinterpret_cast<LPCVOID>(objectBase + profile.autoTradingOffset),
                            &state, sizeof(state), &stateBytes) &&
                            stateBytes == sizeof(state) && state <= 1)
                        {
                            ++result.objects;
                            if (state == 1)
                                ++result.active;
                        }
                        else
                        {
                            ++result.readFailures;
                        }
                    }
                }
            }

            const ULONG_PTR next = base + regionSize;
            if (next <= address)
                break;
            address = next;
        }

        CloseHandle(process);
        return result;
    }

    AutoTradingReadResult ReadNow()
    {
        AutoTradingReadResult result;
        result.lastAttempt = std::chrono::system_clock::now();
        const auto processIds = CollectMultiChartsProcessIds();
        result.processesScanned = static_cast<int>(processIds.size());

        bool chartingFound = false;
        bool anyProfileMatched = false;
        std::wstring compatibilityFailure;
        for (DWORD processId : processIds)
        {
            const StrategyObjectCount count = CountStrategyObjects(processId);
            chartingFound = chartingFound || count.chartingFound;
            anyProfileMatched = anyProfileMatched || count.profileMatched;
            result.strategyObjectsFound += count.objects;
            result.activeStrategies += count.active;
            result.readFailures += count.readFailures;
            if (count.profileMatched && result.compatibilityProfile.empty())
            {
                result.compatibilityProfile = count.compatibilityProfile;
                result.compatibilitySource = count.compatibilitySource;
            }
            if (!count.compatibilityDiagnostic.empty() && !count.profileMatched)
                compatibilityFailure = count.compatibilityDiagnostic;
        }

        result.succeeded = chartingFound && anyProfileMatched && result.strategyObjectsFound > 0;
        if (result.succeeded)
            result.lastSuccessfulRead = result.lastAttempt;
        std::wostringstream diagnostic;
        diagnostic << L"Processes " << result.processesScanned
                   << L", charts " << result.strategyObjectsFound
                   << L", active " << result.activeStrategies
                   << L", read failures " << result.readFailures;
        if (processIds.empty())
            diagnostic << L". No MultiCharts main windows were found.";
        else if (!chartingFound)
            diagnostic << L". Charting.dll was not found in the detected processes.";
        else if (!anyProfileMatched)
            diagnostic << L". " << compatibilityFailure;
        else if (result.strategyObjectsFound == 0)
            diagnostic << L". The selected compatibility profile found no charts.";
        if (!result.compatibilityProfile.empty())
            diagnostic << L". Profile: " << result.compatibilityProfile;
        result.diagnostic = diagnostic.str();
        return result;
    }
}

AutoTradingReadResult ReadAutoTradingStatus(int cacheMinutes, bool forceRefresh)
{
    const auto now = std::chrono::steady_clock::now();
    const auto successCache = std::chrono::minutes((std::max)(1, cacheMinutes));
    const auto failureCache = std::chrono::minutes(1);

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        if (!forceRefresh && g_haveCachedResult)
        {
            const auto cacheDuration = g_cachedResult.succeeded ? successCache : failureCache;
            if (now - g_lastAttempt < cacheDuration)
            {
                AutoTradingReadResult cached = g_cachedResult;
                cached.fromCache = true;
                return cached;
            }
        }
    }

    AutoTradingReadResult current = ReadNow();
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cachedResult = current;
        g_lastAttempt = now;
        g_haveCachedResult = true;
    }
    return current;
}


namespace
{
    struct CandidateVtable
    {
        ULONG_PTR address = 0;
        SIZE_T totalHits = 0;
        std::vector<ULONG_PTR> objects;
    };

    std::wstring HexValue(ULONG_PTR value)
    {
        std::wostringstream out;
        out << L"0x" << std::hex << std::uppercase << value;
        return out.str();
    }

    std::vector<std::wstring> WindowTitlesForProcess(DWORD processId)
    {
        struct Context { DWORD pid; std::vector<std::wstring>* titles; } context{ processId, nullptr };
        std::vector<std::wstring> titles;
        context.titles = &titles;
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto* ctx = reinterpret_cast<Context*>(parameter);
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == ctx->pid && IsWindowVisible(window))
            {
                const std::wstring title = WindowText(window);
                if (!title.empty())
                    ctx->titles->push_back(title);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return titles;
    }

    std::vector<DWORD> FindAsciiOccurrences(HANDLE process, const ModuleRange& module, const std::string& needle)
    {
        std::vector<DWORD> result;
        if (module.size == 0 || needle.empty())
            return result;
        std::vector<unsigned char> image(module.size);
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(module.base), image.data(), image.size(), &got) || got < needle.size())
            return result;
        image.resize(got);
        for (SIZE_T i = 0; i + needle.size() <= image.size(); ++i)
        {
            if (std::memcmp(image.data() + i, needle.data(), needle.size()) == 0)
                result.push_back(static_cast<DWORD>(i));
        }
        return result;
    }

    std::vector<CandidateVtable> CollectChartingPointerCandidates(HANDLE process, const ModuleRange& charting, int& regionsRead, int& readFailures, SIZE_T maxObjectsPerCandidate = 64)
    {
        constexpr SIZE_T kChunk = 1024u * 1024u;
        struct CandidateAccumulator { SIZE_T totalHits = 0; std::vector<ULONG_PTR> objects; };
        std::unordered_map<ULONG_PTR, CandidateAccumulator> candidates;
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        ULONG_PTR address = reinterpret_cast<ULONG_PTR>(info.lpMinimumApplicationAddress);
        const ULONG_PTR maximum = reinterpret_cast<ULONG_PTR>(info.lpMaximumApplicationAddress);
        std::vector<unsigned char> buffer;

        while (address < maximum)
        {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0)
                break;
            const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(memory.BaseAddress);
            const SIZE_T regionSize = memory.RegionSize;
            const bool candidateRegion = memory.State == MEM_COMMIT && IsReadableProtection(memory.Protect) &&
                (memory.Type == MEM_PRIVATE || memory.Type == MEM_MAPPED);
            if (candidateRegion)
            {
                ++regionsRead;
                for (SIZE_T regionOffset = 0; regionOffset < regionSize; regionOffset += kChunk)
                {
                    const SIZE_T requested = (std::min)(kChunk, regionSize - regionOffset);
                    buffer.resize(requested);
                    SIZE_T got = 0;
                    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + regionOffset), buffer.data(), requested, &got) || got < sizeof(ULONG_PTR))
                    {
                        ++readFailures;
                        continue;
                    }
                    for (SIZE_T i = 0; i + sizeof(ULONG_PTR) <= got; i += sizeof(ULONG_PTR))
                    {
                        ULONG_PTR value = 0;
                        std::memcpy(&value, buffer.data() + i, sizeof(value));
                        if (value < charting.base || value >= charting.base + charting.size)
                            continue;
                        auto& accumulator = candidates[value];
                        ++accumulator.totalHits;
                        if (accumulator.objects.size() < maxObjectsPerCandidate)
                            accumulator.objects.push_back(base + regionOffset + i);
                    }
                }
            }
            const ULONG_PTR next = base + regionSize;
            if (next <= address)
                break;
            address = next;
        }

        std::vector<CandidateVtable> result;
        result.reserve(candidates.size());
        for (auto& pair : candidates)
        {
            if (pair.second.totalHits < 2)
                continue;
            CandidateVtable item;
            item.address = pair.first;
            item.totalHits = pair.second.totalHits;
            item.objects = std::move(pair.second.objects);
            result.push_back(std::move(item));
        }
        std::sort(result.begin(), result.end(), [](const CandidateVtable& a, const CandidateVtable& b) {
            if (a.totalHits != b.totalHits) return a.totalHits > b.totalHits;
            return a.address < b.address;
        });
        return result;
    }

    std::vector<SIZE_T> FindMixedBooleanOffsets(HANDLE process, const std::vector<ULONG_PTR>& objects)
    {
        std::vector<SIZE_T> offsets;
        if (objects.size() < 2)
            return offsets;
        constexpr SIZE_T kLimit = 0x300;
        for (SIZE_T offset = 0; offset < kLimit; ++offset)
        {
            bool sawZero = false;
            bool sawOne = false;
            bool valid = true;
            for (ULONG_PTR object : objects)
            {
                unsigned char value = 0xFF;
                SIZE_T got = 0;
                if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(object + offset), &value, sizeof(value), &got) || got != sizeof(value) || value > 1)
                {
                    valid = false;
                    break;
                }
                sawZero = sawZero || value == 0;
                sawOne = sawOne || value == 1;
            }
            if (valid && sawZero && sawOne)
                offsets.push_back(offset);
        }
        return offsets;
    }
}

bool WriteAutoTradingCompatibilityDiagnostics(const std::wstring& path, std::wstring& diagnostic)
{
    std::wofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        diagnostic = L"Could not create AutoTrading compatibility diagnostics: " + path;
        return false;
    }

    out << L"MCST-Watchdog AutoTrading Compatibility Diagnostics 0.571\n"
        << L"==========================================================\n\n"
        << L"Passive diagnostics only: no clicks, no input and no writes to MultiCharts memory.\n"
        << L"Purpose: locate candidate Charting.dll vtables and boolean fields after an MC update.\n\n";

    const auto processIds = CollectMultiChartsProcessIds();
    out << L"Detected MultiCharts processes: " << processIds.size() << L"\n\n";
    constexpr ULONG_PTR kOldVtableRva = 0xA42048;
    constexpr SIZE_T kOldAutoTradingOffset = 0x142;

    for (DWORD processId : processIds)
    {
        out << L"------------------------------------------------------------\n";
        out << L"PID: " << processId << L"\n";
        for (const auto& title : WindowTitlesForProcess(processId))
            out << L"Window: " << title << L"\n";

        const auto modules = EnumerateModules(processId);
        const ModuleRange* charting = nullptr;
        for (const auto& module : modules)
        {
            if (LowerCopy(module.name) == L"charting.dll")
            {
                charting = &module;
                break;
            }
        }
        if (charting == nullptr)
        {
            out << L"Charting.dll: NOT FOUND\n\n";
            continue;
        }

        out << L"Charting.dll base: " << HexValue(charting->base) << L"\n";
        out << L"Charting.dll size: " << charting->size << L" bytes\n";
        out << L"Old expected vtable: " << HexValue(charting->base + kOldVtableRva)
            << L" (RVA " << HexValue(kOldVtableRva) << L")\n";
        out << L"Old expected AutoTrading offset: " << HexValue(kOldAutoTradingOffset) << L"\n";

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (process == nullptr)
        {
            out << L"OpenProcess failed, error=" << GetLastError() << L"\n\n";
            continue;
        }

        DWORD peTimestamp = 0;
        if (ReadRemotePeTimestamp(process, charting->base, peTimestamp))
            out << L"Charting.dll PE timestamp: " << HexValue(peTimestamp) << L"\n";

        const auto strings = FindAsciiOccurrences(process, *charting, "CStrategyObject");
        out << L"ASCII CStrategyObject occurrences in Charting.dll: " << strings.size() << L"\n";
        for (SIZE_T i = 0; i < (std::min)(strings.size(), static_cast<SIZE_T>(20)); ++i)
            out << L"  string RVA " << HexValue(strings[i]) << L"\n";

        int regionsRead = 0;
        int scanReadFailures = 0;
        auto candidates = CollectChartingPointerCandidates(process, *charting, regionsRead, scanReadFailures);
        out << L"Readable private/mapped regions scanned: " << regionsRead << L"\n";
        out << L"Chunk read failures: " << scanReadFailures << L"\n";
        out << L"Repeated Charting.dll pointer candidates: " << candidates.size() << L"\n\n";

        const SIZE_T candidateLimit = (std::min)(candidates.size(), static_cast<SIZE_T>(80));
        for (SIZE_T i = 0; i < candidateLimit; ++i)
        {
            const auto& candidate = candidates[i];
            const ULONG_PTR rva = candidate.address - charting->base;
            const auto booleanOffsets = FindMixedBooleanOffsets(process, candidate.objects);
            out << L"Candidate #" << (i + 1)
                << L" vtable=" << HexValue(candidate.address)
                << L" rva=" << HexValue(rva)
                << L" object_hits=" << candidate.totalHits
                << L" sampled_objects=" << candidate.objects.size();
            if (rva == kOldVtableRva)
                out << L" [OLD EXPECTED VTABLE]";
            out << L"\n";
            out << L"  first object addresses:";
            for (SIZE_T j = 0; j < (std::min)(candidate.objects.size(), static_cast<SIZE_T>(8)); ++j)
                out << L" " << HexValue(candidate.objects[j]);
            out << L"\n";
            out << L"  mixed 0/1 offsets (0x000-0x2FF):";
            if (booleanOffsets.empty())
                out << L" none";
            else
            {
                for (SIZE_T j = 0; j < (std::min)(booleanOffsets.size(), static_cast<SIZE_T>(60)); ++j)
                    out << L" " << HexValue(booleanOffsets[j]);
                if (booleanOffsets.size() > 60)
                    out << L" ... (" << booleanOffsets.size() << L" total)";
            }
            out << L"\n\n";
        }

        CloseHandle(process);
    }

    out << L"Interpretation notes\n"
        << L"--------------------\n"
        << L"1. A candidate whose object_hits approximately matches the chart/strategy count is important.\n"
        << L"2. A mixed 0/1 offset shared by those objects may be an AutoTrading-related field.\n"
        << L"3. The report alone does not authorize a production offset; findings must be verified by toggling known charts.\n";
    out.close();
    diagnostic = L"AutoTrading compatibility diagnostics written to:\n" + path;
    return true;
}

namespace
{
    struct VerificationCandidate
    {
        ULONG_PTR rva = 0;
        int processesPresent = 0;
        SIZE_T objectCount = 0;
        std::vector<SIZE_T> zeroCounts;
        std::vector<SIZE_T> oneCounts;
        std::vector<SIZE_T> invalidCounts;
    };

    struct VerificationMatch
    {
        ULONG_PTR rva = 0;
        SIZE_T offset = 0;
        SIZE_T zeros = 0;
        SIZE_T ones = 0;
        SIZE_T invalid = 0;
        SIZE_T objects = 0;
        int processesPresent = 0;
        int distance = 0;
    };

    bool ReadObjectPrefix(HANDLE process, ULONG_PTR object, std::vector<unsigned char>& bytes)
    {
        constexpr SIZE_T kPrefixSize = 0x300;
        bytes.resize(kPrefixSize);
        SIZE_T got = 0;
        return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(object), bytes.data(), bytes.size(), &got) && got == bytes.size();
    }
}

bool WriteAutoTradingCandidateVerification(const std::wstring& path, int expectedActiveStrategies, std::wstring& diagnostic)
{
    std::wofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        diagnostic = L"Could not create AutoTrading candidate verification report: " + path;
        return false;
    }

    out << L"MCST-Watchdog AutoTrading Candidate Verification 0.572\n"
        << L"========================================================\n\n"
        << L"Passive verification only: no clicks, no input and no writes to MultiCharts memory.\n"
        << L"Known current AutoTrading ON count: " << expectedActiveStrategies << L"\n"
        << L"Purpose: rank Charting.dll vtable RVA + boolean offset combinations against that known count.\n\n";

    const auto processIds = CollectMultiChartsProcessIds();
    out << L"Detected MultiCharts processes: " << processIds.size() << L"\n\n";
    if (processIds.empty())
    {
        diagnostic = L"No MultiCharts main windows were found.";
        return false;
    }

    constexpr SIZE_T kFieldLimit = 0x300;
    constexpr SIZE_T kMaxObjectsPerCandidate = 10000;
    std::map<ULONG_PTR, VerificationCandidate> aggregate;
    DWORD commonTimestamp = 0;
    SIZE_T compatibleProcesses = 0;

    for (DWORD processId : processIds)
    {
        out << L"------------------------------------------------------------\n";
        out << L"PID: " << processId << L"\n";
        for (const auto& title : WindowTitlesForProcess(processId))
            out << L"Window: " << title << L"\n";

        const auto modules = EnumerateModules(processId);
        const ModuleRange* charting = nullptr;
        for (const auto& module : modules)
        {
            if (LowerCopy(module.name) == L"charting.dll")
            {
                charting = &module;
                break;
            }
        }
        if (charting == nullptr)
        {
            out << L"Charting.dll: NOT FOUND\n\n";
            continue;
        }

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (process == nullptr)
        {
            out << L"OpenProcess failed, error=" << GetLastError() << L"\n\n";
            continue;
        }

        DWORD timestamp = 0;
        ReadRemotePeTimestamp(process, charting->base, timestamp);
        if (commonTimestamp == 0)
            commonTimestamp = timestamp;
        out << L"Charting.dll base: " << HexValue(charting->base) << L"\n";
        out << L"Charting.dll size: " << charting->size << L" bytes\n";
        out << L"Charting.dll PE timestamp: " << HexValue(timestamp) << L"\n";

        int regionsRead = 0;
        int readFailures = 0;
        auto candidates = CollectChartingPointerCandidates(process, *charting, regionsRead, readFailures, kMaxObjectsPerCandidate);
        out << L"Readable private/mapped regions scanned: " << regionsRead << L"\n";
        out << L"Chunk read failures: " << readFailures << L"\n";
        out << L"Repeated Charting.dll pointer candidates: " << candidates.size() << L"\n";

        std::set<ULONG_PTR> seenRvasThisProcess;
        SIZE_T candidatesAnalyzed = 0;
        SIZE_T objectsRead = 0;
        SIZE_T objectReadFailures = 0;
        std::vector<unsigned char> objectBytes;

        for (const auto& candidate : candidates)
        {
            if (candidate.totalHits < 2 || candidate.objects.size() < 2)
                continue;
            const ULONG_PTR rva = candidate.address - charting->base;
            auto& item = aggregate[rva];
            item.rva = rva;
            if (item.zeroCounts.empty())
            {
                item.zeroCounts.assign(kFieldLimit, 0);
                item.oneCounts.assign(kFieldLimit, 0);
                item.invalidCounts.assign(kFieldLimit, 0);
            }
            if (seenRvasThisProcess.insert(rva).second)
                ++item.processesPresent;

            ++candidatesAnalyzed;
            for (ULONG_PTR object : candidate.objects)
            {
                ++item.objectCount;
                if (!ReadObjectPrefix(process, object, objectBytes))
                {
                    ++objectReadFailures;
                    for (SIZE_T offset = 0; offset < kFieldLimit; ++offset)
                        ++item.invalidCounts[offset];
                    continue;
                }
                ++objectsRead;
                for (SIZE_T offset = 0; offset < kFieldLimit; ++offset)
                {
                    const unsigned char value = objectBytes[offset];
                    if (value == 0)
                        ++item.zeroCounts[offset];
                    else if (value == 1)
                        ++item.oneCounts[offset];
                    else
                        ++item.invalidCounts[offset];
                }
            }
        }

        out << L"Candidates analyzed: " << candidatesAnalyzed << L"\n";
        out << L"Object prefixes read: " << objectsRead << L"\n";
        out << L"Object prefix read failures: " << objectReadFailures << L"\n\n";
        ++compatibleProcesses;
        CloseHandle(process);
    }

    std::vector<VerificationMatch> matches;
    const int requiredProcesses = static_cast<int>(compatibleProcesses);
    for (const auto& pair : aggregate)
    {
        const auto& candidate = pair.second;
        if (candidate.processesPresent != requiredProcesses || candidate.objectCount < 2)
            continue;
        for (SIZE_T offset = 0; offset < kFieldLimit; ++offset)
        {
            const SIZE_T zeros = candidate.zeroCounts[offset];
            const SIZE_T ones = candidate.oneCounts[offset];
            const SIZE_T invalid = candidate.invalidCounts[offset];
            const SIZE_T valid = zeros + ones;
            if (valid < 2 || zeros == 0 || ones == 0)
                continue;
            // Require at least 95% of the sampled objects to contain a real boolean at this offset.
            if (valid * 100 < candidate.objectCount * 95)
                continue;
            VerificationMatch match;
            match.rva = candidate.rva;
            match.offset = offset;
            match.zeros = zeros;
            match.ones = ones;
            match.invalid = invalid;
            match.objects = candidate.objectCount;
            match.processesPresent = candidate.processesPresent;
            match.distance = std::abs(static_cast<int>(ones) - expectedActiveStrategies);
            matches.push_back(match);
        }
    }

    std::sort(matches.begin(), matches.end(), [](const VerificationMatch& a, const VerificationMatch& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        if (a.invalid != b.invalid) return a.invalid < b.invalid;
        if (a.objects != b.objects) return a.objects < b.objects;
        if (a.rva != b.rva) return a.rva < b.rva;
        return a.offset < b.offset;
    });

    out << L"============================================================\n";
    out << L"RANKED MATCHES SHARED BY ALL DETECTED MC PROCESSES\n";
    out << L"============================================================\n";
    out << L"Expected ON count: " << expectedActiveStrategies << L"\n";
    out << L"Required process coverage: " << requiredProcesses << L"\n";
    out << L"Boolean validity requirement: at least 95%\n\n";

    SIZE_T exactCount = 0;
    for (const auto& match : matches)
        if (match.distance == 0)
            ++exactCount;
    out << L"Exact ON-count matches: " << exactCount << L"\n\n";

    const SIZE_T reportLimit = (std::min)(matches.size(), static_cast<SIZE_T>(150));
    for (SIZE_T index = 0; index < reportLimit; ++index)
    {
        const auto& match = matches[index];
        out << L"Rank #" << (index + 1)
            << L"  RVA=" << HexValue(match.rva)
            << L"  offset=" << HexValue(match.offset)
            << L"  ON=" << match.ones
            << L"  OFF=" << match.zeros
            << L"  invalid=" << match.invalid
            << L"  objects=" << match.objects
            << L"  processes=" << match.processesPresent
            << L"  distance=" << match.distance;
        if (match.distance == 0)
            out << L"  [EXACT ACTIVE COUNT]";
        if (match.offset == 0x142)
            out << L"  [OLD FIELD OFFSET]";
        out << L"\n";
    }

    if (matches.empty())
        out << L"No shared high-validity mixed boolean candidates were found.\n";

    out << L"\nInterpretation\n"
        << L"--------------\n"
        << L"1. Exact count matches are candidates, not automatic proof.\n"
        << L"2. Strong candidates should remain stable across refreshes and react predictably when one known chart is toggled.\n"
        << L"3. The next verification step is a controlled ON/OFF toggle comparison for the best few RVA/offset pairs.\n"
        << L"4. No candidate is activated in the production reader by this diagnostic version.\n";
    out.close();

    std::wostringstream message;
    message << L"AutoTrading candidate verification written to:\n" << path
            << L"\n\nExpected active strategies: " << expectedActiveStrategies
            << L"\nExact candidate matches: " << exactCount;
    diagnostic = message.str();
    return true;
}

namespace
{
    struct ToggleCandidateSpec
    {
        ULONG_PTR rva;
        SIZE_T offset;
    };

    struct ToggleCandidateState
    {
        ULONG_PTR rva = 0;
        SIZE_T offset = 0;
        SIZE_T on = 0;
        SIZE_T off = 0;
        SIZE_T invalid = 0;
        SIZE_T objects = 0;
        int processes = 0;
    };

    constexpr ToggleCandidateSpec kToggleCandidates[] =
    {
        { 0x3CFCF, 0x78 },
        { 0x3CFCF, 0x1A0 },
        { 0x9A2B4, 0x0C },
        { 0x9A2B4, 0x13C },
        { 0x1912D5, 0x38 },
        { 0x1912D5, 0x160 },
        { 0xBA0000, 0x292 },
        { 0xBA0000, 0x2D2 },
    };

    bool CollectToggleStates(std::vector<ToggleCandidateState>& states, std::wstring& diagnostic)
    {
        states.clear();
        for (const auto& spec : kToggleCandidates)
        {
            ToggleCandidateState state;
            state.rva = spec.rva;
            state.offset = spec.offset;
            states.push_back(state);
        }

        const auto processIds = CollectMultiChartsProcessIds();
        if (processIds.empty())
        {
            diagnostic = L"No MultiCharts main windows were found.";
            return false;
        }

        for (DWORD processId : processIds)
        {
            const auto modules = EnumerateModules(processId);
            const ModuleRange* charting = nullptr;
            for (const auto& module : modules)
            {
                if (LowerCopy(module.name) == L"charting.dll")
                {
                    charting = &module;
                    break;
                }
            }
            if (charting == nullptr)
                continue;

            HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
            if (process == nullptr)
                continue;

            int regionsRead = 0;
            int failures = 0;
            auto candidates = CollectChartingPointerCandidates(process, *charting, regionsRead, failures, 10000);
            std::map<ULONG_PTR, const CandidateVtable*> byRva;
            for (const auto& candidate : candidates)
                byRva[candidate.address - charting->base] = &candidate;

            for (auto& state : states)
            {
                const auto found = byRva.find(state.rva);
                if (found == byRva.end())
                    continue;
                ++state.processes;
                for (ULONG_PTR object : found->second->objects)
                {
                    unsigned char value = 0xFF;
                    SIZE_T got = 0;
                    ++state.objects;
                    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(object + state.offset), &value, 1, &got) || got != 1)
                    {
                        ++state.invalid;
                    }
                    else if (value == 1)
                    {
                        ++state.on;
                    }
                    else if (value == 0)
                    {
                        ++state.off;
                    }
                    else
                    {
                        ++state.invalid;
                    }
                }
            }
            CloseHandle(process);
        }

        diagnostic = L"Toggle candidate states collected.";
        return true;
    }

    bool SaveToggleBaseline(const std::wstring& path, const std::vector<ToggleCandidateState>& states)
    {
        std::wofstream out(path, std::ios::out | std::ios::trunc);
        if (!out.is_open()) return false;
        for (const auto& s : states)
            out << std::hex << s.rva << L' ' << s.offset << std::dec << L' ' << s.on << L' ' << s.off << L' ' << s.invalid << L' ' << s.objects << L' ' << s.processes << L'\n';
        return true;
    }

    bool LoadToggleBaseline(const std::wstring& path, std::vector<ToggleCandidateState>& states)
    {
        std::wifstream in(path);
        if (!in.is_open()) return false;
        states.clear();
        while (in)
        {
            ToggleCandidateState s;
            in >> std::hex >> s.rva >> s.offset >> std::dec >> s.on >> s.off >> s.invalid >> s.objects >> s.processes;
            if (in) states.push_back(s);
        }
        return !states.empty();
    }
}

bool RunAutoTradingToggleVerification(const std::wstring& reportPath, const std::wstring& baselinePath, std::wstring& diagnostic)
{
    std::vector<ToggleCandidateState> current;
    if (!CollectToggleStates(current, diagnostic))
        return false;

    std::vector<ToggleCandidateState> baseline;
    if (!LoadToggleBaseline(baselinePath, baseline))
    {
        if (!SaveToggleBaseline(baselinePath, current))
        {
            diagnostic = L"Could not save toggle verification baseline: " + baselinePath;
            return false;
        }

        std::wofstream out(reportPath, std::ios::out | std::ios::trunc);
        out << L"MCST-Watchdog AutoTrading Toggle Verification 0.573\n"
            << L"=====================================================\n\n"
            << L"BASELINE CAPTURED\n\n"
            << L"Expected current state: 19 charts, 18 ON, 1 OFF.\n"
            << L"Now turn exactly ONE currently active strategy AutoTrading OFF.\n"
            << L"Do not close charts or MultiCharts. Then press 'AT Compare' once.\n\n";
        for (SIZE_T i = 0; i < current.size(); ++i)
        {
            const auto& s = current[i];
            out << L"Candidate #" << (i + 1) << L"  RVA=" << HexValue(s.rva) << L"  offset=" << HexValue(s.offset)
                << L"  objects=" << s.objects << L"  ON=" << s.on << L"  OFF=" << s.off << L"  invalid=" << s.invalid
                << L"  processes=" << s.processes << L"\n";
        }
        diagnostic = L"Baseline captured. Turn exactly one active strategy AutoTrading OFF, then press AT Compare.";
        return true;
    }

    std::wofstream out(reportPath, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        diagnostic = L"Could not create toggle verification report: " + reportPath;
        return false;
    }
    out << L"MCST-Watchdog AutoTrading Toggle Verification 0.573\n"
        << L"=====================================================\n\n"
        << L"CONTROLLED COMPARISON\n"
        << L"Expected change after turning one active strategy OFF:\n"
        << L"  objects unchanged, ON -1, OFF +1, invalid unchanged.\n\n";

    int perfect = 0;
    for (SIZE_T i = 0; i < current.size() && i < baseline.size(); ++i)
    {
        const auto& a = baseline[i];
        const auto& b = current[i];
        const long long dOn = static_cast<long long>(b.on) - static_cast<long long>(a.on);
        const long long dOff = static_cast<long long>(b.off) - static_cast<long long>(a.off);
        const long long dObj = static_cast<long long>(b.objects) - static_cast<long long>(a.objects);
        const long long dInvalid = static_cast<long long>(b.invalid) - static_cast<long long>(a.invalid);
        const bool pass = dOn == -1 && dOff == 1 && dObj == 0 && dInvalid == 0;
        if (pass) ++perfect;
        out << L"Candidate #" << (i + 1) << L"  RVA=" << HexValue(b.rva) << L"  offset=" << HexValue(b.offset) << L"\n"
            << L"  baseline: objects=" << a.objects << L" ON=" << a.on << L" OFF=" << a.off << L" invalid=" << a.invalid << L"\n"
            << L"  current : objects=" << b.objects << L" ON=" << b.on << L" OFF=" << b.off << L" invalid=" << b.invalid << L"\n"
            << L"  delta   : objects=" << dObj << L" ON=" << dOn << L" OFF=" << dOff << L" invalid=" << dInvalid << L"\n"
            << L"  result  : " << (pass ? L"PERFECT MATCH" : L"NO MATCH") << L"\n\n";
    }
    out << L"SUMMARY\n-------\nPerfect matches: " << perfect << L"\n";
    if (perfect == 1)
        out << L"One candidate reacted exactly as expected. This is a strong production-reader candidate.\n";
    else if (perfect == 0)
        out << L"No candidate reacted exactly as expected. Verify that exactly one active strategy was turned OFF.\n";
    else
        out << L"Multiple candidates reacted correctly. A reverse OFF->ON comparison may still be required.\n";

    DeleteFileW(baselinePath.c_str());
    diagnostic = L"Toggle comparison completed. Report written to:\n" + reportPath;
    return true;
}


namespace
{
    constexpr DWORD kDynamicBaselineMagic = 0x4454534D; // "MSTD"
    constexpr DWORD kDynamicBaselineVersion = 1;
    constexpr SIZE_T kDynamicPrefixSize = 0x300;
    constexpr SIZE_T kDynamicMaxObjectsPerVtable = 512;

#pragma pack(push, 1)
    struct DynamicBaselineHeader
    {
        DWORD magic = kDynamicBaselineMagic;
        DWORD version = kDynamicBaselineVersion;
        DWORD processCount = 0;
        DWORD reserved = 0;
    };

    struct DynamicProcessHeader
    {
        DWORD processId = 0;
        DWORD candidateCount = 0;
        ULONGLONG chartingBase = 0;
        DWORD peTimestamp = 0;
        DWORD reserved = 0;
    };

    struct DynamicCandidateHeader
    {
        ULONGLONG rva = 0;
        DWORD objectCount = 0;
        DWORD prefixSize = static_cast<DWORD>(kDynamicPrefixSize);
    };

    struct DynamicObjectHeader
    {
        ULONGLONG address = 0;
    };
#pragma pack(pop)

    struct DynamicObjectSnapshot
    {
        ULONG_PTR address = 0;
        std::vector<unsigned char> bytes;
    };

    struct DynamicCandidateSnapshot
    {
        ULONG_PTR rva = 0;
        std::vector<DynamicObjectSnapshot> objects;
    };

    struct DynamicProcessSnapshot
    {
        DWORD processId = 0;
        ULONG_PTR chartingBase = 0;
        DWORD peTimestamp = 0;
        std::vector<DynamicCandidateSnapshot> candidates;
    };

    bool CollectDynamicSnapshots(std::vector<DynamicProcessSnapshot>& snapshots, std::wstring& diagnostic)
    {
        snapshots.clear();
        const auto processIds = CollectMultiChartsProcessIds();
        if (processIds.empty())
        {
            diagnostic = L"No MultiCharts main windows were found.";
            return false;
        }

        SIZE_T totalCandidates = 0;
        SIZE_T totalObjects = 0;
        SIZE_T prefixFailures = 0;

        for (DWORD processId : processIds)
        {
            const auto modules = EnumerateModules(processId);
            const ModuleRange* charting = nullptr;
            for (const auto& module : modules)
            {
                if (LowerCopy(module.name) == L"charting.dll")
                {
                    charting = &module;
                    break;
                }
            }
            if (charting == nullptr)
                continue;

            HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
            if (process == nullptr)
                continue;

            DynamicProcessSnapshot processSnapshot;
            processSnapshot.processId = processId;
            processSnapshot.chartingBase = charting->base;
            ReadRemotePeTimestamp(process, charting->base, processSnapshot.peTimestamp);

            int regionsRead = 0;
            int scanFailures = 0;
            auto candidates = CollectChartingPointerCandidates(process, *charting, regionsRead, scanFailures, kDynamicMaxObjectsPerVtable);
            for (const auto& candidate : candidates)
            {
                // Very large vtable populations generate enormous noise and cannot represent
                // the user's small chart/strategy population. Keep a generous upper bound.
                if (candidate.totalHits < 2 || candidate.totalHits > kDynamicMaxObjectsPerVtable)
                    continue;

                DynamicCandidateSnapshot candidateSnapshot;
                candidateSnapshot.rva = candidate.address - charting->base;
                candidateSnapshot.objects.reserve(candidate.objects.size());

                for (ULONG_PTR objectAddress : candidate.objects)
                {
                    DynamicObjectSnapshot object;
                    object.address = objectAddress;
                    object.bytes.resize(kDynamicPrefixSize);
                    SIZE_T got = 0;
                    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(objectAddress), object.bytes.data(), object.bytes.size(), &got) || got != object.bytes.size())
                    {
                        ++prefixFailures;
                        continue;
                    }
                    candidateSnapshot.objects.push_back(std::move(object));
                    ++totalObjects;
                }

                if (!candidateSnapshot.objects.empty())
                {
                    processSnapshot.candidates.push_back(std::move(candidateSnapshot));
                    ++totalCandidates;
                }
            }

            CloseHandle(process);
            if (!processSnapshot.candidates.empty())
                snapshots.push_back(std::move(processSnapshot));
        }

        std::wostringstream message;
        message << L"Processes " << snapshots.size()
                << L", candidate vtables " << totalCandidates
                << L", object prefixes " << totalObjects
                << L", prefix failures " << prefixFailures;
        diagnostic = message.str();
        return !snapshots.empty();
    }

    bool SaveDynamicBaseline(const std::wstring& path, const std::vector<DynamicProcessSnapshot>& snapshots)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;

        DynamicBaselineHeader header;
        header.processCount = static_cast<DWORD>(snapshots.size());
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));

        for (const auto& process : snapshots)
        {
            DynamicProcessHeader processHeader;
            processHeader.processId = process.processId;
            processHeader.candidateCount = static_cast<DWORD>(process.candidates.size());
            processHeader.chartingBase = static_cast<ULONGLONG>(process.chartingBase);
            processHeader.peTimestamp = process.peTimestamp;
            out.write(reinterpret_cast<const char*>(&processHeader), sizeof(processHeader));

            for (const auto& candidate : process.candidates)
            {
                DynamicCandidateHeader candidateHeader;
                candidateHeader.rva = static_cast<ULONGLONG>(candidate.rva);
                candidateHeader.objectCount = static_cast<DWORD>(candidate.objects.size());
                out.write(reinterpret_cast<const char*>(&candidateHeader), sizeof(candidateHeader));

                for (const auto& object : candidate.objects)
                {
                    DynamicObjectHeader objectHeader;
                    objectHeader.address = static_cast<ULONGLONG>(object.address);
                    out.write(reinterpret_cast<const char*>(&objectHeader), sizeof(objectHeader));
                    out.write(reinterpret_cast<const char*>(object.bytes.data()), static_cast<std::streamsize>(object.bytes.size()));
                }
            }
        }
        return out.good();
    }

    bool LoadDynamicBaseline(const std::wstring& path, std::vector<DynamicProcessSnapshot>& snapshots)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
            return false;

        DynamicBaselineHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || header.magic != kDynamicBaselineMagic || header.version != kDynamicBaselineVersion)
            return false;

        snapshots.clear();
        snapshots.reserve(header.processCount);
        for (DWORD p = 0; p < header.processCount; ++p)
        {
            DynamicProcessHeader processHeader{};
            in.read(reinterpret_cast<char*>(&processHeader), sizeof(processHeader));
            if (!in) return false;

            DynamicProcessSnapshot process;
            process.processId = processHeader.processId;
            process.chartingBase = static_cast<ULONG_PTR>(processHeader.chartingBase);
            process.peTimestamp = processHeader.peTimestamp;
            process.candidates.reserve(processHeader.candidateCount);

            for (DWORD c = 0; c < processHeader.candidateCount; ++c)
            {
                DynamicCandidateHeader candidateHeader{};
                in.read(reinterpret_cast<char*>(&candidateHeader), sizeof(candidateHeader));
                if (!in || candidateHeader.prefixSize != kDynamicPrefixSize) return false;

                DynamicCandidateSnapshot candidate;
                candidate.rva = static_cast<ULONG_PTR>(candidateHeader.rva);
                candidate.objects.reserve(candidateHeader.objectCount);
                for (DWORD o = 0; o < candidateHeader.objectCount; ++o)
                {
                    DynamicObjectHeader objectHeader{};
                    in.read(reinterpret_cast<char*>(&objectHeader), sizeof(objectHeader));
                    if (!in) return false;
                    DynamicObjectSnapshot object;
                    object.address = static_cast<ULONG_PTR>(objectHeader.address);
                    object.bytes.resize(kDynamicPrefixSize);
                    in.read(reinterpret_cast<char*>(object.bytes.data()), static_cast<std::streamsize>(object.bytes.size()));
                    if (!in) return false;
                    candidate.objects.push_back(std::move(object));
                }
                process.candidates.push_back(std::move(candidate));
            }
            snapshots.push_back(std::move(process));
        }
        return true;
    }

    const DynamicProcessSnapshot* FindDynamicProcess(const std::vector<DynamicProcessSnapshot>& snapshots, DWORD processId)
    {
        for (const auto& process : snapshots)
            if (process.processId == processId)
                return &process;
        return nullptr;
    }

    const DynamicCandidateSnapshot* FindDynamicCandidate(const DynamicProcessSnapshot& process, ULONG_PTR rva)
    {
        for (const auto& candidate : process.candidates)
            if (candidate.rva == rva)
                return &candidate;
        return nullptr;
    }

    const DynamicObjectSnapshot* FindDynamicObject(const DynamicCandidateSnapshot& candidate, ULONG_PTR address)
    {
        for (const auto& object : candidate.objects)
            if (object.address == address)
                return &object;
        return nullptr;
    }

    struct DynamicChange
    {
        DWORD processId = 0;
        ULONG_PTR rva = 0;
        ULONG_PTR objectAddress = 0;
        SIZE_T offset = 0;
        unsigned char before = 0;
        unsigned char after = 0;
        SIZE_T baselineObjects = 0;
        SIZE_T currentObjects = 0;
    };
}

bool RunAutoTradingDynamicChangeDetection(const std::wstring& reportPath, const std::wstring& baselinePath, std::wstring& diagnostic)
{
    std::vector<DynamicProcessSnapshot> current;
    if (!CollectDynamicSnapshots(current, diagnostic))
        return false;

    std::vector<DynamicProcessSnapshot> baseline;
    if (!LoadDynamicBaseline(baselinePath, baseline))
    {
        if (!SaveDynamicBaseline(baselinePath, current))
        {
            diagnostic = L"Could not save dynamic AutoTrading baseline: " + baselinePath;
            return false;
        }

        std::wofstream out(reportPath, std::ios::out | std::ios::trunc);
        if (out.is_open())
        {
            out << L"MCST-Watchdog AutoTrading Two-Instance Candidate Confirmation 0.575\n"
                << L"=========================================================\n\n"
                << L"BASELINE CAPTURED\n\n"
                << L"Passive diagnostics only: no clicks, no input and no writes to MultiCharts memory.\n"
                << L"Now change exactly ONE active strategy AutoTrading from ON to OFF.\n"
                << L"Do not close charts or restart MultiCharts. Then press 'AT Detect Change'.\n\n"
                << L"Captured processes: " << current.size() << L"\n";
            for (const auto& process : current)
            {
                SIZE_T objects = 0;
                for (const auto& candidate : process.candidates) objects += candidate.objects.size();
                out << L"PID " << process.processId;
                const auto titles = WindowTitlesForProcess(process.processId);
                if (!titles.empty()) out << L"  Window: " << titles.front();
                out << L": candidate vtables=" << process.candidates.size()
                    << L", object prefixes=" << objects
                    << L", Charting.dll timestamp=" << HexValue(process.peTimestamp) << L"\n";
            }
        }
        diagnostic = L"Dynamic baseline captured. Turn exactly one active strategy AutoTrading OFF, then press AT Detect Change.";
        return true;
    }

    std::vector<DynamicChange> oneToZero;
    std::vector<DynamicChange> zeroToOne;
    SIZE_T comparedObjects = 0;
    SIZE_T disappearedObjects = 0;
    SIZE_T newObjects = 0;

    for (const auto& baseProcess : baseline)
    {
        const DynamicProcessSnapshot* currentProcess = FindDynamicProcess(current, baseProcess.processId);
        if (currentProcess == nullptr || currentProcess->peTimestamp != baseProcess.peTimestamp)
            continue;

        for (const auto& baseCandidate : baseProcess.candidates)
        {
            const DynamicCandidateSnapshot* currentCandidate = FindDynamicCandidate(*currentProcess, baseCandidate.rva);
            if (currentCandidate == nullptr)
                continue;

            for (const auto& baseObject : baseCandidate.objects)
            {
                const DynamicObjectSnapshot* currentObject = FindDynamicObject(*currentCandidate, baseObject.address);
                if (currentObject == nullptr)
                {
                    ++disappearedObjects;
                    continue;
                }
                ++comparedObjects;
                for (SIZE_T offset = 0; offset < kDynamicPrefixSize; ++offset)
                {
                    const unsigned char before = baseObject.bytes[offset];
                    const unsigned char after = currentObject->bytes[offset];
                    if (before == after || before > 1 || after > 1)
                        continue;
                    DynamicChange change;
                    change.processId = baseProcess.processId;
                    change.rva = baseCandidate.rva;
                    change.objectAddress = baseObject.address;
                    change.offset = offset;
                    change.before = before;
                    change.after = after;
                    change.baselineObjects = baseCandidate.objects.size();
                    change.currentObjects = currentCandidate->objects.size();
                    if (before == 1 && after == 0)
                        oneToZero.push_back(change);
                    else if (before == 0 && after == 1)
                        zeroToOne.push_back(change);
                }
            }

            for (const auto& currentObject : currentCandidate->objects)
                if (FindDynamicObject(baseCandidate, currentObject.address) == nullptr)
                    ++newObjects;
        }
    }

    auto stableFirst = [](const DynamicChange& a, const DynamicChange& b) {
        const bool aStable = a.baselineObjects == a.currentObjects;
        const bool bStable = b.baselineObjects == b.currentObjects;
        if (aStable != bStable) return aStable > bStable;
        if (a.baselineObjects != b.baselineObjects) return a.baselineObjects < b.baselineObjects;
        if (a.processId != b.processId) return a.processId < b.processId;
        if (a.rva != b.rva) return a.rva < b.rva;
        if (a.objectAddress != b.objectAddress) return a.objectAddress < b.objectAddress;
        return a.offset < b.offset;
    };
    std::sort(oneToZero.begin(), oneToZero.end(), stableFirst);
    std::sort(zeroToOne.begin(), zeroToOne.end(), stableFirst);

    std::wofstream out(reportPath, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        diagnostic = L"Could not create dynamic AutoTrading change report: " + reportPath;
        return false;
    }

    out << L"MCST-Watchdog AutoTrading Two-Instance Candidate Confirmation 0.575\n"
        << L"=========================================================\n\n"
        << L"CONTROLLED MEMORY COMPARISON\n"
        << L"Expected user action: exactly one active strategy changed ON -> OFF.\n"
        << L"Only boolean byte changes (1->0 and 0->1) at stable object addresses are listed.\n\n"
        << L"Processes in baseline: " << baseline.size() << L"\n"
        << L"Processes in current scan: " << current.size() << L"\n";

    out << L"\nBASELINE PROCESSES\n";
    for (const auto& process : baseline)
    {
        out << L"PID " << process.processId;
        const auto titles = WindowTitlesForProcess(process.processId);
        if (!titles.empty()) out << L"  Window: " << titles.front();
        out << L"\n";
    }
    out << L"\nCURRENT PROCESSES\n";
    for (const auto& process : current)
    {
        out << L"PID " << process.processId;
        const auto titles = WindowTitlesForProcess(process.processId);
        if (!titles.empty()) out << L"  Window: " << titles.front();
        out << L"\n";
    }

    if (baseline.size() != current.size())
    {
        out << L"\nPROCESS COUNT MISMATCH - comparison aborted.\n";
        out.close();
        diagnostic = L"MultiCharts process count changed between baseline and comparison. Capture a new baseline.";
        return false;
    }

    out << L"\nObjects compared at identical addresses: " << comparedObjects << L"\n"
        << L"Objects missing after change: " << disappearedObjects << L"\n"
        << L"New object addresses after change: " << newObjects << L"\n"
        << L"Boolean 1->0 changes: " << oneToZero.size() << L"\n"
        << L"Boolean 0->1 changes: " << zeroToOne.size() << L"\n\n";

    out << L"RANKED 1 -> 0 CHANGES\n"
        << L"---------------------\n";
    const SIZE_T reportLimit = (std::min)(oneToZero.size(), static_cast<SIZE_T>(500));
    for (SIZE_T i = 0; i < reportLimit; ++i)
    {
        const auto& change = oneToZero[i];
        out << L"Rank #" << (i + 1)
            << L"  PID=" << change.processId
            << L"  RVA=" << HexValue(change.rva)
            << L"  object=" << HexValue(change.objectAddress)
            << L"  offset=" << HexValue(change.offset)
            << L"  value=1->0"
            << L"  objects=" << change.baselineObjects << L"->" << change.currentObjects;
        if (change.baselineObjects == change.currentObjects)
            out << L"  [STABLE VTABLE POPULATION]";
        if (change.baselineObjects >= 15 && change.baselineObjects <= 25)
            out << L"  [STRATEGY-SIZED POPULATION]";
        out << L"\n";
    }
    if (oneToZero.empty())
        out << L"No stable boolean 1->0 field changes were detected.\n";

    out << L"\n0 -> 1 CHANGES (unexpected for the requested OFF toggle)\n"
        << L"-----------------------------------------------------\n";
    const SIZE_T reverseLimit = (std::min)(zeroToOne.size(), static_cast<SIZE_T>(100));
    for (SIZE_T i = 0; i < reverseLimit; ++i)
    {
        const auto& change = zeroToOne[i];
        out << L"PID=" << change.processId
            << L"  RVA=" << HexValue(change.rva)
            << L"  object=" << HexValue(change.objectAddress)
            << L"  offset=" << HexValue(change.offset)
            << L"  value=0->1"
            << L"  objects=" << change.baselineObjects << L"->" << change.currentObjects << L"\n";
    }

    out << L"\nINTERPRETATION\n"
        << L"--------------\n"
        << L"1. The strongest candidate is a 1->0 change at one stable object address.\n"
        << L"2. A vtable population near the known 19 strategies is especially interesting, but not mandatory.\n"
        << L"3. The same RVA/offset should reverse 0->1 when the same strategy is turned back ON.\n"
        << L"4. No candidate is activated in the production reader by this diagnostic version.\n";

    out.close();
    DeleteFileW(baselinePath.c_str());

    std::wostringstream message;
    message << L"Dynamic change comparison completed.\n\n1->0 changes: " << oneToZero.size()
            << L"\n0->1 changes: " << zeroToOne.size()
            << L"\n\nReport: " << reportPath;
    diagnostic = message.str();
    return true;
}


bool WriteAutoTradingCandidateMonitor(const std::wstring& reportPath, std::wstring& diagnostic)
{
    struct MonitorSpec
    {
        const wchar_t* name;
        ULONG_PTR rva;
        SIZE_T offset;
    };

    // The primary candidate preserves the old AutoTrading offset 0x142 and
    // reacted exactly 1->0 in the controlled two-instance test.
    constexpr MonitorSpec specs[] =
    {
        { L"PRIMARY", 0xA457B8, 0x142 },
        { L"A",       0xA455D0, 0x162 },
        { L"B",       0xA45618, 0x16A },
        { L"C",       0xA456D8, 0x152 },
        { L"D",       0xA45740, 0x15A },
        { L"E",       0xA45848, 0x14A },
        { L"F",       0xA458C8, 0x132 },
        { L"G",       0xA45940, 0x13A },
        { L"H",       0xA45A00, 0x12A },
        { L"I",       0xAC1CC0, 0x140 },
        { L"J",       0xAC1CE8, 0x148 },
        { L"K",       0xAC1E58, 0x138 },
    };

    struct Totals
    {
        SIZE_T objects = 0;
        SIZE_T on = 0;
        SIZE_T off = 0;
        SIZE_T invalid = 0;
        int processes = 0;
    };

    std::vector<Totals> totals(std::size(specs));
    const auto processIds = CollectMultiChartsProcessIds();
    if (processIds.empty())
    {
        diagnostic = L"No MultiCharts processes were detected.";
        return false;
    }

    std::wofstream out(reportPath, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        diagnostic = L"Could not create candidate monitor report: " + reportPath;
        return false;
    }

    out << L"MCST-Watchdog AutoTrading Live Candidate Monitor 0.5761\n"
        << L"=====================================================\n\n"
        << L"Passive live read only: no clicks, no input and no writes to MultiCharts memory.\n"
        << L"Run this report repeatedly while toggling AutoTrading ON/OFF.\n\n"
        << L"Detected MultiCharts processes: " << processIds.size() << L"\n\n";

    for (DWORD processId : processIds)
    {
        const auto modules = EnumerateModules(processId);
        const ModuleRange* charting = nullptr;
        for (const auto& module : modules)
        {
            if (LowerCopy(module.name) == L"charting.dll")
            {
                charting = &module;
                break;
            }
        }

        out << L"------------------------------------------------------------\n"
            << L"PID: " << processId << L"\n";
        const auto titles = WindowTitlesForProcess(processId);
        if (!titles.empty()) out << L"Window: " << titles.front() << L"\n";
        if (charting == nullptr)
        {
            out << L"Charting.dll: NOT FOUND\n\n";
            continue;
        }

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (process == nullptr)
        {
            out << L"OpenProcess failed.\n\n";
            continue;
        }

        int regionsRead = 0;
        int failures = 0;
        auto candidates = CollectChartingPointerCandidates(process, *charting, regionsRead, failures, 10000);
        std::map<ULONG_PTR, const CandidateVtable*> byRva;
        for (const auto& candidate : candidates)
            byRva[candidate.address - charting->base] = &candidate;

        for (SIZE_T i = 0; i < std::size(specs); ++i)
        {
            const auto& spec = specs[i];
            out << spec.name << L"  RVA=" << HexValue(spec.rva)
                << L"  offset=" << HexValue(spec.offset);

            const auto found = byRva.find(spec.rva);
            if (found == byRva.end())
            {
                out << L"  NOT FOUND\n";
                continue;
            }

            Totals local;
            local.processes = 1;
            for (ULONG_PTR object : found->second->objects)
            {
                unsigned char value = 0xFF;
                SIZE_T got = 0;
                ++local.objects;
                if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(object + spec.offset), &value, 1, &got) || got != 1)
                    ++local.invalid;
                else if (value == 1)
                    ++local.on;
                else if (value == 0)
                    ++local.off;
                else
                    ++local.invalid;
            }

            totals[i].objects += local.objects;
            totals[i].on += local.on;
            totals[i].off += local.off;
            totals[i].invalid += local.invalid;
            totals[i].processes += 1;

            out << L"  objects=" << local.objects
                << L"  ON=" << local.on
                << L"  OFF=" << local.off
                << L"  invalid=" << local.invalid;
            if (spec.rva == 0xA457B8 && spec.offset == 0x142)
                out << L"  [PRIMARY CANDIDATE]";
            out << L"\n";
        }
        out << L"\n";
        CloseHandle(process);
    }

    out << L"============================================================\n"
        << L"AGGREGATE ACROSS ALL DETECTED MC PROCESSES\n"
        << L"============================================================\n";
    for (SIZE_T i = 0; i < std::size(specs); ++i)
    {
        const auto& spec = specs[i];
        const auto& t = totals[i];
        out << spec.name << L"  RVA=" << HexValue(spec.rva)
            << L"  offset=" << HexValue(spec.offset)
            << L"  processes=" << t.processes
            << L"  objects=" << t.objects
            << L"  ON=" << t.on
            << L"  OFF=" << t.off
            << L"  invalid=" << t.invalid;
        if (i == 0) out << L"  [PRIMARY CANDIDATE]";
        out << L"\n";
    }

    out << L"\nINTERPRETATION\n"
        << L"--------------\n"
        << L"1. Toggle one known strategy and run this monitor again.\n"
        << L"2. The correct candidate must follow every ON/OFF change exactly.\n"
        << L"3. The PRIMARY candidate is RVA 0xA457B8 / offset 0x142.\n"
        << L"4. No candidate is activated in the production reader by this version.\n";
    out.close();

    std::wostringstream message;
    message << L"Candidate monitor completed for " << processIds.size()
            << L" MultiCharts process(es).\n\nReport: " << reportPath;
    diagnostic = message.str();
    return true;
}

namespace
{
    struct ResearchSpec
    {
        const wchar_t* name;
        ULONG_PTR rva;
        SIZE_T offset;
    };

    constexpr ResearchSpec kResearchSpecs[] =
    {
        { L"PRIMARY", 0xA457B8, 0x142 },
        { L"A",       0xA455D0, 0x162 },
        { L"B",       0xA45618, 0x16A },
        { L"C",       0xA456D8, 0x152 },
        { L"D",       0xA45740, 0x15A },
        { L"E",       0xA45848, 0x14A },
        { L"F",       0xA458C8, 0x132 },
        { L"G",       0xA45940, 0x13A },
        { L"H",       0xA45A00, 0x12A },
        { L"I",       0xAC1CC0, 0x140 },
        { L"J",       0xAC1CE8, 0x148 },
        { L"K",       0xAC1E58, 0x138 },
    };

    struct ResearchCount
    {
        SIZE_T objects = 0;
        SIZE_T on = 0;
        SIZE_T off = 0;
        SIZE_T invalid = 0;
        int processes = 0;
    };

    struct ResearchSnapshot
    {
        std::chrono::system_clock::time_point time{};
        std::set<DWORD> processIds;
        std::vector<ResearchCount> counts;
    };

    std::mutex g_researchMutex;
    bool g_researchActive = false;
    std::vector<ResearchSnapshot> g_researchSnapshots;

    std::wstring ResearchTime(std::chrono::system_clock::time_point value)
    {
        const std::time_t tt = std::chrono::system_clock::to_time_t(value);
        tm local{};
        localtime_s(&local, &tt);
        std::wostringstream out;
        out << std::put_time(&local, L"%Y-%m-%d %H:%M:%S");
        return out.str();
    }

    bool ReadResearchSnapshot(ResearchSnapshot& snapshot, std::wstring& diagnostic)
    {
        snapshot.time = std::chrono::system_clock::now();
        snapshot.processIds = CollectMultiChartsProcessIds();
        snapshot.counts.assign(std::size(kResearchSpecs), ResearchCount{});
        if (snapshot.processIds.empty())
        {
            diagnostic = L"No MultiCharts processes were detected.";
            return false;
        }

        for (DWORD processId : snapshot.processIds)
        {
            const auto modules = EnumerateModules(processId);
            const ModuleRange* charting = nullptr;
            for (const auto& module : modules)
            {
                if (LowerCopy(module.name) == L"charting.dll")
                {
                    charting = &module;
                    break;
                }
            }
            if (charting == nullptr)
                continue;

            HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
            if (process == nullptr)
                continue;

            int regionsRead = 0;
            int failures = 0;
            auto candidates = CollectChartingPointerCandidates(process, *charting, regionsRead, failures, 10000);
            std::map<ULONG_PTR, const CandidateVtable*> byRva;
            for (const auto& candidate : candidates)
                byRva[candidate.address - charting->base] = &candidate;

            for (SIZE_T i = 0; i < std::size(kResearchSpecs); ++i)
            {
                const auto found = byRva.find(kResearchSpecs[i].rva);
                if (found == byRva.end())
                    continue;

                auto& count = snapshot.counts[i];
                ++count.processes;
                for (ULONG_PTR object : found->second->objects)
                {
                    unsigned char value = 0xFF;
                    SIZE_T got = 0;
                    ++count.objects;
                    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(object + kResearchSpecs[i].offset), &value, 1, &got) || got != 1)
                        ++count.invalid;
                    else if (value == 1)
                        ++count.on;
                    else if (value == 0)
                        ++count.off;
                    else
                        ++count.invalid;
                }
            }
            CloseHandle(process);
        }

        diagnostic = L"Snapshot captured from " + std::to_wstring(snapshot.processIds.size()) + L" MultiCharts process(es).";
        return true;
    }

    void AppendResearchSnapshot(std::wofstream& out, const ResearchSnapshot& snapshot, SIZE_T number)
    {
        out << L"\n============================================================\n"
            << L"SNAPSHOT " << number << L"\n"
            << L"Time: " << ResearchTime(snapshot.time) << L"\n"
            << L"Processes: " << snapshot.processIds.size() << L"\n";
        for (DWORD pid : snapshot.processIds)
        {
            out << L"  PID " << pid;
            const auto titles = WindowTitlesForProcess(pid);
            if (!titles.empty()) out << L"  Window: " << titles.front();
            out << L"\n";
        }
        out << L"\n";
        for (SIZE_T i = 0; i < std::size(kResearchSpecs); ++i)
        {
            const auto& spec = kResearchSpecs[i];
            const auto& c = snapshot.counts[i];
            out << spec.name << L"  RVA=" << HexValue(spec.rva)
                << L"  offset=" << HexValue(spec.offset)
                << L"  processes=" << c.processes
                << L"  objects=" << c.objects
                << L"  ON=" << c.on
                << L"  OFF=" << c.off
                << L"  invalid=" << c.invalid << L"\n";
        }
    }
}

bool StartAutoTradingResearchSession(const std::wstring& historyPath, std::wstring& diagnostic)
{
    std::lock_guard<std::mutex> lock(g_researchMutex);
    ResearchSnapshot first;
    if (!ReadResearchSnapshot(first, diagnostic))
        return false;

    g_researchSnapshots.clear();
    g_researchSnapshots.push_back(first);
    g_researchActive = true;

    std::wofstream out(historyPath, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        g_researchActive = false;
        diagnostic = L"Could not create research history file: " + historyPath;
        return false;
    }
    out << L"MCST-Watchdog AutoTrading Research Session 0.577\n"
        << L"=================================================\n\n"
        << L"Passive research only: no clicks, no input and no writes to MultiCharts memory.\n"
        << L"After each single AutoTrading ON/OFF change, press Capture Snapshot.\n"
        << L"Finish Research writes an automatic ranking to this same file.\n";
    AppendResearchSnapshot(out, first, 1);
    out.close();

    diagnostic = L"AutoTrading research session started.\n\nBaseline snapshot 1 captured from "
        + std::to_wstring(first.processIds.size()) + L" MultiCharts process(es).\n\n"
        + L"Toggle exactly one strategy, then press Capture AT Snapshot.";
    return true;
}

bool CaptureAutoTradingResearchSnapshot(const std::wstring& historyPath, std::wstring& diagnostic)
{
    std::lock_guard<std::mutex> lock(g_researchMutex);
    if (!g_researchActive || g_researchSnapshots.empty())
    {
        diagnostic = L"No active AutoTrading research session. Press Start AT Research first.";
        return false;
    }

    ResearchSnapshot snapshot;
    if (!ReadResearchSnapshot(snapshot, diagnostic))
        return false;
    if (snapshot.processIds != g_researchSnapshots.front().processIds)
    {
        diagnostic = L"The MultiCharts process set changed. Finish or restart the research session with all intended instances running.";
        return false;
    }

    std::wofstream out(historyPath, std::ios::out | std::ios::app);
    if (!out.is_open())
    {
        diagnostic = L"Could not append to research history file: " + historyPath;
        return false;
    }
    const SIZE_T number = g_researchSnapshots.size() + 1;
    AppendResearchSnapshot(out, snapshot, number);

    const auto& previous = g_researchSnapshots.back();
    out << L"\nCHANGES FROM SNAPSHOT " << (number - 1) << L" -> " << number << L"\n";
    for (SIZE_T i = 0; i < std::size(kResearchSpecs); ++i)
    {
        const auto& a = previous.counts[i];
        const auto& b = snapshot.counts[i];
        const long long dOn = static_cast<long long>(b.on) - static_cast<long long>(a.on);
        const long long dOff = static_cast<long long>(b.off) - static_cast<long long>(a.off);
        const long long dObjects = static_cast<long long>(b.objects) - static_cast<long long>(a.objects);
        const bool exactToggle = dObjects == 0 && ((dOn == -1 && dOff == 1) || (dOn == 1 && dOff == -1));
        out << kResearchSpecs[i].name << L"  dObjects=" << dObjects
            << L"  dON=" << dOn << L"  dOFF=" << dOff;
        if (exactToggle) out << L"  [EXACT TOGGLE RESPONSE]";
        out << L"\n";
    }
    out.close();
    g_researchSnapshots.push_back(snapshot);

    diagnostic = L"Snapshot " + std::to_wstring(number) + L" captured and appended to:\n" + historyPath
        + L"\n\nMake another single ON/OFF change and capture again, or press Finish AT Research.";
    return true;
}

bool FinishAutoTradingResearchSession(const std::wstring& historyPath, std::wstring& diagnostic)
{
    std::lock_guard<std::mutex> lock(g_researchMutex);
    if (!g_researchActive || g_researchSnapshots.empty())
    {
        diagnostic = L"No active AutoTrading research session.";
        return false;
    }

    std::wofstream out(historyPath, std::ios::out | std::ios::app);
    if (!out.is_open())
    {
        diagnostic = L"Could not append the research summary: " + historyPath;
        return false;
    }

    struct Score { SIZE_T index = 0; int exact = 0; int stable = 0; int wrong = 0; };
    std::vector<Score> scores(std::size(kResearchSpecs));
    const int transitions = static_cast<int>(g_researchSnapshots.size() > 0 ? g_researchSnapshots.size() - 1 : 0);
    for (SIZE_T i = 0; i < scores.size(); ++i)
    {
        scores[i].index = i;
        for (SIZE_T s = 1; s < g_researchSnapshots.size(); ++s)
        {
            const auto& a = g_researchSnapshots[s - 1].counts[i];
            const auto& b = g_researchSnapshots[s].counts[i];
            const long long dOn = static_cast<long long>(b.on) - static_cast<long long>(a.on);
            const long long dOff = static_cast<long long>(b.off) - static_cast<long long>(a.off);
            const long long dObjects = static_cast<long long>(b.objects) - static_cast<long long>(a.objects);
            if (dObjects == 0 && ((dOn == -1 && dOff == 1) || (dOn == 1 && dOff == -1)))
                ++scores[i].exact;
            else if (dObjects == 0 && dOn == 0 && dOff == 0)
                ++scores[i].stable;
            else
                ++scores[i].wrong;
        }
    }
    std::sort(scores.begin(), scores.end(), [](const Score& left, const Score& right) {
        if (left.exact != right.exact) return left.exact > right.exact;
        if (left.wrong != right.wrong) return left.wrong < right.wrong;
        return left.index < right.index;
    });

    out << L"\n============================================================\n"
        << L"AUTOMATIC SESSION SUMMARY\n"
        << L"============================================================\n"
        << L"Snapshots: " << g_researchSnapshots.size() << L"\n"
        << L"Transitions analyzed: " << transitions << L"\n\n";
    for (SIZE_T rank = 0; rank < scores.size(); ++rank)
    {
        const auto& score = scores[rank];
        const auto& spec = kResearchSpecs[score.index];
        out << L"Rank #" << (rank + 1) << L"  " << spec.name
            << L"  RVA=" << HexValue(spec.rva)
            << L"  offset=" << HexValue(spec.offset)
            << L"  exact=" << score.exact << L"/" << transitions
            << L"  unchanged=" << score.stable
            << L"  other=" << score.wrong;
        if (transitions >= 2 && score.exact == transitions && score.wrong == 0)
            out << L"  [PERFECT SESSION MATCH]";
        out << L"\n";
    }
    if (!scores.empty())
    {
        const auto& best = scores.front();
        const auto& spec = kResearchSpecs[best.index];
        out << L"\nRECOMMENDED CURRENT CANDIDATE\n"
            << L"Name: " << spec.name << L"\n"
            << L"RVA: " << HexValue(spec.rva) << L"\n"
            << L"Offset: " << HexValue(spec.offset) << L"\n"
            << L"Exact responses: " << best.exact << L" / " << transitions << L"\n";
        if (transitions < 2)
            out << L"Confidence: LOW - perform at least two transitions (OFF and back ON).\n";
        else if (best.exact == transitions && best.wrong == 0)
            out << L"Confidence: HIGH for this session; final developer review is still required.\n";
        else
            out << L"Confidence: INCONCLUSIVE - additional controlled transitions are needed.\n";
    }
    out.close();

    g_researchActive = false;
    const SIZE_T snapshots = g_researchSnapshots.size();
    g_researchSnapshots.clear();
    diagnostic = L"AutoTrading research session finished.\n\nSnapshots analyzed: " + std::to_wstring(snapshots)
        + L"\nFull history and automatic ranking:\n" + historyPath;
    return true;
}
