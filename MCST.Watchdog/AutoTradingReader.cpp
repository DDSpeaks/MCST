#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include "AutoTradingReader.h"

#include <algorithm>
#include <chrono>
#include <cstring>
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
        int objects = 0;
        int active = 0;
        int readFailures = 0;
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
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            if (!IsMultiChartsMainWindow(window))
                return TRUE;
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId != 0)
                reinterpret_cast<std::set<DWORD>*>(parameter)->insert(processId);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&processIds));
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

        // Confirmed by the first-generation Watchdog V84/V85 for Bridge-era MC16.
        constexpr ULONG_PTR kPrimaryStrategyVtableRva = 0xA42048;
        constexpr SIZE_T kAutoTradingOffset = 0x142;
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
        const ULONG_PTR wantedVtable = charting->base + kPrimaryStrategyVtableRva;

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (process == nullptr)
        {
            ++result.readFailures;
            return result;
        }

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
                            reinterpret_cast<LPCVOID>(objectBase + kAutoTradingOffset),
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
        for (DWORD processId : processIds)
        {
            const StrategyObjectCount count = CountStrategyObjects(processId);
            chartingFound = chartingFound || count.chartingFound;
            result.strategyObjectsFound += count.objects;
            result.activeStrategies += count.active;
            result.readFailures += count.readFailures;
        }

        result.succeeded = chartingFound && result.strategyObjectsFound > 0;
        if (result.succeeded)
            result.lastSuccessfulRead = result.lastAttempt;
        std::wostringstream diagnostic;
        diagnostic << L"Processes " << result.processesScanned
                   << L", strategy objects " << result.strategyObjectsFound
                   << L", active " << result.activeStrategies
                   << L", read failures " << result.readFailures;
        if (processIds.empty())
            diagnostic << L". No MultiCharts main windows were found.";
        else if (!chartingFound)
            diagnostic << L". Charting.dll was not found in the detected processes.";
        else if (result.strategyObjectsFound == 0)
            diagnostic << L". No RTTI-verified CStrategyObject instances were found.";
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

    bool ReadRemotePeTimestamp(HANDLE process, ULONG_PTR moduleBase, DWORD& timestamp)
    {
        IMAGE_DOS_HEADER dos{};
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(moduleBase), &dos, sizeof(dos), &got) ||
            got != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        DWORD signature = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(moduleBase + dos.e_lfanew), &signature, sizeof(signature), &got) ||
            got != sizeof(signature) || signature != IMAGE_NT_SIGNATURE)
            return false;
        IMAGE_FILE_HEADER header{};
        const ULONG_PTR headerAddress = moduleBase + dos.e_lfanew + sizeof(DWORD);
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(headerAddress), &header, sizeof(header), &got) || got != sizeof(header))
            return false;
        timestamp = header.TimeDateStamp;
        return true;
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

    std::vector<CandidateVtable> CollectChartingPointerCandidates(HANDLE process, const ModuleRange& charting, int& regionsRead, int& readFailures)
    {
        constexpr SIZE_T kChunk = 1024u * 1024u;
        constexpr SIZE_T kMaxObjectsPerCandidate = 64;
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
                        if (accumulator.objects.size() < kMaxObjectsPerCandidate)
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
