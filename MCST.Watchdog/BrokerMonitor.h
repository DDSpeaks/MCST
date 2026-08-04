#pragma once

#include "AppConfig.h"
#include "../MCST.Shared/WatchdogSystemStatus.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"

#include <chrono>
#include <deque>
#include <string>

struct BrokerMonitorDecision
{
    mcst::MonitorStatus status;
    bool stateChanged = false;
    bool sendAlertEmail = false;
    bool sendRecoveryEmail = false;
    std::wstring eventText;
    std::wstring subject;
};

class BrokerMonitor
{
public:
    BrokerMonitorDecision Evaluate(
        const TrackerBridgeSection& recentLogs,
        const AppConfig& config,
        std::chrono::system_clock::time_point now);

    void Reset();

private:
    enum class State
    {
        Unknown,
        Connected,
        GracePeriod,
        Disconnected
    };

    bool IsNewEvent(const std::wstring& eventText);

    State state_ = State::Unknown;
    std::chrono::system_clock::time_point disconnectDetectedAt_{};
    bool criticalAlertSent_ = false;
    std::deque<std::size_t> recentEventHashes_;
};
