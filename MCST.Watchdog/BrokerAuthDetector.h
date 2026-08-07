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
// interacting with the browser. The detector reads the address-bar value by
// the proven Win32 browser window/child-text inspection used by the original production Watchdog and never records URL query strings or fragments.
BrokerAuthenticationDetection DetectBrokerAuthentication(const AppConfig& config);
