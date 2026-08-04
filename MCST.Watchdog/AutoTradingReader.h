#pragma once

#include <chrono>
#include <string>

struct AutoTradingReadResult
{
    bool succeeded = false;
    bool fromCache = false;
    int activeStrategies = 0;
    int strategyObjectsFound = 0;
    int readFailures = 0;
    int processesScanned = 0;
    std::chrono::system_clock::time_point lastAttempt{};
    std::chrono::system_clock::time_point lastSuccessfulRead{};
    std::wstring diagnostic;
};

// Passive MultiCharts AutoTrading reader.
// Uses module enumeration, VirtualQueryEx and ReadProcessMemory only.
// It does not activate windows, click controls, send input or write process memory.
AutoTradingReadResult ReadAutoTradingStatus(int cacheMinutes, bool forceRefresh = false);

// Writes a passive compatibility report for a newly updated MultiCharts version.
// The report does not modify MultiCharts or send UI input.
bool WriteAutoTradingCompatibilityDiagnostics(const std::wstring& path, std::wstring& diagnostic);
