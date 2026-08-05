#define NOMINMAX
#include "BrokerMonitor.h"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <sstream>

namespace
{
    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    std::wstring FlattenRow(const std::vector<std::wstring>& row)
    {
        std::wstring result;
        for (const auto& column : row)
        {
            if (!result.empty())
                result += L" | ";
            result += column;
        }
        return result;
    }

    bool ContainsAny(const std::wstring& text, const std::vector<std::wstring>& patterns)
    {
        for (const auto& pattern : patterns)
        {
            const std::wstring normalizedPattern = ToLower(pattern);
            if (!normalizedPattern.empty() && text.find(normalizedPattern) != std::wstring::npos)
                return true;
        }
        return false;
    }

    bool ContainsAll(const std::wstring& text, std::initializer_list<const wchar_t*> tokens)
    {
        for (const wchar_t* token : tokens)
        {
            if (text.find(token) == std::wstring::npos)
                return false;
        }
        return true;
    }

    bool ContainsFailureQualifier(const std::wstring& text)
    {
        static const std::vector<std::wstring> failureQualifiers = {
            L"failed",
            L"failure",
            L"unable",
            L"cannot",
            L"can't",
            L"not established",
            L"timeout",
            L"timed out",
            L"disconnected",
            L"connection lost",
            L"connection closed"
        };
        return ContainsAny(text, failureQualifiers);
    }

    bool IsSemanticConnectedEvent(const std::wstring& text)
    {
        if (ContainsFailureQualifier(text))
            return false;

        return ContainsAll(text, { L"connection", L"established" })
            || ContainsAll(text, { L"connection", L"restored" })
            || ContainsAll(text, { L"connection", L"connected" })
            || text.find(L"reconnected") != std::wstring::npos
            || ContainsAll(text, { L"logged", L"on" });
    }

    bool IsSemanticDisconnectEvent(const std::wstring& text)
    {
        return ContainsAll(text, { L"connection", L"lost" })
            || ContainsAll(text, { L"connection", L"disconnected" })
            || ContainsAll(text, { L"connection", L"closed" })
            || ContainsAll(text, { L"reconnecting", L"failed" })
            || ContainsAll(text, { L"reconnect", L"failed" });
    }
}

void BrokerMonitor::Reset()
{
    state_ = State::Unknown;
    disconnectDetectedAt_ = {};
    criticalAlertSent_ = false;
    recentEventHashes_.clear();
}

bool BrokerMonitor::IsNewEvent(const std::wstring& eventText)
{
    const std::size_t hash = std::hash<std::wstring>{}(eventText);
    if (std::find(recentEventHashes_.begin(), recentEventHashes_.end(), hash) != recentEventHashes_.end())
        return false;

    recentEventHashes_.push_front(hash);
    if (recentEventHashes_.size() > 250)
        recentEventHashes_.pop_back();
    return true;
}

BrokerMonitorDecision BrokerMonitor::Evaluate(
    const TrackerBridgeSection& recentLogs,
    const AppConfig& config,
    std::chrono::system_clock::time_point now)
{
    BrokerMonitorDecision decision;

    if (!config.brokerMonitoringEnabled)
    {
        decision.status = { mcst::HealthState::Unknown, L"Disabled", L"Explicitly disabled in INI" };
        return decision;
    }

    if (recentLogs.ok)
    {
        // The Bridge returns newest rows first. Processing them oldest-to-newest
        // ensures that a later recovery event supersedes an earlier disconnect.
        for (auto it = recentLogs.rows.rbegin(); it != recentLogs.rows.rend(); ++it)
        {
            const std::wstring original = FlattenRow(*it);
            if (!IsNewEvent(original))
                continue;

            const std::wstring text = ToLower(original);
            const bool connectedEvent = ContainsAny(text, config.brokerConnectedPatterns)
                || IsSemanticConnectedEvent(text);
            const bool disconnectEvent = ContainsAny(text, config.brokerDisconnectPatterns)
                || IsSemanticDisconnectEvent(text);

            // Recovery takes precedence if a provider-specific message contains
            // both generic connection words and an explicit success statement.
            if (connectedEvent)
            {
                const bool recoveredFromAlert = state_ == State::Disconnected && criticalAlertSent_;
                const bool recoveredDuringGrace = state_ == State::GracePeriod;
                state_ = State::Connected;
                disconnectDetectedAt_ = {};
                decision.stateChanged = recoveredFromAlert || recoveredDuringGrace || decision.stateChanged;
                decision.sendRecoveryEmail = recoveredFromAlert && config.brokerRecoveryEmailEnabled;
                criticalAlertSent_ = false;
                decision.eventText = recoveredFromAlert
                    ? L"Broker connection recovered"
                    : (recoveredDuringGrace ? L"Broker connection recovered during grace period; no alert sent" : L"Broker connected");
                decision.subject = L"MCST-Watchdog Broker Connection Recovered";
                continue;
            }

            if (disconnectEvent)
            {
                if (state_ != State::GracePeriod && state_ != State::Disconnected)
                {
                    state_ = State::GracePeriod;
                    disconnectDetectedAt_ = now;
                    decision.stateChanged = true;
                    decision.eventText = L"Broker connection loss detected; " + std::to_wstring(config.brokerDisconnectGraceSeconds) + L"-second recovery grace period started";
                }
                continue;
            }

            if (ContainsAny(text, config.brokerReconnectingPatterns))
            {
                if (state_ == State::Unknown || state_ == State::Connected)
                {
                    state_ = State::GracePeriod;
                    disconnectDetectedAt_ = now;
                    decision.stateChanged = true;
                    decision.eventText = L"Broker reconnection attempt detected; recovery grace period started";
                }
            }
        }
    }

    if (state_ == State::GracePeriod && disconnectDetectedAt_.time_since_epoch().count() != 0)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - disconnectDetectedAt_).count();
        if (elapsed >= config.brokerDisconnectGraceSeconds)
        {
            state_ = State::Disconnected;
            decision.stateChanged = true;
            decision.sendAlertEmail = !criticalAlertSent_ && config.brokerAlertEmailEnabled;
            criticalAlertSent_ = true;
            decision.eventText = L"Broker connection has remained unavailable beyond the recovery grace period";
            decision.subject = L"MCST-Watchdog Broker Connection Alert";
        }
    }

    switch (state_)
    {
    case State::Connected:
        decision.status = { mcst::HealthState::Healthy, L"Connected", L"Connection confirmed from Recent Logs" };
        break;
    case State::GracePeriod:
    {
        const auto elapsed = disconnectDetectedAt_.time_since_epoch().count() == 0
            ? 0LL
            : (std::max)(0LL, std::chrono::duration_cast<std::chrono::seconds>(now - disconnectDetectedAt_).count());
        const auto remaining = (std::max)(0LL, static_cast<long long>(config.brokerDisconnectGraceSeconds) - elapsed);
        decision.status = { mcst::HealthState::Attention, L"Reconnecting", L"Alert delayed for " + std::to_wstring(remaining) + L" s" };
        break;
    }
    case State::Disconnected:
        decision.status = { mcst::HealthState::Critical, L"Disconnected", L"Recovery grace period expired" };
        break;
    default:
        decision.status = { mcst::HealthState::Unknown, L"Waiting", L"No recognized broker connection event in Recent Logs" };
        break;
    }

    return decision;
}
