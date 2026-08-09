#pragma once

inline constexpr wchar_t kTrackerBridgeProductVersion[] = L"1.0";
inline constexpr int kTrackerBridgeInternalBuildVersion = 155;

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
    TrackerBridgeSection accounts;
    TrackerBridgeSection openPositions;
    TrackerBridgeSection recentLogs;
    std::size_t pagesOk = 0;
    std::size_t pagesFailed = 0;
    std::size_t sehFailures = 0;
    std::string rawPayload;
};

// Requests one production snapshot from MCTrackerBridge V155.
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
