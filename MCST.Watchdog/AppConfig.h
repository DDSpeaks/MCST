#pragma once

#include <windows.h>

#include <string>
#include <vector>


struct BrokerAuthProfile
{
    bool enabled = true;
    std::wstring sectionName;
    std::wstring name;
    std::vector<std::wstring> urlContains;
    std::vector<std::wstring> titleContains;
    std::vector<std::wstring> titleOnlyContains;
    std::vector<std::wstring> textContains;
    std::vector<std::wstring> recoveryLogContains;
    int alertAfterSeconds = 10;
};

struct AppConfig
{
    int refreshSeconds = 10;
    bool developerModeEnabled = false;
    int bridgeTimeoutMilliseconds = 5000;
    int snapshotRetryCount = 3;
    int snapshotRetryDelayMilliseconds = 250;
    int trackerStaleCriticalAfterMinutes = 10;
    std::wstring trackerDateOrder = L"auto";

    bool autoTradingMonitoringEnabled = true;
    int autoTradingMinimum = 65;
    int autoTradingCheckMinutes = 5;
    int autoTradingExpectedActiveForDiagnostics = 18;

    bool statusReportsEnabled = false;
    int statusReportIntervalMinutes = 60;
    bool statusReportSendOnStartup = false;
    std::wstring statusReportWeekdays = L"mon-fri";
    std::wstring statusReportSendStart = L"00:00";
    std::wstring statusReportSendEnd = L"23:59";
    bool emailEnabled = true;
    bool emailEnabledSettingPresent = false;
    std::wstring smtpServer;
    int smtpPort = 587;
    bool smtpUseSsl = true;
    std::wstring smtpUser;
    std::wstring smtpPassword;
    std::wstring emailFrom;
    std::wstring emailTo;
    std::wstring alertEmailTo;
    std::wstring reportEmailTo;
    bool autoTradingAlertEmailEnabled = true;
    bool autoTradingRecoveryEmailEnabled = true;
    bool heartbeatEnabled = true;
    bool heartbeatEnabledSettingPresent = false;
    int heartbeatIntervalMinutes = 60;
    bool heartbeatSendOnStartup = false;

    bool brokerMonitoringEnabled = true;
    int brokerDisconnectGraceSeconds = 60;
    int brokerStateCacheMaxAgeMinutes = 1440;
    bool brokerAlertEmailEnabled = true;
    bool brokerRecoveryEmailEnabled = true;

    bool logAlertsEnabled = true;
    bool logAlertEmailEnabled = true;
    bool logAlertNotifyExistingOnStartup = false;
    int logAlertDeduplicationMinutes = 60;
    std::vector<std::wstring> logAlertFatalKeywords;
    std::vector<std::wstring> logAlertCriticalKeywords;
    std::vector<std::wstring> logAlertWarningKeywords;
    std::vector<std::wstring> logAlertIgnoreKeywords;

    std::vector<std::wstring> brokerDisconnectPatterns;
    std::vector<std::wstring> brokerReconnectingPatterns;
    std::vector<std::wstring> brokerConnectedPatterns;
    std::vector<BrokerAuthProfile> brokerAuthProfiles;
    std::wstring universalApplicationMapperPath;

    int windowLeft = -1;
    int windowTop = -1;
    int windowWidth = 980;
    int windowHeight = 960;
    bool windowMaximized = false;

    std::wstring reportPath;
    std::wstring rawSnapshotPath;

    std::vector<std::wstring> normalizationMessages;
};

std::wstring GetApplicationDirectory();
std::wstring GetConfigPath();
AppConfig LoadAppConfig();
void EnsureDefaultConfigFile(const std::wstring& path);
std::vector<std::wstring> NormalizeConfigFile(const std::wstring& path);
void SaveWindowPlacementToConfig(const WINDOWPLACEMENT& placement);
bool SaveStatusReportSettings(bool enabled, const std::wstring& recipient, int intervalMinutes, bool sendOnStartup, const std::wstring& weekdays, const std::wstring& sendStart, const std::wstring& sendEnd, std::wstring& errorOut);
bool SaveAutoTradingSettings(bool enabled, int minimumActive, int intervalMinutes, bool alertEmail, bool recoveryEmail, std::wstring& errorOut);
bool SaveEmailSettings(bool enabled, const std::wstring& server, int port, bool useSsl, const std::wstring& user, const std::wstring& password, const std::wstring& from, const std::wstring& alertTo, const std::wstring& reportTo, std::wstring& errorOut);
bool SaveHeartbeatSettings(bool enabled, int intervalMinutes, bool sendOnStartup, std::wstring& errorOut);
bool SaveDeveloperModeEnabled(bool enabled, std::wstring& errorOut);
