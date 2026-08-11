#pragma once

#include <windows.h>
#include <string>

struct MultiChartsVersionInfo
{
    bool detected = false;
    DWORD processId = 0;
    std::wstring executablePath;
    std::wstring executableName;
    std::wstring productName;
    std::wstring productVersion;
    std::wstring fileVersion;
    std::wstring displayVersion;
    bool chartingDetected = false;
    DWORD chartingPeTimestamp = 0;
    unsigned long long chartingImageSize = 0;
    bool atonpTrackerDetected = false;
    DWORD atonpTrackerPeTimestamp = 0;
    unsigned long long atonpTrackerImageSize = 0;
    std::wstring diagnostic;
};

MultiChartsVersionInfo DetectMultiChartsVersion(DWORD processId);
void WriteDetectedMultiChartsInfoToIni(
    const MultiChartsVersionInfo& info,
    const std::wstring& autoTradingCompatibilityProfile,
    const std::wstring& trackerCompatibilityProfile);
