#pragma once

#include "AppConfig.h"
#include "BrokerAuthDetector.h"
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
        const BrokerAuthenticationDetection& authentication,
        const AppConfig& config,
        std::chrono::system_clock::time_point now);

    void Reset();

    /**
     * @brief Restores a recent last-known CONNECTED state after a Watchdog restart.
     *
     * Current browser authentication and new Recent Logs always override this
     * cached historical state. Stale cache entries are ignored.
     */
    void LoadConnectedStateCache(
        const std::wstring& path,
        std::chrono::system_clock::time_point now,
        int maxAgeMinutes);

    void SaveConnectedStateCache(
        const std::wstring& path,
        std::chrono::system_clock::time_point now) const;

    void ClearConnectedStateCache(const std::wstring& path) const;

private:
    enum class State
    {
        Unknown,
        Connected,
        GracePeriod,
        AuthenticationGrace,
        Disconnected,
        AuthenticationRequired
    };

    bool IsNewEvent(const std::wstring& eventText);

    State state_ = State::Unknown;
    std::chrono::system_clock::time_point disconnectDetectedAt_{};
    std::chrono::system_clock::time_point authenticationDetectedAt_{};
    bool criticalAlertSent_ = false;
    std::wstring authenticationProfile_;
    std::wstring disconnectedDetail_;
    std::deque<std::size_t> recentEventHashes_;
};
