#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct AppConfig
{
    int refreshSeconds = 10;
    bool developerModeEnabled = false;
    int bridgeTimeoutMilliseconds = 5000;
    int snapshotRetryCount = 3;
    int snapshotRetryDelayMilliseconds = 250;

    bool autoTradingMonitoringEnabled = true;
    int autoTradingMinimum = 65;
    int autoTradingCheckMinutes = 5;
    int autoTradingExpectedActiveForDiagnostics = 18;

    bool statusReportsEnabled = false;
    int statusReportIntervalMinutes = 60;
    bool statusReportSendOnStartup = false;
    bool emailEnabled = true;
    bool emailEnabledSettingPresent = false;
    std::wstring smtpServer;
    int smtpPort = 587;
    bool smtpUseSsl = true;
    std::wstring smtpUser;
    std::wstring smtpPassword;
    std::wstring emailFrom;
    std::wstring emailTo;
    bool autoTradingAlertEmailEnabled = true;
    bool autoTradingRecoveryEmailEnabled = true;
    bool heartbeatEnabled = true;
    bool heartbeatEnabledSettingPresent = false;
    int heartbeatIntervalMinutes = 60;
    bool heartbeatSendOnStartup = false;

    int windowLeft = -1;
    int windowTop = -1;
    int windowWidth = 980;
    int windowHeight = 790;
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
