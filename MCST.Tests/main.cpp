#include <algorithm>
#include <chrono>
#include <iostream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "../MCST.Shared/MCBridgeProtocol.h"
#include "../MCST.Shared/ActivityHistory.h"
#include "../MCST.Shared/VisibleWarningPolicy.h"
#include "../MCST.Shared/OverallHeadlinePolicy.h"
#include "../MCST.Shared/ReportReadPolicy.h"
#include "../MCST.Shared/TrackerRecoveryPolicy.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"
#include "../MCST.Watchdog/BrokerMonitor.h"
#include "../MCST.Watchdog/DeveloperHelpContent.h"
#include "../MCST.Watchdog/DashboardLayout.h"
#include "../MCST.Watchdog/StatusReport.h"
#include "../MCST.Watchdog/TrackerDateParser.h"

namespace
{
    void RequireContains(
        const std::wstring& report,
        const std::wstring& expected,
        const char* diagnostic)
    {
        if (report.find(expected) == std::wstring::npos)
            throw std::runtime_error(diagnostic);
    }

    void RequireNotContains(
        const std::wstring& report,
        const std::wstring& unexpected,
        const char* diagnostic)
    {
        if (report.find(unexpected) != std::wstring::npos)
            throw std::runtime_error(diagnostic);
    }

    std::wstring Trim(std::wstring value)
    {
        const std::size_t begin = value.find_first_not_of(L" \t\r\n");
        if (begin == std::wstring::npos)
            return L"";
        const std::size_t end = value.find_last_not_of(L" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    std::wstring FindLineContaining(const std::wstring& report, const std::wstring& expected)
    {
        const std::size_t match = report.find(expected);
        if (match == std::wstring::npos)
            return L"";
        const std::size_t begin = report.rfind(L'\n', match);
        const std::size_t end = report.find(L'\n', match);
        const std::size_t lineBegin = begin == std::wstring::npos ? 0 : begin + 1;
        return report.substr(lineBegin, end == std::wstring::npos ? std::wstring::npos : end - lineBegin);
    }

    std::wstring DelimitedField(const std::wstring& line, std::size_t fieldIndex)
    {
        const std::wstring separator = L" | ";
        std::size_t begin = 0;
        for (std::size_t index = 0;; ++index)
        {
            const std::size_t end = line.find(separator, begin);
            if (index == fieldIndex)
                return Trim(end == std::wstring::npos ? line.substr(begin) : line.substr(begin, end - begin));
            if (end == std::wstring::npos)
                return L"";
            begin = end + separator.size();
        }
    }

    std::size_t CountOccurrences(const std::wstring& text, const std::wstring& expected)
    {
        std::size_t count = 0;
        std::size_t begin = 0;
        while ((begin = text.find(expected, begin)) != std::wstring::npos)
        {
            ++count;
            begin += expected.size();
        }
        return count;
    }

    void RequireDate(
        const std::wstring& text,
        TrackerDateOrder order,
        int year,
        int month,
        int day)
    {
        TrackerCalendarDate parsed;
        if (!TryParseTrackerCalendarDate(text, order, parsed) ||
            parsed.year != year || parsed.month != month || parsed.day != day)
        {
            throw std::runtime_error("Localized Tracker date was not parsed as expected");
        }
    }
}

int RunLogicTests()
{
    const auto activityBaseTime = std::chrono::system_clock::now();
    std::vector<mcst::ActivityItem> currentActivity = {
        { activityBaseTime + std::chrono::seconds(2), mcst::HealthState::Healthy, L"AutoTrading read: 75 active" },
        { activityBaseTime, mcst::HealthState::Healthy, L"Broker connected" },
        { activityBaseTime, mcst::HealthState::Healthy, L"Broker connected" }
    };
    const std::vector<mcst::ActivityItem> previousActivity = {
        { activityBaseTime + std::chrono::seconds(3), mcst::HealthState::Attention, L"Concurrent event" },
        { activityBaseTime + std::chrono::seconds(2), mcst::HealthState::Healthy, L"AutoTrading read: 75 active" },
        { activityBaseTime, mcst::HealthState::Healthy, L"Broker connected" }
    };
    mcst::MergeActivityHistory(currentActivity, previousActivity, 10);
    if (currentActivity.size() != 3 ||
        currentActivity[0].text != L"Concurrent event" ||
        currentActivity[1].text != L"AutoTrading read: 75 active" ||
        currentActivity[2].text != L"Broker connected")
    {
        throw std::runtime_error("Activity history merge did not remove duplicates or retain chronological order");
    }

    static_assert(sizeof(mcbridge::MessageHeader) == 20);
    static_assert(std::is_default_constructible_v<TrackerStatusSnapshot>);

    const auto& helpTopics = GetDeveloperHelpTopics();
    if (helpTopics.size() != 8 || helpTopics.front().buttonLabel != L"Overview")
        throw std::runtime_error("Developer Help topic navigation is incomplete");
    const std::wstring normalUseRule = GetDeveloperToolsNormalUseRule();
    if (normalUseRule.find(L"only after a MultiCharts update") == std::wstring::npos ||
        normalUseRule.find(L"Charting.dll or ATOnPTracker.dll fingerprint changes") == std::wstring::npos ||
        normalUseRule.find(L"developer specifically asks") == std::wstring::npos ||
        normalUseRule.find(L"Do not use them during normal monitoring") == std::wstring::npos)
    {
        throw std::runtime_error("Developer tool normal-use rule is incomplete");
    }
    for (const auto& topic : helpTopics)
    {
        if (topic.content.find(normalUseRule) == std::wstring::npos)
            throw std::runtime_error("A Developer Help topic omits the normal-use rule");
    }
    for (std::size_t index = 1; index < helpTopics.size(); ++index)
    {
        for (const wchar_t* heading : {
            L"GOAL", L"WHAT THIS BUTTON DOES", L"WHEN TO USE IT",
            L"BEFORE YOU PRESS IT", L"PRESS THE BUTTON", L"SUCCESS",
            L"NEXT STEP", L"IF IT FAILS", L"SAFE OPERATION" })
        {
            if (helpTopics[index].content.find(heading) == std::wstring::npos)
                throw std::runtime_error("A Developer Help topic lacks beginner guidance");
        }
    }
    const auto captureTopic = std::find_if(
        helpTopics.begin(), helpTopics.end(),
        [](const DeveloperHelpTopic& topic) { return topic.buttonLabel == L"AT Capture"; });
    if (captureTopic == helpTopics.end() ||
        captureTopic->content.find(L"On ONE chart, choose ONE strategy") == std::wstring::npos ||
        captureTopic->content.find(L"Change only that strategy's AutoTrading state") == std::wstring::npos ||
        captureTopic->content.find(L"ON to OFF, or OFF to ON") == std::wstring::npos)
    {
        throw std::runtime_error("AT Capture does not identify the exact user-controlled change");
    }
    const DeveloperToolbarLayout developerToolbar = CalculateDeveloperToolbarLayout(700);
    const RECT lastDeveloperButton = CalculateDeveloperToolbarButtonRect(developerToolbar, 7);
    if (developerToolbar.buttonHeight >= 34 || developerToolbar.buttonHeight > 24 ||
        developerToolbar.panelTop >= developerToolbar.labelTop ||
        developerToolbar.labelTop >= developerToolbar.top ||
        developerToolbar.top + developerToolbar.buttonHeight >= developerToolbar.panelBottom ||
        700 - developerToolbar.panelBottom < 18 ||
        lastDeveloperButton.right > 920)
    {
        throw std::runtime_error("Developer toolbar is not visibly smaller or does not fit the Dashboard");
    }

    mcst::WatchdogSystemStatus status;
    status.overall = mcst::HealthState::Healthy;
    status.bridge = { mcst::HealthState::Healthy, L"Connected", L"Bridge detail" };
    status.trackerSnapshot = { mcst::HealthState::Healthy, L"Snapshot OK", L"Snapshot detail" };
    status.email = { mcst::HealthState::Healthy, L"Ready", L"alerts@example.com" };
    status.heartbeat = { mcst::HealthState::Unknown, L"Disabled", L"Heartbeat detail" };
    status.lastTrackerAttempt = L"2026-08-22 12:00";
    status.lastCompleteTrackerSnapshot = L"2026-08-22 11:59";
    status.lastAutoTradingRead = L"2026-08-22 11:58";
    status.lastReport = L"2026-08-22 11:00";
    status.lastAlert = L"None";
    TrackerStatusSnapshot snapshot;
    snapshot.bridgeVersion = 180;

    const auto initialRecovery = mcst::SelectTrackerRecoveryPolicy({ 0 });
    if (initialRecovery.retryCooldownMs != 30ull * 1000ull ||
        initialRecovery.targetedTimeBudgetMs != 100 ||
        initialRecovery.targetedByteBudget != 8ull * 1024ull * 1024ull ||
        initialRecovery.processWideTimeBudgetMs != 3000 ||
        initialRecovery.processWideByteBudget != 512ull * 1024ull * 1024ull)
    {
        throw std::runtime_error("Initial Tracker recovery budgets are incorrect");
    }

    const auto shortFailureBackoff = mcst::SelectTrackerRecoveryPolicy({ 3 });
    if (shortFailureBackoff.retryCooldownMs != 60ull * 1000ull)
        throw std::runtime_error("Tracker recovery does not back off after three failures");

    if (mcst::SelectTrackerRecoveryPolicy({ 2 }).retryCooldownMs != 30ull * 1000ull ||
        mcst::SelectTrackerRecoveryPolicy({ 9 }).retryCooldownMs != 60ull * 1000ull)
    {
        throw std::runtime_error("Tracker recovery backoff boundary is incorrect");
    }

    const auto persistentFailureBackoff = mcst::SelectTrackerRecoveryPolicy({ 10 });
    if (persistentFailureBackoff.retryCooldownMs != 5ull * 60ull * 1000ull)
        throw std::runtime_error("Persistent Tracker recovery does not use the five-minute cooldown");

    if (!mcst::TrackerRecoveryCooldownElapsed(30000, 0, 30000) ||
        mcst::TrackerRecoveryCooldownElapsed(59999, 30000, 30000) ||
        !mcst::TrackerRecoveryCooldownElapsed(60000, 30000, 30000))
    {
        throw std::runtime_error("Tracker recovery cooldown boundary is incorrect");
    }
    snapshot.protocolVersion = 2;
    snapshot.atonpTrackerLoaded = true;
    snapshot.atonpTrackerPeTimestamp = 0x6A5E694F;
    snapshot.atonpTrackerImageSize = 3534848;
    snapshot.accounts.present = true;
    snapshot.accounts.ok = true;
    snapshot.accounts.expectedColumns = 12;
    snapshot.accounts.rows = {
        { L"Saxo Group live", L"DEMO100001", L"3 951,77", L"3 951,77", L"0,00", L"", L"", L"", L"", L"", L"", L"" },
        { L"Saxo Group live", L"DEMO200002", L"-1 484,11", L"-1 484,11", L"0,00", L"", L"", L"", L"", L"", L"", L"" }
    };
    snapshot.openPositions.present = true;
    snapshot.openPositions.ok = true;
    snapshot.openPositions.expectedColumns = 8;
    snapshot.openPositions.rows = {
        { L"Saxo", L"A", L"DEMOA:xnas", L"Long", L"3", L"139,97", L"\u20ac 10,50", L"now" },
        { L"Saxo", L"A", L"DEMOB:xnas", L"Long", L"15", L"30,76", L"EUR -2,25", L"now" },
        { L"Saxo", L"A", L"DEMOC:xnys", L"Long", L"2", L"236,00", L"$ 3,00", L"now" },
        { L"Saxo", L"A", L"DEMOD:xnys", L"Long", L"1", L"333,84", L"USD \u20ac 4,00", L"now" }
    };

    const std::wstring report = BuildStatusReport(status, snapshot);
    RequireContains(
        report,
        L"EUR +8,25",
        "Known-currency Open P/L total was not calculated correctly");
    const std::wstring headerLine = FindLineContaining(report, L"Average Price");
    if (DelimitedField(headerLine, 0) != L"Symbol" ||
        DelimitedField(headerLine, 1) != L"Open P/L" ||
        DelimitedField(headerLine, 2) != L"Side" ||
        DelimitedField(headerLine, 3) != L"Qty" ||
        DelimitedField(headerLine, 4) != L"Average Price" ||
        DelimitedField(headerLine, 5) != L"Native Value" ||
        DelimitedField(headerLine, 6) != L"Account" ||
        DelimitedField(headerLine, 7) != L"Profile" ||
        DelimitedField(headerLine, 8) != L"Last Update")
    {
        throw std::runtime_error("Open Positions does not use the selected one-line column order");
    }
    const std::wstring totalLine = FindLineContaining(report, L"EUR +8,25");
    if (DelimitedField(totalLine, 0) != L"Total Open P/L" ||
        DelimitedField(totalLine, 1) != L"EUR +8,25" ||
        !DelimitedField(totalLine, 2).empty())
    {
        throw std::runtime_error("Open P/L total is not aligned or still contains a width-expanding comment");
    }
    RequireContains(
        report,
        L"OPEN P/L ROWS NOT TOTALLED: 2",
        "Ambiguous-currency Open P/L row was not excluded");
    RequireNotContains(
        report,
        L"NATIVE VALUE TOTAL",
        "The removed Native Value total notice is still present");

    const std::wstring reportHtml = BuildStatusReportHtml(report);
    RequireContains(
        reportHtml,
        L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">",
        "Mobile viewport metadata is missing from the Status Report HTML");
    RequireContains(
        reportHtml,
        L"class=\"mcst-open-positions-lines\"",
        "The one-line Open Positions preformatted section is missing");
    RequireContains(
        report,
        L"OVERALL STATUS",
        "Emphasized Overall Status row is missing");
    RequireContains(
        report,
        L"OVERALL STATUS    [OK] OK",
        "Overall status is not the first aligned System Status row");
    const std::wstring overallLine = FindLineContaining(report, L"OVERALL STATUS");
    if (CountOccurrences(overallLine, L"OK") != 2)
        throw std::runtime_error("Overall status is printed more than once");
    const std::wstring bridgeLine = FindLineContaining(report, L"Bridge detail");
    const std::wstring heartbeatLine = FindLineContaining(report, L"Heartbeat detail");
    if (bridgeLine.find(L"Bridge detail") != heartbeatLine.find(L"Heartbeat detail"))
        throw std::runtime_error("System Status detail columns are not aligned");
    const std::wstring positiveAccountLine = FindLineContaining(report, L"3 951,77");
    const std::wstring negativeAccountLine = FindLineContaining(report, L"-1 484,11");
    if (positiveAccountLine.find(L"3 951,77") + std::wstring(L"3 951,77").size() !=
        negativeAccountLine.find(L"-1 484,11") + std::wstring(L"-1 484,11").size())
    {
        throw std::runtime_error("Signed Accounts values do not end in the same column");
    }
    RequireContains(
        reportHtml,
        L"class=\"mcst-system-status-lines\"",
        "System Status is not rendered in a protected one-line flow");
    RequireContains(
        reportHtml,
        L"display:inline-block;width:4ch;text-align:center",
        "Status dots do not preserve the fixed character column");
    RequireContains(
        reportHtml,
        L"display:inline-block;width:18ch;",
        "System Status names do not use a fixed HTML column");
    RequireContains(
        reportHtml,
        L"display:inline-block;width:26ch;",
        "System Status values do not use a fixed HTML column");
    RequireContains(
        reportHtml,
        L"OK&#160;</span></span><span class=\"mcst-status-value-cell\"",
        "Status state and value cells lack a real HTML text separator");
    RequireContains(
        reportHtml,
        L"Ready&#160;</span>alerts@example.com",
        "Status value and detail cells can still concatenate into a false email address");
    RequireContains(
        reportHtml,
        L"font-size:2em;line-height:0.5",
        "Overall Status dot is not emphasized without changing its column width");
    RequireContains(
        reportHtml,
        L"class=\"mcst-status-overall-dot\"",
        "Overall Status dot does not use a nested size-only element");
    RequireContains(
        reportHtml,
        L"class=\"mcst-status-dot\" style=\"font-size:1.5em;line-height:0.65",
        "Normal System Status dots are not large enough to identify their color");
    const std::size_t dotCellStart = reportHtml.find(L"class=\"mcst-status-dot-cell\"");
    const std::size_t dotCellTagEnd = reportHtml.find(L'>', dotCellStart);
    if (dotCellStart == std::wstring::npos || dotCellTagEnd == std::wstring::npos ||
        reportHtml.substr(dotCellStart, dotCellTagEnd - dotCellStart).find(L"font-size:2em") != std::wstring::npos)
    {
        throw std::runtime_error("The fixed-width Status dot cell itself is still enlarged");
    }
    RequireNotContains(
        reportHtml,
        L"<table role=\"presentation\"",
        "Status sections still use an independently shrinkable semantic table");
    RequireContains(
        reportHtml,
        L"font-size:15px !important;line-height:1.28 !important;white-space:pre",
        "Open Positions does not share the normal report font size and line flow");
    RequireNotContains(
        reportHtml,
        L"width=\"1100\"",
        "The iOS-shrunk 1100-pixel Open Positions table is still present");
    RequireNotContains(
        reportHtml,
        L"mcst-horizontal-scroll",
        "Open Positions still relies on an iOS Mail overflow container");
    RequireNotContains(
        reportHtml,
        L"<table class=\"mcst-open-positions\"",
        "Open Positions is still rendered as one wide HTML table");
    RequireNotContains(
        reportHtml,
        L"mcst-open-position-card",
        "Open Positions still uses the rejected multi-line card layout");
    RequireContains(
        reportHtml,
        L"font-size:15px !important",
        "The Open Positions HTML table does not enforce the report font size");
    RequireContains(
        reportHtml,
        L"-webkit-text-size-adjust:100% !important",
        "Mobile automatic text shrinking is not disabled");

    TrackerStatusSnapshot emptyPositionsSnapshot = snapshot;
    emptyPositionsSnapshot.openPositions.rows.clear();
    const std::wstring emptyPositionsReport = BuildStatusReport(status, emptyPositionsSnapshot);
    RequireContains(
        emptyPositionsReport,
        L"OPEN POSITIONS\n--------------\n(no rows)",
        "Empty Open Positions does not use the concise empty-state message");
    if (!FindLineContaining(emptyPositionsReport, L"Average Price").empty())
        throw std::runtime_error("Empty Open Positions still prints position column headings");

    mcst::WatchdogSystemStatus staleStatus = status;
    staleStatus.trackerDataStale = true;
    staleStatus.trackerDataStaleCritical = false;
    staleStatus.trackerDataStaleAgeMinutes = 4;
    staleStatus.trackerDataStaleCriticalAfterMinutes = 10;
    staleStatus.trackerDataTimestamp = L"2026-08-17 16:55";
    staleStatus.lastTrackerAttempt = L"2026-08-17 16:59";
    staleStatus.lastCompleteTrackerSnapshot = L"2026-08-17 16:55";
    const std::wstring staleReport = BuildStatusReport(staleStatus, snapshot);
    RequireContains(
        staleReport,
        L"ATTENTION: STALE TRACKER TABLE DATA - one or more failed sections use the last complete snapshot from 2026-08-17 16:55",
        "A recent retained Tracker snapshot is not visibly marked Attention");
    RequireContains(
        staleReport,
        L"Last Tracker attempt    2026-08-17 16:59",
        "The report does not distinguish the last Tracker attempt");
    RequireContains(
        staleReport,
        L"Last complete snapshot  2026-08-17 16:55",
        "The report does not identify the last complete snapshot");
    RequireContains(
        BuildStatusReportHtml(staleReport),
        L"mcst-stale-attention",
        "The HTML report does not render a recent stale Tracker snapshot as Attention");

    mcst::WatchdogSystemStatus criticalStaleStatus = staleStatus;
    criticalStaleStatus.trackerDataStaleCritical = true;
    criticalStaleStatus.trackerDataStaleAgeMinutes = 10;
    const std::wstring criticalStaleReport = BuildStatusReport(criticalStaleStatus, snapshot);
    RequireContains(
        criticalStaleReport,
        L"CRITICAL: STALE TRACKER TABLE DATA",
        "A stale Tracker snapshot does not escalate at the configured threshold");
    RequireContains(
        BuildStatusReportHtml(criticalStaleReport),
        L"mcst-stale-critical",
        "The HTML report does not render an expired stale Tracker snapshot as Critical");

    RequireContains(reportHtml, L"DEMOA:xnas", "Symbol is missing from the one-line Open Positions section");
    RequireContains(reportHtml, L"139,97", "Average Price is missing from the one-line Open Positions section");
    RequireContains(reportHtml, L"419,91", "Native Value is missing from the one-line Open Positions section");
    RequireContains(reportHtml, L"now", "Last Update is missing from the one-line Open Positions section");
    RequireContains(
        reportHtml,
        L"<span style=\"color:#15803D;font-weight:600;\">\u20ac 10,50</span>",
        "Positive Open P/L detail is not fully green");
    RequireContains(
        reportHtml,
        L"<span style=\"color:#B4232A;font-weight:600;\">EUR -2,25</span>",
        "Negative Open P/L detail is not fully red");
    RequireContains(
        reportHtml,
        L"<span style=\"color:#15803D;font-weight:600;\">EUR +8,25</span>",
        "Positive Open P/L total, currency code, and plus sign are not fully green");

    TrackerStatusSnapshot negativeSnapshot = snapshot;
    negativeSnapshot.openPositions.rows = {
        { L"Saxo", L"A", L"DEMOB:xnas", L"Long", L"15", L"30,76", L"EUR -2,25", L"now" }
    };
    const std::wstring negativeTotalHtml =
        BuildStatusReportHtml(BuildStatusReport(status, negativeSnapshot));
    const std::wstring negativeSpan =
        L"<span style=\"color:#B4232A;font-weight:600;\">EUR -2,25</span>";
    if (CountOccurrences(negativeTotalHtml, negativeSpan) != 2)
        throw std::runtime_error("Negative Open P/L detail and total are not both fully red");

    snapshot.openPositions.rows = {
        { L"Saxo Group live", L"DEMO100001", L"DEMOA:xnas", L"Long", L"3", L"139,970", L"\u20ac -3,64", L"now" },
        { L"Saxo Group live", L"DEMO100001", L"DEMOB:xnas", L"Long", L"15", L"30,76000", L"\u20ac 8,84", L"now" },
        { L"Saxo Group live", L"DEMO100001", L"DEMOC:xnys", L"Long", L"2", L"236,000", L"\u20ac 10,99", L"now" },
        { L"Saxo Group live", L"DEMO100001", L"DEMOD:xnys", L"Long", L"1", L"333,840", L"\u20ac 26,04", L"now" },
        { L"Saxo Group live", L"DEMO100001", L"DEMOE:xnas", L"Long", L"8", L"56,430", L"\u20ac 10,12", L"now" },
        { L"Saxo Group live", L"DEMO100001", L"DEMOF:xnys", L"Long", L"11", L"43,820", L"\u20ac 17,83", L"now" },
        { L"Saxo Group live", L"DEMO100001", L"DEMOG:xnas", L"Long", L"11", L"42,570", L"\u20ac 34,98", L"now" },
        { L"Saxo Group live", L"DEMO100001", L"DEMOH:xnas", L"Long", L"57", L"8,14920", L"\u20ac -15,27", L"now" },
        { L"Saxo Group live", L"DEMO100001", L"DEMOI:xnys", L"Long", L"27", L"16,870", L"\u20ac 8,89", L"now" },
        { L"Saxo Group live", L"DEMO100001", L"DEMOJ:xnas", L"Long", L"9", L"53,367", L"\u20ac 7,90", L"now" }
    };
    const std::wstring currentCaptureReport = BuildStatusReport(status, snapshot);
    RequireContains(
        currentCaptureReport,
        L"EUR +106,68",
        "Current Saxo capture Open P/L total was not calculated correctly");
    const std::wstring currentTotalLine = FindLineContaining(currentCaptureReport, L"EUR +106,68");
    if (DelimitedField(currentTotalLine, 0) != L"Total Open P/L" ||
        DelimitedField(currentTotalLine, 1) != L"EUR +106,68" ||
        !DelimitedField(currentTotalLine, 2).empty())
    {
        throw std::runtime_error("Current Saxo total is not aligned or still contains a removed row-count comment");
    }
    if (currentCaptureReport.find(L"OPEN P/L ROWS NOT TOTALLED") != std::wstring::npos)
        throw std::runtime_error("Current Saxo capture unexpectedly excluded an Open P/L row");

    RequireDate(L"18/08/2026 18.00.38", TrackerDateOrder::DayMonthYear, 2026, 8, 18);
    RequireDate(L"08/18/2026 6:00:38 PM", TrackerDateOrder::MonthDayYear, 2026, 8, 18);
    RequireDate(L"2026-08-18T18:00:38", TrackerDateOrder::YearMonthDay, 2026, 8, 18);
    RequireDate(L"18.8.2026", TrackerDateOrder::DayMonthYear, 2026, 8, 18);
    RequireDate(L"18 08 2026 18:00", TrackerDateOrder::DayMonthYear, 2026, 8, 18);
    if (DetectTrackerDateOrder({ L"18/08/2026", L"17/08/2026" }, TrackerDateOrder::MonthDayYear) !=
        TrackerDateOrder::DayMonthYear)
    {
        throw std::runtime_error("DMY order was not inferred from unambiguous Tracker rows");
    }
    if (DetectTrackerDateOrder({ L"08/18/2026", L"08/17/2026" }, TrackerDateOrder::DayMonthYear) !=
        TrackerDateOrder::MonthDayYear)
    {
        throw std::runtime_error("MDY order was not inferred from unambiguous Tracker rows");
    }
    if (DetectTrackerDateOrder({ L"2026-08-18" }, TrackerDateOrder::DayMonthYear) !=
        TrackerDateOrder::YearMonthDay)
    {
        throw std::runtime_error("YMD order was not inferred from Tracker rows");
    }
    TrackerCalendarDate invalidDate;
    if (TryParseTrackerCalendarDate(
            L"29/02/2025", TrackerDateOrder::DayMonthYear, invalidDate))
    {
        throw std::runtime_error("Invalid calendar date was accepted");
    }

    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    auto dmyDate = [](int year, int month, int day)
    {
        std::wostringstream value;
        value << std::setfill(L'0') << std::setw(2) << day << L'/'
              << std::setw(2) << month << L'/' << std::setw(4) << year
              << L" 18.00.38";
        return value.str();
    };
    int previousMonth = local.tm_mon;
    int previousMonthYear = local.tm_year + 1900;
    if (previousMonth == 0)
    {
        previousMonth = 12;
        --previousMonthYear;
    }
    snapshot.positionHistory.present = true;
    snapshot.positionHistory.ok = true;
    snapshot.positionHistory.expectedColumns = 8;
    snapshot.positionHistory.rows = {
        { dmyDate(local.tm_year + 1900, local.tm_mon + 1, 18), L"Saxo", L"DEMO100001", L"DEMOF:xnys", L"Flat", L"0", L"", L"EUR +5,60" },
        { dmyDate(local.tm_year + 1900, local.tm_mon + 1, 17), L"Saxo", L"DEMO100001", L"DEMOB:xnas", L"Flat", L"0", L"", L"\u20ac -18,78" },
        { dmyDate(local.tm_year + 1900, local.tm_mon + 1, 18), L"Saxo", L"DEMO200002", L"DEMOK:xnas", L"Flat", L"0", L"", L"EUR +7,25" },
        { dmyDate(local.tm_year + 1900, local.tm_mon + 1, 18), L"Saxo", L"UNLISTED", L"HIDDEN:xnas", L"Flat", L"0", L"", L"EUR +900,00" },
        { dmyDate(previousMonthYear, previousMonth, 15), L"Saxo", L"DEMO100001", L"DEMOL:xnas", L"Flat", L"0", L"", L"EUR +100,00" }
    };
    snapshot.accounts.rows = {
        { L"Saxo Group live", L"DEMO100001", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"" },
        { L"Saxo Group live", L"DEMO200002", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"" }
    };
    status.trackerDateOrder = L"dmy";
    const std::wstring historyReport = BuildStatusReport(status, snapshot);
    RequireContains(
        historyReport,
        L"Current month Realized P/L DEMO100001",
        "First visible account's Position History total is missing");
    RequireContains(
        historyReport,
        L"EUR -13,18",
        "Current-month realized P/L included the wrong rows or amount");
    RequireContains(
        historyReport,
        L"Current month Realized P/L DEMO200002",
        "Second visible account's Position History total is missing");
    RequireContains(
        historyReport,
        L"EUR +7,25",
        "Second visible account's Position History amount is incorrect");
    RequireNotContains(
        historyReport,
        L"EUR +900,00",
        "A Position History account absent from Accounts was included");
    const std::wstring realizedLine = FindLineContaining(historyReport, L"EUR -13,18");
    if (DelimitedField(realizedLine, 0) != L"Current month Realized P/L DEMO100001" ||
        DelimitedField(realizedLine, 1) != L"EUR -13,18" ||
        !DelimitedField(realizedLine, 2).empty())
    {
        throw std::runtime_error("Realized P/L is not aligned or still contains a width-expanding comment");
    }
    RequireNotContains(
        historyReport,
        L"not totalled]",
        "Realized P/L still contains the removed row-count comment");
    RequireNotContains(
        historyReport,
        L"unambiguous currency]",
        "Realized P/L still contains the removed explanatory comment");
    RequireContains(
        BuildStatusReportHtml(historyReport),
        L"<span style=\"color:#B4232A;font-weight:600;\">EUR -13,18</span>",
        "Negative current-month realized P/L is not fully red");
    RequireContains(
        BuildStatusReportHtml(historyReport),
        L"<span style=\"color:#15803D;font-weight:600;\">EUR +7,25</span>",
        "Positive account-specific current-month realized P/L is not fully green");

    TrackerStatusSnapshot closedOnlySnapshot = snapshot;
    closedOnlySnapshot.openPositions.rows.clear();
    const std::wstring closedOnlyReport = BuildStatusReport(status, closedOnlySnapshot);
    RequireContains(
        closedOnlyReport,
        L"EUR -13,18",
        "Current-month realized P/L disappeared when there were no open positions");

    AppConfig authenticationConfig;
    authenticationConfig.brokerMonitoringEnabled = true;
    authenticationConfig.brokerAlertEmailEnabled = true;
    BrokerAuthenticationDetection authentication;
    authentication.detected = true;
    authentication.profileName = L"Saxo";
    authentication.sanitizedUrl = L"developer.saxobank.com/login";
    authentication.alertAfterSeconds = 3;
    BrokerMonitor authenticationMonitor;
    TrackerBridgeSection noMonitoringLogs;
    noMonitoringLogs.ok = true;
    const auto authenticationStart = std::chrono::system_clock::now();
    const BrokerMonitorDecision authenticationGrace = authenticationMonitor.Evaluate(
        noMonitoringLogs, authentication, authenticationConfig, authenticationStart);
    if (authenticationGrace.status.state != mcst::HealthState::Attention ||
        authenticationGrace.sendAlertEmail)
    {
        throw std::runtime_error("Broker authentication grace period was not started correctly");
    }
    const BrokerMonitorDecision authenticationAlert = authenticationMonitor.Evaluate(
        noMonitoringLogs, authentication, authenticationConfig,
        authenticationStart + std::chrono::seconds(4));
    if (authenticationAlert.status.state != mcst::HealthState::Critical ||
        !authenticationAlert.sendAlertEmail ||
        authenticationAlert.subject != L"MCST-Watchdog Broker Authentication Required")
    {
        throw std::runtime_error("Persistent Saxo authentication did not produce a critical email alert");
    }

    // Exact regression from the R20 screenshot: rows are newest first. More
    // than ten unrelated warnings follow a successful Saxo connection, while
    // the older event says that no connection existed. The newest broker-state
    // evidence must win even though it is outside the ten-row display window.
    TrackerBridgeSection monitoringHistory;
    monitoringHistory.present = true;
    monitoringHistory.ok = true;
    monitoringHistory.expectedColumns = 6;
    for (int index = 0; index < 20; ++index)
    {
        monitoringHistory.rows.push_back({
            L"17/08/2026 17:" + std::to_wstring(39 - index) + L".00",
            L"Warning", L"", L"Saxo Group live", L"-",
            L"REAL-TIME CHART REQUEST - UIC is not valid"
        });
    }
    monitoringHistory.rows.push_back({
        L"17/08/2026 16.20.37", L"Information", L"", L"Saxo Group live", L"-",
        L"Connection to Saxo Group has been established. [ AccountType : Live ]"
    });
    monitoringHistory.rows.push_back({
        L"17/08/2026 16.20.03", L"Warning", L"", L"Saxo Group live", L"-",
        L"No connection to Saxo Group trading system"
    });
    monitoringHistory.declaredRows = monitoringHistory.rows.size();

    AppConfig brokerConfig;
    brokerConfig.brokerMonitoringEnabled = true;
    brokerConfig.brokerConnectedPatterns = { L"connection established" };
    brokerConfig.brokerDisconnectPatterns = { L"no connection to saxo group trading system" };
    BrokerAuthenticationDetection noAuthentication;
    BrokerMonitor brokerMonitor;
    const BrokerMonitorDecision brokerDecision = brokerMonitor.Evaluate(
        monitoringHistory,
        noAuthentication,
        brokerConfig,
        std::chrono::system_clock::now());
    if (brokerDecision.status.state != mcst::HealthState::Healthy ||
        brokerDecision.status.value != L"Connected")
    {
        throw std::runtime_error("The newest broker-state event outside the ten-row display window did not win");
    }

    int redSamples = 0, clearSamples = 0;
    bool confirmedWarning = false;
    mcst::UpdateVisibleWarning(true, true, redSamples, clearSamples, confirmedWarning);
    if (confirmedWarning) throw std::runtime_error("One red sample must not confirm a warning");
    mcst::UpdateVisibleWarning(true, true, redSamples, clearSamples, confirmedWarning);
    if (!confirmedWarning) throw std::runtime_error("Two red samples must confirm a warning");
    mcst::UpdateVisibleWarning(false, false, redSamples, clearSamples, confirmedWarning);
    if (!confirmedWarning) throw std::runtime_error("An unchecked warning must remain confirmed");
    mcst::UpdateVisibleWarning(true, false, redSamples, clearSamples, confirmedWarning);
    if (!confirmedWarning) throw std::runtime_error("One clear sample must not clear a warning");
    mcst::UpdateVisibleWarning(true, false, redSamples, clearSamples, confirmedWarning);
    if (confirmedWarning) throw std::runtime_error("Two clear samples must clear a warning");
    if (!mcst::IsVisibleRedWarning(80, 80, 60) ||
        mcst::IsVisibleRedWarning(80, 79, 60) || mcst::IsVisibleRedWarning(80, 80, 1))
        throw std::runtime_error("Visible warning background/occlusion policy failed");
    if (std::wstring(mcst::OverallHeadline(mcst::HealthState::Healthy, true)) != L"SYSTEM HEALTHY" ||
        std::wstring(mcst::OverallHeadline(mcst::HealthState::Unknown, false)) != L"INITIALIZING" ||
        std::wstring(mcst::OverallHeadline(mcst::HealthState::Unknown, true)) != L"CHECK INCOMPLETE" ||
        std::wstring(mcst::OverallHeadline(mcst::HealthState::Attention, true)) != L"ATTENTION REQUIRED" ||
        std::wstring(mcst::OverallHeadline(mcst::HealthState::Critical, true)) != L"CRITICAL CONDITION")
        throw std::runtime_error("Overall Dashboard headline policy failed");
    if (!mcst::IsConfirmedRenderedWarning(true, 2) ||
        mcst::IsConfirmedRenderedWarning(false, 2) || mcst::IsConfirmedRenderedWarning(true, 1) ||
        mcst::IsConfirmedRenderedWarning(true, 0))
        throw std::runtime_error("Covered rendering must complete two positive passes");

    const std::vector<std::vector<std::wstring>> oldLogs = {{L"one"}, {L"two"}};
    if (mcst::CountAddedLogRows(oldLogs, oldLogs) != 0 ||
        mcst::CountAddedLogRows(oldLogs, {{L"two"}, {L"three"}}) != 1 ||
        mcst::CountAddedLogRows({{L"one"}}, {{L"one"}, {L"one"}}) != 1)
        throw std::runtime_error("Log row change policy failed");
    const auto cleanReport = mcst::WithoutQueueDiagnostics(
        L"SYSTEM STATUS\nRed queue warning detected\nVISIBLE QUEUE WARNINGS (test)\nPID 1: red\n\nQUEUE READ DIAGNOSTICS (test)\nPID 1: technical\n\nACCOUNTS\naccount data\n");
    if (cleanReport.find(L"PID 1") != std::wstring::npos ||
        cleanReport.find(L"Red queue warning detected") == std::wstring::npos ||
        cleanReport.find(L"account data") == std::wstring::npos)
        throw std::runtime_error("Email queue diagnostic filter failed");
    std::wcout << L"MCST logic and regression tests passed.\n";
    return 0;
}

int wmain()
{
    try
    {
        return RunLogicTests();
    }
    catch (const std::exception& error)
    {
        std::cerr << "MCST logic and regression tests FAILED: " << error.what() << '\n';
        return 1;
    }
    catch (...)
    {
        std::cerr << "MCST logic and regression tests FAILED: unknown exception\n";
        return 1;
    }
}
