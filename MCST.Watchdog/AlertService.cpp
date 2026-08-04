#include "AlertService.h"

AutoTradingAlertDecision AutoTradingAlertTracker::Evaluate(int activeCount, const AppConfig& config)
{
    AutoTradingAlertDecision decision;
    const bool currentKnown = config.autoTradingMonitoringEnabled && activeCount >= 0;
    if (!currentKnown)
        return decision;

    const bool currentBelow = activeCount < config.autoTradingMinimum;
    if (!stateKnown_ || currentBelow != wasBelowMinimum_)
    {
        decision.stateChanged = true;
        decision.belowMinimum = currentBelow;

        if (currentBelow)
        {
            decision.eventText = L"AutoTrading below minimum: " + std::to_wstring(activeCount)
                + L" active, minimum " + std::to_wstring(config.autoTradingMinimum);
            decision.subject = L"CRITICAL: MCST-Watchdog AutoTrading below minimum";
            decision.sendEmail = config.emailEnabled && config.autoTradingAlertEmailEnabled;
        }
        else if (stateKnown_ && wasBelowMinimum_)
        {
            decision.eventText = L"AutoTrading recovered: " + std::to_wstring(activeCount)
                + L" active, minimum " + std::to_wstring(config.autoTradingMinimum);
            decision.subject = L"RECOVERY: MCST-Watchdog AutoTrading restored";
            decision.sendEmail = config.emailEnabled && config.autoTradingRecoveryEmailEnabled;
        }

        stateKnown_ = true;
        wasBelowMinimum_ = currentBelow;
    }

    return decision;
}

void AutoTradingAlertTracker::Reset()
{
    stateKnown_ = false;
    wasBelowMinimum_ = false;
}
