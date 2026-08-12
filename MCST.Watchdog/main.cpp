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
#include "CompatibilityManager.h"
#include "StatusReport.h"
#include "Email.h"
#include "AlertService.h"
#include "ScheduleService.h"
#include "BrokerMonitor.h"
#include "BrokerAuthDetector.h"
#include "LogAlertEngine.h"
#include "DashboardLayout.h"
#include "MultiChartsVersionDetector.h"
#include "../MCST.Shared/WatchdogSystemStatus.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <ctime>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
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
    constexpr int kButtonOpenSettings = 1014;
    constexpr int kButtonTrackerResearch = 1015;
    constexpr int kButtonOpenCompatibility = 1016;
    constexpr int kButtonReloadCompatibility = 1017;
    constexpr int kButtonPositionCurrencyResearch = 1018;
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
        std::uint64_t configRevision = 0;
        mcst::WatchdogSystemStatus status;
        TrackerStatusSnapshot snapshot;
        BrokerAuthenticationDetection brokerAuthentication;
        MultiChartsVersionInfo multiChartsVersionInfo;
        std::wstring diagnostic;
    };

    struct AppState
    {
        AppConfig config;
        mcst::WatchdogSystemStatus status;
        TrackerStatusSnapshot snapshot;
        std::mutex mutex;
        std::uint64_t configRevision = 1;
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
    HFONT g_labelFont = nullptr;
    HFONT g_statusFont = nullptr;
    HFONT g_monoFont = nullptr;
    HFONT g_developerButtonFont = nullptr;
    HWND g_refreshButton = nullptr;
    HWND g_reportButton = nullptr;
    HWND g_settingsButton = nullptr;
    HWND g_openFolderButton = nullptr;
    HWND g_openSettingsButton = nullptr;
    HWND g_autoTradingDiagnosticsButton = nullptr;
    HWND g_autoTradingCaptureButton = nullptr;
    HWND g_autoTradingFinishButton = nullptr;
    HWND g_trackerResearchButton = nullptr;
    HWND g_positionCurrencyResearchButton = nullptr;
    HWND g_openCompatibilityButton = nullptr;
    HWND g_reloadCompatibilityButton = nullptr;
    HWND g_reloadSettingsButton = nullptr;
    HWND g_testEmailButton = nullptr;
    HWND g_autoMenuButton = nullptr;
    HWND g_statusMenuButton = nullptr;
    HWND g_emailMenuButton = nullptr;
    HWND g_heartbeatMenuButton = nullptr;
    ULONG_PTR g_gdiplusToken = 0;


    std::wstring StartupLogPathSafe() noexcept
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return L"MCST-Watchdog-Startup.log";
        std::wstring path(modulePath, length);
        const auto slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            path.resize(slash + 1);
        else
            path.clear();
        path += L"MCST-Watchdog-Startup.log";
        return path;
    }

    void AppendUtf8LineToFileSafe(const std::wstring& path, const std::wstring& line) noexcept
    {
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;

        const int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
            nullptr, 0, nullptr, nullptr);
        if (bytesNeeded > 0)
        {
            std::string utf8(static_cast<size_t>(bytesNeeded), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                utf8.data(), bytesNeeded, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
        CloseHandle(file);
    }

    void AppendStartupLogSafe(const std::wstring& text) noexcept
    {
        try
        {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t prefix[64]{};
            swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u  ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            const std::wstring line = std::wstring(prefix) + text + L"\r\n";

            // Primary log: next to the executable.
            AppendUtf8LineToFileSafe(StartupLogPathSafe(), line);

            // Diagnostic fallback: intentionally independent of the executable directory.
            CreateDirectoryW(L"C:\\Temp", nullptr);
            AppendUtf8LineToFileSafe(L"C:\\Temp\\MCST-Watchdog-Startup.log", line);

            OutputDebugStringW((L"MCST STARTUP: " + text + L"\r\n").c_str());
        }
        catch (...) {}
    }

    LONG WINAPI StartupUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) noexcept
    {
        DWORD code = exceptionInfo && exceptionInfo->ExceptionRecord
            ? exceptionInfo->ExceptionRecord->ExceptionCode : 0;
        AppendStartupLogSafe(L"FATAL SEH exception before normal error handling. Code=0x" +
            [] (DWORD value) { wchar_t b[16]{}; swprintf_s(b, L"%08X", value); return std::wstring(b); }(code));
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void ShowStartupFailureSafe(const std::wstring& detail) noexcept
    {
        AppendStartupLogSafe(L"FATAL: " + detail);
        const std::wstring message =
            L"MCST-Watchdog could not start.\r\n\r\n" + detail +
            L"\r\n\r\nSee MCST-Watchdog-Startup.log next to the EXE for details.";
        MessageBoxW(nullptr, message.c_str(), L"MCST-Watchdog startup error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }

    std::wstring BrokerStateCachePath()
    {
        return GetApplicationDirectory() + L"\\MCST-Watchdog-BrokerState.cache";
    }

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
        if (g_openSettingsButton) MoveWindow(g_openSettingsButton, 408, y, 130, 34, TRUE);
        if (g_reloadSettingsButton) MoveWindow(g_reloadSettingsButton, 548, y, 150, 34, TRUE);
        const DeveloperToolbarLayout developerLayout = CalculateDeveloperToolbarLayout(y);
        if (g_app.config.developerModeEnabled)
        {
            const RECT startRect = CalculateDeveloperToolbarButtonRect(developerLayout, 0);
            const RECT captureRect = CalculateDeveloperToolbarButtonRect(developerLayout, 1);
            const RECT finishRect = CalculateDeveloperToolbarButtonRect(developerLayout, 2);
            const RECT trackerRect = CalculateDeveloperToolbarButtonRect(developerLayout, 3);
            const RECT positionCurrencyRect = CalculateDeveloperToolbarButtonRect(developerLayout, 4);
            const RECT openCompatRect = CalculateDeveloperToolbarButtonRect(developerLayout, 5);
            const RECT reloadCompatRect = CalculateDeveloperToolbarButtonRect(developerLayout, 6);
            if (g_autoTradingDiagnosticsButton) MoveWindow(g_autoTradingDiagnosticsButton, startRect.left, startRect.top, startRect.right - startRect.left, startRect.bottom - startRect.top, TRUE);
            if (g_autoTradingCaptureButton) MoveWindow(g_autoTradingCaptureButton, captureRect.left, captureRect.top, captureRect.right - captureRect.left, captureRect.bottom - captureRect.top, TRUE);
            if (g_autoTradingFinishButton) MoveWindow(g_autoTradingFinishButton, finishRect.left, finishRect.top, finishRect.right - finishRect.left, finishRect.bottom - finishRect.top, TRUE);
            if (g_trackerResearchButton) MoveWindow(g_trackerResearchButton, trackerRect.left, trackerRect.top, trackerRect.right - trackerRect.left, trackerRect.bottom - trackerRect.top, TRUE);
            if (g_positionCurrencyResearchButton) MoveWindow(g_positionCurrencyResearchButton, positionCurrencyRect.left, positionCurrencyRect.top, positionCurrencyRect.right - positionCurrencyRect.left, positionCurrencyRect.bottom - positionCurrencyRect.top, TRUE);
            if (g_openCompatibilityButton) MoveWindow(g_openCompatibilityButton, openCompatRect.left, openCompatRect.top, openCompatRect.right - openCompatRect.left, openCompatRect.bottom - openCompatRect.top, TRUE);
            if (g_reloadCompatibilityButton) MoveWindow(g_reloadCompatibilityButton, reloadCompatRect.left, reloadCompatRect.top, reloadCompatRect.right - reloadCompatRect.left, reloadCompatRect.bottom - reloadCompatRect.top, TRUE);
        }
        else
        {
            // Reload Settings remains on the normal bottom row next to Open Settings.
        }
        const DashboardRowLayout rowLayout = CalculateDashboardRowLayout(static_cast<int>(client.right));
        const int menuX = rowLayout.overflowButtonX;
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
        for (HWND control : { g_autoTradingDiagnosticsButton, g_autoTradingCaptureButton, g_autoTradingFinishButton,
                              g_trackerResearchButton, g_positionCurrencyResearchButton, g_openCompatibilityButton, g_reloadCompatibilityButton })
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

    unsigned long long FileTimeToUInt64(const FILETIME& value)
    {
        ULARGE_INTEGER converted{};
        converted.LowPart = value.dwLowDateTime;
        converted.HighPart = value.dwHighDateTime;
        return converted.QuadPart;
    }

    bool QuerySystemCpuPercent(double& percent)
    {
        static bool initialized = false;
        static unsigned long long previousIdle = 0;
        static unsigned long long previousKernel = 0;
        static unsigned long long previousUser = 0;

        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user))
            return false;

        unsigned long long currentIdle = FileTimeToUInt64(idle);
        unsigned long long currentKernel = FileTimeToUInt64(kernel);
        unsigned long long currentUser = FileTimeToUInt64(user);

        if (!initialized)
        {
            previousIdle = currentIdle;
            previousKernel = currentKernel;
            previousUser = currentUser;
            initialized = true;
            Sleep(120);
            if (!GetSystemTimes(&idle, &kernel, &user))
                return false;
            currentIdle = FileTimeToUInt64(idle);
            currentKernel = FileTimeToUInt64(kernel);
            currentUser = FileTimeToUInt64(user);
        }

        const unsigned long long idleDelta = currentIdle - previousIdle;
        const unsigned long long kernelDelta = currentKernel - previousKernel;
        const unsigned long long userDelta = currentUser - previousUser;
        const unsigned long long total = kernelDelta + userDelta;

        previousIdle = currentIdle;
        previousKernel = currentKernel;
        previousUser = currentUser;

        if (total == 0)
            return false;

        percent = 100.0 * static_cast<double>(total - idleDelta) / static_cast<double>(total);
        percent = (std::max)(0.0, (std::min)(100.0, percent));
        return true;
    }

    void ReadProcessResources(mcst::WatchdogSystemStatus& status)
    {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
            status.privateMemoryBytes = static_cast<std::size_t>(memory.PrivateUsage);
        GetProcessHandleCount(GetCurrentProcess(), &status.handleCount);
        status.uptime = FormatUptime(std::chrono::steady_clock::now() - g_app.started);

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        status.logicalProcessorCount = systemInfo.dwNumberOfProcessors;
        status.cpuAvailable = QuerySystemCpuPercent(status.cpuPercent);

        MEMORYSTATUSEX systemMemory{};
        systemMemory.dwLength = sizeof(systemMemory);
        if (GlobalMemoryStatusEx(&systemMemory))
        {
            status.systemMemoryAvailable = true;
            status.memoryLoadPercent = systemMemory.dwMemoryLoad;
            status.totalPhysicalMemoryBytes = systemMemory.ullTotalPhys;
            status.availablePhysicalMemoryBytes = systemMemory.ullAvailPhys;
        }

        wchar_t windowsDirectory[MAX_PATH]{};
        std::wstring root = L"C:\\";
        if (GetWindowsDirectoryW(windowsDirectory, MAX_PATH) > 0 && windowsDirectory[1] == L':')
            root[0] = windowsDirectory[0];
        status.systemDiskRoot = root;

        ULARGE_INTEGER freeBytesAvailable{}, totalBytes{}, totalFreeBytes{};
        if (GetDiskFreeSpaceExW(root.c_str(), &freeBytesAvailable, &totalBytes, &totalFreeBytes) && totalBytes.QuadPart > 0)
        {
            status.systemDiskAvailable = true;
            status.diskTotalBytes = totalBytes.QuadPart;
            status.diskFreeBytes = totalFreeBytes.QuadPart;
            const unsigned long long usedBytes = totalBytes.QuadPart - totalFreeBytes.QuadPart;
            status.diskUsedPercent = 100.0 * static_cast<double>(usedBytes) / static_cast<double>(totalBytes.QuadPart);
        }
    }

    RefreshResult CollectStatus(const AppConfig& config, std::uint64_t configRevision, bool forceAutoTradingRefresh)
    {
        RefreshResult result;
        result.configRevision = configRevision;
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            result.status.activity = g_app.status.activity;
            result.status.lastReport = g_app.lastReport;
            result.status.lastAlert = g_app.lastAlert;
        }
        result.status.lastSnapshot = L"Never";
        result.status.autoTradingMinimum = config.autoTradingMinimum;

        std::wstring diagnostic;
        bool readOk = false;
        for (int attempt = 1; attempt <= config.snapshotRetryCount; ++attempt)
        {
            diagnostic.clear();
            if (ReadTrackerStatusSnapshot(result.snapshot, diagnostic, static_cast<unsigned long>(config.bridgeTimeoutMilliseconds)))
            {
                readOk = true;
                break;
            }
            if (attempt < config.snapshotRetryCount)
                Sleep(static_cast<DWORD>(config.snapshotRetryDelayMilliseconds));
        }

        const auto now = std::chrono::system_clock::now();
        if (readOk)
        {
            result.status.bridge = { mcst::HealthState::Healthy, L"Connected",
                L"MCST Tracker Bridge 1.0 - internal V" + std::to_wstring(result.snapshot.bridgeVersion) + L" - Protocol V" + std::to_wstring(result.snapshot.protocolVersion) };
            const bool trackerAvailable = result.snapshot.trackerFound && result.snapshot.trackerSameProcess;
            const int readableTrackerSections =
                static_cast<int>(result.snapshot.accounts.ok) +
                static_cast<int>(result.snapshot.openPositions.ok) +
                static_cast<int>(result.snapshot.recentLogs.ok);
            const bool trackerOk = trackerAvailable && readableTrackerSections == 3;
            const bool trackerPartial = trackerAvailable && readableTrackerSections > 0 && readableTrackerSections < 3;
            std::wstring trackerDetail = result.snapshot.trackerCompatibilityDiagnostic;
            if (trackerDetail.empty()) trackerDetail = diagnostic;
            if (!result.snapshot.trackerCompatibilityProfile.empty())
                trackerDetail += L" - Profile: " + result.snapshot.trackerCompatibilityProfile;
            result.status.trackerCompatibilityProfile = result.snapshot.trackerCompatibilityProfile;
            result.status.trackerSnapshot = {
                trackerOk ? mcst::HealthState::Healthy : (trackerPartial ? mcst::HealthState::Attention : mcst::HealthState::Critical),
                trackerOk ? L"Snapshot OK" : (trackerPartial ? L"Partial snapshot" : (trackerAvailable ? L"Read failed" : L"Tracker not available")),
                trackerDetail
            };
            result.status.accountRows = result.snapshot.accounts.rows.size();
            result.status.openPositionRows = result.snapshot.openPositions.rows.size();
            result.status.recentLogRows = result.snapshot.recentLogs.rows.size();
            result.status.processId = result.snapshot.processId;
            result.multiChartsVersionInfo = DetectMultiChartsVersion(result.snapshot.processId);
            if (result.multiChartsVersionInfo.detected)
            {
                result.status.multiChartsVersion = result.multiChartsVersionInfo.displayVersion;
                result.status.multiChartsFileVersion = result.multiChartsVersionInfo.fileVersion;
                result.status.multiChartsExecutable = result.multiChartsVersionInfo.executableName;
            }
            result.status.lastSuccessfulUpdate = now;
            result.status.lastSnapshot = FormatLocalTime(now);

            if (result.snapshot.recentLogs.ok && !result.snapshot.recentLogs.rows.empty())
                result.status.recentLogs = { mcst::HealthState::Healthy, std::to_wstring(result.snapshot.recentLogs.rows.size()) + L" rows", L"Updating" };
            else if (result.snapshot.recentLogs.ok)
                result.status.recentLogs = { mcst::HealthState::Attention, L"0 rows", L"Unusual but readable" };
            else
                result.status.recentLogs = { mcst::HealthState::Critical, L"Read failed", result.snapshot.recentLogs.diagnostic };

            std::wstring rawDiagnostic;
            WriteTrackerStatusRawPayload(result.snapshot, config.rawSnapshotPath, rawDiagnostic);
            if (trackerOk)
                AddActivity(result.status, mcst::HealthState::Healthy, L"Tracker snapshot read successfully");
            else if (trackerPartial)
                AddActivity(result.status, mcst::HealthState::Attention, L"Tracker snapshot partially readable");
            else
                AddActivity(result.status, mcst::HealthState::Critical, L"Tracker snapshot reader is unavailable for the detected build");
        }
        else
        {
            result.status.bridge = { mcst::HealthState::Critical, L"Disconnected", diagnostic };
            result.status.trackerSnapshot = { mcst::HealthState::Critical, L"Unavailable", L"Snapshot read failed" };
            result.status.recentLogs = { mcst::HealthState::Unknown, L"Unknown", L"No snapshot" };
            result.status.lastError = diagnostic;
            AddActivity(result.status, mcst::HealthState::Critical, L"Tracker snapshot failed");
        }

        if (config.autoTradingMonitoringEnabled)
        {
            const AutoTradingReadResult autoTrading = ReadAutoTradingStatus(config.autoTradingCheckMinutes, forceAutoTradingRefresh);
            if (autoTrading.succeeded)
            {
                result.status.lastAutoTradingRead = FormatLocalTime(autoTrading.lastSuccessfulRead);
                result.status.autoTradingActive = autoTrading.activeStrategies;
                const bool belowMinimum = autoTrading.activeStrategies < config.autoTradingMinimum;
                result.status.multiChartsCompatibilityProfile = autoTrading.compatibilityProfile;
                result.status.autoTrading = {
                    belowMinimum ? mcst::HealthState::Critical : mcst::HealthState::Healthy,
                    std::to_wstring(autoTrading.activeStrategies) + L" Active",
                    L"Minimum required " + std::to_wstring(config.autoTradingMinimum) +
                        L" - Objects found " + std::to_wstring(autoTrading.strategyObjectsFound) +
                        (result.status.multiChartsVersion.empty() ? L"" : L" - MC " + result.status.multiChartsVersion) +
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
                    L"Minimum required " + std::to_wstring(config.autoTradingMinimum) + L" - " + autoTrading.diagnostic
                };
                if (!autoTrading.fromCache)
                    AddActivity(result.status, mcst::HealthState::Attention, L"AutoTrading read unavailable");
            }
        }
        else
        {
            result.status.autoTrading = { mcst::HealthState::Unknown, L"Disabled", L"" };
        }

        if (readOk && result.multiChartsVersionInfo.detected)
            WriteDetectedMultiChartsInfoToIni(
                result.multiChartsVersionInfo,
                result.status.multiChartsCompatibilityProfile,
                result.status.trackerCompatibilityProfile);

        result.status.broker = { mcst::HealthState::Unknown, L"Waiting", L"Broker events are evaluated from Recent Logs" };
        result.status.statusReports = {
            config.statusReportsEnabled ? mcst::HealthState::Healthy : mcst::HealthState::Unknown,
            config.statusReportsEnabled ? L"Scheduled" : L"Disabled",
            config.statusReportsEnabled
                ? (L"Every " + std::to_wstring(config.statusReportIntervalMinutes) + L" min to " + config.reportEmailTo)
                : L"Manual report remains available"
        };
        {
            EmailSender sender(config);
            std::wstring emailReason;
            const bool configured = sender.IsConfigured(&emailReason, config.alertEmailTo);
            const bool explicitlyDisabled = config.emailEnabledSettingPresent && !config.emailEnabled;
            result.status.email = {
                configured ? mcst::HealthState::Healthy : (explicitlyDisabled ? mcst::HealthState::Unknown : mcst::HealthState::Attention),
                configured ? L"Ready" : (explicitlyDisabled ? L"Disabled" : L"Not configured"),
                configured ? config.alertEmailTo : emailReason
            };
        }
        {
            EmailSender sender(config);
            std::wstring emailReason;
            const bool emailConfigured = sender.IsConfigured(&emailReason, config.reportEmailTo);
            const bool explicitlyDisabled = config.heartbeatEnabledSettingPresent && !config.heartbeatEnabled;
            const bool operational = config.heartbeatEnabled && emailConfigured;
            result.status.heartbeat = {
                operational ? mcst::HealthState::Healthy
                            : (explicitlyDisabled ? mcst::HealthState::Unknown : mcst::HealthState::Attention),
                operational ? L"Running"
                            : (explicitlyDisabled ? L"Disabled" : L"Not configured"),
                operational
                    ? (L"Email every " + std::to_wstring(config.heartbeatIntervalMinutes) + L" min to " + config.reportEmailTo)
                    : (explicitlyDisabled ? L"Explicitly disabled in INI" : emailReason)
            };
        }

        result.status.overall = mcst::HealthState::Healthy;
        result.status.overall = Worst(result.status.overall, result.status.bridge.state);
        result.status.overall = Worst(result.status.overall, result.status.trackerSnapshot.state);
        result.status.overall = Worst(result.status.overall, result.status.recentLogs.state);
        result.status.overall = Worst(result.status.overall, result.status.autoTrading.state);
        if (config.statusReportsEnabled)
            result.status.overall = Worst(result.status.overall, result.status.statusReports.state);
        if (!(config.emailEnabledSettingPresent && !config.emailEnabled))
            result.status.overall = Worst(result.status.overall, result.status.email.state);
        if (config.heartbeatEnabled)
            result.status.overall = Worst(result.status.overall, result.status.heartbeat.state);

        // Browser authentication is sampled on the refresh worker thread. The
        // resulting signal is applied by BrokerMonitor on the UI thread, where
        // it overrides weaker historical Recent Logs when a broker login page is visible.
        result.brokerAuthentication = DetectBrokerAuthentication(config);

        ReadProcessResources(result.status);
        result.diagnostic = diagnostic;
        return result;
    }

    void StartRefresh(HWND hwnd, bool forceAutoTradingRefresh = false)
    {
        AppConfig configSnapshot;
        std::uint64_t configRevision = 0;
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            if (g_app.refreshRunning)
                return;
            g_app.refreshRunning = true;
            configSnapshot = g_app.config;
            configRevision = g_app.configRevision;
        }

        std::thread([hwnd, forceAutoTradingRefresh, configSnapshot = std::move(configSnapshot), configRevision]() {
            auto result = std::make_unique<RefreshResult>(
                CollectStatus(configSnapshot, configRevision, forceAutoTradingRefresh));
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
        return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, 22, y + 4, 160, 22, parent, nullptr, nullptr, nullptr);
    }

    HWND AddEdit(HWND parent, const std::wstring& text, int id, int y, int width = 310, DWORD extra = 0)
    {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra,
            188, y, width, 26, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
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
                AddLabel(hwnd, L"Password / App Password", y); state->fields[3] = AddEdit(hwnd, L"", kSimpleField4, y, 310, ES_PASSWORD); y += 36;
                AddLabel(hwnd, L"From address", y); state->fields[4] = AddEdit(hwnd, c.emailFrom, kSimpleField5, y); y += 36;
                AddLabel(hwnd, L"Alert recipient", y); state->fields[5] = AddEdit(hwnd, c.alertEmailTo, kSimpleField6, y); y += 36;
                AddLabel(hwnd, L"Report recipient", y); state->fields[6] = AddEdit(hwnd, c.reportEmailTo, kSimpleField7, y); y += 38;
                state->checks[0] = CreateWindowW(L"BUTTON", L"Use SSL/TLS", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 22, y, 160, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleCheck1)), nullptr, nullptr);
                SendMessageW(state->checks[0], BM_SETCHECK, c.smtpUseSsl ? BST_CHECKED : BST_UNCHECKED, 0); y += 36;
                CreateWindowW(L"STATIC", L"Use the SMTP credential required by your provider; Gmail normally requires a Google App Password.", WS_CHILD | WS_VISIBLE, 22, y, 480, 34, hwnd, nullptr, nullptr, nullptr); y += 34;
                CreateWindowW(L"STATIC", L"Leave Password / App Password empty to keep the current stored value.", WS_CHILD | WS_VISIBLE, 22, y, 480, 22, hwnd, nullptr, nullptr, nullptr); y += 34;
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
        const int height = kind == SettingsPanelKind::Email ? 540 : 320;
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
        AppConfig reloadedConfig = LoadAppConfig();
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            g_app.config = std::move(reloadedConfig);
            ++g_app.configRevision;
            AddActivity(g_app.status, mcst::HealthState::Healthy, activity);
        }
        g_app.schedule.PreserveOnReload(std::chrono::system_clock::now());
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
            ShellExecuteW(hwnd, L"open", L"C:\\Temp\\MCST-Watchdog\\AutoTradingResearch.txt", nullptr, nullptr, SW_SHOWNORMAL);
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
        const DashboardRowLayout layout = CalculateDashboardRowLayout(width);
        DrawModernIndicator(dc, layout.indicatorX, y + 7, 17, item.state);

        DrawTextSimple(dc, { layout.labelLeft, y, layout.labelRight, y + 32 }, label, g_labelFont, RGB(28, 31, 36), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { layout.stateLeft, y, layout.stateRight, y + 32 }, mcst::HealthStateText(item.state), g_statusFont, StateColor(item.state), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { layout.descriptionLeft, y, layout.descriptionRight, y + 32 },
            item.value + (item.detail.empty() ? L"" : L"  -  " + item.detail),
            g_bodyFont, RGB(45, 49, 56),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
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

        DrawTextSimple(dc, { 28, 20, client.right - 28, 64 }, L"MCST-Watchdog 1.114-R2", g_titleFont, RGB(25, 28, 34), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

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

        const std::wstring mcSuffix = status.multiChartsVersion.empty() ? L"" : L"    MC " + status.multiChartsVersion;
        const std::wstring updateText = status.lastSuccessfulUpdate.time_since_epoch().count() == 0
            ? L"Last successful system update: waiting for first successful update"
            : L"Last successful system update: " + FormatClock(status.lastSuccessfulUpdate) + L"  (" + FormatAge(status.lastSuccessfulUpdate) + L")" + mcSuffix;
        DrawTextSimple(dc, { 28, 80, client.right - 250, 106 }, updateText, g_bodyFont, RGB(68, 73, 82), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextSimple(dc, { client.right - 245, 80, client.right - 28, 106 }, L"Uptime  " + status.uptime, g_bodyFont, RGB(68, 73, 82), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        DrawTextSimple(dc, { 28, 108, client.right - 28, 138 }, L"SYSTEM STATUS", g_headerFont, RGB(43, 47, 54), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int y = 144;
        DrawStatusRow(dc, y, L"Bridge", status.bridge, client.right); y += 34;
        DrawStatusRow(dc, y, L"Tracker Snapshot", status.trackerSnapshot, client.right); y += 34;
        DrawStatusRow(dc, y, L"AutoTrading", status.autoTrading, client.right); y += 34;
        DrawStatusRow(dc, y, L"Broker", status.broker, client.right); y += 34;
        DrawStatusRow(dc, y, L"Recent Logs", status.recentLogs, client.right); y += 34;
        DrawStatusRow(dc, y, L"Status Reports", status.statusReports, client.right); y += 34;
        DrawStatusRow(dc, y, L"Email", status.email, client.right); y += 34;
        DrawStatusRow(dc, y, L"Heartbeat", status.heartbeat, client.right); y += 46;

        DrawTextSimple(dc, { 28, y, client.right - 28, y + 30 }, L"LATEST ACTIVITY", g_headerFont, RGB(43, 47, 54), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 34;
        const int middle = client.right / 2;
        constexpr int latestLabelLeft = 34;
        constexpr int latestLabelRight = 184;
        constexpr int latestValueLeft = 194;
        DrawTextSimple(dc, { latestLabelLeft, y, latestLabelRight, y + 28 }, L"Last Snapshot", g_bodyFont, RGB(68, 73, 82), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { latestValueLeft, y, middle - 10, y + 28 }, status.lastSnapshot, g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { middle + 10, y, client.right - 28, y + 28 }, L"Accounts  " + std::to_wstring(status.accountRows) + L"    Positions  " + std::to_wstring(status.openPositionRows) + L"    Logs  " + std::to_wstring(status.recentLogRows), g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 32;
        DrawTextSimple(dc, { latestLabelLeft, y, latestLabelRight, y + 28 }, L"Last AutoTrading Read", g_bodyFont, RGB(68, 73, 82), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { latestValueLeft, y, middle - 10, y + 28 }, status.lastAutoTradingRead.empty() ? L"Never" : status.lastAutoTradingRead, g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { middle + 10, y, client.right - 28, y + 28 }, L"Uptime  " + status.uptime + L"    Memory  " + std::to_wstring(status.privateMemoryBytes / (1024 * 1024)) + L" MB    Handles  " + std::to_wstring(status.handleCount), g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += 44;
    }

    HFONT CreateUiFont(int pointSize, int weight, const wchar_t* faceName, DWORD pitchAndFamily = DEFAULT_PITCH)
    {
        HDC screenDc = GetDC(nullptr);
        const int dpiY = screenDc ? GetDeviceCaps(screenDc, LOGPIXELSY) : 96;
        if (screenDc)
            ReleaseDC(nullptr, screenDc);

        // A negative height requests the actual character height instead of the full cell height.
        // This gives more predictable ClearType rendering on Windows 10 and under RDP.
        const int height = -MulDiv(pointSize, dpiY, 72);
        return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, pitchAndFamily, faceName);
    }

    void CreateFonts()
    {
        // Segoe UI is present on Windows 10. Avoid Segoe UI Variable Text here because it is
        // a Windows 11 font and may be substituted by a less suitable fallback on Windows 10.
        g_titleFont = CreateUiFont(16, FW_SEMIBOLD, L"Segoe UI");
        g_headerFont = CreateUiFont(10, FW_SEMIBOLD, L"Segoe UI");
        g_bodyFont = CreateUiFont(10, FW_NORMAL, L"Segoe UI");
        g_labelFont = CreateUiFont(10, FW_SEMIBOLD, L"Segoe UI");
        g_statusFont = CreateUiFont(10, FW_SEMIBOLD, L"Segoe UI");
        g_monoFont = CreateUiFont(10, FW_NORMAL, L"Consolas", FIXED_PITCH | FF_MODERN);
        g_developerButtonFont = CreateUiFont(9, FW_NORMAL, L"Segoe UI");
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
            g_openSettingsButton = CreateWindowW(L"BUTTON", L"Open Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 528, 690, 130, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonOpenSettings)), nullptr, nullptr);
            g_autoTradingDiagnosticsButton = CreateWindowW(L"BUTTON", L"AT Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 28, 648, 140, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAutoTradingDiagnostics)), nullptr, nullptr);
            g_autoTradingCaptureButton = CreateWindowW(L"BUTTON", L"AT Capture", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 176, 648, 140, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAutoTradingCapture)), nullptr, nullptr);
            g_autoTradingFinishButton = CreateWindowW(L"BUTTON", L"AT Finish", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 372, 648, 140, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAutoTradingFinish)), nullptr, nullptr);
            g_trackerResearchButton = CreateWindowW(L"BUTTON", L"Tracker Capture", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 520, 648, 140, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonTrackerResearch)), nullptr, nullptr);
            g_positionCurrencyResearchButton = CreateWindowW(L"BUTTON", L"Position CCY", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 668, 648, 140, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonPositionCurrencyResearch)), nullptr, nullptr);
            g_openCompatibilityButton = CreateWindowW(L"BUTTON", L"Open Compat", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 668, 648, 140, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonOpenCompatibility)), nullptr, nullptr);
            g_reloadCompatibilityButton = CreateWindowW(L"BUTTON", L"Reload Compat", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 816, 648, 140, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonReloadCompatibility)), nullptr, nullptr);
            g_reloadSettingsButton = CreateWindowW(L"BUTTON", L"Reload Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 628, 648, 150, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonReloadSettings)), nullptr, nullptr);
            g_testEmailButton = CreateWindowW(L"BUTTON", L"Send Test Email", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 788, 648, 150, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonTestEmail)), nullptr, nullptr);
            g_autoMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 212, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAutoMenu)), nullptr, nullptr);
            g_statusMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 314, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonStatusMenu)), nullptr, nullptr);
            g_emailMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 348, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonEmailMenu)), nullptr, nullptr);
            g_heartbeatMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 382, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonHeartbeatMenu)), nullptr, nullptr);
            for (HWND button : { g_refreshButton, g_reportButton, g_settingsButton, g_openFolderButton, g_openSettingsButton, g_autoTradingDiagnosticsButton, g_autoTradingCaptureButton, g_autoTradingFinishButton, g_trackerResearchButton, g_positionCurrencyResearchButton, g_openCompatibilityButton, g_reloadCompatibilityButton, g_reloadSettingsButton, g_testEmailButton, g_autoMenuButton, g_statusMenuButton, g_emailMenuButton, g_heartbeatMenuButton })
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            for (HWND button : { g_autoTradingDiagnosticsButton, g_autoTradingCaptureButton, g_autoTradingFinishButton, g_trackerResearchButton, g_positionCurrencyResearchButton, g_openCompatibilityButton, g_reloadCompatibilityButton })
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_developerButtonFont), TRUE);
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
                    AppConfig reloadedConfig = LoadAppConfig();
                    {
                        std::lock_guard<std::mutex> lock(g_app.mutex);
                        g_app.config = std::move(reloadedConfig);
                        ++g_app.configRevision;
                        AddActivity(g_app.status, mcst::HealthState::Healthy, L"Status Report settings saved and reloaded");
                    }
                    g_app.schedule.PreserveOnReload(std::chrono::system_clock::now());
                    UpdateDeveloperControlVisibility(hwnd);
                    StartRefresh(hwnd, true);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case kButtonOpenFolder:
                ShellExecuteW(hwnd, L"open", GetApplicationDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            case kButtonOpenSettings:
            {
                const std::wstring settingsPath = GetApplicationDirectory() + L"\\MCST-Watchdog.ini";
                const HINSTANCE result = ShellExecuteW(hwnd, L"open", settingsPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(result) <= 32)
                    MessageBoxW(hwnd, (L"Could not open settings file:\r\n" + settingsPath).c_str(), L"Open Settings", MB_OK | MB_ICONWARNING);
                return 0;
            }
            case kButtonReloadSettings:
            {
                AppConfig reloadedConfig = LoadAppConfig();
                {
                    std::lock_guard<std::mutex> lock(g_app.mutex);
                    g_app.config = std::move(reloadedConfig);
                    ++g_app.configRevision;
                    for (const auto& normalizationMessage : g_app.config.normalizationMessages)
                        AddActivity(g_app.status, mcst::HealthState::Attention, L"INI normalized: " + normalizationMessage);
                }
                g_app.schedule.PreserveOnReload(std::chrono::system_clock::now());
                // Preserve AutoTrading alert state across an INI reload. The next
                // forced refresh evaluates the new minimum against the previous
                // below/healthy state, avoiding duplicate critical alerts while still
                // producing a real transition when the edited threshold crosses the
                // current active-strategy count.
                // Preserve the last confirmed Broker state across an INI reload.
                // A settings reload is not broker evidence and must not force UNKNOWN.
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
                SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, L"MCST-Watchdog Test Email", L"MCST-Watchdog email configuration is working.\r\n\r\nVersion: 1.114-R2 Research", false, L"Test email", false, g_app.config.alertEmailTo);
                MessageBoxW(hwnd, L"Test email is being sent.", L"Email", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            case kButtonAutoTradingDiagnostics:
            {
                CreateDirectoryW(L"C:\\Temp", nullptr);
                const std::wstring path = L"C:\\Temp\\MCST-Watchdog\\AutoTradingResearch.txt";
                CreateDirectoryW(L"C:\\Temp\\MCST-Watchdog", nullptr);
                std::wstring diagnostic;
                SetWindowTextW(g_autoTradingDiagnosticsButton, L"Starting...");
                EnableWindow(g_autoTradingDiagnosticsButton, FALSE);
                const bool ok = StartAutoTradingResearchSession(path, diagnostic);
                EnableWindow(g_autoTradingDiagnosticsButton, TRUE);
                SetWindowTextW(g_autoTradingDiagnosticsButton, L"AT Start");
                MessageBoxW(hwnd, diagnostic.c_str(), ok ? L"AutoTrading research" : L"AutoTrading research error",
                    ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
                if (ok) ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            case kButtonAutoTradingCapture:
            {
                const std::wstring path = L"C:\\Temp\\MCST-Watchdog\\AutoTradingResearch.txt";
                std::wstring diagnostic;
                SetWindowTextW(g_autoTradingCaptureButton, L"Capturing...");
                EnableWindow(g_autoTradingCaptureButton, FALSE);
                const bool ok = CaptureAutoTradingResearchSnapshot(path, diagnostic);
                EnableWindow(g_autoTradingCaptureButton, TRUE);
                SetWindowTextW(g_autoTradingCaptureButton, L"AT Capture");
                MessageBoxW(hwnd, diagnostic.c_str(), ok ? L"AutoTrading research" : L"AutoTrading research error",
                    ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
                return 0;
            }
            case kButtonAutoTradingFinish:
            {
                const std::wstring path = L"C:\\Temp\\MCST-Watchdog\\AutoTradingResearch.txt";
                std::wstring diagnostic;
                SetWindowTextW(g_autoTradingFinishButton, L"Analyzing...");
                EnableWindow(g_autoTradingFinishButton, FALSE);
                const bool ok = FinishAutoTradingResearchSession(path, diagnostic);
                EnableWindow(g_autoTradingFinishButton, TRUE);
                SetWindowTextW(g_autoTradingFinishButton, L"AT Finish");
                MessageBoxW(hwnd, diagnostic.c_str(), ok ? L"AutoTrading research summary" : L"AutoTrading research error",
                    ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
                if (ok) ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            case kButtonTrackerResearch:
            {
                std::wstring summary;
                std::wstring diagnostic;
                SetWindowTextW(g_trackerResearchButton, L"Capturing...");
                EnableWindow(g_trackerResearchButton, FALSE);
                const bool ok = CaptureTrackerResearchBundle(
                    summary, diagnostic, static_cast<unsigned long>(g_app.config.bridgeTimeoutMilliseconds));
                EnableWindow(g_trackerResearchButton, TRUE);
                SetWindowTextW(g_trackerResearchButton, L"Tracker Capture");
                std::wstring dialogMessage = diagnostic;
                if (!summary.empty())
                    dialogMessage += L"\r\n\r\n" + summary;
                MessageBoxW(hwnd, dialogMessage.c_str(), ok ? L"Tracker compatibility research" : L"Tracker research error",
                    ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
                if (ok)
                    ShellExecuteW(hwnd, L"open", L"C:\\Temp", nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            case kButtonPositionCurrencyResearch:
            {
                // Capture a fresh production snapshot first. Its raw payload is the
                // ground-truth row reference that lets the offline analysis correlate
                // symbol/profile/account values with the passive memory probe.
                TrackerStatusSnapshot referenceSnapshot;
                std::wstring referenceDiagnostic;
                const unsigned long timeoutMilliseconds =
                    static_cast<unsigned long>(g_app.config.bridgeTimeoutMilliseconds);

                SetWindowTextW(g_positionCurrencyResearchButton, L"Capturing...");
                EnableWindow(g_positionCurrencyResearchButton, FALSE);

                const bool referenceRead = ReadTrackerStatusSnapshot(
                    referenceSnapshot, referenceDiagnostic, timeoutMilliseconds);

                bool referenceWritten = false;
                std::wstring referencePath;
                std::wstring referenceWriteDiagnostic;
                if (referenceRead && referenceSnapshot.openPositions.present &&
                    referenceSnapshot.openPositions.ok &&
                    !referenceSnapshot.openPositions.rows.empty())
                {
                    std::wostringstream path;
                    path << L"C:\\Temp\\MCST_Position_Currency_Reference_"
                         << referenceSnapshot.processId << L".txt";
                    referencePath = path.str();
                    referenceWritten = WriteTrackerStatusRawPayload(
                        referenceSnapshot, referencePath, referenceWriteDiagnostic);
                }

                std::wstring summary;
                std::wstring captureDiagnostic;
                bool captureOk = false;
                if (referenceWritten && referenceSnapshot.bridgeVersion >= kPositionCurrencyResearchBridgeVersion)
                {
                    captureOk = CapturePositionCurrencyResearch(
                        summary, captureDiagnostic, timeoutMilliseconds);
                }

                EnableWindow(g_positionCurrencyResearchButton, TRUE);
                SetWindowTextW(g_positionCurrencyResearchButton, L"Position CCY");

                std::wstring dialogMessage;
                if (!referenceRead)
                {
                    dialogMessage = L"Could not read a fresh Tracker reference snapshot.\r\n\r\n" + referenceDiagnostic;
                }
                else if (referenceSnapshot.bridgeVersion < kPositionCurrencyResearchBridgeVersion)
                {
                    dialogMessage = L"Position CCY R2 requires MCST Tracker Bridge V" +
                        std::to_wstring(kPositionCurrencyResearchBridgeVersion) +
                        L" or newer. The currently loaded Bridge is V" +
                        std::to_wstring(referenceSnapshot.bridgeVersion) +
                        L". Replace C:\\MCExtras\\MCST-TrackerBridge.dll with the V157 build from this package and restart MultiCharts.";
                }
                else if (!referenceSnapshot.openPositions.present || !referenceSnapshot.openPositions.ok)
                {
                    dialogMessage = L"Open Positions is not readable, so Position Currency research cannot be correlated safely.\r\n\r\n" +
                        referenceSnapshot.openPositions.diagnostic;
                }
                else if (referenceSnapshot.openPositions.rows.empty())
                {
                    dialogMessage = L"No open positions were found. Keep positions from at least two currencies open and run Position CCY again.";
                }
                else if (!referenceWritten)
                {
                    dialogMessage = L"The Tracker reference snapshot was read, but the research reference file could not be written.\r\n\r\n" +
                        referenceWriteDiagnostic;
                }
                else
                {
                    dialogMessage = captureDiagnostic + L"\r\n\r\nReference rows:\r\n" + referencePath;
                    if (!summary.empty())
                        dialogMessage += L"\r\n\r\nBridge capture:\r\n" + summary;
                    dialogMessage +=
                        L"\r\n\r\nResearch files are under C:\\Temp. "
                        L"The primary R2 result is MCST_Position_Currency_Direct_<pid>.txt. "
                        L"It correlates visible Open Positions rows with the mapped position-record storage and inspects separate native-currency and P/L-currency candidates.";
                }

                MessageBoxW(
                    hwnd,
                    dialogMessage.c_str(),
                    referenceWritten && captureOk ? L"Position Currency research" : L"Position Currency research error",
                    referenceWritten && captureOk ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
                if (referenceWritten && captureOk)
                    ShellExecuteW(hwnd, L"open", L"C:\\Temp", nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            case kButtonOpenCompatibility:
            {
                std::wstring ensureDiagnostic;
                if (!EnsureCompatibilityDatabase(ensureDiagnostic))
                {
                    MessageBoxW(hwnd, ensureDiagnostic.c_str(), L"Compatibility database", MB_OK | MB_ICONERROR);
                    return 0;
                }
                const std::wstring path = GetCompatibilityDatabasePath();
                const HINSTANCE result = ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(result) <= 32)
                    MessageBoxW(hwnd, (L"Could not open compatibility database:\r\n" + path).c_str(), L"Compatibility database", MB_OK | MB_ICONWARNING);
                return 0;
            }
            case kButtonReloadCompatibility:
            {
                // The Bridge resolves Tracker profiles on every snapshot request.
                // forceAutoTradingRefresh bypasses the Watchdog AutoTrading cache,
                // so one refresh reloads both compatibility consumers.
                StartRefresh(hwnd, true);
                MessageBoxW(hwnd, L"Compatibility profiles will be re-evaluated during the forced refresh.",
                    L"Reload Compat", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            }
            break;

        case WM_APP_REFRESH_COMPLETE:
        {
            std::unique_ptr<RefreshResult> result(reinterpret_cast<RefreshResult*>(lParam));
            if (result)
            {
                bool staleConfigurationResult = false;
                {
                    std::lock_guard<std::mutex> lock(g_app.mutex);
                    staleConfigurationResult = result->configRevision != g_app.configRevision;
                    if (staleConfigurationResult)
                        g_app.refreshRunning = false;
                }
                if (staleConfigurationResult)
                {
                    // A settings reload happened while this worker was running. Never let
                    // the old configuration overwrite the Dashboard or AutoTrading minimum.
                    // Start one forced read using the newly loaded configuration instead.
                    StartRefresh(hwnd, true);
                    return 0;
                }

                std::lock_guard<std::mutex> lock(g_app.mutex);
                const auto previousActivity = g_app.status.activity;
                const int active = result->status.autoTradingActive;

                g_app.status = std::move(result->status);
                g_app.snapshot = std::move(result->snapshot);
                g_app.status.lastReport = g_app.lastReport;
                g_app.status.lastAlert = g_app.lastAlert;

                const auto monitorNow = std::chrono::system_clock::now();
                const BrokerMonitorDecision brokerDecision = g_app.brokerMonitor.Evaluate(
                    g_app.snapshot.recentLogs, result->brokerAuthentication, g_app.config, monitorNow);
                g_app.status.broker = brokerDecision.status;
                g_app.status.overall = Worst(g_app.status.overall, g_app.status.broker.state);
                if (brokerDecision.stateChanged)
                {
                    if (brokerDecision.status.state == mcst::HealthState::Healthy)
                        g_app.brokerMonitor.SaveConnectedStateCache(BrokerStateCachePath(), monitorNow);
                    else if (brokerDecision.status.state == mcst::HealthState::Critical)
                        g_app.brokerMonitor.ClearConnectedStateCache(BrokerStateCachePath());
                }
                if (brokerDecision.stateChanged && !brokerDecision.eventText.empty())
                {
                    AddActivity(g_app.status, brokerDecision.status.state, brokerDecision.eventText);
                    if (brokerDecision.sendAlertEmail || brokerDecision.sendRecoveryEmail)
                    {
                        g_app.lastAlert = FormatLocalTime(monitorNow) + L" - " + brokerDecision.eventText;
                        g_app.status.lastAlert = g_app.lastAlert;
                        const std::wstring report = BuildStatusReport(g_app.status, g_app.snapshot);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, brokerDecision.subject,
                            BuildAlertWithStatusReportHtml(brokerDecision.eventText, report), true, brokerDecision.eventText, true, g_app.config.alertEmailTo);
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
                        const std::wstring report = BuildStatusReport(g_app.status, g_app.snapshot);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config,
                            logAlertDecision.subject, BuildAlertWithStatusReportHtml(logAlertDecision.body, report), true,
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
                        const std::wstring report = BuildStatusReport(g_app.status, g_app.snapshot);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, alertDecision.subject,
                            BuildAlertWithStatusReportHtml(alertDecision.eventText, report), true, alertDecision.eventText, true, g_app.config.alertEmailTo);
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
                    const std::wstring heartbeatIntroduction = L"MCST-Watchdog heartbeat. Monitoring is active.";
                    const std::wstring heartbeatReport = BuildStatusReport(g_app.status, g_app.snapshot);
                    AddActivity(g_app.status, mcst::HealthState::Healthy, L"Heartbeat generated");
                    if (g_app.config.emailEnabled)
                    {
                        g_app.heartbeatEmailInFlight = true;
                        AddActivity(g_app.status, mcst::HealthState::Healthy,
                            L"Heartbeat queued to " + g_app.config.reportEmailTo);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, L"MCST-Watchdog HEARTBEAT - " + FormatLocalTime(scheduleNow),
                            BuildAlertWithStatusReportHtml(heartbeatIntroduction, heartbeatReport), false, L"Heartbeat email", true, g_app.config.reportEmailTo);
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
    // This is deliberately the first executable application code. If the process reaches
    // wWinMain, a marker should be visible either beside the EXE or in C:\Temp.
    SetUnhandledExceptionFilter(StartupUnhandledExceptionFilter);
    AppendStartupLogSafe(L"Startup -1: entered wWinMain before application initialization");

    HANDLE singleInstanceMutex = nullptr;
    bool gdiplusStarted = false;

    try
    {
        AppendStartupLogSafe(L"Startup 0: MCST-Watchdog 1.114-R2 research process entered protected startup");

        singleInstanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
        if (!singleInstanceMutex)
        {
            ShowStartupFailureSafe(L"Startup 1 failed: unable to create the single-instance mutex. Win32 error " + std::to_wstring(GetLastError()) + L".");
            return 1;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            AppendStartupLogSafe(L"Startup 1: another MCST-Watchdog instance is already running");
            MessageBoxW(nullptr, L"MCST-Watchdog is already running.", L"MCST-Watchdog", MB_OK | MB_ICONINFORMATION);
            CloseHandle(singleInstanceMutex);
            return 0;
        }
        AppendStartupLogSafe(L"Startup 1: single-instance mutex OK");

        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok)
            throw std::runtime_error("GDI+ initialization failed");
        gdiplusStarted = true;
        AppendStartupLogSafe(L"Startup 2: GDI+ OK");

        AppendStartupLogSafe(L"Startup 3: loading and normalizing configuration");
        g_app.config = LoadAppConfig();
        AppendStartupLogSafe(L"Startup 4: configuration OK");

        g_app.brokerMonitor.LoadConnectedStateCache(
            BrokerStateCachePath(), std::chrono::system_clock::now(), g_app.config.brokerStateCacheMaxAgeMinutes);
        AppendStartupLogSafe(L"Startup 5: broker state cache OK");

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
        AppendStartupLogSafe(L"Startup 6: fonts OK");

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
            throw std::runtime_error("RegisterClassExW failed");
        AppendStartupLogSafe(L"Startup 7: window class OK");

        const RECT initialRect = ResolveInitialWindowRect(g_app.config);
        const int initialX = initialRect.left == CW_USEDEFAULT ? CW_USEDEFAULT : initialRect.left;
        const int initialY = initialRect.top == CW_USEDEFAULT ? CW_USEDEFAULT : initialRect.top;
        const int initialWidth = initialRect.left == CW_USEDEFAULT ? g_app.config.windowWidth : initialRect.right - initialRect.left;
        const int initialHeight = initialRect.top == CW_USEDEFAULT ? g_app.config.windowHeight : initialRect.bottom - initialRect.top;

        HWND window = CreateWindowExW(
            0, kWindowClass, L"MCST-Watchdog 1.114-R2 - Position Currency Research",
            WS_OVERLAPPEDWINDOW,
            initialX, initialY, initialWidth, initialHeight,
            nullptr, nullptr, instance, nullptr);
        if (!window)
            throw std::runtime_error("CreateWindowExW failed");
        AppendStartupLogSafe(L"Startup 8: main window created");

        ShowWindow(window, g_app.config.windowMaximized ? SW_SHOWMAXIMIZED : showCommand);
        UpdateWindow(window);
        AppendStartupLogSafe(L"Startup 9: entering message loop");

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        AppendStartupLogSafe(L"Shutdown: normal message-loop exit");
        DeleteObject(g_titleFont);
        DeleteObject(g_headerFont);
        DeleteObject(g_bodyFont);
        DeleteObject(g_labelFont);
        DeleteObject(g_statusFont);
        DeleteObject(g_monoFont);
        DeleteObject(g_developerButtonFont);
        if (gdiplusStarted)
            Gdiplus::GdiplusShutdown(g_gdiplusToken);
        if (singleInstanceMutex)
            CloseHandle(singleInstanceMutex);
        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& ex)
    {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, nullptr, 0);
        std::wstring detail = L"Unhandled startup exception";
        if (needed > 1)
        {
            std::wstring converted(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, converted.data(), needed);
            if (!converted.empty() && converted.back() == L'\0') converted.pop_back();
            detail += L": " + converted;
        }
        ShowStartupFailureSafe(detail);
    }
    catch (...)
    {
        ShowStartupFailureSafe(L"Unhandled non-standard exception during startup.");
    }

    if (gdiplusStarted)
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
    if (singleInstanceMutex)
        CloseHandle(singleInstanceMutex);
    return 1;
}
