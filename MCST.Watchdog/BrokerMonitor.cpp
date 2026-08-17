#define NOMINMAX
#include "BrokerMonitor.h"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <sstream>
#include <fstream>
#include <filesystem>

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

        // Use semantic token combinations instead of relying only on exact
        // broker-specific phrases. This makes the initial state much more
        // likely to be recovered from the available Recent Logs window.
        return ContainsAll(text, { L"connection", L"established" })
            || ContainsAll(text, { L"connection", L"restored" })
            || ContainsAll(text, { L"connection", L"connected" })
            || ContainsAll(text, { L"connected", L"to" })
            || ContainsAll(text, { L"successfully", L"connected" })
            || ContainsAll(text, { L"session", L"established" })
            || ContainsAll(text, { L"session", L"connected" })
            || ContainsAll(text, { L"trading", L"system", L"connected" })
            || ContainsAll(text, { L"login", L"successful" })
            || ContainsAll(text, { L"logon", L"successful" })
            || ContainsAll(text, { L"authentication", L"successful" })
            || text.find(L"reconnected") != std::wstring::npos
            || ContainsAll(text, { L"logged", L"on" })
            || ContainsAll(text, { L"logged", L"in" });
    }

    bool IsSemanticDisconnectEvent(const std::wstring& text)
    {
        return ContainsAll(text, { L"connection", L"lost" })
            || ContainsAll(text, { L"connection", L"disconnected" })
            || ContainsAll(text, { L"connection", L"closed" })
            || ContainsAll(text, { L"reconnecting", L"failed" })
            || ContainsAll(text, { L"reconnect", L"failed" });
    }

    const BrokerAuthProfile* FindAuthProfile(const AppConfig& config, const std::wstring& name)
    {
        for (const auto& profile : config.brokerAuthProfiles)
        {
            if (ToLower(profile.name) == ToLower(name))
                return &profile;
        }
        return nullptr;
    }
}


void BrokerMonitor::LoadConnectedStateCache(
    const std::wstring& path,
    std::chrono::system_clock::time_point now,
    int maxAgeMinutes)
{
    std::wifstream input{ std::filesystem::path(path) };
    if (!input)
        return;

    long long savedEpochSeconds = 0;
    std::wstring stateText;
    input >> stateText >> savedEpochSeconds;
    if (!input || ToLower(stateText) != L"connected")
        return;

    const auto saved = std::chrono::system_clock::time_point(std::chrono::seconds(savedEpochSeconds));
    const auto age = std::chrono::duration_cast<std::chrono::minutes>(now - saved).count();
    if (age < 0 || age > (std::max)(1, maxAgeMinutes))
        return;

    state_ = State::Connected;
    disconnectDetectedAt_ = {};
    authenticationDetectedAt_ = {};
    authenticationProfile_.clear();
    disconnectedDetail_.clear();
    criticalAlertSent_ = false;
}

void BrokerMonitor::SaveConnectedStateCache(
    const std::wstring& path,
    std::chrono::system_clock::time_point now) const
{
    if (state_ != State::Connected)
        return;

    std::error_code ec;
    const std::filesystem::path cachePath(path);
    if (cachePath.has_parent_path())
        std::filesystem::create_directories(cachePath.parent_path(), ec);

    std::wofstream output{ std::filesystem::path(path), std::ios::trunc };
    if (!output)
        return;

    const auto epochSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    output << L"connected " << epochSeconds << L"\n";
}

void BrokerMonitor::ClearConnectedStateCache(const std::wstring& path) const
{
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path), ec);
}

void BrokerMonitor::Reset()
{
    state_ = State::Unknown;
    disconnectDetectedAt_ = {};
    authenticationDetectedAt_ = {};
    criticalAlertSent_ = false;
    authenticationProfile_.clear();
    disconnectedDetail_.clear();
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
    const TrackerBridgeSection& monitoringLogs,
    const BrokerAuthenticationDetection& authentication,
    const AppConfig& config,
    std::chrono::system_clock::time_point now)
{
    BrokerMonitorDecision decision;

    if (!config.brokerMonitoringEnabled)
    {
        decision.status = { mcst::HealthState::Unknown, L"Disabled", L"Explicitly disabled in INI" };
        return decision;
    }

    // A current broker authentication page is stronger evidence than historical
    // monitoring history. While it is visible, no old "connection established" line is
    // allowed to make the Broker row green.
    if (authentication.detected)
    {
        const bool newAuthenticationEpisode =
            (state_ != State::AuthenticationGrace && state_ != State::AuthenticationRequired)
            || authenticationProfile_ != authentication.profileName;

        if (newAuthenticationEpisode)
        {
            state_ = State::AuthenticationGrace;
            authenticationDetectedAt_ = now;
            authenticationProfile_ = authentication.profileName;
            disconnectedDetail_.clear();
            decision.stateChanged = true;
            decision.eventText = L"Broker authentication page detected for " + authentication.profileName
                + L"; confirming before alert";
        }

        const auto elapsed = authenticationDetectedAt_.time_since_epoch().count() == 0
            ? 0LL
            : (std::max)(0LL, static_cast<long long>(
                std::chrono::duration_cast<std::chrono::seconds>(now - authenticationDetectedAt_).count()));
        const int threshold = (std::max)(3, authentication.alertAfterSeconds);

        if (elapsed >= threshold)
        {
            const bool newlyCritical = state_ != State::AuthenticationRequired;
            state_ = State::AuthenticationRequired;
            disconnectedDetail_ = L"Authentication required - " + authentication.profileName + L" login detected";
            if (!authentication.sanitizedUrl.empty())
                disconnectedDetail_ += L" (" + authentication.sanitizedUrl + L")";

            if (newlyCritical)
            {
                decision.stateChanged = true;
                decision.sendAlertEmail = !criticalAlertSent_ && config.brokerAlertEmailEnabled;
                criticalAlertSent_ = true;
                decision.eventText = disconnectedDetail_;
                decision.subject = L"MCST-Watchdog Broker Authentication Required";
            }
            decision.status = { mcst::HealthState::Critical, L"Authentication required", disconnectedDetail_ };
        }
        else
        {
            const auto remaining = (std::max)(0LL, static_cast<long long>(threshold) - elapsed);
            decision.status = {
                mcst::HealthState::Attention,
                L"Login detected",
                authentication.profileName + L" authentication page - alert in " + std::to_wstring(remaining) + L" s"
            };
        }
        return decision;
    }

    // If a confirmed authentication-required page disappears, keep Broker in a
    // critical waiting state until a new positive connection event arrives.
    if (state_ == State::AuthenticationRequired)
    {
        state_ = State::Disconnected;
        authenticationDetectedAt_ = {};
        disconnectedDetail_ = L"Authentication page closed; waiting for broker connection confirmation";
        decision.stateChanged = true;
        decision.eventText = disconnectedDetail_;
    }
    else if (state_ == State::AuthenticationGrace)
    {
        state_ = State::Unknown;
        authenticationDetectedAt_ = {};
        authenticationProfile_.clear();
        decision.stateChanged = true;
        decision.eventText = L"Broker authentication page disappeared before the alert threshold";
    }

    if (monitoringLogs.ok)
    {
        // The Bridge returns newest rows first. Processing them oldest-to-newest
        // ensures that a later recovery event supersedes an earlier disconnect.
        for (auto it = monitoringLogs.rows.rbegin(); it != monitoringLogs.rows.rend(); ++it)
        {
            const std::wstring original = FlattenRow(*it);
            if (!IsNewEvent(original))
                continue;

            const std::wstring text = ToLower(original);
            const bool connectedEvent = ContainsAny(text, config.brokerConnectedPatterns)
                || IsSemanticConnectedEvent(text);
            const bool disconnectEvent = ContainsAny(text, config.brokerDisconnectPatterns)
                || IsSemanticDisconnectEvent(text);

            if (connectedEvent)
            {
                // When recovering from a browser authentication alert, do not
                // let another broker's connection message clear the condition.
                // Broker-specific recovery terms are configurable in the same
                // [BrokerAuth.*] profile as the login URL.
                if (criticalAlertSent_ && !authenticationProfile_.empty())
                {
                    const BrokerAuthProfile* authProfile = FindAuthProfile(config, authenticationProfile_);
                    if (authProfile && !authProfile->recoveryLogContains.empty()
                        && !ContainsAny(text, authProfile->recoveryLogContains))
                    {
                        continue;
                    }
                }

                const bool recoveredFromAlert =
                    (state_ == State::Disconnected || state_ == State::AuthenticationRequired) && criticalAlertSent_;
                const bool recoveredDuringGrace = state_ == State::GracePeriod;
                state_ = State::Connected;
                disconnectDetectedAt_ = {};
                authenticationDetectedAt_ = {};
                authenticationProfile_.clear();
                disconnectedDetail_.clear();
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
                    disconnectedDetail_.clear();
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
                    disconnectedDetail_.clear();
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
            disconnectedDetail_ = L"Recovery grace period expired";
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
        decision.status = { mcst::HealthState::Healthy, L"Connected",
            L"Last confirmed connected; no newer contradictory broker evidence" };
        break;
    case State::GracePeriod:
    {
        const auto elapsed = disconnectDetectedAt_.time_since_epoch().count() == 0
            ? 0LL
            : (std::max)(0LL, static_cast<long long>(
                std::chrono::duration_cast<std::chrono::seconds>(now - disconnectDetectedAt_).count()));
        const auto remaining = (std::max)(0LL, static_cast<long long>(config.brokerDisconnectGraceSeconds) - elapsed);
        decision.status = { mcst::HealthState::Attention, L"Reconnecting", L"Alert delayed for " + std::to_wstring(remaining) + L" s" };
        break;
    }
    case State::Disconnected:
        decision.status = { mcst::HealthState::Critical, L"Disconnected",
            disconnectedDetail_.empty() ? L"Recovery grace period expired" : disconnectedDetail_ };
        break;
    case State::AuthenticationGrace:
        decision.status = { mcst::HealthState::Attention, L"Login detected", L"Authentication page detected" };
        break;
    case State::AuthenticationRequired:
        decision.status = { mcst::HealthState::Critical, L"Authentication required", disconnectedDetail_ };
        break;
    default:
        decision.status = { mcst::HealthState::Unknown, L"Waiting",
            L"No broker state has been confirmed yet; waiting for connection evidence" };
        break;
    }

    return decision;
}
