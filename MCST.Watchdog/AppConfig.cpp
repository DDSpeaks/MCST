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
#include <initializer_list>
#include <sstream>
#include <utility>

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

    void EnsureChoiceKey(
        const std::wstring& path,
        const wchar_t* section,
        const wchar_t* key,
        const wchar_t* fallback,
        std::initializer_list<const wchar_t*> choices,
        std::vector<std::wstring>& changes)
    {
        std::wstring raw;
        if (TryReadRaw(path, section, key, raw))
        {
            const std::wstring normalized = ToLower(Trim(raw));
            for (const wchar_t* choice : choices)
            {
                if (normalized == choice)
                {
                    if (raw != normalized)
                        WriteValue(path, section, key, normalized);
                    return;
                }
            }
            WriteValue(path, section, key, fallback);
            changes.push_back(SettingName(section, key) + L" had invalid value '" + raw +
                L"'; default " + fallback + L" was written.");
            return;
        }

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

    std::vector<std::wstring> ReadSectionNames(const std::wstring& path)
    {
        std::vector<wchar_t> buffer(32768, L'\0');
        const DWORD copied = GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
        std::vector<std::wstring> result;
        if (copied == 0 || copied >= buffer.size() - 2)
            return result;

        const wchar_t* current = buffer.data();
        while (*current != L'\0')
        {
            result.emplace_back(current);
            current += std::wcslen(current) + 1;
        }
        return result;
    }

    std::vector<BrokerAuthProfile> ReadBrokerAuthProfiles(const std::wstring& path)
    {
        std::vector<BrokerAuthProfile> profiles;
        constexpr wchar_t prefix[] = L"BrokerAuth.";
        const std::wstring lowerPrefix = ToLower(prefix);
        for (const auto& section : ReadSectionNames(path))
        {
            const std::wstring lowerSection = ToLower(section);
            if (lowerSection.rfind(lowerPrefix, 0) != 0)
                continue;

            BrokerAuthProfile profile;
            profile.sectionName = section;
            const std::wstring suffix = section.substr(std::size(prefix) - 1);
            profile.enabled = ReadBool(path, section.c_str(), L"enabled", true);
            profile.name = Trim(ReadString(path, section.c_str(), L"name", suffix));
            if (profile.name.empty())
                profile.name = suffix;
            profile.urlContains = SplitPatterns(ReadString(path, section.c_str(), L"url_contains", L""));
            profile.titleContains = SplitPatterns(ReadString(path, section.c_str(), L"title_contains", L""));
            profile.titleOnlyContains = SplitPatterns(ReadString(path, section.c_str(), L"title_only_contains", L""));
            profile.textContains = SplitPatterns(ReadString(path, section.c_str(), L"text_contains", L""));
            profile.recoveryLogContains = SplitPatterns(ReadString(path, section.c_str(), L"recovery_log_contains", L""));
            profile.alertAfterSeconds = ReadInt(path, section.c_str(), L"alert_after_seconds", 10, 3, 600);

            if (!profile.urlContains.empty() || !profile.titleContains.empty() ||
                !profile.titleOnlyContains.empty() || !profile.textContains.empty())
                profiles.push_back(std::move(profile));
        }
        return profiles;
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

static void RefreshIniProfileCache(const std::wstring& path)
{
    // Windows profile APIs maintain process-level compatibility caching.
    // A null-section/key/value write tells the profile subsystem to flush and
    // refresh its view of this INI file before a manual Reload Settings read.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

std::vector<std::wstring> NormalizeConfigFile(const std::wstring& path)
{
    std::vector<std::wstring> changes;

    const std::wstring previousVersion = ReadString(path, L"General", L"version", L"");

    // Migrate the original production key once when upgrading to 1.07.
    if (previousVersion != L"1.07" && ReadBool(path, L"StatusReport", L"send_immediately_on_start", false))
        WriteValue(path, L"StatusReport", L"send_on_startup", L"true");

    // Version is owned by the program and is always updated to the current build.
    WriteValue(path, L"General", L"version", L"1.114-R44");

    EnsureIntKey(path, L"Dashboard", L"refresh_seconds", 10, 2, 3600, changes);
    EnsureBoolKey(path, L"Developer", L"enabled", false, changes);
    EnsureIntKey(path, L"Bridge", L"timeout_milliseconds", 5000, 500, 60000, changes);
    EnsureIntKey(path, L"Bridge", L"snapshot_retry_count", 3, 1, 10, changes);
    EnsureIntKey(path, L"Bridge", L"snapshot_retry_delay_milliseconds", 250, 0, 10000, changes);
    EnsureIntKey(path, L"TrackerMonitor", L"stale_critical_after_minutes", 10, 1, 1440, changes);
    EnsureChoiceKey(
        path, L"Tracker", L"date_order", L"auto",
        { L"auto", L"dmy", L"mdy", L"ymd" }, changes);

    EnsureBoolKey(path, L"AutoTrading", L"enabled", true, changes);
    EnsureIntKey(path, L"AutoTrading", L"minimum_active_strategies", 65, 0, 10000, changes);
    EnsureIntKey(path, L"AutoTrading", L"check_interval_minutes", 5, 1, 1440, changes);
    EnsureIntKey(path, L"AutoTradingDiagnostics", L"expected_active_strategies", 18, 0, 10000, changes);

    // Preserve the original Watchdog startup semantics. Older production INI files
    // used send_immediately_on_start instead of send_on_startup.
    if (ReadBool(path, L"StatusReport", L"send_immediately_on_start", false))
        WriteValue(path, L"StatusReport", L"send_on_startup", L"true");

    EnsureBoolKey(path, L"StatusReport", L"enabled", true, changes);
    EnsureIntKey(path, L"StatusReport", L"interval_minutes", 60, 1, 10080, changes);
    EnsureBoolKey(path, L"StatusReport", L"send_on_startup", false, changes);
    EnsureStringKey(path, L"StatusReport", L"weekdays", L"mon-fri", changes);
    EnsureStringKey(path, L"StatusReport", L"send_start", L"00:00", changes);
    EnsureStringKey(path, L"StatusReport", L"send_end", L"23:59", changes);

    EnsureBoolKey(path, L"Email", L"enabled", true, changes);
    EnsureStringKey(path, L"Email", L"smtp_server", L"", changes);
    EnsureIntKey(path, L"Email", L"smtp_port", 587, 1, 65535, changes);
    EnsureBoolKey(path, L"Email", L"use_ssl", true, changes);
    EnsureStringKey(path, L"Email", L"smtp_user", L"", changes);
    EnsureStringKey(path, L"Email", L"smtp_password", L"", changes);
    EnsureStringKey(path, L"Email", L"from", L"", changes);
    EnsureStringKey(path, L"Email", L"to", L"", changes);
    // alert_to and report_to provide strict channel separation while keeping
    // the legacy [Email] to and [StatusReport] to keys compatible.
    std::wstring legacyEmailTo = ReadString(path, L"Email", L"to", L"");
    std::wstring legacyReportTo = ReadString(path, L"StatusReport", L"to", L"");
    EnsureStringKey(path, L"Email", L"alert_to", legacyEmailTo.c_str(), changes);
    EnsureStringKey(path, L"Email", L"report_to", legacyReportTo.empty() ? legacyEmailTo.c_str() : legacyReportTo.c_str(), changes);
    EnsureStringKey(path, L"StatusReport", L"to", legacyReportTo.empty() ? legacyEmailTo.c_str() : legacyReportTo.c_str(), changes);
    EnsureBoolKey(path, L"Email", L"autotrading_alerts", true, changes);
    EnsureBoolKey(path, L"Email", L"autotrading_recovery", true, changes);

    EnsureBoolKey(path, L"LogAlerts", L"enabled", true, changes);
    EnsureBoolKey(path, L"LogAlerts", L"email_enabled", true, changes);
    EnsureBoolKey(path, L"LogAlerts", L"notify_existing_on_startup", false, changes);
    EnsureIntKey(path, L"LogAlerts", L"deduplication_minutes", 60, 0, 10080, changes);
    EnsureStringKey(path, L"LogAlerts", L"fatal_keywords",
        L"fatal|unhandled exception|access violation|application crash", changes);
    EnsureStringKey(path, L"LogAlerts", L"critical_keywords",
        L"status: rejected|order: rejected|invalid stop price|order failed|boxed positions are not permitted", changes);
    EnsureStringKey(path, L"LogAlerts", L"warning_keywords", L"", changes);
    EnsureStringKey(path, L"LogAlerts", L"ignore_keywords",
        L"simulated trades are not shown on historical data", changes);

    EnsureBoolKey(path, L"Heartbeat", L"enabled", true, changes);
    EnsureIntKey(path, L"Heartbeat", L"interval_minutes", 60, 1, 10080, changes);
    EnsureBoolKey(path, L"Heartbeat", L"send_on_startup", false, changes);

    EnsureBoolKey(path, L"BrokerMonitor", L"enabled", true, changes);
    EnsureIntKey(path, L"BrokerMonitor", L"disconnect_grace_seconds", 60, 10, 3600, changes);
    EnsureIntKey(path, L"BrokerMonitor", L"state_cache_max_age_minutes", 1440, 1, 10080, changes);
    EnsureBoolKey(path, L"BrokerMonitor", L"alert_email", true, changes);
    EnsureBoolKey(path, L"BrokerMonitor", L"recovery_email", true, changes);
    EnsureStringKey(path, L"BrokerMonitor", L"disconnect_patterns", L"connection lost|connection disconnected|broker disconnected|connection closed", changes);
    EnsureStringKey(path, L"BrokerMonitor", L"reconnecting_patterns", L"reconnecting|reconnect attempt|trying to connect", changes);
    EnsureStringKey(path, L"BrokerMonitor", L"connected_patterns", L"connection restored|reconnected|connection established|connected to|successfully connected|session established|session connected|trading system connected|login successful|logon successful|authentication successful|connection with tradestation established|connection to tradestation established|connection with saxo established|connection to saxo established|logged on|logged in", changes);

    // Browser authentication profiles are deliberately data-driven. A detected
    // broker login URL is stronger current evidence than historical Recent Logs.
    EnsureBoolKey(path, L"BrokerAuth.Saxo", L"enabled", true, changes);
    EnsureStringKey(path, L"BrokerAuth.Saxo", L"name", L"Saxo", changes);
    EnsureStringKey(path, L"BrokerAuth.Saxo", L"url_contains", L"developer.saxobank.com/login", changes);
    EnsureStringKey(path, L"BrokerAuth.Saxo", L"title_contains", L"MultiCharts (OpenAPI Web App)|Saxo", changes);
    EnsureStringKey(path, L"BrokerAuth.Saxo", L"title_only_contains", L"MultiCharts (OpenAPI Web App)", changes);
    EnsureStringKey(path, L"BrokerAuth.Saxo", L"text_contains", L"login|account authentication", changes);
    EnsureStringKey(path, L"BrokerAuth.Saxo", L"recovery_log_contains", L"saxo|saxo group", changes);
    EnsureIntKey(path, L"BrokerAuth.Saxo", L"alert_after_seconds", 10, 3, 600, changes);

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
    RefreshIniProfileCache(path);

    AppConfig config;
    config.normalizationMessages = NormalizeConfigFile(path);
    // Normalization may have written missing/default values. Flush once more so
    // every subsequent read in this load observes the same on-disk generation.
    RefreshIniProfileCache(path);

    config.refreshSeconds = ReadInt(path, L"Dashboard", L"refresh_seconds", 10, 2, 3600);
    config.developerModeEnabled = ReadBool(path, L"Developer", L"enabled", false);
    config.bridgeTimeoutMilliseconds = ReadInt(path, L"Bridge", L"timeout_milliseconds", 5000, 500, 60000);
    config.snapshotRetryCount = ReadInt(path, L"Bridge", L"snapshot_retry_count", 3, 1, 10);
    config.snapshotRetryDelayMilliseconds = ReadInt(path, L"Bridge", L"snapshot_retry_delay_milliseconds", 250, 0, 10000);
    config.trackerStaleCriticalAfterMinutes = ReadInt(
        path, L"TrackerMonitor", L"stale_critical_after_minutes", 10, 1, 1440);
    config.trackerDateOrder = ReadString(path, L"Tracker", L"date_order", L"auto");
    config.autoTradingMonitoringEnabled = ReadBool(path, L"AutoTrading", L"enabled", true);
    config.autoTradingMinimum = ReadInt(path, L"AutoTrading", L"minimum_active_strategies", 65, 0, 10000);
    config.autoTradingCheckMinutes = ReadInt(path, L"AutoTrading", L"check_interval_minutes", 5, 1, 1440);
    config.autoTradingExpectedActiveForDiagnostics = ReadInt(path, L"AutoTradingDiagnostics", L"expected_active_strategies", 18, 0, 10000);
    config.statusReportsEnabled = ReadBool(path, L"StatusReport", L"enabled", true);
    config.statusReportIntervalMinutes = ReadInt(path, L"StatusReport", L"interval_minutes", 60, 1, 10080);
    config.statusReportSendOnStartup = ReadBool(path, L"StatusReport", L"send_on_startup", false) || ReadBool(path, L"StatusReport", L"send_immediately_on_start", false);
    config.statusReportWeekdays = ReadString(path, L"StatusReport", L"weekdays", L"mon-fri");
    config.statusReportSendStart = ReadString(path, L"StatusReport", L"send_start", L"00:00");
    config.statusReportSendEnd = ReadString(path, L"StatusReport", L"send_end", L"23:59");
    config.emailEnabledSettingPresent = true;
    config.emailEnabled = ReadBool(path, L"Email", L"enabled", true);
    config.smtpServer = ReadString(path, L"Email", L"smtp_server", L"");
    config.smtpPort = ReadInt(path, L"Email", L"smtp_port", 587, 1, 65535);
    config.smtpUseSsl = ReadBool(path, L"Email", L"use_ssl", true);
    config.smtpUser = ReadString(path, L"Email", L"smtp_user", L"");
    config.smtpPassword = ReadString(path, L"Email", L"smtp_password", L"");
    config.emailFrom = ReadString(path, L"Email", L"from", L"");
    config.emailTo = ReadString(path, L"Email", L"to", L"");
    config.alertEmailTo = ReadString(path, L"Email", L"alert_to", config.emailTo);
    const std::wstring emailReportFallback = ReadString(path, L"Email", L"report_to", config.emailTo);
    config.reportEmailTo = ReadString(path, L"StatusReport", L"to", emailReportFallback);
    config.autoTradingAlertEmailEnabled = ReadBool(path, L"Email", L"autotrading_alerts", true);
    config.autoTradingRecoveryEmailEnabled = ReadBool(path, L"Email", L"autotrading_recovery", true);
    config.logAlertsEnabled = ReadBool(path, L"LogAlerts", L"enabled", true);
    config.logAlertEmailEnabled = ReadBool(path, L"LogAlerts", L"email_enabled", true);
    config.logAlertNotifyExistingOnStartup = ReadBool(path, L"LogAlerts", L"notify_existing_on_startup", false);
    config.logAlertDeduplicationMinutes = ReadInt(path, L"LogAlerts", L"deduplication_minutes", 60, 0, 10080);
    config.logAlertFatalKeywords = SplitPatterns(ReadString(path, L"LogAlerts", L"fatal_keywords",
        L"fatal|unhandled exception|access violation|application crash"));
    config.logAlertCriticalKeywords = SplitPatterns(ReadString(path, L"LogAlerts", L"critical_keywords",
        L"status: rejected|order: rejected|invalid stop price|order failed|boxed positions are not permitted"));
    config.logAlertWarningKeywords = SplitPatterns(ReadString(path, L"LogAlerts", L"warning_keywords", L""));
    config.logAlertIgnoreKeywords = SplitPatterns(ReadString(path, L"LogAlerts", L"ignore_keywords",
        L"simulated trades are not shown on historical data"));
    config.heartbeatEnabledSettingPresent = true;
    config.heartbeatEnabled = ReadBool(path, L"Heartbeat", L"enabled", true);
    config.heartbeatIntervalMinutes = ReadInt(path, L"Heartbeat", L"interval_minutes", 60, 1, 10080);
    config.heartbeatSendOnStartup = ReadBool(path, L"Heartbeat", L"send_on_startup", false);
    config.brokerMonitoringEnabled = ReadBool(path, L"BrokerMonitor", L"enabled", true);
    config.brokerDisconnectGraceSeconds = ReadInt(path, L"BrokerMonitor", L"disconnect_grace_seconds", 60, 10, 3600);
    config.brokerStateCacheMaxAgeMinutes = ReadInt(path, L"BrokerMonitor", L"state_cache_max_age_minutes", 1440, 1, 10080);
    config.brokerAlertEmailEnabled = ReadBool(path, L"BrokerMonitor", L"alert_email", true);
    config.brokerRecoveryEmailEnabled = ReadBool(path, L"BrokerMonitor", L"recovery_email", true);
    config.brokerDisconnectPatterns = SplitPatterns(ReadString(path, L"BrokerMonitor", L"disconnect_patterns", L"connection lost|connection disconnected|broker disconnected|connection closed"));
    config.brokerReconnectingPatterns = SplitPatterns(ReadString(path, L"BrokerMonitor", L"reconnecting_patterns", L"reconnecting|reconnect attempt|trying to connect"));
    config.brokerConnectedPatterns = SplitPatterns(ReadString(path, L"BrokerMonitor", L"connected_patterns", L"connection restored|reconnected|connection established|connected to|successfully connected|session established|session connected|trading system connected|login successful|logon successful|authentication successful|connection with tradestation established|connection to tradestation established|connection with saxo established|connection to saxo established|logged on|logged in"));
    config.brokerAuthProfiles = ReadBrokerAuthProfiles(path);
    config.universalApplicationMapperPath = ReadString(path, L"DeveloperTools", L"universal_application_mapper_path", L"C:\\MCExtras\\UniversalApplicationMapper.exe");
    config.windowLeft = ReadInt(path, L"Window", L"left", -1, -32000, 32000);
    config.windowTop = ReadInt(path, L"Window", L"top", -1, -32000, 32000);
    config.windowWidth = ReadInt(path, L"Window", L"width", 980, 760, 3840);
    config.windowHeight = ReadInt(path, L"Window", L"height", 810, 720, 2160);
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


bool SaveStatusReportSettings(bool enabled, const std::wstring& recipient, int intervalMinutes, bool sendOnStartup, const std::wstring& weekdays, const std::wstring& sendStart, const std::wstring& sendEnd, std::wstring& errorOut)
{
    const std::wstring path = GetConfigPath();
    const std::wstring trimmedRecipient = Trim(recipient);
    if (trimmedRecipient.empty())
    {
        errorOut = L"Status Report recipient cannot be empty.";
        return false;
    }
    if (intervalMinutes < 1 || intervalMinutes > 10080)
    {
        errorOut = L"Status Report interval must be between 1 and 10080 minutes.";
        return false;
    }

    const std::wstring trimmedWeekdays = Trim(weekdays);
    const std::wstring trimmedStart = Trim(sendStart);
    const std::wstring trimmedEnd = Trim(sendEnd);
    if (trimmedWeekdays.empty() || trimmedStart.empty() || trimmedEnd.empty())
    {
        errorOut = L"Weekdays and sending window values cannot be empty.";
        return false;
    }

    const bool ok =
        WritePrivateProfileStringW(L"StatusReport", L"enabled", enabled ? L"true" : L"false", path.c_str()) &&
        WritePrivateProfileStringW(L"StatusReport", L"to", trimmedRecipient.c_str(), path.c_str()) &&
        WritePrivateProfileStringW(L"StatusReport", L"interval_minutes", std::to_wstring(intervalMinutes).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(L"StatusReport", L"send_on_startup", sendOnStartup ? L"true" : L"false", path.c_str()) &&
        WritePrivateProfileStringW(L"StatusReport", L"send_immediately_on_start", sendOnStartup ? L"true" : L"false", path.c_str()) &&
        WritePrivateProfileStringW(L"StatusReport", L"weekdays", trimmedWeekdays.c_str(), path.c_str()) &&
        WritePrivateProfileStringW(L"StatusReport", L"send_start", trimmedStart.c_str(), path.c_str()) &&
        WritePrivateProfileStringW(L"StatusReport", L"send_end", trimmedEnd.c_str(), path.c_str()) &&
        WritePrivateProfileStringW(L"Email", L"report_to", trimmedRecipient.c_str(), path.c_str());

    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    if (!ok)
    {
        errorOut = L"Windows could not write the Status Report settings to MCST-Watchdog.ini.";
        return false;
    }
    errorOut.clear();
    return true;
}

bool SaveDeveloperModeEnabled(bool enabled, std::wstring& errorOut)
{
    const std::wstring path = GetConfigPath();
    const bool ok = WritePrivateProfileStringW(
        L"Developer", L"enabled", enabled ? L"true" : L"false", path.c_str()) != FALSE;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    if (!ok)
    {
        errorOut = L"Windows could not save Developer mode to MCST-Watchdog.ini.";
        return false;
    }
    errorOut.clear();
    return true;
}


bool SaveAutoTradingSettings(bool enabled, int minimumActive, int intervalMinutes, bool alertEmail, bool recoveryEmail, std::wstring& errorOut)
{
    if (minimumActive < 0 || minimumActive > 10000 || intervalMinutes < 1 || intervalMinutes > 1440)
    {
        errorOut = L"AutoTrading values are outside the allowed range.";
        return false;
    }
    const std::wstring path = GetConfigPath();
    const bool ok =
        WritePrivateProfileStringW(L"AutoTrading", L"enabled", enabled ? L"true" : L"false", path.c_str()) &&
        WritePrivateProfileStringW(L"AutoTrading", L"minimum_active_strategies", std::to_wstring(minimumActive).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(L"AutoTrading", L"check_interval_minutes", std::to_wstring(intervalMinutes).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(L"Email", L"autotrading_alerts", alertEmail ? L"true" : L"false", path.c_str()) &&
        WritePrivateProfileStringW(L"Email", L"autotrading_recovery", recoveryEmail ? L"true" : L"false", path.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    if (!ok) errorOut = L"Unable to write AutoTrading settings to MCST-Watchdog.ini.";
    return ok;
}

bool SaveEmailSettings(bool enabled, const std::wstring& server, int port, bool useSsl, const std::wstring& user, const std::wstring& password, const std::wstring& from, const std::wstring& alertTo, const std::wstring& reportTo, std::wstring& errorOut)
{
    if (port < 1 || port > 65535)
    {
        errorOut = L"SMTP port must be between 1 and 65535.";
        return false;
    }
    const std::wstring path = GetConfigPath();
    bool ok = true;
    auto put=[&](const wchar_t* section,const wchar_t* key,const std::wstring& value){ ok = WritePrivateProfileStringW(section,key,value.c_str(),path.c_str()) && ok; };
    put(L"Email",L"enabled",enabled?L"true":L"false");
    put(L"Email",L"smtp_server",server);
    put(L"Email",L"smtp_port",std::to_wstring(port));
    put(L"Email",L"use_ssl",useSsl?L"true":L"false");
    put(L"Email",L"smtp_user",user);
    if (!password.empty()) put(L"Email",L"smtp_password",password);
    put(L"Email",L"from",from);
    put(L"Email",L"alert_to",alertTo);
    put(L"Email",L"to",alertTo);
    put(L"Email",L"report_to",reportTo);
    put(L"StatusReport",L"to",reportTo);
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());
    if (!ok) errorOut=L"Unable to write Email settings to MCST-Watchdog.ini.";
    return ok;
}

bool SaveHeartbeatSettings(bool enabled, int intervalMinutes, bool sendOnStartup, std::wstring& errorOut)
{
    if (intervalMinutes < 1 || intervalMinutes > 10080)
    {
        errorOut = L"Heartbeat interval must be between 1 and 10080 minutes.";
        return false;
    }
    const std::wstring path=GetConfigPath();
    const bool ok=
        WritePrivateProfileStringW(L"Heartbeat",L"enabled",enabled?L"true":L"false",path.c_str()) &&
        WritePrivateProfileStringW(L"Heartbeat",L"interval_minutes",std::to_wstring(intervalMinutes).c_str(),path.c_str()) &&
        WritePrivateProfileStringW(L"Heartbeat",L"send_on_startup",sendOnStartup?L"true":L"false",path.c_str());
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());
    if(!ok) errorOut=L"Unable to write Heartbeat settings to MCST-Watchdog.ini.";
    return ok;
}
