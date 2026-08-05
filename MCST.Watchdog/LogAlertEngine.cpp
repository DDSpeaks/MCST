#include "LogAlertEngine.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <vector>

namespace
{
    constexpr std::size_t kMaximumRememberedEvents = 2000;

    enum class Severity
    {
        None,
        Warning,
        Critical,
        Fatal
    };

    struct ClassifiedEvent
    {
        Severity severity = Severity::None;
        std::wstring dateTime;
        std::wstring category;
        std::wstring instrument;
        std::wstring profile;
        std::wstring strategy;
        std::wstring message;
        std::wstring fingerprint;
    };

    std::wstring Trim(std::wstring value)
    {
        const auto notSpace = [](wchar_t ch) { return std::iswspace(ch) == 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    bool ContainsAny(const std::wstring& lowerText, const std::vector<std::wstring>& patterns)
    {
        for (const auto& pattern : patterns)
        {
            const std::wstring normalized = ToLower(Trim(pattern));
            if (!normalized.empty() && lowerText.find(normalized) != std::wstring::npos)
                return true;
        }
        return false;
    }

    std::wstring Column(const std::vector<std::wstring>& row, std::size_t index)
    {
        return index < row.size() ? Trim(row[index]) : L"";
    }

    std::wstring JoinRow(const std::vector<std::wstring>& row, bool includeDateTime)
    {
        std::wostringstream out;
        const std::size_t start = includeDateTime ? 0 : 1;
        for (std::size_t index = start; index < row.size(); ++index)
        {
            if (index != start)
                out << L" | ";
            out << Trim(row[index]);
        }
        return out.str();
    }

    Severity Classify(const std::wstring& lowerText, const AppConfig& config)
    {
        if (ContainsAny(lowerText, config.logAlertIgnoreKeywords))
            return Severity::None;
        if (ContainsAny(lowerText, config.logAlertFatalKeywords))
            return Severity::Fatal;
        if (ContainsAny(lowerText, config.logAlertCriticalKeywords))
            return Severity::Critical;
        if (ContainsAny(lowerText, config.logAlertWarningKeywords))
            return Severity::Warning;
        return Severity::None;
    }

    int SeverityRank(Severity severity)
    {
        switch (severity)
        {
        case Severity::Fatal: return 3;
        case Severity::Critical: return 2;
        case Severity::Warning: return 1;
        default: return 0;
        }
    }

    const wchar_t* SeverityText(Severity severity)
    {
        switch (severity)
        {
        case Severity::Fatal: return L"FATAL";
        case Severity::Critical: return L"CRITICAL";
        case Severity::Warning: return L"WARNING";
        default: return L"INFORMATION";
        }
    }

    mcst::HealthState ToHealthState(Severity severity)
    {
        switch (severity)
        {
        case Severity::Fatal:
        case Severity::Critical:
            return mcst::HealthState::Critical;
        case Severity::Warning:
            return mcst::HealthState::Attention;
        default:
            return mcst::HealthState::Unknown;
        }
    }

    std::wstring ShortSubjectDetail(const ClassifiedEvent& event)
    {
        if (!event.instrument.empty() && event.instrument != L"-")
            return event.instrument;
        if (!event.profile.empty() && event.profile != L"-")
            return event.profile;
        return L"MultiCharts";
    }
}

LogAlertEngine::Decision LogAlertEngine::Evaluate(
    const TrackerBridgeSection& recentLogs,
    const AppConfig& config,
    std::chrono::system_clock::time_point now)
{
    Decision decision;
    if (!config.logAlertsEnabled || !recentLogs.ok)
        return decision;

    std::vector<ClassifiedEvent> alerts;

    // Bridge rows are newest first. Process oldest first so alert ordering follows time.
    for (auto iterator = recentLogs.rows.rbegin(); iterator != recentLogs.rows.rend(); ++iterator)
    {
        const auto& row = *iterator;
        const std::wstring eventKey = JoinRow(row, true);
        if (eventKey.empty())
            continue;

        const bool alreadySeen = seenEvents_.find(eventKey) != seenEvents_.end();
        if (!alreadySeen)
        {
            seenEvents_.insert(eventKey);
            seenOrder_.push_back(eventKey);
            while (seenOrder_.size() > kMaximumRememberedEvents)
            {
                seenEvents_.erase(seenOrder_.front());
                seenOrder_.pop_front();
            }
        }

        if (alreadySeen || (!initialized_ && !config.logAlertNotifyExistingOnStartup))
            continue;

        const std::wstring searchable = ToLower(JoinRow(row, false));
        const Severity severity = Classify(searchable, config);
        if (severity == Severity::None)
            continue;

        const std::wstring fingerprint = ToLower(JoinRow(row, false));
        const auto lastSent = lastSentByFingerprint_.find(fingerprint);
        if (lastSent != lastSentByFingerprint_.end() && config.logAlertDeduplicationMinutes > 0)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - lastSent->second).count();
            if (elapsed < config.logAlertDeduplicationMinutes)
                continue;
        }

        ClassifiedEvent event;
        event.severity = severity;
        event.dateTime = Column(row, 0);
        event.category = Column(row, 1);
        event.instrument = Column(row, 2);
        event.profile = Column(row, 3);
        event.strategy = Column(row, 4);
        event.message = Column(row, 5);
        event.fingerprint = fingerprint;
        alerts.push_back(std::move(event));
    }

    initialized_ = true;
    if (alerts.empty())
        return decision;

    Severity highest = Severity::Warning;
    for (const auto& event : alerts)
    {
        if (SeverityRank(event.severity) > SeverityRank(highest))
            highest = event.severity;
        lastSentByFingerprint_[event.fingerprint] = now;
    }

    decision.eventCount = alerts.size();
    decision.state = ToHealthState(highest);
    decision.sendEmail = config.logAlertEmailEnabled;

    const std::wstring detail = alerts.size() == 1
        ? ShortSubjectDetail(alerts.front())
        : std::to_wstring(alerts.size()) + L" events";
    decision.subject = std::wstring(SeverityText(highest)) + L": MCST-Watchdog Log Alert - " + detail;
    decision.eventText = std::to_wstring(alerts.size()) + L" new " + ToLower(SeverityText(highest))
        + (alerts.size() == 1 ? L" log alert detected" : L" log alerts detected");

    std::wostringstream body;
    body << L"MCST-Watchdog Log Alert\r\n"
         << L"========================\r\n\r\n"
         << L"Severity: " << SeverityText(highest) << L"\r\n"
         << L"New matching events: " << alerts.size() << L"\r\n\r\n";

    for (std::size_t index = 0; index < alerts.size(); ++index)
    {
        const auto& event = alerts[index];
        body << L"EVENT " << (index + 1) << L"\r\n"
             << L"-------\r\n"
             << L"Severity:   " << SeverityText(event.severity) << L"\r\n"
             << L"Time:       " << (event.dateTime.empty() ? L"Unknown" : event.dateTime) << L"\r\n"
             << L"Category:   " << (event.category.empty() ? L"-" : event.category) << L"\r\n"
             << L"Instrument: " << (event.instrument.empty() ? L"-" : event.instrument) << L"\r\n"
             << L"Profile:    " << (event.profile.empty() ? L"-" : event.profile) << L"\r\n"
             << L"Strategy:   " << (event.strategy.empty() ? L"-" : event.strategy) << L"\r\n"
             << L"Message:    " << (event.message.empty() ? L"-" : event.message) << L"\r\n\r\n";
    }

    body << L"Duplicate suppression: " << config.logAlertDeduplicationMinutes << L" minutes\r\n";
    decision.body = body.str();
    return decision;
}

void LogAlertEngine::Reset()
{
    initialized_ = false;
    seenEvents_.clear();
    seenOrder_.clear();
    lastSentByFingerprint_.clear();
}
