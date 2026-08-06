#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

#include "AppConfig.h"
#include "AutoTradingReader.h"
#include "StatusReport.h"
#include "Email.h"
#include "AlertService.h"
#include "ScheduleService.h"
#include "BrokerMonitor.h"
#include "LogAlertEngine.h"
#include "../MCST.Shared/WatchdogSystemStatus.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace
{
    constexpr wchar_t kWindowClass[] = L"MCSTWatchdogDashboardWindow";
    constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\MCST-Watchdog-SingleInstance";
    constexpr UINT WM_APP_REFRESH_COMPLETE = WM_APP + 1;
    constexpr UINT WM_APP_EMAIL_COMPLETE = WM_APP + 2;
    constexpr UINT_PTR kRefreshTimer = 1;
    constexpr UINT_PTR kClockTimer = 2;
    constexpr UINT_PTR kAnimationTimer = 3;
    constexpr int kButtonRefresh = 1001;
    constexpr int kButtonReport = 1002;
    constexpr int kButtonSettings = 1003;
    constexpr int kButtonOpenFolder = 1004;
    constexpr int kButtonAutoTradingDiagnostics = 1005;
    constexpr int kButtonAutoTradingCapture = 1006;
    constexpr int kButtonAutoTradingFinish = 1007;
    constexpr int kButtonReloadSettings = 1008;
    constexpr int kButtonTestEmail = 1009;
    constexpr int kButtonAutoMenu = 1010;
    constexpr int kButtonStatusMenu = 1011;
    constexpr int kButtonEmailMenu = 1012;
    constexpr int kButtonHeartbeatMenu = 1013;
    constexpr int kMenuConfigure = 3001;
    constexpr int kMenuAction1 = 3002;
    constexpr int kMenuAction2 = 3003;
    constexpr int kSimpleSave = 3101;
    constexpr int kSimpleCancel = 3102;
    constexpr int kSimpleEnabled = 3103;
    constexpr int kSimpleField1 = 3110;
    constexpr int kSimpleField2 = 3111;
    constexpr int kSimpleField3 = 3112;
    constexpr int kSimpleField4 = 3113;
    constexpr int kSimpleField5 = 3114;
    constexpr int kSimpleField6 = 3115;
    constexpr int kSimpleField7 = 3116;
    constexpr int kSimpleCheck1 = 3120;
    constexpr int kSimpleCheck2 = 3121;
    constexpr int kStatusSettingsEnabled = 2001;
    constexpr int kStatusSettingsRecipient = 2002;
    constexpr int kStatusSettingsInterval = 2003;
    constexpr int kStatusSettingsStartup = 2004;
    constexpr int kStatusSettingsSave = 2005;
    constexpr int kStatusSettingsCancel = 2006;
    constexpr int kStatusSettingsWeekdays = 2007;
    constexpr int kStatusSettingsStart = 2008;
    constexpr int kStatusSettingsEnd = 2009;

    struct RefreshResult
    {
        mcst::WatchdogSystemStatus status;
        TrackerStatusSnapshot snapshot;
        std::wstring diagnostic;
    };

    struct AppState
    {
        AppConfig config;
        mcst::WatchdogSystemStatus status;
        TrackerStatusSnapshot snapshot;
        std::mutex mutex;
        bool refreshRunning = false;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        std::deque<mcst::ActivityItem> activity;
        AutoTradingAlertTracker autoTradingAlerts;
        std::wstring lastReport = L"Never";
        std::wstring lastAlert = L"None";
        ScheduleTracker schedule;
        BrokerMonitor brokerMonitor;
        LogAlertEngine logAlertEngine;
        bool scheduledStatusReportEmailInFlight = false;
        bool heartbeatEmailInFlight = false;
    };

    AppState g_app;
    HFONT g_titleFont = nullptr;
    HFONT g_headerFont = nullptr;
    HFONT g_bodyFont = nullptr;
    HFONT g_statusFont = nullptr;
    HFONT g_monoFont = nullptr;
    HWND g_refreshButton = nullptr;
    HWND g_reportButton = nullptr;
    HWND g_settingsButton = nullptr;
    HWND g_openFolderButton = nullptr;
    HWND g_autoTradingDiagnosticsButton = nullptr;
    HWND g_autoTradingCaptureButton = nullptr;
    HWND g_autoTradingFinishButton = nullptr;
    HWND g_reloadSettingsButton = nullptr;
    HWND g_testEmailButton = nullptr;
    HWND g_autoMenuButton = nullptr;
    HWND g_statusMenuButton = nullptr;
    HWND g_emailMenuButton = nullptr;
    HWND g_heartbeatMenuButton = nullptr;
    ULONG_PTR g_gdiplusToken = 0;

    std::wstring FormatLocalTime(std::chrono::system_clock::time_point value)
    {
        if (value.time_since_epoch().count() == 0)
            return L"Never";
        const std::time_t tt = std::chrono::system_clock::to_time_t(value);
        tm local{};
        localtime_s(&local, &tt);
        std::wostringstream out;
        out << std::put_time(&local, L"%Y-%m-%d %H:%M");
        return out.str();
    }

    std::wstring FormatClock(std::chrono::system_clock::time_point value)
    {
        const std::time_t tt = std::chrono::system_clock::to_time_t(value);
        tm local{};
        localtime_s(&local, &tt);
        std::wostringstream out;
        out << std::put_time(&local, L"%H:%M");
        return out.str();
    }

    std::wstring FormatUptime(std::chrono::steady_clock::duration duration)
    {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        const auto days = seconds / 86400;
        const auto hours = (seconds % 86400) / 3600;
        const auto minutes = (seconds % 3600) / 60;
        std::wostringstream out;
        if (days > 0) out << days << L" d ";
        out << hours << L" h " << minutes << L" min";
        return out.str();
    }

    std::wstring FormatAge(std::chrono::system_clock::time_point value)
    {
        if (value.time_since_epoch().count() == 0)
            return L"waiting for first successful update";
        const auto seconds = (std::max)(0LL, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - value).count());
        const auto minutes = seconds / 60;
        if (minutes == 0)
            return L"less than 1 min ago";
        if (minutes < 60)
            return std::to_wstring(minutes) + L" min ago";
        const auto hours = minutes / 60;
        const auto remainingMinutes = minutes % 60;
        if (remainingMinutes == 0)
            return std::to_wstring(hours) + L" h ago";
        return std::to_wstring(hours) + L" h " + std::to_wstring(remainingMinutes) + L" min ago";
    }

    void LayoutButtons(HWND hwnd)
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int y = (std::max)(590, static_cast<int>(client.bottom) - 58);
        if (g_refreshButton) MoveWindow(g_refreshButton, 28, y, 110, 34, TRUE);
        if (g_reportButton) MoveWindow(g_reportButton, 148, y, 120, 34, TRUE);
        if (g_openFolderButton) MoveWindow(g_openFolderButton, 278, y, 120, 34, TRUE);
        const int researchY = y - 42;
        if (g_app.config.developerModeEnabled)
        {
            if (g_autoTradingDiagnosticsButton) MoveWindow(g_autoTradingDiagnosticsButton, 28, researchY, 190, 34, TRUE);
            if (g_autoTradingCaptureButton) MoveWindow(g_autoTradingCaptureButton, 228, researchY, 190, 34, TRUE);
            if (g_autoTradingFinishButton) MoveWindow(g_autoTradingFinishButton, 428, researchY, 190, 34, TRUE);
            if (g_reloadSettingsButton) MoveWindow(g_reloadSettingsButton, 628, researchY, 150, 34, TRUE);
            if (g_testEmailButton) MoveWindow(g_testEmailButton, 788, researchY, 150, 34, TRUE);
        }
        else
        {
            if (g_reloadSettingsButton) MoveWindow(g_reloadSettingsButton, 408, y, 150, 34, TRUE);
        }
        const int menuX = (std::max)(690, static_cast<int>(client.right) - 58);
        if (g_autoMenuButton) MoveWindow(g_autoMenuButton, menuX, 216, 30, 24, TRUE);
        if (g_statusMenuButton) MoveWindow(g_statusMenuButton, menuX, 318, 30, 24, TRUE);
        if (g_emailMenuButton) MoveWindow(g_emailMenuButton, menuX, 352, 30, 24, TRUE);
        if (g_heartbeatMenuButton) MoveWindow(g_heartbeatMenuButton, menuX, 386, 30, 24, TRUE);
    }

    void UpdateDeveloperControlVisibility(HWND hwnd)
    {
        const int showResearch = g_app.config.developerModeEnabled ? SW_SHOW : SW_HIDE;
        if (g_settingsButton) ShowWindow(g_settingsButton, SW_HIDE);
        if (g_testEmailButton) ShowWindow(g_testEmailButton, SW_HIDE);
        for (HWND control : { g_autoTradingDiagnosticsButton, g_autoTradingCaptureButton, g_autoTradingFinishButton })
        {
            if (control)
                ShowWindow(control, showResearch);
        }
        LayoutButtons(hwnd);
    }

    struct StatusSettingsDialogState
    {
        HWND owner = nullptr;
        bool saved = false;
        HWND enabled = nullptr;
        HWND recipient = nullptr;
        HWND interval = nullptr;
        HWND startup = nullptr;
        HWND weekdays = nullptr;
        HWND sendStart = nullptr;
        HWND sendEnd = nullptr;
    };

    std::wstring GetControlText(HWND control)
    {
        const int length = GetWindowTextLengthW(control);
        std::wstring value(static_cast<std::size_t>((std::max)(0, length)) + 1, L'\0');
        if (length > 0)
            GetWindowTextW(control, value.data(), length + 1);
        value.resize(static_cast<std::size_t>((std::max)(0, length)));
        return value;
    }

    LRESULT CALLBACK StatusSettingsWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<StatusSettingsDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = static_cast<StatusSettingsDialogState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }

        switch (message)
        {
        case WM_CREATE:
        {
            const AppConfig& config = g_app.config;
            CreateWindowW(L"STATIC", L"Status Report Settings", WS_CHILD | WS_VISIBLE,
                20, 16, 360, 26, hwnd, nullptr, nullptr, nullptr);
            state->enabled = CreateWindowW(L"BUTTON", L"Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                22, 54, 160, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusSettingsEnabled)), nullptr, nullptr);
            SendMessageW(state->enabled, BM_SETCHECK, config.statusReportsEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

            CreateWindowW(L"STATIC", L"Recipient", WS_CHILD | WS_VISIBLE,
                22, 92, 110, 22, hwnd, nullptr, nullptr, nullptr);
            state->recipient = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", config.reportEmailTo.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 132, 88, 340, 26, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusSettingsRecipient)), nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Interval (minutes)", WS_CHILD | WS_VISIBLE,
                22, 132, 110, 22, hwnd, nullptr, nullptr, nullptr);
            state->interval = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(config.statusReportIntervalMinutes).c_str(),
                WS_CHILD | WS_VISIBLE | ES_NUMBER, 132, 128, 100, 26, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusSettingsInterval)), nullptr, nullptr);

            state->startup = CreateWindowW(L"BUTTON", L"Send immediately on application start", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                22, 172, 320, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusSettingsStartup)), nullptr, nullptr);
            SendMessageW(state->startup, BM_SETCHECK, config.statusReportSendOnStartup ? BST_CHECKED : BST_UNCHECKED, 0);

            CreateWindowW(L"STATIC", L"Weekdays", WS_CHILD | WS_VISIBLE,
                22, 212, 110, 22, hwnd, nullptr, nullptr, nullptr);
            state->weekdays = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", config.statusReportWeekdays.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 132, 208, 140, 26, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusSettingsWeekdays)), nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Sending window", WS_CHILD | WS_VISIBLE,
                22, 252, 110, 22, hwnd, nullptr, nullptr, nullptr);
            state->sendStart = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", config.statusReportSendStart.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 132, 248, 80, 26, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusSettingsStart)), nullptr, nullptr);
            CreateWindowW(L"STATIC", L"to", WS_CHILD | WS_VISIBLE,
                220, 252, 24, 22, hwnd, nullptr, nullptr, nullptr);
            state->sendEnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", config.statusReportSendEnd.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 248, 248, 80, 26, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusSettingsEnd)), nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"Status Reports and Heartbeats use the report recipient. Alerts use [Email] alert_to.",
                WS_CHILD | WS_VISIBLE, 22, 292, 450, 42, hwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                278, 350, 92, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusSettingsSave)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                380, 350, 92, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusSettingsCancel)), nullptr, nullptr);

            EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
                return TRUE;
            }, 0);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kStatusSettingsSave)
            {
                const bool enabled = SendMessageW(state->enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
                const bool sendOnStartup = SendMessageW(state->startup, BM_GETCHECK, 0, 0) == BST_CHECKED;
                const std::wstring recipient = GetControlText(state->recipient);
                const std::wstring intervalText = GetControlText(state->interval);
                const std::wstring weekdays = GetControlText(state->weekdays);
                const std::wstring sendStart = GetControlText(state->sendStart);
                const std::wstring sendEnd = GetControlText(state->sendEnd);
                wchar_t* end = nullptr;
                const long interval = std::wcstol(intervalText.c_str(), &end, 10);
                if (end == intervalText.c_str() || *end != L'\0')
                {
                    MessageBoxW(hwnd, L"Enter a valid whole number for the interval.", L"Status Report Settings", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                std::wstring error;
                if (!SaveStatusReportSettings(enabled, recipient, static_cast<int>(interval), sendOnStartup, weekdays, sendStart, sendEnd, error))
                {
                    MessageBoxW(hwnd, error.c_str(), L"Status Report Settings", MB_OK | MB_ICONERROR);
                    return 0;
                }
                state->saved = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == kStatusSettingsCancel)
            {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool ShowStatusReportSettings(HWND owner)
    {
        static bool registered = false;
        constexpr wchar_t className[] = L"MCSTStatusReportSettingsWindow";
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = StatusSettingsWindowProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = className;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
            registered = true;
        }

        StatusSettingsDialogState state;
        state.owner = owner;
        RECT ownerRect{};
        GetWindowRect(owner, &ownerRect);
        const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - 510) / 2;
        const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - 440) / 2;
        HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, className, L"MCST-Watchdog - Status Report Settings",
            WS_CAPTION | WS_SYSMENU | WS_POPUP, x, y, 510, 440, owner, nullptr, GetModuleHandleW(nullptr), &state);
        if (!window)
            return false;

        EnableWindow(owner, FALSE);
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
        MSG msg{};
        while (IsWindow(window) && GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        return state.saved;
    }

    RECT ResolveInitialWindowRect(const AppConfig& config)
    {
        RECT rect{ config.windowLeft, config.windowTop, config.windowLeft + config.windowWidth, config.windowTop + config.windowHeight };
        if (config.windowLeft == -1 && config.windowTop == -1)
            return { CW_USEDEFAULT, CW_USEDEFAULT, config.windowWidth, config.windowHeight };

        HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info))
        {
            const int width = rect.right - rect.left;
            const int height = rect.bottom - rect.top;
            rect.left = (std::max)(info.rcWork.left, (std::min)(rect.left, info.rcWork.right - width));
            rect.top = (std::max)(info.rcWork.top, (std::min)(rect.top, info.rcWork.bottom - height));
            rect.right = rect.left + width;
            rect.bottom = rect.top + height;
        }
        return rect;
    }

    COLORREF StateColor(mcst::HealthState state)
    {
        switch (state)
        {
        case mcst::HealthState::Healthy: return RGB(0, 184, 84);
        case mcst::HealthState::Attention: return RGB(242, 166, 0);
        case mcst::HealthState::Critical: return RGB(229, 45, 55);
        default: return RGB(146, 153, 164);
        }
    }

    COLORREF IndicatorColor(mcst::HealthState state)
    {
        switch (state)
        {
        case mcst::HealthState::Healthy: return RGB(0, 255, 72);
        case mcst::HealthState::Attention: return RGB(255, 190, 0);
        case mcst::HealthState::Critical: return RGB(255, 48, 62);
        default: return RGB(170, 178, 190);
        }
    }

    mcst::HealthState Worst(mcst::HealthState left, mcst::HealthState right)
    {
        auto rank = [](mcst::HealthState state) {
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

    void AddActivity(mcst::WatchdogSystemStatus& status, mcst::HealthState state, const std::wstring& text)
    {
        status.activity.insert(status.activity.begin(), { std::chrono::system_clock::now(), state, text });
        if (status.activity.size() > 10)
            status.activity.resize(10);
    }

    void ReadProcessResources(mcst::WatchdogSystemStatus& status)
    {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
            status.privateMemoryBytes = static_cast<std::size_t>(memory.PrivateUsage);
        GetProcessHandleCount(GetCurrentProcess(), &status.handleCount);
        status.uptime = FormatUptime(std::chrono::steady_clock::now() - g_app.started);
    }

    RefreshResult CollectStatus(bool forceAutoTradingRefresh)
    {
        RefreshResult result;
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            result.status.activity = g_app.status.activity;
        }
        result.status.lastSnapshot = L"Never";
        result.status.lastReport = g_app.lastReport;
        result.status.lastAlert = g_app.lastAlert;
        result.status.autoTradingMinimum = g_app.config.autoTradingMinimum;

        std::wstring diagnostic;
        bool readOk = false;
        for (int attempt = 1; attempt <= g_app.config.snapshotRetryCount; ++attempt)
        {
            diagnostic.clear();
            if (ReadTrackerStatusSnapshot(result.snapshot, diagnostic, static_cast<unsigned long>(g_app.config.bridgeTimeoutMilliseconds)))
            {
                readOk = true;
                break;
            }
            if (attempt < g_app.config.snapshotRetryCount)
                Sleep(static_cast<DWORD>(g_app.config.snapshotRetryDelayMilliseconds));
        }

        const auto now = std::chrono::system_clock::now();
        if (readOk)
        {
            result.status.bridge = { mcst::HealthState::Healthy, L"Connected", L"Bridge V" + std::to_wstring(result.snapshot.bridgeVersion) };
            const bool trackerOk = result.snapshot.trackerFound && result.snapshot.trackerSameProcess;
            result.status.trackerSnapshot = {
                trackerOk ? mcst::HealthState::Healthy : mcst::HealthState::Critical,
                trackerOk ? L"Snapshot OK" : L"Tracker not available",
                diagnostic
            };
            result.status.accountRows = result.snapshot.accounts.rows.size();
            result.status.openPositionRows = result.snapshot.openPositions.rows.size();
            result.status.recentLogRows = result.snapshot.recentLogs.rows.size();
            result.status.processId = result.snapshot.processId;
            result.status.lastSuccessfulUpdate = now;
            result.status.lastSnapshot = FormatLocalTime(now);

            if (result.snapshot.recentLogs.ok && !result.snapshot.recentLogs.rows.empty())
                result.status.recentLogs = { mcst::HealthState::Healthy, std::to_wstring(result.snapshot.recentLogs.rows.size()) + L" rows", L"Updating" };
            else if (result.snapshot.recentLogs.ok)
                result.status.recentLogs = { mcst::HealthState::Attention, L"0 rows", L"Unusual but readable" };
            else
                result.status.recentLogs = { mcst::HealthState::Critical, L"Read failed", result.snapshot.recentLogs.diagnostic };

            std::wstring rawDiagnostic;
            WriteTrackerStatusRawPayload(result.snapshot, g_app.config.rawSnapshotPath, rawDiagnostic);
            AddActivity(result.status, mcst::HealthState::Healthy, L"Tracker snapshot read successfully");
        }
        else
        {
            result.status.bridge = { mcst::HealthState::Critical, L"Disconnected", diagnostic };
            result.status.trackerSnapshot = { mcst::HealthState::Critical, L"Unavailable", L"Snapshot read failed" };
            result.status.recentLogs = { mcst::HealthState::Unknown, L"Unknown", L"No snapshot" };
            result.status.lastError = diagnostic;
            AddActivity(result.status, mcst::HealthState::Critical, L"Tracker snapshot failed");
        }

        if (g_app.config.autoTradingMonitoringEnabled)
        {
            const AutoTradingReadResult autoTrading = ReadAutoTradingStatus(g_app.config.autoTradingCheckMinutes, forceAutoTradingRefresh);
            if (autoTrading.succeeded)
            {
                result.status.lastAutoTradingRead = FormatLocalTime(autoTrading.lastSuccessfulRead);
                result.status.autoTradingActive = autoTrading.activeStrategies;
                const bool belowMinimum = autoTrading.activeStrategies < g_app.config.autoTradingMinimum;
                result.status.autoTrading = {
                    belowMinimum ? mcst::HealthState::Critical : mcst::HealthState::Healthy,
                    std::to_wstring(autoTrading.activeStrategies) + L" Active",
                    L"Minimum required " + std::to_wstring(g_app.config.autoTradingMinimum) +
                        L" - Objects found " + std::to_wstring(autoTrading.strategyObjectsFound) +
                        (autoTrading.compatibilityProfile.empty() ? L"" : L" - " + autoTrading.compatibilityProfile)
                };
                if (!autoTrading.fromCache)
                {
                    AddActivity(result.status,
                        belowMinimum ? mcst::HealthState::Critical : mcst::HealthState::Healthy,
                        L"AutoTrading read: " + std::to_wstring(autoTrading.activeStrategies) + L" active");
                }
            }
            else
            {
                result.status.autoTradingActive = -1;
                if (autoTrading.lastSuccessfulRead.time_since_epoch().count() != 0)
                    result.status.lastAutoTradingRead = FormatLocalTime(autoTrading.lastSuccessfulRead);
                result.status.autoTrading = {
                    mcst::HealthState::Unknown,
                    L"Read unavailable",
                    L"Minimum required " + std::to_wstring(g_app.config.autoTradingMinimum) + L" - " + autoTrading.diagnostic
                };
                if (!autoTrading.fromCache)
                    AddActivity(result.status, mcst::HealthState::Attention, L"AutoTrading read unavailable");
            }
        }
        else
        {
            result.status.autoTrading = { mcst::HealthState::Unknown, L"Disabled", L"" };
        }

        result.status.broker = { mcst::HealthState::Unknown, L"Waiting", L"Broker events are evaluated from Recent Logs" };
        result.status.statusReports = {
            g_app.config.statusReportsEnabled ? mcst::HealthState::Healthy : mcst::HealthState::Unknown,
            g_app.config.statusReportsEnabled ? L"Scheduled" : L"Disabled",
            g_app.config.statusReportsEnabled
                ? (L"Every " + std::to_wstring(g_app.config.statusReportIntervalMinutes) + L" min to " + g_app.config.reportEmailTo)
                : L"Manual report remains available"
        };
        {
            EmailSender sender(g_app.config);
            std::wstring emailReason;
            const bool configured = sender.IsConfigured(&emailReason, g_app.config.alertEmailTo);
            const bool explicitlyDisabled = g_app.config.emailEnabledSettingPresent && !g_app.config.emailEnabled;
            result.status.email = {
                configured ? mcst::HealthState::Healthy : (explicitlyDisabled ? mcst::HealthState::Unknown : mcst::HealthState::Attention),
                configured ? L"Ready" : (explicitlyDisabled ? L"Disabled" : L"Not configured"),
                configured ? g_app.config.alertEmailTo : emailReason
            };
        }
        {
            EmailSender sender(g_app.config);
            std::wstring emailReason;
            const bool emailConfigured = sender.IsConfigured(&emailReason, g_app.config.reportEmailTo);
            const bool explicitlyDisabled = g_app.config.heartbeatEnabledSettingPresent && !g_app.config.heartbeatEnabled;
            const bool operational = g_app.config.heartbeatEnabled && emailConfigured;
            result.status.heartbeat = {
                operational ? mcst::HealthState::Healthy
                            : (explicitlyDisabled ? mcst::HealthState::Unknown : mcst::HealthState::Attention),
                operational ? L"Running"
                            : (explicitlyDisabled ? L"Disabled" : L"Not configured"),
                operational
                    ? (L"Email every " + std::to_wstring(g_app.config.heartbeatIntervalMinutes) + L" min to " + g_app.config.reportEmailTo)
                    : (explicitlyDisabled ? L"Explicitly disabled in INI" : emailReason)
            };
        }

        result.status.overall = mcst::HealthState::Healthy;
        result.status.overall = Worst(result.status.overall, result.status.bridge.state);
        result.status.overall = Worst(result.status.overall, result.status.trackerSnapshot.state);
        result.status.overall = Worst(result.status.overall, result.status.recentLogs.state);
        result.status.overall = Worst(result.status.overall, result.status.autoTrading.state);
        if (g_app.config.statusReportsEnabled)
            result.status.overall = Worst(result.status.overall, result.status.statusReports.state);
        if (!(g_app.config.emailEnabledSettingPresent && !g_app.config.emailEnabled))
            result.status.overall = Worst(result.status.overall, result.status.email.state);
        if (g_app.config.heartbeatEnabled)
            result.status.overall = Worst(result.status.overall, result.status.heartbeat.state);

        ReadProcessResources(result.status);
        result.diagnostic = diagnostic;
        return result;
    }

    void StartRefresh(HWND hwnd, bool forceAutoTradingRefresh = false)
    {
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            if (g_app.refreshRunning)
                return;
            g_app.refreshRunning = true;
        }

        std::thread([hwnd, forceAutoTradingRefresh]() {
            auto result = std::make_unique<RefreshResult>(CollectStatus(forceAutoTradingRefresh));
            PostMessageW(hwnd, WM_APP_REFRESH_COMPLETE, 0, reinterpret_cast<LPARAM>(result.release()));
        }).detach();
    }

    void DrawTextSimple(HDC dc, const RECT& rect, const std::wstring& text, HFONT font, COLORREF color, UINT format)
    {
        const HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        RECT copy = rect;
        DrawTextW(dc, text.c_str(), -1, &copy, format);
        SelectObject(dc, oldFont);
    }


    enum class SettingsPanelKind { AutoTrading, Email, Heartbeat };

    struct SimpleSettingsState
    {
        SettingsPanelKind kind{};
        bool saved = false;
        HWND enabled = nullptr;
        HWND fields[7]{};
        HWND checks[2]{};
    };

    HWND AddLabel(HWND parent, const wchar_t* text, int y)
    {
        return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, 22, y + 4, 150, 22, parent, nullptr, nullptr, nullptr);
    }

    HWND AddEdit(HWND parent, const std::wstring& text, int id, int y, int width = 310, DWORD extra = 0)
    {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra,
            178, y, width, 26, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    }

    LRESULT CALLBACK SimpleSettingsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<SimpleSettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = static_cast<SimpleSettingsState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        switch (message)
        {
        case WM_CREATE:
        {
            const AppConfig& c = g_app.config;
            const wchar_t* title = state->kind == SettingsPanelKind::AutoTrading ? L"AutoTrading Settings" :
                state->kind == SettingsPanelKind::Email ? L"Email Settings" : L"Heartbeat Settings";
            CreateWindowW(L"STATIC", title, WS_CHILD | WS_VISIBLE, 20, 14, 430, 28, hwnd, nullptr, nullptr, nullptr);
            state->enabled = CreateWindowW(L"BUTTON", L"Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                22, 48, 160, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleEnabled)), nullptr, nullptr);
            bool enabled = state->kind == SettingsPanelKind::AutoTrading ? c.autoTradingMonitoringEnabled :
                state->kind == SettingsPanelKind::Email ? c.emailEnabled : c.heartbeatEnabled;
            SendMessageW(state->enabled, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            int y = 84;
            if (state->kind == SettingsPanelKind::AutoTrading)
            {
                AddLabel(hwnd, L"Minimum active strategies", y); state->fields[0] = AddEdit(hwnd, std::to_wstring(c.autoTradingMinimum), kSimpleField1, y, 100, ES_NUMBER); y += 38;
                AddLabel(hwnd, L"Check interval (minutes)", y); state->fields[1] = AddEdit(hwnd, std::to_wstring(c.autoTradingCheckMinutes), kSimpleField2, y, 100, ES_NUMBER); y += 40;
                state->checks[0] = CreateWindowW(L"BUTTON", L"Send alert email", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 22, y, 200, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleCheck1)), nullptr, nullptr);
                SendMessageW(state->checks[0], BM_SETCHECK, c.autoTradingAlertEmailEnabled ? BST_CHECKED : BST_UNCHECKED, 0); y += 30;
                state->checks[1] = CreateWindowW(L"BUTTON", L"Send recovery email", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 22, y, 220, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleCheck2)), nullptr, nullptr);
                SendMessageW(state->checks[1], BM_SETCHECK, c.autoTradingRecoveryEmailEnabled ? BST_CHECKED : BST_UNCHECKED, 0); y += 44;
            }
            else if (state->kind == SettingsPanelKind::Heartbeat)
            {
                AddLabel(hwnd, L"Interval (minutes)", y); state->fields[0] = AddEdit(hwnd, std::to_wstring(c.heartbeatIntervalMinutes), kSimpleField1, y, 100, ES_NUMBER); y += 42;
                state->checks[0] = CreateWindowW(L"BUTTON", L"Send on application start", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 22, y, 250, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleCheck1)), nullptr, nullptr);
                SendMessageW(state->checks[0], BM_SETCHECK, c.heartbeatSendOnStartup ? BST_CHECKED : BST_UNCHECKED, 0); y += 40;
                CreateWindowW(L"STATIC", L"Recipient is managed in Email Settings as the report recipient.", WS_CHILD | WS_VISIBLE, 22, y, 450, 38, hwnd, nullptr, nullptr, nullptr); y += 48;
            }
            else
            {
                AddLabel(hwnd, L"SMTP server", y); state->fields[0] = AddEdit(hwnd, c.smtpServer, kSimpleField1, y); y += 36;
                AddLabel(hwnd, L"SMTP port", y); state->fields[1] = AddEdit(hwnd, std::to_wstring(c.smtpPort), kSimpleField2, y, 100, ES_NUMBER); y += 36;
                AddLabel(hwnd, L"User name", y); state->fields[2] = AddEdit(hwnd, c.smtpUser, kSimpleField3, y); y += 36;
                AddLabel(hwnd, L"Password", y); state->fields[3] = AddEdit(hwnd, L"", kSimpleField4, y, 310, ES_PASSWORD); y += 36;
                AddLabel(hwnd, L"From address", y); state->fields[4] = AddEdit(hwnd, c.emailFrom, kSimpleField5, y); y += 36;
                AddLabel(hwnd, L"Alert recipient", y); state->fields[5] = AddEdit(hwnd, c.alertEmailTo, kSimpleField6, y); y += 36;
                AddLabel(hwnd, L"Report recipient", y); state->fields[6] = AddEdit(hwnd, c.reportEmailTo, kSimpleField7, y); y += 38;
                state->checks[0] = CreateWindowW(L"BUTTON", L"Use SSL/TLS", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 22, y, 160, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleCheck1)), nullptr, nullptr);
                SendMessageW(state->checks[0], BM_SETCHECK, c.smtpUseSsl ? BST_CHECKED : BST_UNCHECKED, 0); y += 36;
                CreateWindowW(L"STATIC", L"Leave Password empty to keep the current stored password.", WS_CHILD | WS_VISIBLE, 22, y, 450, 22, hwnd, nullptr, nullptr, nullptr); y += 34;
            }
            CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 300, y, 90, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleSave)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 400, y, 90, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleCancel)), nullptr, nullptr);
            EnumChildWindows(hwnd, [](HWND child, LPARAM)->BOOL { SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE); return TRUE; }, 0);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kSimpleCancel) { DestroyWindow(hwnd); return 0; }
            if (LOWORD(wParam) == kSimpleSave)
            {
                std::wstring error;
                const bool enabled = SendMessageW(state->enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool ok = false;
                if (state->kind == SettingsPanelKind::AutoTrading)
                {
                    const int minimum = _wtoi(GetControlText(state->fields[0]).c_str());
                    const int interval = _wtoi(GetControlText(state->fields[1]).c_str());
                    ok = SaveAutoTradingSettings(enabled, minimum, interval,
                        SendMessageW(state->checks[0], BM_GETCHECK, 0, 0) == BST_CHECKED,
                        SendMessageW(state->checks[1], BM_GETCHECK, 0, 0) == BST_CHECKED, error);
                }
                else if (state->kind == SettingsPanelKind::Heartbeat)
                {
                    ok = SaveHeartbeatSettings(enabled, _wtoi(GetControlText(state->fields[0]).c_str()),
                        SendMessageW(state->checks[0], BM_GETCHECK, 0, 0) == BST_CHECKED, error);
                }
                else
                {
                    ok = SaveEmailSettings(enabled, GetControlText(state->fields[0]), _wtoi(GetControlText(state->fields[1]).c_str()),
                        SendMessageW(state->checks[0], BM_GETCHECK, 0, 0) == BST_CHECKED,
                        GetControlText(state->fields[2]), GetControlText(state->fields[3]), GetControlText(state->fields[4]),
                        GetControlText(state->fields[5]), GetControlText(state->fields[6]), error);
                }
                if (!ok) { MessageBoxW(hwnd, error.c_str(), L"Settings", MB_OK | MB_ICONERROR); return 0; }
                state->saved = true; DestroyWindow(hwnd); return 0;
            }
            break;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool ShowSimpleSettings(HWND owner, SettingsPanelKind kind)
    {
        static bool registered = false;
        constexpr wchar_t className[] = L"MCSTSimpleSettingsWindow";
        if (!registered)
        {
            WNDCLASSW wc{}; wc.lpfnWndProc = SimpleSettingsProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = className;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
            registered = true;
        }
        SimpleSettingsState state{}; state.kind = kind;
        const int height = kind == SettingsPanelKind::Email ? 500 : 320;
        HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, className, L"MCST-Watchdog Settings", WS_CAPTION | WS_SYSMENU | WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, 530, height, owner, nullptr, GetModuleHandleW(nullptr), &state);
        if (!window) return false;
        RECT ownerRect{}, dialogRect{}; GetWindowRect(owner, &ownerRect); GetWindowRect(window, &dialogRect);
        SetWindowPos(window, HWND_TOP, ownerRect.left + ((ownerRect.right-ownerRect.left)-(dialogRect.right-dialogRect.left))/2,
            ownerRect.top + ((ownerRect.bottom-ownerRect.top)-(dialogRect.bottom-dialogRect.top))/2, 0, 0, SWP_NOSIZE);
        EnableWindow(owner, FALSE); ShowWindow(window, SW_SHOW); UpdateWindow(window);
        MSG msg{}; while (IsWindow(window) && GetMessageW(&msg, nullptr, 0, 0) > 0) { if (!IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
        EnableWindow(owner, TRUE); SetForegroundWindow(owner); return state.saved;
    }

    void ReloadAfterPanelSave(HWND hwnd, const wchar_t* activity)
    {
    g_app.config = LoadAppConfig();
        g_app.schedule.PreserveOnReload(std::chrono::system_clock::now());
        AddActivity(g_app.status, mcst::HealthState::Healthy, activity);
        UpdateDeveloperControlVisibility(hwnd);
        StartRefresh(hwnd, true);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void ShowRowMenu(HWND hwnd, HWND button, int rowId)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuConfigure, L"Configure...");
        if (rowId == kButtonAutoMenu && g_app.config.developerModeEnabled)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kMenuAction1, L"Start AutoTrading research");
            AppendMenuW(menu, MF_STRING, kMenuAction2, L"Open research file");
        }
        else if (rowId == kButtonStatusMenu)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kMenuAction1, L"Send status report now");
            AppendMenuW(menu, MF_STRING, kMenuAction2, L"Open report folder");
        }
        else if (rowId == kButtonEmailMenu)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kMenuAction1, L"Send alert test");
            AppendMenuW(menu, MF_STRING, kMenuAction2, L"Send report test");
        }
        else if (rowId == kButtonHeartbeatMenu)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kMenuAction1, L"Send heartbeat now");
        }
        RECT r{}; GetWindowRect(button, &r);
        const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (!command) return;
        if (command == kMenuConfigure)
        {
            bool saved = false;
            if (rowId == kButtonAutoMenu) saved = ShowSimpleSettings(hwnd, SettingsPanelKind::AutoTrading);
            else if (rowId == kButtonStatusMenu) saved = ShowStatusReportSettings(hwnd);
            else if (rowId == kButtonEmailMenu) saved = ShowSimpleSettings(hwnd, SettingsPanelKind::Email);
            else if (rowId == kButtonHeartbeatMenu) saved = ShowSimpleSettings(hwnd, SettingsPanelKind::Heartbeat);
            if (saved) ReloadAfterPanelSave(hwnd, L"Dashboard settings panel saved");
        }
        else if (rowId == kButtonStatusMenu && command == kMenuAction1)
            SendMessageW(hwnd, WM_COMMAND, kButtonReport, 0);
        else if (rowId == kButtonStatusMenu && command == kMenuAction2)
            ShellExecuteW(hwnd, L"open", GetApplicationDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (rowId == kButtonEmailMenu && command == kMenuAction1)
            SendMessageW(hwnd, WM_COMMAND, kButtonTestEmail, 0);
        else if (rowId == kButtonEmailMenu && command == kMenuAction2)
        {
            SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, L"MCST-Watchdog Report Channel Test",
                L"This test was sent through the report email channel.", false, L"Report test email", false, g_app.config.reportEmailTo);
        }
        else if (rowId == kButtonHeartbeatMenu && command == kMenuAction1)
        {
            mcst::WatchdogSystemStatus status; TrackerStatusSnapshot snapshot;
            { std::lock_guard<std::mutex> lock(g_app.mutex); status=g_app.status; snapshot=g_app.snapshot; }
            const std::wstring body=BuildStatusReport(status,snapshot);
            SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, L"MCST-Watchdog HEARTBEAT (manual)", BuildStatusReportHtml(body), false, L"Heartbeat email", true, g_app.config.reportEmailTo);
        }
        else if (rowId == kButtonAutoMenu && command == kMenuAction1)
            SendMessageW(hwnd, WM_COMMAND, kButtonAutoTradingDiagnostics, 0);
        else if (rowId == kButtonAutoMenu && command == kMenuAction2)
            ShellExecuteW(hwnd, L"open", L"C:\Temp\MCST-Watchdog\AutoTradingResearch.txt", nullptr, nullptr, SW_SHOWNORMAL);
    }

    Gdiplus::Color IndicatorTopColor(mcst::HealthState state, BYTE alpha = 255)
    {
        switch (state)
        {
        case mcst::HealthState::Healthy: return Gdiplus::Color(alpha, 54, 190, 72);
        case mcst::HealthState::Attention: return Gdiplus::Color(alpha, 246, 190, 54);
        case mcst::HealthState::Critical: return Gdiplus::Color(alpha, 224, 74, 78);
        default: return Gdiplus::Color(alpha, 174, 179, 187);
        }
    }

    Gdiplus::Color IndicatorBottomColor(mcst::HealthState state, BYTE alpha = 255)
    {
        switch (state)
        {
        case mcst::HealthState::Healthy: return Gdiplus::Color(alpha, 18, 145, 47);
        case mcst::HealthState::Attention: return Gdiplus::Color(alpha, 218, 145, 20);
        case mcst::HealthState::Critical: return Gdiplus::Color(alpha, 190, 38, 43);
        default: return Gdiplus::Color(alpha, 128, 134, 144);
        }
    }

    void DrawModernIndicator(HDC dc, int x, int y, int diameter, mcst::HealthState state, BYTE alpha = 255)
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        const Gdiplus::RectF shadowRect(static_cast<Gdiplus::REAL>(x + 1), static_cast<Gdiplus::REAL>(y + 2),
            static_cast<Gdiplus::REAL>(diameter), static_cast<Gdiplus::REAL>(diameter));
        Gdiplus::SolidBrush shadow(Gdiplus::Color(35, 0, 0, 0));
        graphics.FillEllipse(&shadow, shadowRect);

        const Gdiplus::RectF indicatorRect(static_cast<Gdiplus::REAL>(x), static_cast<Gdiplus::REAL>(y),
            static_cast<Gdiplus::REAL>(diameter), static_cast<Gdiplus::REAL>(diameter));
        Gdiplus::LinearGradientBrush fill(indicatorRect, IndicatorTopColor(state, alpha),
            IndicatorBottomColor(state, alpha), Gdiplus::LinearGradientModeVertical);
        graphics.FillEllipse(&fill, indicatorRect);

        Gdiplus::Pen rim(Gdiplus::Color(70, 255, 255, 255), 1.0f);
        graphics.DrawEllipse(&rim, indicatorRect);
    }

    void DrawStatusRow(HDC dc, int y, const wchar_t* label, const mcst::MonitorStatus& item, int width)
    {
        const int left = 34;
        DrawModernIndicator(dc, left + 8, y + 7, 17, item.state);

        DrawTextSimple(dc, { left + 37, y, 250, y + 32 }, label, g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { 250, y, 410, y + 32 }, mcst::HealthStateText(item.state), g_statusFont, StateColor(item.state), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { 405, y, width - 28, y + 32 }, item.value + (item.detail.empty() ? L"" : L"  -  " + item.detail), g_bodyFont, RGB(70, 76, 86), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void PaintDashboard(HWND hwnd, HDC dc)
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

        mcst::WatchdogSystemStatus status;
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            status = g_app.status;
        }

        DrawTextSimple(dc, { 28, 20, client.right - 28, 64 }, L"MCST-Watchdog 1.091", g_titleFont, RGB(25, 28, 34), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        const wchar_t* overallText = L"INITIALIZING";
        switch (status.overall)
        {
        case mcst::HealthState::Healthy: overallText = L"SYSTEM HEALTHY"; break;
        case mcst::HealthState::Attention: overallText = L"ATTENTION REQUIRED"; break;
        case mcst::HealthState::Critical: overallText = L"CRITICAL CONDITION"; break;
        default: break;
        }

        const int overallDotLeft = client.right - 350;
        const int overallDotTop = 28;
        BYTE overallAlpha = 255;
        if (status.overall == mcst::HealthState::Healthy)
        {
            const double phase = static_cast<double>(GetTickCount64() % 4000ULL) / 4000.0;
            const double wave = (1.0 - std::cos(phase * 6.283185307179586)) * 0.5;
            overallAlpha = static_cast<BYTE>(235 + static_cast<int>(20.0 * wave));
        }
        DrawModernIndicator(dc, overallDotLeft, overallDotTop, 27, status.overall, overallAlpha);

        DrawTextSimple(dc, { client.right - 318, 20, client.right - 28, 64 }, overallText, g_titleFont, StateColor(status.overall), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        HPEN divider = CreatePen(PS_SOLID, 1, RGB(229, 232, 237));
        HGDIOBJ oldPen = SelectObject(dc, divider);
        MoveToEx(dc, 28, 74, nullptr); LineTo(dc, client.right - 28, 74);
        SelectObject(dc, oldPen);
        DeleteObject(divider);

        const std::wstring updateText = status.lastSuccessfulUpdate.time_since_epoch().count() == 0
            ? L"Last successful system update: waiting for first successful update"
            : L"Last successful system update: " + FormatClock(status.lastSuccessfulUpdate) + L"  (" + FormatAge(status.lastSuccessfulUpdate) + L")";
        DrawTextSimple(dc, { 28, 80, client.right - 250, 106 }, updateText, g_bodyFont, RGB(90, 96, 106), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextSimple(dc, { client.right - 245, 80, client.right - 28, 106 }, L"Uptime  " + status.uptime, g_bodyFont, RGB(90, 96, 106), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        DrawTextSimple(dc, { 28, 108, client.right - 28, 138 }, L"SYSTEM STATUS", g_headerFont, RGB(55, 60, 70), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int y = 144;
        DrawStatusRow(dc, y, L"Bridge", status.bridge, client.right); y += 34;
        DrawStatusRow(dc, y, L"Tracker Snapshot", status.trackerSnapshot, client.right); y += 34;
        DrawStatusRow(dc, y, L"AutoTrading", status.autoTrading, client.right); y += 34;
        DrawStatusRow(dc, y, L"Broker", status.broker, client.right); y += 34;
        DrawStatusRow(dc, y, L"Recent Logs", status.recentLogs, client.right); y += 34;
        DrawStatusRow(dc, y, L"Status Reports", status.statusReports, client.right); y += 34;
        DrawStatusRow(dc, y, L"Email", status.email, client.right); y += 34;
        DrawStatusRow(dc, y, L"Heartbeat", status.heartbeat, client.right); y += 46;

        DrawTextSimple(dc, { 28, y, client.right - 28, y + 30 }, L"LATEST ACTIVITY", g_headerFont, RGB(55, 60, 70), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 34;
        const int middle = client.right / 2;
        constexpr int latestLabelLeft = 34;
        constexpr int latestLabelRight = 184;
        constexpr int latestValueLeft = 194;
        DrawTextSimple(dc, { latestLabelLeft, y, latestLabelRight, y + 28 }, L"Last Snapshot", g_bodyFont, RGB(90, 96, 106), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { latestValueLeft, y, middle - 10, y + 28 }, status.lastSnapshot, g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { middle + 10, y, client.right - 28, y + 28 }, L"Accounts  " + std::to_wstring(status.accountRows) + L"    Positions  " + std::to_wstring(status.openPositionRows) + L"    Logs  " + std::to_wstring(status.recentLogRows), g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 32;
        DrawTextSimple(dc, { latestLabelLeft, y, latestLabelRight, y + 28 }, L"Last AutoTrading Read", g_bodyFont, RGB(90, 96, 106), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { latestValueLeft, y, middle - 10, y + 28 }, status.lastAutoTradingRead.empty() ? L"Never" : status.lastAutoTradingRead, g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { middle + 10, y, client.right - 28, y + 28 }, L"Uptime  " + status.uptime + L"    Memory  " + std::to_wstring(status.privateMemoryBytes / (1024 * 1024)) + L" MB    Handles  " + std::to_wstring(status.handleCount), g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += 44;

        DrawTextSimple(dc, { 28, y, client.right - 28, y + 30 }, L"RECENT ACTIVITY", g_headerFont, RGB(55, 60, 70), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 34;
        for (std::size_t i = 0; i < status.activity.size() && i < 5 && y + 26 < client.bottom - 72; ++i)
        {
            const auto& item = status.activity[i];
            DrawTextSimple(dc, { 34, y, 88, y + 26 }, FormatClock(item.time), g_monoFont, RGB(105, 110, 120), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawTextSimple(dc, { 100, y, client.right - 28, y + 26 }, item.text, g_bodyFont, StateColor(item.state), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += 26;
        }
    }

    void CreateFonts()
    {
        g_titleFont = CreateFontW(26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_headerFont = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_bodyFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
        g_statusFont = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
        g_monoFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            g_refreshButton = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 28, 690, 110, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonRefresh)), nullptr, nullptr);
            g_reportButton = CreateWindowW(L"BUTTON", L"Save Report", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 148, 690, 120, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonReport)), nullptr, nullptr);
            g_settingsButton = CreateWindowW(L"BUTTON", L"Status Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 278, 690, 110, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonSettings)), nullptr, nullptr);
            g_openFolderButton = CreateWindowW(L"BUTTON", L"Open Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 398, 690, 120, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonOpenFolder)), nullptr, nullptr);
            g_autoTradingDiagnosticsButton = CreateWindowW(L"BUTTON", L"Start AT Research", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 28, 648, 190, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAutoTradingDiagnostics)), nullptr, nullptr);
            g_autoTradingCaptureButton = CreateWindowW(L"BUTTON", L"Capture AT Snapshot", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 228, 648, 190, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAutoTradingCapture)), nullptr, nullptr);
            g_autoTradingFinishButton = CreateWindowW(L"BUTTON", L"Finish AT Research", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 428, 648, 190, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAutoTradingFinish)), nullptr, nullptr);
            g_reloadSettingsButton = CreateWindowW(L"BUTTON", L"Reload Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 628, 648, 150, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonReloadSettings)), nullptr, nullptr);
            g_testEmailButton = CreateWindowW(L"BUTTON", L"Send Test Email", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 788, 648, 150, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonTestEmail)), nullptr, nullptr);
            g_autoMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 212, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAutoMenu)), nullptr, nullptr);
            g_statusMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 314, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonStatusMenu)), nullptr, nullptr);
            g_emailMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 348, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonEmailMenu)), nullptr, nullptr);
            g_heartbeatMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 382, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonHeartbeatMenu)), nullptr, nullptr);
            for (HWND button : { g_refreshButton, g_reportButton, g_settingsButton, g_openFolderButton, g_autoTradingDiagnosticsButton, g_autoTradingCaptureButton, g_autoTradingFinishButton, g_reloadSettingsButton, g_testEmailButton, g_autoMenuButton, g_statusMenuButton, g_emailMenuButton, g_heartbeatMenuButton })
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            UpdateDeveloperControlVisibility(hwnd);
            SetTimer(hwnd, kRefreshTimer, static_cast<UINT>(g_app.config.refreshSeconds * 1000), nullptr);
            SetTimer(hwnd, kClockTimer, 60000, nullptr);
            SetTimer(hwnd, kAnimationTimer, 250, nullptr);
            StartRefresh(hwnd, true);
            return 0;

        case WM_TIMER:
            if (wParam == kRefreshTimer)
                StartRefresh(hwnd);
            else if (wParam == kClockTimer)
                InvalidateRect(hwnd, nullptr, FALSE);
            else if (wParam == kAnimationTimer)
            {
                RECT pulseArea{ 0, 12, 10000, 70 };
                InvalidateRect(hwnd, &pulseArea, FALSE);
            }
            return 0;

        case WM_SIZE:
            LayoutButtons(hwnd);
            return 0;

        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 760;
            info->ptMinTrackSize.y = 680;
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kButtonAutoMenu: ShowRowMenu(hwnd, g_autoMenuButton, kButtonAutoMenu); return 0;
            case kButtonStatusMenu: ShowRowMenu(hwnd, g_statusMenuButton, kButtonStatusMenu); return 0;
            case kButtonEmailMenu: ShowRowMenu(hwnd, g_emailMenuButton, kButtonEmailMenu); return 0;
            case kButtonHeartbeatMenu: ShowRowMenu(hwnd, g_heartbeatMenuButton, kButtonHeartbeatMenu); return 0;
            case kButtonRefresh:
                StartRefresh(hwnd, true);
                return 0;
            case kButtonReport:
            {
                mcst::WatchdogSystemStatus status;
                TrackerStatusSnapshot snapshot;
                {
                    std::lock_guard<std::mutex> lock(g_app.mutex);
                    status = g_app.status;
                    snapshot = g_app.snapshot;
                }
                std::wstring diagnostic;
                const std::wstring report = BuildStatusReport(status, snapshot);
                const bool reportWritten = WriteUtf8TextFile(g_app.config.reportPath, report, diagnostic);
                {
                    std::lock_guard<std::mutex> lock(g_app.mutex);
                    g_app.lastReport = FormatLocalTime(std::chrono::system_clock::now());
                    g_app.status.lastReport = g_app.lastReport;
                    if (!reportWritten)
                        AddActivity(g_app.status, mcst::HealthState::Attention,
                            L"Local status report archive failed: " + diagnostic);
                }

                bool emailQueued = false;
                if (g_app.config.emailEnabled)
                {
                    emailQueued = true;
                    SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config,
                        L"MCST-Watchdog Status Report", BuildStatusReportHtml(report), false,
                        L"Status report email", true, g_app.config.reportEmailTo);
                }

                std::wstring dialogMessage;
                if (reportWritten)
                {
                    dialogMessage = diagnostic;
                }
                else
                {
                    dialogMessage =
                        L"The HTML status report email is independent of the local archive.\r\n\r\n"
                        L"Local archive warning: " + diagnostic;
                }

                if (emailQueued)
                {
                    dialogMessage += L"\r\n\r\nThe status report email has been queued.";
                }
                else
                {
                    dialogMessage += L"\r\n\r\nEmail is not enabled or configured.";
                }

                MessageBoxW(hwnd, dialogMessage.c_str(),
                    reportWritten ? L"Status report" : L"Status report archive warning",
                    reportWritten ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
                return 0;
            }
            case kButtonSettings:
                if (ShowStatusReportSettings(hwnd))
                {
                g_app.config = LoadAppConfig();
                    g_app.schedule.PreserveOnReload(std::chrono::system_clock::now());
                    AddActivity(g_app.status, mcst::HealthState::Healthy, L"Status Report settings saved and reloaded");
                    UpdateDeveloperControlVisibility(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case kButtonOpenFolder:
                ShellExecuteW(hwnd, L"open", GetApplicationDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            case kButtonReloadSettings:
            {
            g_app.config = LoadAppConfig();
                {
                    std::lock_guard<std::mutex> lock(g_app.mutex);
                    for (const auto& normalizationMessage : g_app.config.normalizationMessages)
                        AddActivity(g_app.status, mcst::HealthState::Attention, L"INI normalized: " + normalizationMessage);
                }
                g_app.schedule.PreserveOnReload(std::chrono::system_clock::now());
                g_app.autoTradingAlerts.Reset();
                g_app.brokerMonitor.Reset();
                g_app.logAlertEngine.Reset();
                KillTimer(hwnd, kRefreshTimer);
                SetTimer(hwnd, kRefreshTimer, static_cast<UINT>(g_app.config.refreshSeconds * 1000), nullptr);
                UpdateDeveloperControlVisibility(hwnd);
                StartRefresh(hwnd, true);
                const std::wstring reloadMessage = g_app.config.normalizationMessages.empty()
                    ? L"Settings were reloaded from MCST-Watchdog.ini. No corrections were needed."
                    : L"Settings were reloaded and missing or invalid values were normalized in MCST-Watchdog.ini. Details were written to MCST-Watchdog-ConfigNormalization.log.";
                MessageBoxW(hwnd, reloadMessage.c_str(), L"Settings", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            case kButtonTestEmail:
            {
                EmailSender sender(g_app.config);
                std::wstring reason;
                if (!sender.IsConfigured(&reason, g_app.config.alertEmailTo))
                {
                    MessageBoxW(hwnd, reason.c_str(), L"Email configuration", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, L"MCST-Watchdog Test Email", L"MCST-Watchdog email configuration is working.\r\n\r\nVersion: 1.091", false, L"Test email", false, g_app.config.alertEmailTo);
                MessageBoxW(hwnd, L"Test email is being sent.", L"Email", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            case kButtonAutoTradingDiagnostics:
            {
                CreateDirectoryW(L"C:\\Temp", nullptr);
                const std::wstring path = L"C:\Temp\MCST-Watchdog\AutoTradingResearch.txt";
                CreateDirectoryW(L"C:\\Temp\\MCST-Watchdog", nullptr);
                std::wstring diagnostic;
                SetWindowTextW(g_autoTradingDiagnosticsButton, L"Starting...");
                EnableWindow(g_autoTradingDiagnosticsButton, FALSE);
                const bool ok = StartAutoTradingResearchSession(path, diagnostic);
                EnableWindow(g_autoTradingDiagnosticsButton, TRUE);
                SetWindowTextW(g_autoTradingDiagnosticsButton, L"Start AT Research");
                MessageBoxW(hwnd, diagnostic.c_str(), ok ? L"AutoTrading research" : L"AutoTrading research error",
                    ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
                if (ok) ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            case kButtonAutoTradingCapture:
            {
                const std::wstring path = L"C:\Temp\MCST-Watchdog\AutoTradingResearch.txt";
                std::wstring diagnostic;
                SetWindowTextW(g_autoTradingCaptureButton, L"Capturing...");
                EnableWindow(g_autoTradingCaptureButton, FALSE);
                const bool ok = CaptureAutoTradingResearchSnapshot(path, diagnostic);
                EnableWindow(g_autoTradingCaptureButton, TRUE);
                SetWindowTextW(g_autoTradingCaptureButton, L"Capture AT Snapshot");
                MessageBoxW(hwnd, diagnostic.c_str(), ok ? L"AutoTrading research" : L"AutoTrading research error",
                    ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
                return 0;
            }
            case kButtonAutoTradingFinish:
            {
                const std::wstring path = L"C:\Temp\MCST-Watchdog\AutoTradingResearch.txt";
                std::wstring diagnostic;
                SetWindowTextW(g_autoTradingFinishButton, L"Analyzing...");
                EnableWindow(g_autoTradingFinishButton, FALSE);
                const bool ok = FinishAutoTradingResearchSession(path, diagnostic);
                EnableWindow(g_autoTradingFinishButton, TRUE);
                SetWindowTextW(g_autoTradingFinishButton, L"Finish AT Research");
                MessageBoxW(hwnd, diagnostic.c_str(), ok ? L"AutoTrading research summary" : L"AutoTrading research error",
                    ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
                if (ok) ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            }
            break;

        case WM_APP_REFRESH_COMPLETE:
        {
            std::unique_ptr<RefreshResult> result(reinterpret_cast<RefreshResult*>(lParam));
            if (result)
            {
                std::lock_guard<std::mutex> lock(g_app.mutex);
                const auto previousActivity = g_app.status.activity;
                const int active = result->status.autoTradingActive;

                g_app.status = std::move(result->status);
                g_app.snapshot = std::move(result->snapshot);
                g_app.status.lastReport = g_app.lastReport;
                g_app.status.lastAlert = g_app.lastAlert;

                const auto monitorNow = std::chrono::system_clock::now();
                const BrokerMonitorDecision brokerDecision = g_app.brokerMonitor.Evaluate(
                    g_app.snapshot.recentLogs, g_app.config, monitorNow);
                g_app.status.broker = brokerDecision.status;
                g_app.status.overall = Worst(g_app.status.overall, g_app.status.broker.state);
                if (brokerDecision.stateChanged && !brokerDecision.eventText.empty())
                {
                    AddActivity(g_app.status, brokerDecision.status.state, brokerDecision.eventText);
                    if (brokerDecision.sendAlertEmail || brokerDecision.sendRecoveryEmail)
                    {
                        g_app.lastAlert = FormatLocalTime(monitorNow) + L" - " + brokerDecision.eventText;
                        g_app.status.lastAlert = g_app.lastAlert;
                        const std::wstring body = brokerDecision.eventText + L"\r\n\r\n"
                            + BuildStatusReport(g_app.status, g_app.snapshot);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, brokerDecision.subject, BuildStatusReportHtml(body), true, brokerDecision.eventText, true, g_app.config.alertEmailTo);
                    }
                }

                const LogAlertEngine::Decision logAlertDecision = g_app.logAlertEngine.Evaluate(
                    g_app.snapshot.recentLogs, g_app.config, monitorNow);
                if (logAlertDecision.eventCount > 0)
                {
                    g_app.status.recentLogs.state = Worst(g_app.status.recentLogs.state, logAlertDecision.state);
                    g_app.status.recentLogs.detail = std::to_wstring(logAlertDecision.eventCount) + L" new alert match(es)";
                    g_app.lastAlert = FormatLocalTime(monitorNow) + L" - " + logAlertDecision.eventText;
                    g_app.status.lastAlert = g_app.lastAlert;
                    AddActivity(g_app.status, logAlertDecision.state, logAlertDecision.eventText);

                    if (logAlertDecision.sendEmail)
                    {
                        const std::wstring body = logAlertDecision.body + L"\r\n"
                            + BuildStatusReport(g_app.status, g_app.snapshot);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config,
                            logAlertDecision.subject, BuildStatusReportHtml(body), true,
                            L"Log alert email", true, g_app.config.alertEmailTo);
                    }
                }

                const AutoTradingAlertDecision alertDecision = g_app.autoTradingAlerts.Evaluate(active, g_app.config);
                if (alertDecision.stateChanged && !alertDecision.eventText.empty())
                {
                    g_app.lastAlert = FormatLocalTime(std::chrono::system_clock::now()) + L" - " + alertDecision.eventText;
                    g_app.status.lastAlert = g_app.lastAlert;
                    AddActivity(g_app.status, alertDecision.belowMinimum ? mcst::HealthState::Critical : mcst::HealthState::Healthy,
                        alertDecision.eventText);
                    if (alertDecision.sendEmail)
                    {
                        const std::wstring body = alertDecision.eventText + L"\r\n\r\n"
                            + BuildStatusReport(g_app.status, g_app.snapshot);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, alertDecision.subject, BuildStatusReportHtml(body), true, alertDecision.eventText, true, g_app.config.alertEmailTo);
                    }
                }

                const auto scheduleNow = std::chrono::system_clock::now();
                const ScheduleDecision scheduleDecision = g_app.schedule.Evaluate(g_app.config, scheduleNow);

                if (scheduleDecision.sendStatusReport && !g_app.scheduledStatusReportEmailInFlight)
                {
                    const std::wstring report = BuildStatusReport(g_app.status, g_app.snapshot);
                    std::wstring fileDiagnostic;
                    const bool reportWritten = WriteUtf8TextFile(g_app.config.reportPath, report, fileDiagnostic);
                    g_app.lastReport = FormatLocalTime(scheduleNow);
                    g_app.status.lastReport = g_app.lastReport;

                    if (reportWritten)
                    {
                        AddActivity(g_app.status, mcst::HealthState::Healthy,
                            L"Scheduled status report archived locally");
                    }
                    else
                    {
                        AddActivity(g_app.status, mcst::HealthState::Attention,
                            L"Local scheduled report archive failed: " + fileDiagnostic);
                    }

                    // Local archiving and email delivery are intentionally independent.
                    // A missing folder, locked file or invalid report_path must never suppress
                    // the scheduled HTML status report email.
                    if (g_app.config.emailEnabled)
                    {
                        g_app.scheduledStatusReportEmailInFlight = true;
                        AddActivity(g_app.status, mcst::HealthState::Healthy,
                            L"Status Report queued to " + g_app.config.reportEmailTo);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config,
                            L"MCST-Watchdog STATUS REPORT - " + FormatLocalTime(scheduleNow), BuildStatusReportHtml(report),
                            false, L"Scheduled status report", true, g_app.config.reportEmailTo);
                    }
                    else
                    {
                        AddActivity(g_app.status, mcst::HealthState::Attention,
                            L"Scheduled status report email was not queued because email is disabled or not configured");
                    }
                }

                if (scheduleDecision.sendHeartbeat && !g_app.heartbeatEmailInFlight)
                {
                    const std::wstring heartbeatBody = L"MCST-Watchdog heartbeat. Monitoring is active.\r\n\r\n"
                        + BuildStatusReport(g_app.status, g_app.snapshot);
                    AddActivity(g_app.status, mcst::HealthState::Healthy, L"Heartbeat generated");
                    if (g_app.config.emailEnabled)
                    {
                        g_app.heartbeatEmailInFlight = true;
                        AddActivity(g_app.status, mcst::HealthState::Healthy,
                            L"Heartbeat queued to " + g_app.config.reportEmailTo);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, L"MCST-Watchdog HEARTBEAT - " + FormatLocalTime(scheduleNow),
                            BuildStatusReportHtml(heartbeatBody), false, L"Heartbeat email", true, g_app.config.reportEmailTo);
                    }
                }

                for (const auto& old : previousActivity)
                {
                    if (g_app.status.activity.size() >= 10) break;
                    g_app.status.activity.push_back(old);
                }
                g_app.refreshRunning = false;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_EMAIL_COMPLETE:
        {
            std::unique_ptr<EmailSendResult> result(reinterpret_cast<EmailSendResult*>(lParam));
            if (result)
            {
                std::lock_guard<std::mutex> lock(g_app.mutex);
                if (result->eventText == L"Scheduled status report")
                    g_app.scheduledStatusReportEmailInFlight = false;
                else if (result->eventText == L"Heartbeat email")
                    g_app.heartbeatEmailInFlight = false;

                g_app.status.email = result->ok
                    ? mcst::MonitorStatus{ mcst::HealthState::Healthy, L"Ready", L"Last send succeeded" }
                    : mcst::MonitorStatus{ mcst::HealthState::Critical, L"Send failed", result->message };
                AddActivity(g_app.status, result->ok ? mcst::HealthState::Healthy : mcst::HealthState::Critical,
                    result->eventText + (result->ok ? L" sent" : L" failed: " + result->message));
                if (!result->ok) g_app.status.lastError = result->message;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            HDC memoryDc = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
            PaintDashboard(hwnd, memoryDc);
            BitBlt(dc, 0, 0, client.right, client.bottom, memoryDc, 0, 0, SRCCOPY);
            SelectObject(memoryDc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(memoryDc);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_DESTROY:
        {
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            if (GetWindowPlacement(hwnd, &placement))
                SaveWindowPlacementToConfig(placement);
            KillTimer(hwnd, kRefreshTimer);
            KillTimer(hwnd, kClockTimer);
            KillTimer(hwnd, kAnimationTimer);
            PostQuitMessage(0);
            return 0;
        }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (!singleInstanceMutex)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"MCST-Watchdog is already running.", L"MCST-Watchdog", MB_OK | MB_ICONINFORMATION);
        CloseHandle(singleInstanceMutex);
        return 0;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok)
    {
        CloseHandle(singleInstanceMutex);
        return 1;
    }

    g_app.config = LoadAppConfig();
    g_app.status.overall = mcst::HealthState::Unknown;
    g_app.status.bridge = { mcst::HealthState::Unknown, L"Starting", L"" };
    g_app.status.trackerSnapshot = { mcst::HealthState::Unknown, L"Waiting", L"" };
    g_app.status.autoTrading = { mcst::HealthState::Unknown, L"Waiting", L"" };
    g_app.status.broker = { mcst::HealthState::Unknown, L"Waiting", L"" };
    g_app.status.recentLogs = { mcst::HealthState::Unknown, L"Waiting", L"" };
    g_app.status.statusReports = { mcst::HealthState::Unknown, L"Waiting", L"" };
    g_app.status.email = { mcst::HealthState::Unknown, L"Waiting", L"" };
    g_app.status.heartbeat = { mcst::HealthState::Unknown, L"Waiting", L"" };
    g_app.status.lastSnapshot = L"Never";
    g_app.status.lastAutoTradingRead = L"Never";
    g_app.status.lastReport = L"Never";
    g_app.status.lastAlert = L"None";
    for (const auto& normalizationMessage : g_app.config.normalizationMessages)
        AddActivity(g_app.status, mcst::HealthState::Attention, L"INI normalized: " + normalizationMessage);
    CreateFonts();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass))
        return 1;

    const RECT initialRect = ResolveInitialWindowRect(g_app.config);
    const int initialX = initialRect.left == CW_USEDEFAULT ? CW_USEDEFAULT : initialRect.left;
    const int initialY = initialRect.top == CW_USEDEFAULT ? CW_USEDEFAULT : initialRect.top;
    const int initialWidth = initialRect.left == CW_USEDEFAULT ? g_app.config.windowWidth : initialRect.right - initialRect.left;
    const int initialHeight = initialRect.top == CW_USEDEFAULT ? g_app.config.windowHeight : initialRect.bottom - initialRect.top;

    HWND window = CreateWindowExW(
        0, kWindowClass, L"MCST-Watchdog 1.091 - UI Polish",
        WS_OVERLAPPEDWINDOW,
        initialX, initialY, initialWidth, initialHeight,
        nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;

    ShowWindow(window, g_app.config.windowMaximized ? SW_SHOWMAXIMIZED : showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DeleteObject(g_titleFont);
    DeleteObject(g_headerFont);
    DeleteObject(g_bodyFont);
    DeleteObject(g_statusFont);
    DeleteObject(g_monoFont);
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CloseHandle(singleInstanceMutex);
    return static_cast<int>(message.wParam);
}
