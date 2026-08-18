#pragma once

inline constexpr wchar_t kTrackerBridgeProductVersion[] = L"1.0";
inline constexpr int kTrackerBridgeInternalBuildVersion = 156;
inline constexpr int kPositionCurrencyResearchBridgeVersion = 171;

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct TrackerBridgeSection
{
    bool present = false;
    bool ok = false;
    std::size_t expectedColumns = 0;
    std::size_t declaredRows = 0;
    std::uint32_t reportedCapacity = 0;
    std::size_t callsAttempted = 0;
    std::size_t callsSucceeded = 0;
    std::size_t sehFailures = 0;
    std::wstring diagnostic;
    std::vector<std::vector<std::wstring>> rows;
};

struct TrackerStatusSnapshot
{
    int bridgeVersion = 0;
    int protocolVersion = 0;
    unsigned long processId = 0;
    std::wstring capturedUtc;
    bool trackerFound = false;
    bool trackerSameProcess = false;
    bool atonpTrackerLoaded = false;
    std::uint32_t atonpTrackerPeTimestamp = 0;
    std::uint64_t atonpTrackerImageSize = 0;
    bool trackerCompatibilityMatched = false;
    std::wstring trackerCompatibilityMode;
    std::wstring trackerCompatibilityProfile;
    std::wstring trackerCompatibilitySource;
    std::wstring trackerCompatibilityDiagnostic;
    bool recoveryAttempted = false;
    std::wstring recoveryResult;
    TrackerBridgeSection accounts;
    TrackerBridgeSection openPositions;
    // Optional V175 Position History capture. It is report enrichment only and
    // is not part of the three-section Tracker health/recovery decision.
    TrackerBridgeSection positionHistory;
    TrackerBridgeSection recentLogs;
    // Optional R21 extended history used by state/alert engines. The normal
    // Recent Logs section remains limited to ten display rows.
    TrackerBridgeSection monitoringLogs;
    std::size_t pagesOk = 0;
    std::size_t pagesFailed = 0;
    std::size_t sehFailures = 0;
    std::string rawPayload;
};

// Requests one production snapshot from MCTrackerBridge V156 or newer.
// The Bridge response contains Accounts, Open Positions and at most ten newest
// display Logs rows. V173 or newer also supplies an optional bounded monitoring
// history, and V175 can supply optional Position History rows. A parsed partial
// response is returned even when one section failed.
bool ReadTrackerStatusSnapshot(
    TrackerStatusSnapshot& snapshot,
    std::wstring& diagnostic,
    unsigned long connectTimeoutMilliseconds = 5000);

bool WriteTrackerStatusRawPayload(
    const TrackerStatusSnapshot& snapshot,
    const std::wstring& path,
    std::wstring& diagnostic);

/**
 * @brief Requests the existing passive Tracker research bundle from Bridge V171.
 *
 * The bundle is written by the Bridge inside the MultiCharts process to its
 * normal research-output location. This does not change Bridge Protocol V2.
 */
bool CaptureTrackerResearchBundle(
    std::wstring& responseSummary,
    std::wstring& diagnostic,
    unsigned long connectTimeoutMilliseconds = 5000);

/**
 * @brief Captures the focused Open Positions currency research report used by the
 *        Position Currency research workflow.
 *
 * Bridge V171 verifies the fingerprint-scoped ATCenterProxy position interface,
 * locates its live objects, correlates their numeric fields with the visible rows,
 * and decodes two bounded MSVC wstring currency fields. It does not modify
 * MultiCharts state. Protocol V2 is retained; command 50 is research-only.
 */
bool CapturePositionCurrencyResearch(
    std::wstring& responseSummary,
    std::wstring& diagnostic,
    unsigned long connectTimeoutMilliseconds = 5000);
