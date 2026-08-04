#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "AppConfig.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <iterator>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    constexpr wchar_t kMissingMarker[] = L"{MCST_KEY_MISSING_8C6B0E2D}";

    std::wstring Trim(std::wstring value)
    {
        const auto notSpace = [](wchar_t ch) { return std::iswspace(ch) == 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    bool TryReadRaw(const std::wstring& path, const wchar_t* section, const wchar_t* key, std::wstring& value)
    {
        wchar_t buffer[4096]{};
        GetPrivateProfileStringW(section, key, kMissingMarker, buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
        value = buffer;
        return value != kMissingMarker;
    }

    bool TryParseBool(const std::wstring& raw, bool& value)
    {
        const std::wstring normalized = ToLower(Trim(raw));
        if (normalized == L"true" || normalized == L"1" || normalized == L"yes" || normalized == L"on")
        {
            value = true;
            return true;
        }
        if (normalized == L"false" || normalized == L"0" || normalized == L"no" || normalized == L"off")
        {
            value = false;
            return true;
        }
        return false;
    }

    bool TryParseInt(const std::wstring& raw, int& value)
    {
        const std::wstring normalized = Trim(raw);
        if (normalized.empty())
            return false;

        wchar_t* end = nullptr;
        errno = 0;
        const long parsed = std::wcstol(normalized.c_str(), &end, 10);
        if (errno == ERANGE || end == normalized.c_str() || *end != L'\0' || parsed < INT_MIN || parsed > INT_MAX)
            return false;

        value = static_cast<int>(parsed);
        return true;
    }

    void WriteValue(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& value)
    {
        WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
    }

    std::wstring SettingName(const wchar_t* section, const wchar_t* key)
    {
        return L"[" + std::wstring(section) + L"] " + key;
    }

    void EnsureStringKey(const std::wstring& path, const wchar_t* section, const wchar_t* key,
        const wchar_t* fallback, std::vector<std::wstring>& changes)
    {
        std::wstring raw;
        if (TryReadRaw(path, section, key, raw))
            return;

        WriteValue(path, section, key, fallback);
        changes.push_back(SettingName(section, key) + L" was missing; default value was written.");
    }

    void EnsureBoolKey(const std::wstring& path, const wchar_t* section, const wchar_t* key,
        bool fallback, std::vector<std::wstring>& changes)
    {
        std::wstring raw;
        bool parsed = false;
        if (!TryReadRaw(path, section, key, raw))
        {
            WriteValue(path, section, key, fallback ? L"true" : L"false");
            changes.push_back(SettingName(section, key) + L" was missing; default " + (fallback ? L"true" : L"false") + L" was written.");
            return;
        }

        if (TryParseBool(raw, parsed))
            return;

        WriteValue(path, section, key, fallback ? L"true" : L"false");
        changes.push_back(SettingName(section, key) + L" had invalid value '" + raw + L"'; default " +
            (fallback ? L"true" : L"false") + L" was written.");
    }

    void EnsureIntKey(const std::wstring& path, const wchar_t* section, const wchar_t* key,
        int fallback, int minValue, int maxValue, std::vector<std::wstring>& changes)
    {
        std::wstring raw;
        int parsed = fallback;
        if (!TryReadRaw(path, section, key, raw))
        {
            WriteValue(path, section, key, std::to_wstring(fallback));
            changes.push_back(SettingName(section, key) + L" was missing; default " + std::to_wstring(fallback) + L" was written.");
            return;
        }

        if (!TryParseInt(raw, parsed))
        {
            WriteValue(path, section, key, std::to_wstring(fallback));
            changes.push_back(SettingName(section, key) + L" had invalid value '" + raw + L"'; default " +
                std::to_wstring(fallback) + L" was written.");
            return;
        }

        const int corrected = (std::max)(minValue, (std::min)(maxValue, parsed));
        if (corrected != parsed)
        {
            WriteValue(path, section, key, std::to_wstring(corrected));
            changes.push_back(SettingName(section, key) + L" was outside the allowed range; corrected to " +
                std::to_wstring(corrected) + L".");
        }
    }

    void AppendNormalizationLog(const std::wstring& configPath, const std::vector<std::wstring>& changes)
    {
        if (changes.empty())
            return;

        const std::filesystem::path logPath = std::filesystem::path(configPath).parent_path() / L"MCST-Watchdog-ConfigNormalization.log";
        std::wofstream out(logPath, std::ios::app);
        if (!out)
            return;

        SYSTEMTIME local{};
        GetLocalTime(&local);
        out << L"\n[" << local.wYear << L"-";
        out << (local.wMonth < 10 ? L"0" : L"") << local.wMonth << L"-";
        out << (local.wDay < 10 ? L"0" : L"") << local.wDay << L" ";
        out << (local.wHour < 10 ? L"0" : L"") << local.wHour << L":";
        out << (local.wMinute < 10 ? L"0" : L"") << local.wMinute << L"]\n";
        for (const auto& change : changes)
            out << L"- " << change << L"\n";
    }

    int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback, int minValue, int maxValue)
    {
        std::wstring raw;
        int value = fallback;
        if (!TryReadRaw(path, section, key, raw) || !TryParseInt(raw, value))
            value = fallback;
        return (std::max)(minValue, (std::min)(maxValue, value));
    }

    bool ReadBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool fallback)
    {
        std::wstring raw;
        bool value = fallback;
        if (!TryReadRaw(path, section, key, raw) || !TryParseBool(raw, value))
            return fallback;
        return value;
    }

    std::wstring ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& fallback)
    {
        std::wstring value;
        return TryReadRaw(path, section, key, value) ? value : fallback;
    }
    std::vector<std::wstring> SplitPatterns(const std::wstring& value)
    {
        std::vector<std::wstring> result;
        std::wstring current;
        for (wchar_t ch : value)
        {
            if (ch == L'|')
            {
                current = Trim(current);
                if (!current.empty()) result.push_back(current);
                current.clear();
            }
            else
            {
                current.push_back(ch);
            }
        }
        current = Trim(current);
        if (!current.empty()) result.push_back(current);
        return result;
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

std::vector<std::wstring> NormalizeConfigFile(const std::wstring& path)
{
    std::vector<std::wstring> changes;

    // Version is owned by the program and is always updated to the current build.
    WriteValue(path, L"General", L"version", L"1.01");

    EnsureIntKey(path, L"Dashboard", L"refresh_seconds", 10, 2, 3600, changes);
    EnsureBoolKey(path, L"Developer", L"enabled", false, changes);
    EnsureIntKey(path, L"Bridge", L"timeout_milliseconds", 5000, 500, 60000, changes);
    EnsureIntKey(path, L"Bridge", L"snapshot_retry_count", 3, 1, 10, changes);
    EnsureIntKey(path, L"Bridge", L"snapshot_retry_delay_milliseconds", 250, 0, 10000, changes);

    EnsureBoolKey(path, L"AutoTrading", L"enabled", true, changes);
    EnsureIntKey(path, L"AutoTrading", L"minimum_active_strategies", 65, 0, 10000, changes);
    EnsureIntKey(path, L"AutoTrading", L"check_interval_minutes", 5, 1, 1440, changes);
    EnsureIntKey(path, L"AutoTradingDiagnostics", L"expected_active_strategies", 18, 0, 10000, changes);

    EnsureBoolKey(path, L"StatusReport", L"enabled", false, changes);
    EnsureIntKey(path, L"StatusReport", L"interval_minutes", 60, 1, 10080, changes);
    EnsureBoolKey(path, L"StatusReport", L"send_on_startup", false, changes);

    EnsureBoolKey(path, L"Email", L"enabled", true, changes);
    EnsureStringKey(path, L"Email", L"smtp_server", L"", changes);
    EnsureIntKey(path, L"Email", L"smtp_port", 587, 1, 65535, changes);
    EnsureBoolKey(path, L"Email", L"use_ssl", true, changes);
    EnsureStringKey(path, L"Email", L"smtp_user", L"", changes);
    EnsureStringKey(path, L"Email", L"smtp_password", L"", changes);
    EnsureStringKey(path, L"Email", L"from", L"", changes);
    EnsureStringKey(path, L"Email", L"to", L"", changes);
    EnsureBoolKey(path, L"Email", L"autotrading_alerts", true, changes);
    EnsureBoolKey(path, L"Email", L"autotrading_recovery", true, changes);

    EnsureBoolKey(path, L"Heartbeat", L"enabled", true, changes);
    EnsureIntKey(path, L"Heartbeat", L"interval_minutes", 60, 1, 10080, changes);
    EnsureBoolKey(path, L"Heartbeat", L"send_on_startup", false, changes);

    EnsureBoolKey(path, L"BrokerMonitor", L"enabled", true, changes);
    EnsureIntKey(path, L"BrokerMonitor", L"disconnect_grace_seconds", 60, 10, 3600, changes);
    EnsureBoolKey(path, L"BrokerMonitor", L"alert_email", true, changes);
    EnsureBoolKey(path, L"BrokerMonitor", L"recovery_email", true, changes);
    EnsureStringKey(path, L"BrokerMonitor", L"disconnect_patterns", L"connection lost|connection disconnected|broker disconnected|connection closed", changes);
    EnsureStringKey(path, L"BrokerMonitor", L"reconnecting_patterns", L"reconnecting|reconnect attempt|trying to connect", changes);
    EnsureStringKey(path, L"BrokerMonitor", L"connected_patterns", L"connection restored|reconnected|connection established|logged on", changes);
    EnsureStringKey(path, L"DeveloperTools", L"universal_application_mapper_path", L"C:\\MCExtras\\UniversalApplicationMapper.exe", changes);

    EnsureIntKey(path, L"Window", L"left", -1, -32000, 32000, changes);
    EnsureIntKey(path, L"Window", L"top", -1, -32000, 32000, changes);
    EnsureIntKey(path, L"Window", L"width", 980, 760, 3840, changes);
    EnsureIntKey(path, L"Window", L"height", 790, 620, 2160, changes);
    EnsureBoolKey(path, L"Window", L"maximized", false, changes);

    EnsureStringKey(path, L"Files", L"report_path", L"", changes);
    EnsureStringKey(path, L"Files", L"raw_snapshot_path", L"", changes);

    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    AppendNormalizationLog(path, changes);
    return changes;
}

void EnsureDefaultConfigFile(const std::wstring& path)
{
    NormalizeConfigFile(path);
}

AppConfig LoadAppConfig()
{
    const std::wstring path = GetConfigPath();
    AppConfig config;
    config.normalizationMessages = NormalizeConfigFile(path);

    config.refreshSeconds = ReadInt(path, L"Dashboard", L"refresh_seconds", 10, 2, 3600);
    config.developerModeEnabled = ReadBool(path, L"Developer", L"enabled", false);
    config.bridgeTimeoutMilliseconds = ReadInt(path, L"Bridge", L"timeout_milliseconds", 5000, 500, 60000);
    config.snapshotRetryCount = ReadInt(path, L"Bridge", L"snapshot_retry_count", 3, 1, 10);
    config.snapshotRetryDelayMilliseconds = ReadInt(path, L"Bridge", L"snapshot_retry_delay_milliseconds", 250, 0, 10000);
    config.autoTradingMonitoringEnabled = ReadBool(path, L"AutoTrading", L"enabled", true);
    config.autoTradingMinimum = ReadInt(path, L"AutoTrading", L"minimum_active_strategies", 65, 0, 10000);
    config.autoTradingCheckMinutes = ReadInt(path, L"AutoTrading", L"check_interval_minutes", 5, 1, 1440);
    config.autoTradingExpectedActiveForDiagnostics = ReadInt(path, L"AutoTradingDiagnostics", L"expected_active_strategies", 18, 0, 10000);
    config.statusReportsEnabled = ReadBool(path, L"StatusReport", L"enabled", false);
    config.statusReportIntervalMinutes = ReadInt(path, L"StatusReport", L"interval_minutes", 60, 1, 10080);
    config.statusReportSendOnStartup = ReadBool(path, L"StatusReport", L"send_on_startup", false);
    config.emailEnabledSettingPresent = true;
    config.emailEnabled = ReadBool(path, L"Email", L"enabled", true);
    config.smtpServer = ReadString(path, L"Email", L"smtp_server", L"");
    config.smtpPort = ReadInt(path, L"Email", L"smtp_port", 587, 1, 65535);
    config.smtpUseSsl = ReadBool(path, L"Email", L"use_ssl", true);
    config.smtpUser = ReadString(path, L"Email", L"smtp_user", L"");
    config.smtpPassword = ReadString(path, L"Email", L"smtp_password", L"");
    config.emailFrom = ReadString(path, L"Email", L"from", L"");
    config.emailTo = ReadString(path, L"Email", L"to", L"");
    config.autoTradingAlertEmailEnabled = ReadBool(path, L"Email", L"autotrading_alerts", true);
    config.autoTradingRecoveryEmailEnabled = ReadBool(path, L"Email", L"autotrading_recovery", true);
    config.heartbeatEnabledSettingPresent = true;
    config.heartbeatEnabled = ReadBool(path, L"Heartbeat", L"enabled", true);
    config.heartbeatIntervalMinutes = ReadInt(path, L"Heartbeat", L"interval_minutes", 60, 1, 10080);
    config.heartbeatSendOnStartup = ReadBool(path, L"Heartbeat", L"send_on_startup", false);
    config.brokerMonitoringEnabled = ReadBool(path, L"BrokerMonitor", L"enabled", true);
    config.brokerDisconnectGraceSeconds = ReadInt(path, L"BrokerMonitor", L"disconnect_grace_seconds", 60, 10, 3600);
    config.brokerAlertEmailEnabled = ReadBool(path, L"BrokerMonitor", L"alert_email", true);
    config.brokerRecoveryEmailEnabled = ReadBool(path, L"BrokerMonitor", L"recovery_email", true);
    config.brokerDisconnectPatterns = SplitPatterns(ReadString(path, L"BrokerMonitor", L"disconnect_patterns", L"connection lost|connection disconnected|broker disconnected|connection closed"));
    config.brokerReconnectingPatterns = SplitPatterns(ReadString(path, L"BrokerMonitor", L"reconnecting_patterns", L"reconnecting|reconnect attempt|trying to connect"));
    config.brokerConnectedPatterns = SplitPatterns(ReadString(path, L"BrokerMonitor", L"connected_patterns", L"connection restored|reconnected|connection established|logged on"));
    config.universalApplicationMapperPath = ReadString(path, L"DeveloperTools", L"universal_application_mapper_path", L"C:\\MCExtras\\UniversalApplicationMapper.exe");
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
