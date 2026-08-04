#pragma once

#include <windows.h>

#include <string>

struct AppConfig
{
    int refreshSeconds = 10;
    int bridgeTimeoutMilliseconds = 5000;
    int snapshotRetryCount = 3;
    int snapshotRetryDelayMilliseconds = 250;

    bool autoTradingMonitoringEnabled = true;
    int autoTradingMinimum = 65;
    int autoTradingCheckMinutes = 5;

    bool statusReportsEnabled = false;
    bool emailEnabled = false;
    bool heartbeatEnabled = false;

    int windowLeft = -1;
    int windowTop = -1;
    int windowWidth = 980;
    int windowHeight = 790;
    bool windowMaximized = false;

    std::wstring reportPath;
    std::wstring rawSnapshotPath;
};

std::wstring GetApplicationDirectory();
std::wstring GetConfigPath();
AppConfig LoadAppConfig();
void EnsureDefaultConfigFile(const std::wstring& path);
void SaveWindowPlacementToConfig(const WINDOWPLACEMENT& placement);
