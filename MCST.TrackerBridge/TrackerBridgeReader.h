#pragma once

inline constexpr wchar_t kTrackerBridgeProductVersion[] = L"1.0";
inline constexpr int kTrackerBridgeInternalBuildVersion = 156;
inline constexpr int kPositionCurrencyResearchBridgeVersion = 158;

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
    TrackerBridgeSection accounts;
    TrackerBridgeSection openPositions;
    TrackerBridgeSection recentLogs;
    std::size_t pagesOk = 0;
    std::size_t pagesFailed = 0;
    std::size_t sehFailures = 0;
    std::string rawPayload;
};

// Requests one production snapshot from MCTrackerBridge V156 or newer.
// The Bridge response contains Accounts, Open Positions and at most ten newest
// Logs rows. A parsed partial response is returned even when one section failed.
bool ReadTrackerStatusSnapshot(
    TrackerStatusSnapshot& snapshot,
    std::wstring& diagnostic,
    unsigned long connectTimeoutMilliseconds = 5000);

bool WriteTrackerStatusRawPayload(
    const TrackerStatusSnapshot& snapshot,
    const std::wstring& path,
    std::wstring& diagnostic);

/**
 * @brief Requests the existing passive Tracker research bundle from Bridge V158.
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
 * Bridge V158 dynamically correlates visible Open Positions rows with readable
 * Tracker-related data memory by Quantity and Average Price, then inspects
 * native-currency and P/L-currency candidates without modifying MultiCharts state. Protocol V2 is
 * retained; command 50 is additive and research-only.
 */
bool CapturePositionCurrencyResearch(
    std::wstring& responseSummary,
    std::wstring& diagnostic,
    unsigned long connectTimeoutMilliseconds = 5000);
