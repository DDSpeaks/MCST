#pragma once

#include "AppConfig.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"
#include "../MCST.Shared/WatchdogSystemStatus.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

class LogAlertEngine
{
public:
    struct Decision
    {
        bool sendEmail = false;
        mcst::HealthState state = mcst::HealthState::Unknown;
        std::size_t eventCount = 0;
        std::wstring subject;
        std::wstring eventText;
        std::wstring body;
    };

    Decision Evaluate(
        const TrackerBridgeSection& recentLogs,
        const AppConfig& config,
        std::chrono::system_clock::time_point now);

    void Reset();

private:
    bool initialized_ = false;
    std::unordered_set<std::wstring> seenEvents_;
    std::deque<std::wstring> seenOrder_;
    std::unordered_map<std::wstring, std::chrono::system_clock::time_point> lastSentByFingerprint_;
};
