#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <ole2.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <oleacc.h>
#pragma comment(lib, "oleacc.lib")

#include "MultiChartsHealthMonitor.h"
#include "CoveredQueueProbe.h"
#include "../MCST.Shared/VisibleWarningPolicy.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cwctype>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct ProcessHistory
    {
        unsigned long long processTime = 0;
        Clock::time_point cpuSampleTime{};
        Clock::time_point lastSeen{};
        int unresponsiveSamples = 0;
        int queueGrowthSamples = 0;
        int highCpuSamples = 0;
        unsigned long queueCount = 0;
        unsigned long queueAgeSeconds = 0;
        bool queueSeen = false;
        int redWarningSamples = 0;
        int clearWarningSamples = 0;
        bool redWarningConfirmed = false;
    };

    std::mutex g_historyMutex;
    std::map<DWORD, ProcessHistory> g_history;
    DWORD g_recentlyDisappearedProcess = 0;
    Clock::time_point g_recentDisappearanceTime{};

    template <typename T>
    void ReleaseCom(T*& value)
    {
        if (value)
        {
            value->Release();
            value = nullptr;
        }
    }

    struct ComApartmentScope
    {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool usable = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
        bool ownsInitialization = result == S_OK || result == S_FALSE;

        ~ComApartmentScope()
        {
            if (ownsInitialization)
                CoUninitialize();
        }
    };

    std::wstring LowerCopy(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    std::wstring WindowText(HWND window)
    {
        if (!window)
            return L"";

        DWORD_PTR lengthResult = 0;
        if (!SendMessageTimeoutW(window, WM_GETTEXTLENGTH, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &lengthResult))
        {
            return L"";
        }

        const std::size_t length = (std::min)(
            static_cast<std::size_t>(lengthResult), static_cast<std::size_t>(8191));
        if (length == 0)
            return L"";

        std::vector<wchar_t> buffer(length + 1u, L'\0');
        DWORD_PTR copiedResult = 0;
        if (!SendMessageTimeoutW(window, WM_GETTEXT,
                static_cast<WPARAM>(buffer.size()), reinterpret_cast<LPARAM>(buffer.data()),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copiedResult))
        {
            return L"";
        }
        const std::size_t copied = (std::min)(
            static_cast<std::size_t>(copiedResult), length);
        return copied > 0 ? std::wstring(buffer.data(), copied) : L"";
    }

    std::wstring WindowClass(HWND window)
    {
        wchar_t buffer[512]{};
        const int copied = GetClassNameW(window, buffer, static_cast<int>(std::size(buffer)));
        return copied > 0 ? std::wstring(buffer, static_cast<std::size_t>(copied)) : L"";
    }

    bool IsMainExecutableName(const std::wstring& executable)
    {
        const std::wstring lower = LowerCopy(executable);
        return lower == L"multicharts.exe" || lower == L"multicharts64.exe";
    }

    std::set<DWORD> CollectProcessIds()
    {
        std::set<DWORD> result;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return result;

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (IsMainExecutableName(entry.szExeFile))
                    result.insert(entry.th32ProcessID);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    struct MainWindowSearch
    {
        DWORD processId = 0;
        HWND window = nullptr;
    };

    BOOL CALLBACK FindMainWindowProc(HWND window, LPARAM parameter)
    {
        auto* search = reinterpret_cast<MainWindowSearch*>(parameter);
        if (!search || search->window)
            return FALSE;

        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != search->processId)
            return TRUE;

        const std::wstring lowerTitle = LowerCopy(WindowText(window));
        const std::wstring lowerClass = LowerCopy(WindowClass(window));
        if (lowerTitle.find(L"quote manager") != std::wstring::npos ||
            lowerTitle.find(L"order and position tracker") != std::wstring::npos ||
            lowerTitle.find(L"portfolio trader") != std::wstring::npos)
        {
            return TRUE;
        }

        if (lowerTitle.find(L"multicharts") != std::wstring::npos ||
            lowerClass.find(L"atl_mcmdimainframe") != std::wstring::npos)
        {
            search->window = window;
            return FALSE;
        }
        return TRUE;
    }

    HWND FindMainWindow(DWORD processId)
    {
        MainWindowSearch search;
        search.processId = processId;
        EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&search));
        return search.window;
    }

    bool ParseQueueText(const std::wstring& text, unsigned long& queueCount, unsigned long& queueAgeSeconds)
    {
        static const std::wregex pattern(LR"((\d+)\s*q\s*/\s*(\d+)\s*s)", std::regex_constants::icase);
        std::wsmatch match;
        if (!std::regex_search(text, match, pattern) || match.size() < 3)
            return false;

        try
        {
            const unsigned long long count = std::stoull(match[1].str());
            const unsigned long long age = std::stoull(match[2].str());
            queueCount = static_cast<unsigned long>((std::min)(count, static_cast<unsigned long long>(ULONG_MAX)));
            queueAgeSeconds = static_cast<unsigned long>((std::min)(age, static_cast<unsigned long long>(ULONG_MAX)));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void AppendBstr(std::wstring& text, BSTR value)
    {
        if (value && value[0] != L'\0' && text.size() < 8192)
        {
            if (!text.empty())
                text += L' ';
            text.append(value, SysStringLen(value));
        }
        SysFreeString(value);
    }

    std::wstring ReadAutomationText(IUIAutomation* automation, HWND window)
    {
        if (!automation || !window)
            return L"";

        IUIAutomationElement* root = nullptr;
        IUIAutomationCondition* condition = nullptr;
        IUIAutomationElementArray* elements = nullptr;
        std::wstring text;

        if (SUCCEEDED(automation->ElementFromHandle(window, &root)) && root)
        {
            BSTR value = nullptr;
            if (SUCCEEDED(root->get_CurrentName(&value)))
                AppendBstr(text, value);

            if (SUCCEEDED(automation->CreateTrueCondition(&condition)) && condition &&
                SUCCEEDED(root->FindAll(TreeScope_Subtree, condition, &elements)) && elements)
            {
                int count = 0;
                if (SUCCEEDED(elements->get_Length(&count)))
                {
                    // Status bars have only a handful of descendants. Keep the
                    // accessibility read bounded so monitoring remains light.
                    count = (std::min)(count, 96);
                    for (int index = 0; index < count && text.size() < 8192; ++index)
                    {
                        IUIAutomationElement* element = nullptr;
                        if (FAILED(elements->GetElement(index, &element)) || !element)
                            continue;

                        value = nullptr;
                        if (SUCCEEDED(element->get_CurrentName(&value)))
                            AppendBstr(text, value);

                        IUIAutomationValuePattern* valuePattern = nullptr;
                        if (SUCCEEDED(element->GetCurrentPatternAs(
                                UIA_ValuePatternId, IID_PPV_ARGS(&valuePattern))) && valuePattern)
                        {
                            value = nullptr;
                            if (SUCCEEDED(valuePattern->get_CurrentValue(&value)))
                                AppendBstr(text, value);
                            ReleaseCom(valuePattern);
                        }
                        ReleaseCom(element);
                    }
                }
            }
        }

        ReleaseCom(elements);
        ReleaseCom(condition);
        ReleaseCom(root);
        return text;
    }

    struct QueueSearch
    {
        IUIAutomation* automation = nullptr;
        unsigned long queueCount = 0;
        unsigned long queueAgeSeconds = 0;
        bool found = false;
        std::wstring diagnostic;
    };

    std::wstring ReadLegacyStatusText(HWND window)
    {
        IAccessible* accessible = nullptr;
        std::wstring text;
        if (FAILED(AccessibleObjectFromWindow(window, OBJID_CLIENT,
                IID_IAccessible, reinterpret_cast<void**>(&accessible))) || !accessible)
            return text;
        long count = 0;
        accessible->get_accChildCount(&count);
        count = (std::min)((std::max)(count, 0L), 64L);
        for (long index = 0; index <= count; ++index)
        {
            VARIANT child{};
            child.vt = VT_I4;
            child.lVal = index == 0 ? CHILDID_SELF : index;
            BSTR value = nullptr;
            if (SUCCEEDED(accessible->get_accName(child, &value)))
                AppendBstr(text, value);
            else
                SysFreeString(value);
            value = nullptr;
            if (SUCCEEDED(accessible->get_accValue(child, &value)))
                AppendBstr(text, value);
            else
                SysFreeString(value);
        }
        accessible->Release();
        return text;
    }

    BOOL CALLBACK FindQueueTextProc(HWND window, LPARAM parameter)
    {
        auto* search = reinterpret_cast<QueueSearch*>(parameter);
        if (!search || search->found)
            return FALSE;

        const std::wstring directText = WindowText(window);
        if (ParseQueueText(directText, search->queueCount, search->queueAgeSeconds))
        {
            search->found = true;
            return FALSE;
        }

        const std::wstring lowerClass = LowerCopy(WindowClass(window));
        RECT childRect{}, rootRect{};
        const HWND root = GetAncestor(window, GA_ROOT);
        const bool bottomField = IsWindowVisible(window) &&
            GetWindowRect(window, &childRect) && GetWindowRect(root, &rootRect) &&
            childRect.bottom >= rootRect.bottom - 90 &&
            childRect.top >= rootRect.bottom - 150;
        if (lowerClass.find(L"statusbar") != std::wstring::npos ||
            lowerClass.find(L"status") != std::wstring::npos || bottomField)
        {
            const std::wstring legacyText = ReadLegacyStatusText(window);
            if (search->diagnostic.size() < 8192)
                search->diagnostic += L" [" + WindowClass(window) + L"; window=" +
                    directText + L"; MSAA=" + legacyText + L"]";
            if (ParseQueueText(legacyText, search->queueCount, search->queueAgeSeconds))
            {
                search->found = true;
                search->diagnostic += L" [queue read via MSAA]";
                return FALSE;
            }
            const std::wstring automationText = ReadAutomationText(search->automation, window);
            if (search->diagnostic.size() < 8192)
                search->diagnostic += L" [UIA=" + automationText + L"]";
            if (ParseQueueText(automationText, search->queueCount, search->queueAgeSeconds))
            {
                search->found = true;
                return FALSE;
            }
        }
        return TRUE;
    }

    #include "QueueStatusReader.h"
    #include "VisibleQueueWarning.h"

    bool ReadQueueIndicator(HWND mainWindow, unsigned long& queueCount, unsigned long& queueAgeSeconds,
        std::wstring& diagnostic)
    {
        return ReadStatusBarQueue(mainWindow, queueCount, queueAgeSeconds, diagnostic);
    }

    unsigned long long FileTimeValue(const FILETIME& value)
    {
        ULARGE_INTEGER converted{};
        converted.LowPart = value.dwLowDateTime;
        converted.HighPart = value.dwHighDateTime;
        return converted.QuadPart;
    }

    bool QueryResponsive(HWND window)
    {
        if (!window)
            return false;
        DWORD_PTR ignored = 0;
        return SendMessageTimeoutW(window, WM_NULL, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored) != 0;
    }

    mcst::HealthState Worse(mcst::HealthState left, mcst::HealthState right)
    {
        const auto rank = [](mcst::HealthState state) {
            switch (state)
            {
            case mcst::HealthState::Critical: return 3;
            case mcst::HealthState::Attention: return 2;
            case mcst::HealthState::Unknown: return 1;
            case mcst::HealthState::Healthy: return 0;
            }
            return 1;
        };
        return rank(right) > rank(left) ? right : left;
    }

    std::wstring FormatCorePercent(double value)
    {
        std::wostringstream out;
        out.setf(std::ios::fixed);
        out.precision(0);
        out << value << L"%";
        return out.str();
    }
}

mcst::MultiChartsHealthSnapshot ReadMultiChartsHealth()
{
    mcst::MultiChartsHealthSnapshot snapshot;
    const auto now = Clock::now();
    const std::set<DWORD> processIds = CollectProcessIds();
    std::lock_guard<std::mutex> lock(g_historyMutex);
    mcstprobe::ResetBudget(processIds);

    std::set<DWORD> disappeared;
    for (const auto& item : g_history)
    {
        if (processIds.find(item.first) == processIds.end() && item.second.lastSeen.time_since_epoch().count() != 0)
            disappeared.insert(item.first);
    }
    for (DWORD processId : disappeared)
    {
        snapshot.processSetChanged = true;
        g_recentlyDisappearedProcess = processId;
        g_recentDisappearanceTime = now;
        g_history.erase(processId);
    }

    snapshot.state = processIds.empty() ? mcst::HealthState::Critical : mcst::HealthState::Healthy;
    snapshot.processCount = processIds.size();

    for (DWORD processId : processIds)
    {
        mcst::MultiChartsProcessStatus processStatus;
        processStatus.processId = processId;
        ProcessHistory& history = g_history[processId];
        history.lastSeen = now;

        const HWND mainWindow = FindMainWindow(processId);
        processStatus.mainWindowFound = mainWindow != nullptr;
        processStatus.responsive = QueryResponsive(mainWindow);
        history.unresponsiveSamples = processStatus.responsive ? 0 : history.unresponsiveSamples + 1;
        processStatus.unresponsiveSamples = history.unresponsiveSamples;

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (process)
        {
            FILETIME creation{}, exit{}, kernel{}, user{};
            if (GetProcessTimes(process, &creation, &exit, &kernel, &user))
            {
                const unsigned long long currentProcessTime = FileTimeValue(kernel) + FileTimeValue(user);
                if (history.processTime != 0 && history.cpuSampleTime.time_since_epoch().count() != 0)
                {
                    const double elapsed100ns =
                        std::chrono::duration<double>(now - history.cpuSampleTime).count() * 10000000.0;
                    if (elapsed100ns > 0.0 && currentProcessTime >= history.processTime)
                    {
                        processStatus.cpuAvailable = true;
                        processStatus.cpuCorePercent =
                            100.0 * static_cast<double>(currentProcessTime - history.processTime) / elapsed100ns;
                    }
                }
                history.processTime = currentProcessTime;
                history.cpuSampleTime = now;
            }

            if (processStatus.cpuAvailable && processStatus.cpuCorePercent >= 90.0)
                ++history.highCpuSamples;
            else
                history.highCpuSamples = 0;
            processStatus.highCpuSamples = history.highCpuSamples;

            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            if (GetProcessMemoryInfo(process,
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
            {
                processStatus.resourcesAvailable = true;
                processStatus.workingSetBytes = memory.WorkingSetSize;
                processStatus.privateMemoryBytes = memory.PrivateUsage;
            }
            GetProcessHandleCount(process, &processStatus.handleCount);
            processStatus.gdiObjects = GetGuiResources(process, GR_GDIOBJECTS);
            processStatus.userObjects = GetGuiResources(process, GR_USEROBJECTS);
            CloseHandle(process);
        }

        if (processStatus.responsive && mainWindow)
        {
            ReadVisibleQueueWarning(mainWindow, processStatus.visibleQueueWarningChecked,
                processStatus.visibleQueueWarningRed, processStatus.visibleQueueWarningRendered,
                processStatus.queueReadDiagnostic);
        }

        mcst::UpdateVisibleWarning(processStatus.visibleQueueWarningChecked,
            processStatus.visibleQueueWarningRed, history.redWarningSamples,
            history.clearWarningSamples, history.redWarningConfirmed);
        // One completed covered probe already contains two positive renderings.
        if (processStatus.visibleQueueWarningRendered)
        {
            history.redWarningConfirmed = true;
            history.redWarningSamples = 2;
            history.clearWarningSamples = 0;
        }
        processStatus.visibleQueueWarningConfirmed = history.redWarningConfirmed;

        if (processStatus.queueIndicatorFound)
        {
            if (history.queueSeen && processStatus.queueCount > history.queueCount &&
                processStatus.queueAgeSeconds > history.queueAgeSeconds)
            {
                ++history.queueGrowthSamples;
            }
            else
            {
                history.queueGrowthSamples = 0;
            }
            history.queueSeen = true;
            history.queueCount = processStatus.queueCount;
            history.queueAgeSeconds = processStatus.queueAgeSeconds;
        }
        else
        {
            history.queueSeen = false;
            history.queueCount = 0;
            history.queueAgeSeconds = 0;
            history.queueGrowthSamples = 0;
        }
        processStatus.queueGrowthSamples = history.queueGrowthSamples;

        processStatus.state = mcst::HealthState::Healthy;
        if (processStatus.visibleQueueWarningConfirmed)
            processStatus.state = mcst::HealthState::Attention;
        if (!processStatus.mainWindowFound && history.unresponsiveSamples >= 3)
            processStatus.state = mcst::HealthState::Attention;
        if (processStatus.mainWindowFound && !processStatus.responsive &&
            history.unresponsiveSamples >= 3)
            processStatus.state = mcst::HealthState::Critical;
        if (processStatus.queueIndicatorFound)
        {
            if (processStatus.queueAgeSeconds >= 10 || processStatus.queueCount >= 1000 ||
                processStatus.queueGrowthSamples >= 3)
            {
                processStatus.state = mcst::HealthState::Critical;
            }
            else if (processStatus.queueAgeSeconds >= 3)
            {
                processStatus.state = Worse(processStatus.state, mcst::HealthState::Attention);
            }
        }
        if (processStatus.gdiObjects >= 8000 || processStatus.userObjects >= 8000)
            processStatus.state = mcst::HealthState::Critical;
        else if (processStatus.gdiObjects >= 6000 || processStatus.userObjects >= 6000 ||
                 processStatus.handleCount >= 20000)
            processStatus.state = Worse(processStatus.state, mcst::HealthState::Attention);
        if (processStatus.highCpuSamples >= 3)
            processStatus.state = Worse(processStatus.state, mcst::HealthState::Attention);

        snapshot.totalCpuCorePercent += processStatus.cpuAvailable ? processStatus.cpuCorePercent : 0.0;
        if (processStatus.visibleQueueWarningConfirmed) ++snapshot.visibleQueueWarningCount;
        if (!processStatus.visibleQueueWarningChecked) ++snapshot.visibleQueueUncheckedCount;
        snapshot.totalPrivateMemoryBytes += processStatus.privateMemoryBytes;
        snapshot.maximumQueueCount = (std::max)(snapshot.maximumQueueCount, processStatus.queueCount);
        snapshot.maximumQueueAgeSeconds = (std::max)(snapshot.maximumQueueAgeSeconds, processStatus.queueAgeSeconds);
        if (!processStatus.responsive)
            ++snapshot.unresponsiveProcessCount;
        if (processStatus.highCpuSamples >= 3)
            ++snapshot.sustainedHighCpuProcessCount;
        snapshot.state = Worse(snapshot.state, processStatus.state);
        snapshot.processes.push_back(processStatus);
    }

    if (g_recentDisappearanceTime.time_since_epoch().count() != 0 &&
        now - g_recentDisappearanceTime <= std::chrono::seconds(60) && !processIds.empty())
    {
        snapshot.recentlyDisappearedProcessId = g_recentlyDisappearedProcess;
        snapshot.state = Worse(snapshot.state, mcst::HealthState::Attention);
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        snapshot.availableSystemMemoryBytes = memory.ullAvailPhys;
        if (memory.ullAvailPhys < 512ULL * 1024ULL * 1024ULL)
            snapshot.state = mcst::HealthState::Critical;
        else if (memory.ullAvailPhys < 2ULL * 1024ULL * 1024ULL * 1024ULL)
            snapshot.state = Worse(snapshot.state, mcst::HealthState::Attention);
    }

    snapshot.value = std::to_wstring(snapshot.processCount) +
        (snapshot.processCount == 1 ? L" instance" : L" instances");

    if (processIds.empty())
    {
        snapshot.detail = L"No MultiCharts process found";
    }
    else if (snapshot.visibleQueueWarningCount > 0)
    {
        snapshot.detail = L"Red MultiCharts queue warning detected - queue values unavailable";
        if (snapshot.visibleQueueUncheckedCount > 0)
            snapshot.detail += L"; " + std::to_wstring(snapshot.visibleQueueUncheckedCount) +
                L" instance(s) not visually checked; last confirmed warnings retained";
    }
    else if (snapshot.maximumQueueAgeSeconds >= 3)
    {
        snapshot.detail = std::to_wstring(snapshot.maximumQueueCount) + L" q / " +
            std::to_wstring(snapshot.maximumQueueAgeSeconds) + L" s - " +
            (snapshot.unresponsiveProcessCount == 0 ? L"responsive" : L"UI response problem");
    }
    else if (snapshot.unresponsiveProcessCount > 0)
    {
        snapshot.detail = std::to_wstring(snapshot.unresponsiveProcessCount) + L" not responding yet";
    }
    else if (snapshot.sustainedHighCpuProcessCount > 0)
    {
        snapshot.detail = std::to_wstring(snapshot.sustainedHighCpuProcessCount) +
            L" instance(s) above 90% of one logical core for 3 samples";
    }
    else if (snapshot.recentlyDisappearedProcessId != 0)
    {
        snapshot.detail = L"PID " + std::to_wstring(snapshot.recentlyDisappearedProcessId) +
            L" terminated; remaining instances responsive";
    }
    else
    {
        const bool cpuSampleAvailable = std::any_of(
            snapshot.processes.begin(), snapshot.processes.end(),
            [](const mcst::MultiChartsProcessStatus& process) { return process.cpuAvailable; });
        snapshot.detail = cpuSampleAvailable
            ? L"Responsive - queue warning check (covered rendering experimental); " +
                std::to_wstring(snapshot.visibleQueueUncheckedCount) + L" instance(s) not checked - MC CPU " +
                FormatCorePercent(snapshot.totalCpuCorePercent) + L" of one logical core"
            : L"Responsive - queue warning check (covered rendering experimental); " +
                std::to_wstring(snapshot.visibleQueueUncheckedCount) + L" instance(s) not checked - CPU sampling starts on next refresh";
    }
    return snapshot;
}
