#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace mcst
{
    enum class HealthState
    {
        Unknown,
        Healthy,
        Attention,
        Critical
    };

    inline const wchar_t* HealthStateText(HealthState state)
    {
        switch (state)
        {
        case HealthState::Healthy: return L"OK";
        case HealthState::Attention: return L"WARNING";
        case HealthState::Critical: return L"CRITICAL";
        default: return L"UNKNOWN";
        }
    }

    struct MonitorStatus
    {
        HealthState state = HealthState::Unknown;
        std::wstring value;
        std::wstring detail;
    };

    struct ActivityItem
    {
        std::chrono::system_clock::time_point time{};
        HealthState state = HealthState::Unknown;
        std::wstring text;
    };

    struct MultiChartsProcessStatus
    {
        unsigned long processId = 0;
        HealthState state = HealthState::Unknown;
        bool mainWindowFound = false;
        bool responsive = false;
        int unresponsiveSamples = 0;
        bool cpuAvailable = false;
        double cpuCorePercent = 0.0; // 100% equals one fully used logical core.
        bool resourcesAvailable = false;
        std::size_t workingSetBytes = 0;
        std::size_t privateMemoryBytes = 0;
        unsigned long handleCount = 0;
        unsigned long gdiObjects = 0;
        unsigned long userObjects = 0;
        bool queueIndicatorFound = false;
        bool visibleQueueWarningChecked = false;
        bool visibleQueueWarningRed = false;
        bool visibleQueueWarningConfirmed = false;
        std::wstring queueReadDiagnostic;
        unsigned long queueCount = 0;
        unsigned long queueAgeSeconds = 0;
        int queueGrowthSamples = 0;
        int highCpuSamples = 0;
    };

    struct MultiChartsHealthSnapshot
    {
        HealthState state = HealthState::Unknown;
        std::wstring value;
        std::wstring detail;
        std::size_t processCount = 0;
        std::size_t unresponsiveProcessCount = 0;
        std::size_t sustainedHighCpuProcessCount = 0;
        std::size_t visibleQueueWarningCount = 0;
        std::size_t visibleQueueUncheckedCount = 0;
        unsigned long maximumQueueCount = 0;
        unsigned long maximumQueueAgeSeconds = 0;
        unsigned long recentlyDisappearedProcessId = 0;
        bool processSetChanged = false;
        double totalCpuCorePercent = 0.0;
        std::size_t totalPrivateMemoryBytes = 0;
        unsigned long long availableSystemMemoryBytes = 0;
        std::vector<MultiChartsProcessStatus> processes;
    };

    struct WatchdogSystemStatus
    {
        HealthState overall = HealthState::Unknown;

        MonitorStatus multiChartsHealth;
        MonitorStatus bridge;
        MonitorStatus trackerSnapshot;
        MonitorStatus autoTrading;
        MonitorStatus broker;
        MonitorStatus recentLogs;
        MonitorStatus statusReports;
        MonitorStatus email;
        MonitorStatus heartbeat;

        std::chrono::system_clock::time_point lastSuccessfulUpdate{};

        std::wstring lastTrackerAttempt;
        std::wstring lastCompleteTrackerSnapshot;
        std::wstring lastAutoTradingRead;
        std::wstring lastReport;
        std::wstring lastAlert;
        std::wstring lastError;

        std::wstring multiChartsVersion;
        std::wstring multiChartsFileVersion;
        std::wstring multiChartsExecutable;
        std::wstring multiChartsCompatibilityProfile;
        std::wstring trackerCompatibilityProfile;
        std::wstring trackerDateOrder = L"auto";

        // When a current Tracker read fails, the dashboard/report may retain the
        // last complete table snapshot for operational context. It is Attention
        // during the configured grace interval and Critical after that threshold.
        bool trackerDataStale = false;
        bool trackerDataStaleCritical = false;
        int trackerDataStaleAgeMinutes = 0;
        int trackerDataStaleCriticalAfterMinutes = 10;
        std::wstring trackerDataTimestamp;

        unsigned long processId = 0;
        std::size_t accountRows = 0;
        std::size_t openPositionRows = 0;
        std::size_t recentLogRows = 0;
        int autoTradingActive = -1;
        int autoTradingMinimum = 0;

        double cpuPercent = 0.0;
        bool cpuAvailable = false;
        unsigned long logicalProcessorCount = 0;

        bool systemMemoryAvailable = false;
        unsigned long memoryLoadPercent = 0;
        unsigned long long totalPhysicalMemoryBytes = 0;
        unsigned long long availablePhysicalMemoryBytes = 0;

        bool systemDiskAvailable = false;
        std::wstring systemDiskRoot;
        unsigned long long diskTotalBytes = 0;
        unsigned long long diskFreeBytes = 0;
        double diskUsedPercent = 0.0;

        std::size_t privateMemoryBytes = 0;
        unsigned long handleCount = 0;
        std::wstring uptime;

        MultiChartsHealthSnapshot multiChartsProcesses;

        std::vector<ActivityItem> activity;
    };
}
