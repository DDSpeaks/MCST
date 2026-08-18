#include <iostream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include "../MCST.Shared/MCBridgeProtocol.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"
#include "../MCST.Watchdog/BrokerMonitor.h"
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

int wmain()
{
    static_assert(sizeof(mcbridge::MessageHeader) == 20);
    static_assert(std::is_default_constructible_v<TrackerStatusSnapshot>);

    mcst::WatchdogSystemStatus status;
    TrackerStatusSnapshot snapshot;
    snapshot.bridgeVersion = 175;
    snapshot.protocolVersion = 2;
    snapshot.openPositions.present = true;
    snapshot.openPositions.ok = true;
    snapshot.openPositions.expectedColumns = 8;
    snapshot.openPositions.rows = {
        { L"Saxo", L"A", L"AAOI:xnas", L"Long", L"3", L"139,97", L"\u20ac 10,50", L"now" },
        { L"Saxo", L"A", L"APLD:xnas", L"Long", L"15", L"30,76", L"EUR -2,25", L"now" },
        { L"Saxo", L"A", L"BE:xnys", L"Long", L"2", L"236,00", L"$ 3,00", L"now" },
        { L"Saxo", L"A", L"CLS:xnys", L"Long", L"1", L"333,84", L"USD \u20ac 4,00", L"now" }
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
        DelimitedField(totalLine, 2) != L"[2 rows]")
    {
        throw std::runtime_error("Open P/L total is not aligned below the Open P/L detail column");
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
        reportHtml,
        L"class=\"mcst-overall-status-lines\"",
        "Overall Status is not rendered in a protected one-line flow");
    RequireContains(
        reportHtml,
        L"class=\"mcst-system-status-lines\"",
        "System Status is not rendered in a protected one-line flow");
    RequireContains(
        reportHtml,
        L"display:inline-block;width:4ch;text-align:center",
        "Status dots do not preserve the fixed character column");
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

    RequireContains(reportHtml, L"AAOI:xnas", "Symbol is missing from the one-line Open Positions section");
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
        { L"Saxo", L"A", L"APLD:xnas", L"Long", L"15", L"30,76", L"EUR -2,25", L"now" }
    };
    const std::wstring negativeTotalHtml =
        BuildStatusReportHtml(BuildStatusReport(status, negativeSnapshot));
    const std::wstring negativeSpan =
        L"<span style=\"color:#B4232A;font-weight:600;\">EUR -2,25</span>";
    if (CountOccurrences(negativeTotalHtml, negativeSpan) != 2)
        throw std::runtime_error("Negative Open P/L detail and total are not both fully red");

    snapshot.openPositions.rows = {
        { L"Saxo Group live", L"910792INET", L"AAOI:xnas", L"Long", L"3", L"139,970", L"\u20ac -3,64", L"now" },
        { L"Saxo Group live", L"910792INET", L"APLD:xnas", L"Long", L"15", L"30,76000", L"\u20ac 8,84", L"now" },
        { L"Saxo Group live", L"910792INET", L"BE:xnys", L"Long", L"2", L"236,000", L"\u20ac 10,99", L"now" },
        { L"Saxo Group live", L"910792INET", L"CLS:xnys", L"Long", L"1", L"333,840", L"\u20ac 26,04", L"now" },
        { L"Saxo Group live", L"910792INET", L"COHU:xnas", L"Long", L"8", L"56,430", L"\u20ac 10,12", L"now" },
        { L"Saxo Group live", L"910792INET", L"IONQ:xnys", L"Long", L"11", L"43,820", L"\u20ac 17,83", L"now" },
        { L"Saxo Group live", L"910792INET", L"IREN:xnas", L"Long", L"11", L"42,570", L"\u20ac 34,98", L"now" },
        { L"Saxo Group live", L"910792INET", L"LWLG:xnas", L"Long", L"57", L"8,14920", L"\u20ac -15,27", L"now" },
        { L"Saxo Group live", L"910792INET", L"MEI:xnys", L"Long", L"27", L"16,870", L"\u20ac 8,89", L"now" },
        { L"Saxo Group live", L"910792INET", L"VECO:xnas", L"Long", L"9", L"53,367", L"\u20ac 7,90", L"now" }
    };
    const std::wstring currentCaptureReport = BuildStatusReport(status, snapshot);
    RequireContains(
        currentCaptureReport,
        L"EUR +106,68",
        "Current Saxo capture Open P/L total was not calculated correctly");
    const std::wstring currentTotalLine = FindLineContaining(currentCaptureReport, L"EUR +106,68");
    if (DelimitedField(currentTotalLine, 1) != L"EUR +106,68" ||
        DelimitedField(currentTotalLine, 2) != L"[10 rows]")
    {
        throw std::runtime_error("Current Saxo total is not in the Open P/L column");
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
        { dmyDate(local.tm_year + 1900, local.tm_mon + 1, 18), L"Saxo", L"A", L"IONQ:xnys", L"Flat", L"0", L"", L"EUR +5,60" },
        { dmyDate(local.tm_year + 1900, local.tm_mon + 1, 17), L"Saxo", L"A", L"APLD:xnas", L"Flat", L"0", L"", L"\u20ac -18,78" },
        { dmyDate(previousMonthYear, previousMonth, 15), L"Saxo", L"A", L"OLD:xnas", L"Flat", L"0", L"", L"EUR +100,00" }
    };
    status.trackerDateOrder = L"dmy";
    const std::wstring historyReport = BuildStatusReport(status, snapshot);
    RequireContains(
        historyReport,
        L"Current month Realized P/L",
        "Current-month Position History total is missing");
    RequireContains(
        historyReport,
        L"EUR -13,18",
        "Current-month realized P/L included the wrong rows or amount");
    const std::wstring realizedLine = FindLineContaining(historyReport, L"EUR -13,18");
    if (DelimitedField(realizedLine, 0) != L"Current month Realized P/L" ||
        DelimitedField(realizedLine, 1) != L"EUR -13,18" ||
        DelimitedField(realizedLine, 2) != L"[2 rows]")
    {
        throw std::runtime_error("Realized P/L is not aligned below the Open P/L column");
    }
    RequireContains(
        BuildStatusReportHtml(historyReport),
        L"<span style=\"color:#B4232A;font-weight:600;\">EUR -13,18</span>",
        "Negative current-month realized P/L is not fully red");

    TrackerStatusSnapshot closedOnlySnapshot = snapshot;
    closedOnlySnapshot.openPositions.rows.clear();
    const std::wstring closedOnlyReport = BuildStatusReport(status, closedOnlySnapshot);
    RequireContains(
        closedOnlyReport,
        L"EUR -13,18",
        "Current-month realized P/L disappeared when there were no open positions");

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

    std::wcout << L"MCST foundation smoke tests passed.\n";
    return 0;
}
