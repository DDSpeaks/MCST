#pragma once

#include <chrono>
#include <string>

struct AutoTradingReadResult
{
    bool succeeded = false;
    bool fromCache = false;
    bool autoAdapted = false;
    int activeStrategies = 0;
    int strategyObjectsFound = 0;
    int readFailures = 0;
    int processesScanned = 0;
    std::chrono::system_clock::time_point lastAttempt{};
    std::chrono::system_clock::time_point lastSuccessfulRead{};
    std::wstring compatibilityProfile;
    std::wstring compatibilitySource;
    std::wstring diagnostic;
};

// Passive MultiCharts AutoTrading reader.
// Uses module enumeration, VirtualQueryEx and ReadProcessMemory only.
// It does not activate windows, click controls, send input or write process memory.
AutoTradingReadResult ReadAutoTradingStatus(int cacheMinutes, bool forceRefresh = false);

// Writes a passive compatibility report for a newly updated MultiCharts version.
// The report does not modify MultiCharts or send UI input.
bool WriteAutoTradingCompatibilityDiagnostics(const std::wstring& path, std::wstring& diagnostic);

// Verifies candidate vtable/field combinations against a known current AutoTrading ON count.
// This is passive and reads MultiCharts memory only.
bool WriteAutoTradingCandidateVerification(const std::wstring& path, int expectedActiveStrategies, std::wstring& diagnostic);

// Two-stage controlled toggle verification for the strongest 19-object candidates.
// First call stores a baseline; second call compares after one strategy is toggled OFF.
bool RunAutoTradingToggleVerification(const std::wstring& reportPath, const std::wstring& baselinePath, std::wstring& diagnostic);

// Two-stage dynamic field-change detector for a newly updated MultiCharts build.
// The baseline captures candidate object bytes; the comparison reports 1->0 and 0->1 changes.
bool RunAutoTradingDynamicChangeDetection(const std::wstring& reportPath, const std::wstring& baselinePath, std::wstring& diagnostic);

// Reads a small set of strongest candidate fields and writes their live ON/OFF counts.
// This is passive and can be run repeatedly while strategies are toggled.
bool WriteAutoTradingCandidateMonitor(const std::wstring& reportPath, std::wstring& diagnostic);

// Automated multi-snapshot research session for an unknown MultiCharts build.
// AT Start dynamically maps the active Charting.dll candidate objects; the user
// toggles one chart between captures, and AT Finish ranks exact 0/1 field changes.
// All reads are passive. Verified production compatibility profiles are not changed.
bool StartAutoTradingResearchSession(const std::wstring& historyPath, std::wstring& diagnostic);
bool CaptureAutoTradingResearchSnapshot(const std::wstring& historyPath, std::wstring& diagnostic);
bool FinishAutoTradingResearchSession(const std::wstring& historyPath, std::wstring& diagnostic);
