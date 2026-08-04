#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "AppConfig.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace
{
    int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback, int minValue, int maxValue)
    {
        const int value = static_cast<int>(GetPrivateProfileIntW(section, key, fallback, path.c_str()));
        return (std::max)(minValue, (std::min)(maxValue, value));
    }

    bool ReadBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool fallback)
    {
        wchar_t buffer[32]{};
        GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false", buffer, 32, path.c_str());
        std::wstring value(buffer);
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return value == L"true" || value == L"1" || value == L"yes" || value == L"on";
    }

    std::wstring ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& fallback)
    {
        wchar_t buffer[2048]{};
        GetPrivateProfileStringW(section, key, fallback.c_str(), buffer, 2048, path.c_str());
        return buffer;
    }
}

std::wstring GetApplicationDirectory()
{
    wchar_t path[MAX_PATH]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (count == 0 || count >= MAX_PATH)
        return L".";
    return std::filesystem::path(path).parent_path().wstring();
}

std::wstring GetConfigPath()
{
    return (std::filesystem::path(GetApplicationDirectory()) / L"MCST-Watchdog.ini").wstring();
}

void EnsureDefaultConfigFile(const std::wstring& path)
{
    if (std::filesystem::exists(path))
        return;

    WritePrivateProfileStringW(L"General", L"version", L"0.578", path.c_str());
    WritePrivateProfileStringW(L"Dashboard", L"refresh_seconds", L"10", path.c_str());
    WritePrivateProfileStringW(L"Bridge", L"timeout_milliseconds", L"5000", path.c_str());
    WritePrivateProfileStringW(L"Bridge", L"snapshot_retry_count", L"3", path.c_str());
    WritePrivateProfileStringW(L"Bridge", L"snapshot_retry_delay_milliseconds", L"250", path.c_str());
    WritePrivateProfileStringW(L"AutoTrading", L"enabled", L"true", path.c_str());
    WritePrivateProfileStringW(L"AutoTrading", L"minimum_active_strategies", L"65", path.c_str());
    WritePrivateProfileStringW(L"AutoTrading", L"check_interval_minutes", L"5", path.c_str());
    WritePrivateProfileStringW(L"AutoTradingDiagnostics", L"expected_active_strategies", L"18", path.c_str());
    WritePrivateProfileStringW(L"StatusReport", L"enabled", L"false", path.c_str());
    WritePrivateProfileStringW(L"Email", L"enabled", L"false", path.c_str());
    WritePrivateProfileStringW(L"Heartbeat", L"enabled", L"false", path.c_str());
    WritePrivateProfileStringW(L"Window", L"left", L"-1", path.c_str());
    WritePrivateProfileStringW(L"Window", L"top", L"-1", path.c_str());
    WritePrivateProfileStringW(L"Window", L"width", L"980", path.c_str());
    WritePrivateProfileStringW(L"Window", L"height", L"790", path.c_str());
    WritePrivateProfileStringW(L"Window", L"maximized", L"false", path.c_str());
    WritePrivateProfileStringW(L"Files", L"report_path", L"", path.c_str());
    WritePrivateProfileStringW(L"Files", L"raw_snapshot_path", L"", path.c_str());
}

AppConfig LoadAppConfig()
{
    const std::wstring path = GetConfigPath();
    EnsureDefaultConfigFile(path);
    WritePrivateProfileStringW(L"General", L"version", L"0.578", path.c_str());

    AppConfig config;
    config.refreshSeconds = ReadInt(path, L"Dashboard", L"refresh_seconds", 10, 2, 3600);
    config.bridgeTimeoutMilliseconds = ReadInt(path, L"Bridge", L"timeout_milliseconds", 5000, 500, 60000);
    config.snapshotRetryCount = ReadInt(path, L"Bridge", L"snapshot_retry_count", 3, 1, 10);
    config.snapshotRetryDelayMilliseconds = ReadInt(path, L"Bridge", L"snapshot_retry_delay_milliseconds", 250, 0, 10000);
    config.autoTradingMonitoringEnabled = ReadBool(path, L"AutoTrading", L"enabled", true);
    config.autoTradingMinimum = ReadInt(path, L"AutoTrading", L"minimum_active_strategies", 65, 0, 10000);
    config.autoTradingCheckMinutes = ReadInt(path, L"AutoTrading", L"check_interval_minutes", 5, 1, 1440);
    config.autoTradingExpectedActiveForDiagnostics = ReadInt(path, L"AutoTradingDiagnostics", L"expected_active_strategies", 18, 0, 10000);
    config.statusReportsEnabled = ReadBool(path, L"StatusReport", L"enabled", false);
    config.emailEnabled = ReadBool(path, L"Email", L"enabled", false);
    config.heartbeatEnabled = ReadBool(path, L"Heartbeat", L"enabled", false);
    config.windowLeft = ReadInt(path, L"Window", L"left", -1, -32000, 32000);
    config.windowTop = ReadInt(path, L"Window", L"top", -1, -32000, 32000);
    config.windowWidth = ReadInt(path, L"Window", L"width", 980, 760, 3840);
    config.windowHeight = ReadInt(path, L"Window", L"height", 790, 620, 2160);
    config.windowMaximized = ReadBool(path, L"Window", L"maximized", false);

    const std::filesystem::path appDir(GetApplicationDirectory());
    config.reportPath = ReadString(path, L"Files", L"report_path", L"");
    config.rawSnapshotPath = ReadString(path, L"Files", L"raw_snapshot_path", L"");
    if (config.reportPath.empty())
        config.reportPath = (appDir / L"MCST-Watchdog-StatusReport.txt").wstring();
    if (config.rawSnapshotPath.empty())
        config.rawSnapshotPath = (appDir / L"MCST-Watchdog-LatestSnapshot.tsv").wstring();
    return config;
}

void SaveWindowPlacementToConfig(const WINDOWPLACEMENT& placement)
{
    const std::wstring path = GetConfigPath();
    const RECT& normal = placement.rcNormalPosition;
    const int width = static_cast<int>((std::max)(760L, normal.right - normal.left));
    const int height = static_cast<int>((std::max)(620L, normal.bottom - normal.top));

    WritePrivateProfileStringW(L"Window", L"left", std::to_wstring(normal.left).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Window", L"top", std::to_wstring(normal.top).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Window", L"width", std::to_wstring(width).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Window", L"height", std::to_wstring(height).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Window", L"maximized", placement.showCmd == SW_SHOWMAXIMIZED ? L"true" : L"false", path.c_str());
}
