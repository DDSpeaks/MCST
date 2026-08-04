#pragma once

#include "AppConfig.h"
#include <chrono>

struct ScheduleDecision
{
    bool sendStatusReport = false;
    bool sendHeartbeat = false;
};

class ScheduleTracker
{
public:
    ScheduleDecision Evaluate(const AppConfig& config, std::chrono::system_clock::time_point now);
    void MarkStatusReportSent(std::chrono::system_clock::time_point now);
    void MarkHeartbeatSent(std::chrono::system_clock::time_point now);
    void Reset();

private:
    bool initialized_ = false;
    std::chrono::system_clock::time_point lastStatusReport_{};
    std::chrono::system_clock::time_point lastHeartbeat_{};
};
