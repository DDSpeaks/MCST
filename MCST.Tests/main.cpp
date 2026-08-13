#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include "../MCST.Shared/MCBridgeProtocol.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"
#include "../MCST.Watchdog/StatusReport.h"

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
}

int wmain()
{
    static_assert(sizeof(mcbridge::MessageHeader) == 20);
    static_assert(std::is_default_constructible_v<TrackerStatusSnapshot>);

    mcst::WatchdogSystemStatus status;
    TrackerStatusSnapshot snapshot;
    snapshot.bridgeVersion = 171;
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
    const std::wstring totalLine = FindLineContaining(report, L"EUR +8,25");
    if (DelimitedField(totalLine, 6) != L"Total Open P/L" ||
        DelimitedField(totalLine, 7) != L"EUR +8,25" ||
        DelimitedField(totalLine, 8) != L"[2 rows]")
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
    if (DelimitedField(currentTotalLine, 7) != L"EUR +106,68" ||
        DelimitedField(currentTotalLine, 8) != L"[10 rows]")
    {
        throw std::runtime_error("Current Saxo total is not in the Open P/L column");
    }
    if (currentCaptureReport.find(L"OPEN P/L ROWS NOT TOTALLED") != std::wstring::npos)
        throw std::runtime_error("Current Saxo capture unexpectedly excluded an Open P/L row");

    std::wcout << L"MCST foundation smoke tests passed.\n";
    return 0;
}
