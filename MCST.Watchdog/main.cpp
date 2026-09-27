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
#include "DeveloperHelpContent.h"
#include "MultiChartsVersionDetector.h"
#include "MultiChartsHealthMonitor.h"
#include "CoveredQueueProbe.h"
#include "resource.h"
#include "../MCST.Shared/ReportReadPolicy.h"
#include "../MCST.Shared/OverallHeadlinePolicy.h"
#include "../MCST.Shared/ActivityHistory.h"
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
    constexpr int kButtonDeveloperHelp = 1019;
    constexpr int kCheckDeveloperMode = 1020;
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
    constexpr int kDeveloperHelpClose = 3201;
    constexpr int kDeveloperHelpTopics = 3202;
    constexpr int kDeveloperHelpText = 3203;
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
        TrackerStatusSnapshot lastGoodTrackerSnapshot;
        std::chrono::system_clock::time_point lastGoodTrackerSnapshotTime{};
        bool hasLastGoodTrackerSnapshot = false;
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
        bool multiChartsHealthObserved = false;
        bool multiChartsHealthEmailInFlight = false;
        TrackerBridgeSection lastGoodLogs;
        unsigned long lastLogsProcessId = 0;
        bool hasGoodLogs = false;
        std::wstring lastLogsRead = L"Never";
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
    HWND g_developerHelpButton = nullptr;
    HWND g_developerModeCheckbox = nullptr;
    HWND g_reloadSettingsButton = nullptr;
    HWND g_testEmailButton = nullptr;
    HWND g_autoMenuButton = nullptr;
    HWND g_statusMenuButton = nullptr;
    HWND g_emailMenuButton = nullptr;
    HWND g_heartbeatMenuButton = nullptr;
    ULONG_PTR g_gdiplusToken = 0;
    std::unique_ptr<Gdiplus::Image> g_headerLogo;

    std::unique_ptr<Gdiplus::Image> LoadPngResource(HINSTANCE instance, int resourceId)
    {
        HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!resource)
            return nullptr;

        HGLOBAL loadedResource = LoadResource(instance, resource);
        const DWORD resourceSize = SizeofResource(instance, resource);
        const void* resourceBytes = loadedResource ? LockResource(loadedResource) : nullptr;
        if (!resourceBytes || resourceSize == 0)
            return nullptr;

        HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
        if (!copy)
            return nullptr;
        void* destination = GlobalLock(copy);
        if (!destination)
        {
            GlobalFree(copy);
            return nullptr;
        }
        CopyMemory(destination, resourceBytes, resourceSize);
        GlobalUnlock(copy);

        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK)
        {
            GlobalFree(copy);
            return nullptr;
        }

        std::unique_ptr<Gdiplus::Image> source(Gdiplus::Image::FromStream(stream, FALSE));
        if (!source || source->GetLastStatus() != Gdiplus::Ok ||
            source->GetWidth() == 0 || source->GetHeight() == 0)
        {
            stream->Release();
            return nullptr;
        }

        auto bitmap = std::make_unique<Gdiplus::Bitmap>(
            source->GetWidth(), source->GetHeight(), PixelFormat32bppARGB);
        if (bitmap->GetLastStatus() != Gdiplus::Ok)
        {
            stream->Release();
            return nullptr;
        }
        {
            Gdiplus::Graphics graphics(bitmap.get());
            graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            graphics.DrawImage(source.get(), 0, 0,
                static_cast<INT>(source->GetWidth()), static_cast<INT>(source->GetHeight()));
        }
        source.reset();
        stream->Release();
        return bitmap;
    }


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
        const int y = (std::max)(620, static_cast<int>(client.bottom) - 58);
        if (g_refreshButton) MoveWindow(g_refreshButton, 28, y, 110, 34, TRUE);
        if (g_reportButton) MoveWindow(g_reportButton, 148, y, 120, 34, TRUE);
        if (g_openFolderButton) MoveWindow(g_openFolderButton, 278, y, 120, 34, TRUE);
        if (g_openSettingsButton) MoveWindow(g_openSettingsButton, 408, y, 130, 34, TRUE);
        if (g_reloadSettingsButton) MoveWindow(g_reloadSettingsButton, 548, y, 150, 34, TRUE);
        if (g_developerModeCheckbox)
            MoveWindow(g_developerModeCheckbox, (std::max)(730, static_cast<int>(client.right) - 190), y + 7, 162, 22, TRUE);
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
            const RECT helpRect = CalculateDeveloperToolbarButtonRect(developerLayout, 7);
            if (g_autoTradingDiagnosticsButton) MoveWindow(g_autoTradingDiagnosticsButton, startRect.left, startRect.top, startRect.right - startRect.left, startRect.bottom - startRect.top, TRUE);
            if (g_autoTradingCaptureButton) MoveWindow(g_autoTradingCaptureButton, captureRect.left, captureRect.top, captureRect.right - captureRect.left, captureRect.bottom - captureRect.top, TRUE);
            if (g_autoTradingFinishButton) MoveWindow(g_autoTradingFinishButton, finishRect.left, finishRect.top, finishRect.right - finishRect.left, finishRect.bottom - finishRect.top, TRUE);
            if (g_trackerResearchButton) MoveWindow(g_trackerResearchButton, trackerRect.left, trackerRect.top, trackerRect.right - trackerRect.left, trackerRect.bottom - trackerRect.top, TRUE);
            if (g_positionCurrencyResearchButton) MoveWindow(g_positionCurrencyResearchButton, positionCurrencyRect.left, positionCurrencyRect.top, positionCurrencyRect.right - positionCurrencyRect.left, positionCurrencyRect.bottom - positionCurrencyRect.top, TRUE);
            if (g_openCompatibilityButton) MoveWindow(g_openCompatibilityButton, openCompatRect.left, openCompatRect.top, openCompatRect.right - openCompatRect.left, openCompatRect.bottom - openCompatRect.top, TRUE);
            if (g_reloadCompatibilityButton) MoveWindow(g_reloadCompatibilityButton, reloadCompatRect.left, reloadCompatRect.top, reloadCompatRect.right - reloadCompatRect.left, reloadCompatRect.bottom - reloadCompatRect.top, TRUE);
            if (g_developerHelpButton) MoveWindow(g_developerHelpButton, helpRect.left, helpRect.top, 70, helpRect.bottom - helpRect.top, TRUE);
        }
        else
        {
            // Reload Settings remains on the normal bottom row next to Open Settings.
        }
        const DashboardRowLayout rowLayout = CalculateDashboardRowLayout(static_cast<int>(client.right));
        const int menuX = rowLayout.overflowButtonX;
        if (g_autoMenuButton) MoveWindow(g_autoMenuButton, menuX, 220, 30, 24, TRUE);
        if (g_statusMenuButton) MoveWindow(g_statusMenuButton, menuX, 310, 30, 24, TRUE);
        if (g_emailMenuButton) MoveWindow(g_emailMenuButton, menuX, 340, 30, 24, TRUE);
        if (g_heartbeatMenuButton) MoveWindow(g_heartbeatMenuButton, menuX, 370, 30, 24, TRUE);
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
        if (g_developerHelpButton)
            ShowWindow(g_developerHelpButton, showResearch);
        if (g_developerModeCheckbox)
            SendMessageW(g_developerModeCheckbox, BM_SETCHECK,
                g_app.config.developerModeEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        LayoutButtons(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
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

    void RecalculateOverallStatus(
        mcst::WatchdogSystemStatus& status,
        const AppConfig& config,
        bool includeBroker)
    {
        status.overall = mcst::HealthState::Healthy;
        status.overall = Worst(status.overall, status.multiChartsHealth.state);
        status.overall = Worst(status.overall, status.bridge.state);
        status.overall = Worst(status.overall, status.trackerSnapshot.state);
        status.overall = Worst(status.overall, status.recentLogs.state);
        status.overall = Worst(status.overall, status.autoTrading.state);
        if (includeBroker)
            status.overall = Worst(status.overall, status.broker.state);
        if (config.statusReportsEnabled)
            status.overall = Worst(status.overall, status.statusReports.state);
        if (!(config.emailEnabledSettingPresent && !config.emailEnabled))
            status.overall = Worst(status.overall, status.email.state);
        if (config.heartbeatEnabled)
            status.overall = Worst(status.overall, status.heartbeat.state);
    }

    bool IsCompleteTrackerSnapshot(const TrackerStatusSnapshot& snapshot)
    {
        return snapshot.trackerFound &&
            snapshot.trackerSameProcess &&
            snapshot.trackerCompatibilityMatched &&
            snapshot.accounts.ok &&
            snapshot.openPositions.ok &&
            snapshot.recentLogs.ok;
    }

    bool IsRecoverableTrackerSnapshotFailure(const TrackerStatusSnapshot& snapshot)
    {
        return snapshot.trackerFound &&
            snapshot.trackerSameProcess &&
            snapshot.trackerCompatibilityMatched &&
            !IsCompleteTrackerSnapshot(snapshot);
    }

    const TrackerBridgeSection& MonitoringLogs(const TrackerStatusSnapshot& snapshot)
    {
        // Bridge V173 or newer supplies up to 200 rows for state engines while the
        // report-facing Recent Logs section remains at ten rows. Retain a safe
        // fallback for older compatible Bridge builds.
        return snapshot.monitoringLogs.present && snapshot.monitoringLogs.ok
            ? snapshot.monitoringLogs
            : snapshot.recentLogs;
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
        result.status.lastTrackerAttempt = L"Never";
        result.status.lastCompleteTrackerSnapshot = L"Never";
        result.status.autoTradingMinimum = config.autoTradingMinimum;
        result.status.trackerDataStaleCriticalAfterMinutes = config.trackerStaleCriticalAfterMinutes;
        result.status.trackerDateOrder = config.trackerDateOrder;

        std::wstring diagnostic;
        bool readOk = false;
        int snapshotAttempts = 0;
        bool anyBridgeRecoveryAttempted = false;
        std::vector<std::wstring> bridgeRecoveryAttempts;
        for (int attempt = 1; attempt <= config.snapshotRetryCount; ++attempt)
        {
            snapshotAttempts = attempt;
            TrackerStatusSnapshot candidate;
            std::wstring attemptDiagnostic;
            if (ReadTrackerStatusSnapshot(
                    candidate,
                    attemptDiagnostic,
                    static_cast<unsigned long>(config.bridgeTimeoutMilliseconds)))
            {
                if (candidate.recoveryAttempted ||
                    (!candidate.recoveryResult.empty() && candidate.recoveryResult != L"not_needed"))
                {
                    anyBridgeRecoveryAttempted = anyBridgeRecoveryAttempted || candidate.recoveryAttempted;
                    bridgeRecoveryAttempts.push_back(
                        L"attempt " + std::to_wstring(attempt) + L"=" +
                        (candidate.recoveryResult.empty() ? L"attempted" : candidate.recoveryResult));
                }
                readOk = true;
                result.snapshot = std::move(candidate);
                diagnostic = std::move(attemptDiagnostic);

                // A parsed ExtractorCallFailed payload is useful evidence, but it
                // is not a successful Tracker snapshot. Give transient section
                // failures time to settle and request a new Bridge-side discovery.
                if (IsCompleteTrackerSnapshot(result.snapshot) ||
                    !IsRecoverableTrackerSnapshotFailure(result.snapshot) ||
                    attempt == config.snapshotRetryCount)
                {
                    break;
                }

                Sleep(static_cast<DWORD>((std::max)(1000, config.snapshotRetryDelayMilliseconds)));
                continue;
            }
            readOk = false;
            result.snapshot = TrackerStatusSnapshot{};
            diagnostic = std::move(attemptDiagnostic);
            if (attempt < config.snapshotRetryCount)
                Sleep(static_cast<DWORD>(config.snapshotRetryDelayMilliseconds));
        }

        const auto now = std::chrono::system_clock::now();
        result.status.lastTrackerAttempt = FormatLocalTime(now);
        if (readOk)
        {
            if (!bridgeRecoveryAttempts.empty())
            {
                std::wstring combined;
                for (const std::wstring& item : bridgeRecoveryAttempts)
                {
                    if (!combined.empty())
                        combined += L"; ";
                    combined += item;
                }
                result.snapshot.recoveryAttempted = anyBridgeRecoveryAttempted;
                result.snapshot.recoveryResult = std::move(combined);
            }
            result.status.bridge = { mcst::HealthState::Healthy, L"Connected",
                L"MCST Tracker Bridge 1.0 - internal V" + std::to_wstring(result.snapshot.bridgeVersion) + L" - Protocol V" + std::to_wstring(result.snapshot.protocolVersion) };
            const bool trackerAvailable = result.snapshot.trackerFound && result.snapshot.trackerSameProcess;
            const int readableTrackerSections =
                static_cast<int>(result.snapshot.accounts.ok) +
                static_cast<int>(result.snapshot.openPositions.ok) +
                static_cast<int>(result.snapshot.recentLogs.ok);
            const bool trackerOk = IsCompleteTrackerSnapshot(result.snapshot);
            const bool trackerPartial = trackerAvailable && readableTrackerSections > 0 && readableTrackerSections < 3;
            std::wstring trackerDetail = result.snapshot.trackerCompatibilityDiagnostic;
            if (trackerDetail.empty()) trackerDetail = diagnostic;
            if (!result.snapshot.trackerCompatibilityProfile.empty())
                trackerDetail += L" - Profile: " + result.snapshot.trackerCompatibilityProfile;
            if (!result.snapshot.tabViewDiagnostic.empty())
                trackerDetail += L" - CATPTTabView: " + result.snapshot.tabViewDiagnostic;
            if (result.snapshot.recoveryAttempted ||
                (!result.snapshot.recoveryResult.empty() && result.snapshot.recoveryResult != L"not_needed"))
                trackerDetail += L" - Bridge recovery: " +
                    (result.snapshot.recoveryResult.empty() ? L"attempted" : result.snapshot.recoveryResult);
            if (snapshotAttempts > 1)
                trackerDetail += L" - Snapshot attempts: " + std::to_wstring(snapshotAttempts);
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

            if (result.snapshot.recentLogs.ok && !result.snapshot.recentLogs.rows.empty())
                result.status.recentLogs = { mcst::HealthState::Healthy, std::to_wstring(result.snapshot.recentLogs.rows.size()) + L" rows", L"Read OK" };
            else if (result.snapshot.recentLogs.ok)
                result.status.recentLogs = { mcst::HealthState::Healthy, L"0 rows", L"Read OK - no events" };
            else
                result.status.recentLogs = {
                    mcst::HealthState::Critical,
                    L"Read failed",
                    result.snapshot.recentLogs.diagnostic +
                        (result.snapshot.tabViewDiagnostic.empty()
                            ? L""
                            : L" - " + result.snapshot.tabViewDiagnostic)
                };

            std::wstring rawDiagnostic;
            WriteTrackerStatusRawPayload(result.snapshot, config.rawSnapshotPath, rawDiagnostic);
        }
        else
        {
            result.status.bridge = { mcst::HealthState::Critical, L"Disconnected", diagnostic };
            result.status.trackerSnapshot = { mcst::HealthState::Critical, L"Unavailable", L"Snapshot read failed" };
            result.status.recentLogs = { mcst::HealthState::Unknown, L"Unknown", L"No snapshot" };
            result.status.lastError = diagnostic;
            AddActivity(result.status, mcst::HealthState::Critical, L"Tracker snapshot failed");
        }

        result.status.multiChartsProcesses = ReadMultiChartsHealth();
        result.status.multiChartsHealth = {
            result.status.multiChartsProcesses.state,
            result.status.multiChartsProcesses.value,
            result.status.multiChartsProcesses.detail
        };

        if (config.autoTradingMonitoringEnabled)
        {
            // A disappeared MC process invalidates an earlier aggregate
            // AutoTrading count. Bypass the normal cache once so the existing
            // minimum-active check can distinguish an empty auxiliary instance
            // (health WARNING) from a lost trading instance (overall CRITICAL).
            const bool processSetChanged = result.status.multiChartsProcesses.processSetChanged;
            const AutoTradingReadResult autoTrading = ReadAutoTradingStatus(
                config.autoTradingCheckMinutes,
                forceAutoTradingRefresh || processSetChanged);
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
                        L" - Charts found " + std::to_wstring(autoTrading.strategyObjectsFound) +
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

        RecalculateOverallStatus(result.status, config, false);

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

    void UpdateDeveloperHelpTopic(HWND hwnd)
    {
        const HWND list = GetDlgItem(hwnd, kDeveloperHelpTopics);
        int selected = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        const auto& topics = GetDeveloperHelpTopics();
        if (selected < 0 || static_cast<std::size_t>(selected) >= topics.size())
            selected = 0;
        SetWindowTextW(GetDlgItem(hwnd, kDeveloperHelpText), topics[selected].content.c_str());
        SendMessageW(GetDlgItem(hwnd, kDeveloperHelpText), EM_SETSEL, 0, 0);
        SendMessageW(GetDlgItem(hwnd, kDeveloperHelpText), EM_SCROLLCARET, 0, 0);
    }

    LRESULT CALLBACK DeveloperHelpProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        (void)lParam;
        switch (message)
        {
        case WM_CREATE:
        {
            HWND heading = CreateWindowW(L"STATIC", L"Developer Mode Help - choose a button to see exact instructions",
                WS_CHILD | WS_VISIBLE, 18, 14, 900, 28, hwnd, nullptr, nullptr, nullptr);
            HWND useRule = CreateWindowW(L"STATIC",
                L"Normally use only after a MultiCharts/module update, a changed fingerprint, or a developer request - not during normal monitoring.",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 18, 44, 932, 22, hwnd, nullptr, nullptr, nullptr);
            HWND choose = CreateWindowW(L"STATIC", L"CHOOSE A BUTTON",
                WS_CHILD | WS_VISIBLE, 18, 76, 220, 22, hwnd, nullptr, nullptr, nullptr);
            HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                18, 102, 220, 386, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeveloperHelpTopics)), nullptr, nullptr);
            const auto& topics = GetDeveloperHelpTopics();
            for (const auto& topic : topics)
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(topic.buttonLabel.c_str()));
            SendMessageW(list, LB_SETCURSEL, 0, 0);
            HWND note = CreateWindowW(L"STATIC",
                L"Research tools only. They do not switch live trading on or off.",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 18, 502, 220, 50, hwnd, nullptr, nullptr, nullptr);
            HWND text = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_READONLY,
                254, 76, 696, 476, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeveloperHelpText)), nullptr, nullptr);
            HWND close = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                860, 564, 90, 32, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeveloperHelpClose)), nullptr, nullptr);
            SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(g_headerFont), TRUE);
            SendMessageW(useRule, WM_SETFONT, reinterpret_cast<WPARAM>(g_labelFont), TRUE);
            SendMessageW(choose, WM_SETFONT, reinterpret_cast<WPARAM>(g_labelFont), TRUE);
            SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            SendMessageW(note, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            SendMessageW(close, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            UpdateDeveloperHelpTopic(hwnd);
            return 0;
        }
        case WM_SIZE:
        {
            RECT client{};
            GetClientRect(hwnd, &client);
            const int contentHeight = (std::max)(180, static_cast<int>(client.bottom) - 126);
            MoveWindow(GetDlgItem(hwnd, kDeveloperHelpTopics), 18, 102, 220,
                (std::max)(120, contentHeight - 64), TRUE);
            MoveWindow(GetDlgItem(hwnd, kDeveloperHelpText), 254, 76,
                (std::max)(300, static_cast<int>(client.right) - 272),
                (std::max)(160, contentHeight - 24), TRUE);
            MoveWindow(GetDlgItem(hwnd, kDeveloperHelpClose),
                (std::max)(18, static_cast<int>(client.right) - 108),
                (std::max)(60, static_cast<int>(client.bottom) - 48), 90, 32, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kDeveloperHelpTopics && HIWORD(wParam) == LBN_SELCHANGE)
            {
                UpdateDeveloperHelpTopic(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == kDeveloperHelpClose)
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

    void ShowDeveloperHelp(HWND owner)
    {
        static bool registered = false;
        constexpr wchar_t className[] = L"MCSTDeveloperHelpWindow";
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = DeveloperHelpProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = className;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return;
            registered = true;
        }

        HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, className,
            L"MCST-Watchdog Developer Mode Help",
            WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 660,
            owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window)
            return;

        RECT ownerRect{}, dialogRect{};
        GetWindowRect(owner, &ownerRect);
        GetWindowRect(window, &dialogRect);
        SetWindowPos(window, HWND_TOP,
            ownerRect.left + ((ownerRect.right - ownerRect.left) - (dialogRect.right - dialogRect.left)) / 2,
            ownerRect.top + ((ownerRect.bottom - ownerRect.top) - (dialogRect.bottom - dialogRect.top)) / 2,
            0, 0, SWP_NOSIZE);
        EnableWindow(owner, FALSE);
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
        MSG msg{};
        while (IsWindow(window) && GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(window, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
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
        DrawModernIndicator(dc, layout.indicatorX, y + 6, 17, item.state);

        DrawTextSimple(dc, { layout.labelLeft, y, layout.labelRight, y + 28 }, label, g_labelFont, RGB(28, 31, 36), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { layout.stateLeft, y, layout.stateRight, y + 28 }, mcst::HealthStateText(item.state), g_statusFont, StateColor(item.state), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { layout.descriptionLeft, y, layout.descriptionRight, y + 28 },
            item.value + (item.detail.empty() ? L"" : L"  -  " + item.detail),
            g_bodyFont, RGB(45, 49, 56),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    void DrawDeveloperToolsPanel(HDC dc, int clientWidth, int productionButtonY)
    {
        const DeveloperToolbarLayout layout = CalculateDeveloperToolbarLayout(productionButtonY);
        const RECT panel = { 20, layout.panelTop, clientWidth - 20, layout.panelBottom };

        HBRUSH panelBrush = CreateSolidBrush(RGB(244, 248, 253));
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(187, 203, 222));
        HGDIOBJ oldBrush = SelectObject(dc, panelBrush);
        HGDIOBJ oldPen = SelectObject(dc, borderPen);
        RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, 8, 8);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(borderPen);
        DeleteObject(panelBrush);

        RECT accent = { panel.left, panel.top + 7, panel.left + 4, panel.bottom - 7 };
        HBRUSH accentBrush = CreateSolidBrush(RGB(72, 111, 165));
        FillRect(dc, &accent, accentBrush);
        DeleteObject(accentBrush);

        DrawTextSimple(dc, { 32, layout.labelTop, clientWidth - 32, layout.labelTop + 16 },
            L"DEVELOPER TOOLS  ·  READ-ONLY DIAGNOSTICS", g_developerButtonFont,
            RGB(58, 82, 116), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    void DrawLatestActivityRows(HDC dc, const std::vector<mcst::ActivityItem>& activity,
        int& y, int bottom, int width, int rowHeight)
    {
        constexpr int labelLeft = 34;
        constexpr int labelRight = 184;
        const DashboardRowLayout rowLayout = CalculateDashboardRowLayout(width);
        const int valueLeft = rowLayout.stateLeft;
        const int detailLeft = rowLayout.descriptionLeft;

        for (const auto& item : activity)
        {
            if (y + rowHeight > bottom)
                break;

            DrawTextSimple(dc, { labelLeft, y, labelRight, y + rowHeight }, item.text,
                g_bodyFont, RGB(68, 73, 82),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            DrawTextSimple(dc, { valueLeft, y, detailLeft - 10, y + rowHeight }, FormatLocalTime(item.time),
                g_bodyFont, RGB(35, 39, 47),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            DrawTextSimple(dc, { detailLeft, y, width - 28, y + rowHeight }, item.text,
                g_bodyFont, RGB(35, 39, 47),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += rowHeight;
        }
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

        if (g_headerLogo && g_headerLogo->GetLastStatus() == Gdiplus::Ok)
        {
            Gdiplus::Graphics graphics(dc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            graphics.DrawImage(g_headerLogo.get(), Gdiplus::Rect(28, 7, 58, 44));
        }
        DrawTextSimple(dc, { 98, 8, client.right - 370, 52 }, L"MCST-Watchdog 1.20.14", g_titleFont, RGB(25, 28, 34), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const bool firstUpdateCompleted = status.lastSuccessfulUpdate.time_since_epoch().count() != 0;
        const wchar_t* overallText = mcst::OverallHeadline(status.overall, firstUpdateCompleted);

        const int overallDotLeft = client.right - 350;
        const int overallDotTop = 16;
        BYTE overallAlpha = 255;
        if (status.overall == mcst::HealthState::Healthy)
        {
            const double phase = static_cast<double>(GetTickCount64() % 4000ULL) / 4000.0;
            const double wave = (1.0 - std::cos(phase * 6.283185307179586)) * 0.5;
            overallAlpha = static_cast<BYTE>(235 + static_cast<int>(20.0 * wave));
        }
        DrawModernIndicator(dc, overallDotLeft, overallDotTop, 27, status.overall, overallAlpha);

        DrawTextSimple(dc, { client.right - 318, 8, client.right - 28, 52 }, overallText, g_titleFont, StateColor(status.overall), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        HPEN divider = CreatePen(PS_SOLID, 1, RGB(229, 232, 237));
        HGDIOBJ oldPen = SelectObject(dc, divider);
        MoveToEx(dc, 28, 62, nullptr); LineTo(dc, client.right - 28, 62);
        SelectObject(dc, oldPen);
        DeleteObject(divider);

        const std::wstring mcSuffix = status.multiChartsVersion.empty() ? L"" : L"    MC " + status.multiChartsVersion;
        const std::wstring updateText = status.lastSuccessfulUpdate.time_since_epoch().count() == 0
            ? L"Last successful system update: waiting for first successful update"
            : L"Last successful system update: " + FormatClock(status.lastSuccessfulUpdate) + L"  (" + FormatAge(status.lastSuccessfulUpdate) + L")" + mcSuffix;
        DrawTextSimple(dc, { 28, 66, client.right - 250, 92 }, updateText, g_bodyFont, RGB(68, 73, 82), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextSimple(dc, { client.right - 245, 66, client.right - 28, 92 }, L"Uptime  " + status.uptime, g_bodyFont, RGB(68, 73, 82), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        DrawTextSimple(dc, { 28, 94, client.right - 28, 124 }, L"SYSTEM STATUS", g_headerFont, RGB(43, 47, 54), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int y = 128;
        DrawStatusRow(dc, y, L"MultiCharts Health", status.multiChartsHealth, client.right); y += 30;
        DrawStatusRow(dc, y, L"Bridge", status.bridge, client.right); y += 30;
        DrawStatusRow(dc, y, L"Tracker Snapshot", status.trackerSnapshot, client.right); y += 30;
        DrawStatusRow(dc, y, L"AutoTrading", status.autoTrading, client.right); y += 30;
        DrawStatusRow(dc, y, L"Broker", status.broker, client.right); y += 30;
        DrawStatusRow(dc, y, L"Recent Logs", status.recentLogs, client.right); y += 30;
        DrawStatusRow(dc, y, L"Status Reports", status.statusReports, client.right); y += 30;
        DrawStatusRow(dc, y, L"Email", status.email, client.right); y += 30;
        DrawStatusRow(dc, y, L"Heartbeat", status.heartbeat, client.right); y += 36;

        DrawTextSimple(dc, { 28, y, client.right - 28, y + 24 }, L"LATEST ACTIVITY", g_headerFont, RGB(43, 47, 54), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 26;
        const int middle = client.right / 2;
        constexpr int latestLabelLeft = 34;
        constexpr int latestLabelRight = 184;
        constexpr int latestRowHeight = 20;
        const DashboardRowLayout latestLayout = CalculateDashboardRowLayout(client.right);
        const int latestValueLeft = latestLayout.stateLeft;
        const int latestDetailLeft = latestLayout.descriptionLeft;
        DrawTextSimple(dc, { latestLabelLeft, y, latestLabelRight, y + latestRowHeight }, L"Last Tracker attempt", g_bodyFont, RGB(68, 73, 82), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { latestValueLeft, y, middle - 10, y + latestRowHeight }, status.lastTrackerAttempt, g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += latestRowHeight;
        DrawTextSimple(dc, { latestLabelLeft, y, latestLabelRight, y + latestRowHeight }, L"Last complete snapshot", g_bodyFont, RGB(68, 73, 82), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { latestValueLeft, y, middle - 10, y + latestRowHeight }, status.lastCompleteTrackerSnapshot, g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { latestDetailLeft, y, client.right - 28, y + latestRowHeight }, L"Accounts  " + std::to_wstring(status.accountRows) + L"    Positions  " + std::to_wstring(status.openPositionRows) + L"    Logs  " + std::to_wstring(status.recentLogRows), g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += latestRowHeight;
        DrawTextSimple(dc, { latestLabelLeft, y, latestLabelRight, y + latestRowHeight }, L"Last AutoTrading Read", g_bodyFont, RGB(68, 73, 82), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { latestValueLeft, y, middle - 10, y + latestRowHeight }, status.lastAutoTradingRead.empty() ? L"Never" : status.lastAutoTradingRead, g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextSimple(dc, { latestDetailLeft, y, client.right - 28, y + latestRowHeight }, L"Uptime  " + status.uptime + L"    Memory  " + std::to_wstring(status.privateMemoryBytes / (1024 * 1024)) + L" MB    Handles  " + std::to_wstring(status.handleCount), g_bodyFont, RGB(35, 39, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const int productionButtonY = (std::max)(620, static_cast<int>(client.bottom) - 58);

        if (g_app.config.developerModeEnabled)
        {
            DrawDeveloperToolsPanel(dc, client.right, productionButtonY);
        }
        else
        {
            y += latestRowHeight;
            DrawLatestActivityRows(dc, status.activity, y, productionButtonY - 10,
                client.right, latestRowHeight);
        }
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
        g_developerButtonFont = CreateUiFont(9, FW_SEMIBOLD, L"Segoe UI");
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
            g_developerHelpButton = CreateWindowW(L"BUTTON", L"Help", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 934, 648, 70, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonDeveloperHelp)), nullptr, nullptr);
            g_developerModeCheckbox = CreateWindowW(L"BUTTON", L"Developer mode",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                730, 697, 162, 22, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckDeveloperMode)), nullptr, nullptr);
            g_reloadSettingsButton = CreateWindowW(L"BUTTON", L"Reload Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 628, 648, 150, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonReloadSettings)), nullptr, nullptr);
            g_testEmailButton = CreateWindowW(L"BUTTON", L"Send Test Email", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 788, 648, 150, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonTestEmail)), nullptr, nullptr);
            g_autoMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 220, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAutoMenu)), nullptr, nullptr);
            g_statusMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 310, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonStatusMenu)), nullptr, nullptr);
            g_emailMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 340, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonEmailMenu)), nullptr, nullptr);
            g_heartbeatMenuButton = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 930, 370, 30, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonHeartbeatMenu)), nullptr, nullptr);
            for (HWND button : { g_refreshButton, g_reportButton, g_settingsButton, g_openFolderButton, g_openSettingsButton, g_autoTradingDiagnosticsButton, g_autoTradingCaptureButton, g_autoTradingFinishButton, g_trackerResearchButton, g_positionCurrencyResearchButton, g_openCompatibilityButton, g_reloadCompatibilityButton, g_developerHelpButton, g_developerModeCheckbox, g_reloadSettingsButton, g_testEmailButton, g_autoMenuButton, g_statusMenuButton, g_emailMenuButton, g_heartbeatMenuButton })
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            for (HWND button : { g_autoTradingDiagnosticsButton, g_autoTradingCaptureButton, g_autoTradingFinishButton, g_trackerResearchButton, g_positionCurrencyResearchButton, g_openCompatibilityButton, g_reloadCompatibilityButton, g_developerHelpButton })
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
            info->ptMinTrackSize.x = 920;
            info->ptMinTrackSize.y = 720;
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kButtonAutoMenu: ShowRowMenu(hwnd, g_autoMenuButton, kButtonAutoMenu); return 0;
            case kButtonStatusMenu: ShowRowMenu(hwnd, g_statusMenuButton, kButtonStatusMenu); return 0;
            case kButtonEmailMenu: ShowRowMenu(hwnd, g_emailMenuButton, kButtonEmailMenu); return 0;
            case kButtonHeartbeatMenu: ShowRowMenu(hwnd, g_heartbeatMenuButton, kButtonHeartbeatMenu); return 0;
            case kButtonDeveloperHelp:
                ShowDeveloperHelp(hwnd);
                return 0;
            case kCheckDeveloperMode:
                if (HIWORD(wParam) == BN_CLICKED)
                {
                    const bool enabled = SendMessageW(g_developerModeCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    std::wstring error;
                    if (!SaveDeveloperModeEnabled(enabled, error))
                    {
                        SendMessageW(g_developerModeCheckbox, BM_SETCHECK,
                            g_app.config.developerModeEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                        MessageBoxW(hwnd, error.c_str(), L"Developer mode", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    g_app.config.developerModeEnabled = enabled;
                    UpdateDeveloperControlVisibility(hwnd);
                    return 0;
                }
                break;
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
                SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config, L"MCST-Watchdog Test Email", L"MCST-Watchdog email configuration is working.\r\n\r\nVersion: 1.20.14", false, L"Test email", false, g_app.config.alertEmailTo);
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
                    dialogMessage = L"Position CCY R16 requires MCST Tracker Bridge V" +
                        std::to_wstring(kPositionCurrencyResearchBridgeVersion) +
                        L" or newer. The currently loaded Bridge is V" +
                        std::to_wstring(referenceSnapshot.bridgeVersion) +
                        L". Replace C:\\MCExtras\\MCST-TrackerBridge.dll with the V175 build from this package and restart MultiCharts.";
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
                        L"The primary R16 result is MCST_Position_Currency_Dynamic_<pid>.txt. "
                        L"It verifies the exact ATCenterProxy position interface, correlates its objects with visible rows, and reads the two bounded currency strings without calling unknown MultiCharts functions.";
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
                const bool previouslyShowingStaleTrackerData = g_app.status.trackerDataStale;
                const bool previouslyCriticalStaleTrackerData = g_app.status.trackerDataStaleCritical;
                const mcst::HealthState previousTrackerState = g_app.status.trackerSnapshot.state;
                const mcst::HealthState previousMultiChartsHealthState = g_app.status.multiChartsHealth.state;
                std::vector<unsigned long> previousVisibleWarningPids;
                for (const auto& process : g_app.status.multiChartsProcesses.processes)
                    if (process.visibleQueueWarningConfirmed)
                        previousVisibleWarningPids.push_back(process.processId);
                const bool multiChartsHealthPreviouslyObserved = g_app.multiChartsHealthObserved;
                const int active = result->status.autoTradingActive;
                TrackerStatusSnapshot liveSnapshot = std::move(result->snapshot);
                const bool liveTrackerSnapshotComplete = IsCompleteTrackerSnapshot(liveSnapshot);

                g_app.status = std::move(result->status);
                const auto monitorNow = std::chrono::system_clock::now();
                if (liveTrackerSnapshotComplete)
                {
                    g_app.lastGoodTrackerSnapshot = liveSnapshot;
                    g_app.lastGoodTrackerSnapshotTime = monitorNow;
                    g_app.hasLastGoodTrackerSnapshot = true;
                    g_app.snapshot = liveSnapshot;
                    g_app.status.trackerDataStale = false;
                    g_app.status.trackerDataStaleCritical = false;
                    g_app.status.trackerDataStaleAgeMinutes = 0;
                    g_app.status.trackerDataTimestamp.clear();
                    g_app.status.lastCompleteTrackerSnapshot = g_app.status.lastTrackerAttempt;
                    if (previouslyShowingStaleTrackerData)
                    {
                        AddActivity(
                            g_app.status,
                            mcst::HealthState::Healthy,
                            L"Live Tracker table data recovered without restarting Watchdog");
                    }
                }
                else if (g_app.hasLastGoodTrackerSnapshot)
                {
                    // Preserve every table that is live and replace only failed
                    // sections with their last complete counterpart.
                    g_app.snapshot = liveSnapshot;
                    if (!g_app.snapshot.accounts.ok)
                        g_app.snapshot.accounts = g_app.lastGoodTrackerSnapshot.accounts;
                    if (!g_app.snapshot.openPositions.ok)
                        g_app.snapshot.openPositions = g_app.lastGoodTrackerSnapshot.openPositions;
                    if (!g_app.snapshot.positionHistory.ok &&
                        g_app.lastGoodTrackerSnapshot.positionHistory.ok)
                    {
                        g_app.snapshot.positionHistory = g_app.lastGoodTrackerSnapshot.positionHistory;
                    }
                    if (!g_app.snapshot.recentLogs.ok)
                        g_app.snapshot.recentLogs = g_app.lastGoodTrackerSnapshot.recentLogs;
                    g_app.status.trackerDataStale = true;
                    g_app.status.trackerDataTimestamp = FormatLocalTime(g_app.lastGoodTrackerSnapshotTime);
                    g_app.status.lastCompleteTrackerSnapshot = g_app.status.trackerDataTimestamp;
                    const long long staleAgeSeconds = (std::max)(0LL, static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            monitorNow - g_app.lastGoodTrackerSnapshotTime).count()));
                    const long long criticalAfterSeconds =
                        static_cast<long long>((std::max)(1, g_app.config.trackerStaleCriticalAfterMinutes)) * 60LL;
                    const bool staleCritical = staleAgeSeconds >= criticalAfterSeconds;
                    const mcst::HealthState staleState = staleCritical
                        ? mcst::HealthState::Critical
                        : mcst::HealthState::Attention;
                    g_app.status.trackerDataStaleCritical = staleCritical;
                    g_app.status.trackerDataStaleAgeMinutes = static_cast<int>(staleAgeSeconds / 60LL);
                    g_app.status.trackerDataStaleCriticalAfterMinutes =
                        g_app.config.trackerStaleCriticalAfterMinutes;
                    g_app.status.trackerSnapshot.state = staleState;
                    g_app.status.trackerSnapshot.value = staleCritical
                        ? L"Read failed / STALE data"
                        : L"STALE data / retrying";
                    g_app.status.accountRows = g_app.snapshot.accounts.rows.size();
                    g_app.status.openPositionRows = g_app.snapshot.openPositions.rows.size();
                    g_app.status.recentLogRows = g_app.snapshot.recentLogs.rows.size();

                    std::wstring staleDetail =
                        L"One or more failed tables use STALE data from " +
                        g_app.status.trackerDataTimestamp + L"; current Tracker read is incomplete; stale age " +
                        std::to_wstring(g_app.status.trackerDataStaleAgeMinutes) + L" min";
                    if (!staleCritical)
                    {
                        const long long remainingMinutes =
                            (criticalAfterSeconds - staleAgeSeconds + 59LL) / 60LL;
                        staleDetail += L"; CRITICAL in " + std::to_wstring(remainingMinutes) + L" min if reading does not recover";
                    }
                    if (!g_app.status.trackerSnapshot.detail.empty())
                        g_app.status.trackerSnapshot.detail += L" - ";
                    g_app.status.trackerSnapshot.detail += staleDetail;
                    if (!liveSnapshot.recentLogs.ok)
                    {
                        g_app.status.recentLogs.state = staleState;
                        g_app.status.recentLogs.value = staleCritical
                            ? L"Read failed / STALE data"
                            : L"STALE data / retrying";
                        if (!g_app.status.recentLogs.detail.empty())
                            g_app.status.recentLogs.detail += L" - ";
                        g_app.status.recentLogs.detail += staleDetail;
                    }
                    if (!previouslyShowingStaleTrackerData)
                    {
                        AddActivity(
                            g_app.status,
                            staleState,
                            staleCritical
                                ? L"Live Tracker read failed; the last complete snapshot is already beyond the CRITICAL threshold"
                                : L"Live Tracker read failed; using the last complete snapshot during the stale-data grace interval");
                    }
                    else if (staleCritical && !previouslyCriticalStaleTrackerData)
                    {
                        AddActivity(
                            g_app.status,
                            mcst::HealthState::Critical,
                            L"Tracker stale-data interval reached the CRITICAL threshold");
                    }
                }
                else
                {
                    g_app.snapshot = liveSnapshot;
                    g_app.status.lastCompleteTrackerSnapshot = L"Never";
                    if (g_app.status.trackerSnapshot.state != previousTrackerState)
                    {
                        AddActivity(
                            g_app.status,
                            g_app.status.trackerSnapshot.state,
                            L"Tracker snapshot state changed to " + g_app.status.trackerSnapshot.value);
                    }
                }
                g_app.status.lastReport = g_app.lastReport;
                g_app.status.lastAlert = g_app.lastAlert;

                if (liveSnapshot.recentLogs.present && liveSnapshot.recentLogs.ok)
                {
                    const bool baseline = !g_app.hasGoodLogs ||
                        g_app.lastLogsProcessId != liveSnapshot.processId;
                    const std::size_t added = baseline ? 0 : mcst::CountAddedLogRows(
                        g_app.lastGoodLogs.rows, liveSnapshot.recentLogs.rows);
                    g_app.lastGoodLogs = liveSnapshot.recentLogs;
                    g_app.lastLogsProcessId = liveSnapshot.processId;
                    g_app.hasGoodLogs = true;
                    g_app.lastLogsRead = FormatLocalTime(monitorNow);
                    g_app.status.recentLogs = { mcst::HealthState::Healthy,
                        std::to_wstring(liveSnapshot.recentLogs.rows.size()) + L" rows",
                        baseline ? L"Read OK - baseline captured" : added > 0
                            ? L"Read OK - " + std::to_wstring(added) + L" newly observed row(s)"
                            : L"Read OK - no new events" };
                    g_app.status.recentLogs.detail += L" - Last read " + g_app.lastLogsRead;
                    if (!liveSnapshot.recentLogs.rows.empty() && !liveSnapshot.recentLogs.rows.front().empty())
                        g_app.status.recentLogs.detail += L" - First displayed event " +
                            liveSnapshot.recentLogs.rows.front().front();
                }
                else
                {
                    // Never label cached rows as live or feed them to alert engines.
                    g_app.status.recentLogs.value = L"Read unavailable";
                    if (g_app.hasGoodLogs && g_app.lastLogsProcessId == liveSnapshot.processId)
                    {
                        g_app.snapshot.recentLogs = g_app.lastGoodLogs;
                        g_app.status.recentLogRows = g_app.lastGoodLogs.rows.size();
                        g_app.status.recentLogs.detail += L" - Cached rows; last successful Logs read " + g_app.lastLogsRead;
                    }
                    else
                        g_app.status.recentLogs.detail += L" - No successful Logs read for this source";
                }

                const BrokerMonitorDecision brokerDecision = g_app.brokerMonitor.Evaluate(
                    MonitoringLogs(liveSnapshot), result->brokerAuthentication, g_app.config, monitorNow);
                g_app.status.broker = brokerDecision.status;
                RecalculateOverallStatus(g_app.status, g_app.config, true);

                bool newVisibleQueueWarning = false;
                for (const auto& process : g_app.status.multiChartsProcesses.processes)
                    if (process.visibleQueueWarningConfirmed &&
                        std::find(previousVisibleWarningPids.begin(), previousVisibleWarningPids.end(),
                            process.processId) == previousVisibleWarningPids.end())
                        newVisibleQueueWarning = true;
                const bool multiChartsHealthChanged = !multiChartsHealthPreviouslyObserved ||
                    g_app.status.multiChartsHealth.state != previousMultiChartsHealthState || newVisibleQueueWarning;
                g_app.multiChartsHealthObserved = true;
                if (multiChartsHealthChanged)
                {
                    const bool recovered = multiChartsHealthPreviouslyObserved &&
                        g_app.status.multiChartsHealth.state == mcst::HealthState::Healthy;
                    const std::wstring healthEvent = recovered
                        ? L"MultiCharts health recovered: " + g_app.status.multiChartsHealth.value + L" - " + g_app.status.multiChartsHealth.detail
                        : L"MultiCharts health changed to " +
                            std::wstring(mcst::HealthStateText(g_app.status.multiChartsHealth.state)) + L": " +
                            g_app.status.multiChartsHealth.value + L" - " + g_app.status.multiChartsHealth.detail;
                    AddActivity(g_app.status, g_app.status.multiChartsHealth.state, healthEvent);

                    const bool shouldEmail = g_app.config.emailEnabled &&
                        !g_app.config.alertEmailTo.empty() &&
                        !g_app.multiChartsHealthEmailInFlight &&
                        (g_app.status.multiChartsHealth.state == mcst::HealthState::Attention ||
                         g_app.status.multiChartsHealth.state == mcst::HealthState::Critical || recovered);
                    if (shouldEmail)
                    {
                        g_app.multiChartsHealthEmailInFlight = true;
                        g_app.lastAlert = FormatLocalTime(monitorNow) + L" - " + healthEvent;
                        g_app.status.lastAlert = g_app.lastAlert;
                        const std::wstring report = BuildStatusReport(g_app.status, g_app.snapshot);
                        SendEmailAsync(hwnd, WM_APP_EMAIL_COMPLETE, g_app.config,
                            recovered ? L"MCST-Watchdog MULTICHARTS RECOVERED"
                                      : L"MCST-Watchdog MULTICHARTS HEALTH ALERT",
                            BuildAlertWithStatusReportHtml(healthEvent, report), true,
                            recovered ? L"MultiCharts health recovery email"
                                      : L"MultiCharts health alert email",
                            true, g_app.config.alertEmailTo);
                    }
                }
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
                    MonitoringLogs(liveSnapshot), g_app.config, monitorNow);
                if (logAlertDecision.eventCount > 0)
                {
                    g_app.status.recentLogs.state = Worst(g_app.status.recentLogs.state, logAlertDecision.state);
                    g_app.status.recentLogs.detail += L" - " + std::to_wstring(logAlertDecision.eventCount) + L" new alert match(es)";
                    RecalculateOverallStatus(g_app.status, g_app.config, true);
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

                mcst::MergeActivityHistory(g_app.status.activity, previousActivity, 10);
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
                else if (result->eventText == L"MultiCharts health alert email" ||
                         result->eventText == L"MultiCharts health recovery email")
                    g_app.multiChartsHealthEmailInFlight = false;

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
    int probeResult = 0;
    if (mcstprobe::TryWorkerMode(probeResult)) return probeResult;
    // This is deliberately the first executable application code. If the process reaches
    // wWinMain, a marker should be visible either beside the EXE or in C:\Temp.
    SetUnhandledExceptionFilter(StartupUnhandledExceptionFilter);
    AppendStartupLogSafe(L"Startup -1: entered wWinMain before application initialization");

    HANDLE singleInstanceMutex = nullptr;
    bool gdiplusStarted = false;

    try
    {
        AppendStartupLogSafe(L"Startup 0: MCST-Watchdog 1.20.14 process entered protected startup");

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
        g_headerLogo = LoadPngResource(instance, IDR_WATCHDOG_HEADER_LOGO);
        AppendStartupLogSafe(g_headerLogo ? L"Startup 2a: header logo OK" : L"Startup 2a: header logo unavailable; continuing without it");

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
        g_app.status.lastTrackerAttempt = L"Never";
        g_app.status.lastCompleteTrackerSnapshot = L"Never";
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
            0, kWindowClass, L"MCST-Watchdog 1.20.14 - MC16 + MC17 AutoTrading",
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
        g_headerLogo.reset();
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

    g_headerLogo.reset();
    if (gdiplusStarted)
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
    if (singleInstanceMutex)
        CloseHandle(singleInstanceMutex);
    return 1;
}
