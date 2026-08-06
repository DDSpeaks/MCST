#include "ScheduleService.h"

#include <algorithm>
#include <cwctype>
#include <ctime>


namespace
{
    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    int ParseMinutes(const std::wstring& value, int fallback)
    {
        int hour = -1;
        int minute = -1;
        if (swscanf_s(value.c_str(), L"%d:%d", &hour, &minute) != 2 || hour < 0 || hour > 23 || minute < 0 || minute > 59)
            return fallback;
        return hour * 60 + minute;
    }

    bool DayAllowed(const std::wstring& rule, int tmWday)
    {
        const std::wstring value = Lower(rule);
        if (value.empty() || value == L"all" || value == L"daily" || value == L"sun-sat")
            return true;
        if (value == L"mon-fri" || value == L"weekdays")
            return tmWday >= 1 && tmWday <= 5;
        if (value == L"sat-sun" || value == L"weekends")
            return tmWday == 0 || tmWday == 6;
        static const wchar_t* names[] = { L"sun", L"mon", L"tue", L"wed", L"thu", L"fri", L"sat" };
        return value.find(names[tmWday]) != std::wstring::npos;
    }

    bool StatusReportWindowOpen(const AppConfig& config, std::chrono::system_clock::time_point now)
    {
        const std::time_t tt = std::chrono::system_clock::to_time_t(now);
        tm local{};
        localtime_s(&local, &tt);
        if (!DayAllowed(config.statusReportWeekdays, local.tm_wday))
            return false;

        const int current = local.tm_hour * 60 + local.tm_min;
        const int start = ParseMinutes(config.statusReportSendStart, 0);
        const int end = ParseMinutes(config.statusReportSendEnd, 1439);
        if (start <= end)
            return current >= start && current <= end;
        return current >= start || current <= end;
    }
}

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

    if (config.statusReportsEnabled && StatusReportWindowOpen(config, now))
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
