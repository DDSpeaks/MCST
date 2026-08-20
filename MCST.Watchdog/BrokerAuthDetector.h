#pragma once

#include "AppConfig.h"

#include <string>

struct BrokerAuthenticationDetection
{
    bool detected = false;
    std::wstring profileName;
    std::wstring browserTitle;
    std::wstring sanitizedUrl;
    std::wstring matchedPattern;
    int alertAfterSeconds = 10;
};

// Detects broker authentication pages in supported browser windows without
// interacting with the browser. The detector reads bounded Win32 and UI
// Automation accessibility evidence and never records URL query strings,
// fragments, credentials, or raw page text.
BrokerAuthenticationDetection DetectBrokerAuthentication(const AppConfig& config);
