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
        return decision;
    }

    if (config.statusReportsEnabled)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - lastStatusReport_).count();
        decision.sendStatusReport = elapsed >= config.statusReportIntervalMinutes;
    }
    if (config.heartbeatEnabled)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - lastHeartbeat_).count();
        decision.sendHeartbeat = elapsed >= config.heartbeatIntervalMinutes;
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
