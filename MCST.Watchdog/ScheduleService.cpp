#include "ScheduleService.h"

ScheduleDecision ScheduleTracker::Evaluate(const AppConfig& config, std::chrono::system_clock::time_point now)
{
    ScheduleDecision decision;
    if (!initialized_)
    {
        initialized_ = true;
        lastStatusReport_ = now;
        lastHeartbeat_ = now;
        decision.sendStatusReport = config.statusReportsEnabled && config.statusReportSendOnStartup;
        decision.sendHeartbeat = config.heartbeatEnabled && config.heartbeatSendOnStartup;
        // The initial timestamps are already claimed here, so a second refresh completion
        // cannot dispatch the same startup message again.
        return decision;
    }

    if (config.statusReportsEnabled)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - lastStatusReport_).count();
        decision.sendStatusReport = elapsed >= config.statusReportIntervalMinutes;
        if (decision.sendStatusReport)
            lastStatusReport_ = now;
    }
    if (config.heartbeatEnabled)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - lastHeartbeat_).count();
        decision.sendHeartbeat = elapsed >= config.heartbeatIntervalMinutes;
        if (decision.sendHeartbeat)
            lastHeartbeat_ = now;
    }
    return decision;
}

void ScheduleTracker::MarkStatusReportSent(std::chrono::system_clock::time_point now)
{
    lastStatusReport_ = now;
}

void ScheduleTracker::MarkHeartbeatSent(std::chrono::system_clock::time_point now)
{
    lastHeartbeat_ = now;
}

void ScheduleTracker::Reset()
{
    initialized_ = false;
    lastStatusReport_ = {};
    lastHeartbeat_ = {};
}

void ScheduleTracker::PreserveOnReload(std::chrono::system_clock::time_point now)
{
    // Reloading settings must not be treated as a fresh application start.
    // Keep existing schedule timestamps. If the scheduler has not yet been
    // initialized, initialize it without triggering send_on_startup actions.
    if (!initialized_)
    {
        initialized_ = true;
        lastStatusReport_ = now;
        lastHeartbeat_ = now;
    }
}
