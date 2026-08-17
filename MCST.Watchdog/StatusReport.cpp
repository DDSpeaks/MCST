#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "StatusReport.h"

#include <iomanip>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cwchar>
#include <map>

namespace
{
    const wchar_t* StateMarker(mcst::HealthState state)
    {
        switch (state)
        {
        case mcst::HealthState::Healthy: return L"[OK]";
        case mcst::HealthState::Attention: return L"[!]";
        case mcst::HealthState::Critical: return L"[X]";
        default: return L"[?]";
        }
    }

    std::wstring StateCell(mcst::HealthState state)
    {
        return std::wstring(StateMarker(state)) + L" " + mcst::HealthStateText(state);
    }

    std::wstring MonitorLine(const wchar_t* name, const mcst::MonitorStatus& item)
    {
        std::wostringstream out;
        out << std::left << std::setw(22) << name
            << std::setw(18) << StateCell(item.state)
            << item.value;
        if (!item.detail.empty())
            out << L"  " << item.detail;
        out << L'\n';
        return out.str();
    }

    std::wstring FormatGb(unsigned long long bytes)
    {
        std::wostringstream out;
        out << std::fixed << std::setprecision(1)
            << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << L" GB";
        return out.str();
    }

    std::wstring FormatPercent(double value)
    {
        std::wostringstream out;
        out << std::fixed << std::setprecision(0) << value << L"%";
        return out.str();
    }

    void AppendSystemResources(std::wostringstream& out, const mcst::WatchdogSystemStatus& status)
    {
        constexpr int resourceWidth = 16;
        constexpr int totalWidth = 19;
        constexpr int freeWidth = 19;
        constexpr int usedWidth = 10;

        out << L"SYSTEM RESOURCES\n"
            << L"----------------------------------------------------------------\n"
            << std::left << std::setw(resourceWidth) << L"Resource"
            << std::right << std::setw(totalWidth) << L"Total/Capacity"
            << std::setw(freeWidth) << L"Free"
            << std::setw(usedWidth) << L"Used %" << L'\n'
            << L"----------------------------------------------------------------\n";

        if (status.systemMemoryAvailable)
        {
            out << std::left << std::setw(resourceWidth) << L"RAM"
                << std::right << std::setw(totalWidth) << FormatGb(status.totalPhysicalMemoryBytes)
                << std::setw(freeWidth) << FormatGb(status.availablePhysicalMemoryBytes)
                << std::setw(usedWidth) << (std::to_wstring(status.memoryLoadPercent) + L"%") << L'\n';
        }
        else
        {
            out << std::left << std::setw(resourceWidth) << L"RAM"
                << std::right << std::setw(totalWidth) << L"n/a"
                << std::setw(freeWidth) << L"n/a"
                << std::setw(usedWidth) << L"n/a" << L'\n';
        }

        const std::wstring cpuCapacity = status.logicalProcessorCount > 0
            ? std::to_wstring(status.logicalProcessorCount) + L" logical CPUs"
            : L"n/a";
        out << std::left << std::setw(resourceWidth) << L"CPU"
            << std::right << std::setw(totalWidth) << cpuCapacity
            << std::setw(freeWidth) << L"n/a"
            << std::setw(usedWidth) << (status.cpuAvailable ? FormatPercent(status.cpuPercent) : L"n/a") << L'\n';

        const std::wstring diskLabel = status.systemDiskRoot.empty() ? L"Disk" : L"Disk " + status.systemDiskRoot;
        if (status.systemDiskAvailable)
        {
            out << std::left << std::setw(resourceWidth) << diskLabel
                << std::right << std::setw(totalWidth) << FormatGb(status.diskTotalBytes)
                << std::setw(freeWidth) << FormatGb(status.diskFreeBytes)
                << std::setw(usedWidth) << FormatPercent(status.diskUsedPercent) << L'\n';
        }
        else
        {
            out << std::left << std::setw(resourceWidth) << diskLabel
                << std::right << std::setw(totalWidth) << L"n/a"
                << std::setw(freeWidth) << L"n/a"
                << std::setw(usedWidth) << L"n/a" << L'\n';
        }

        out << L"\nWATCHDOG PROCESS\n"
            << L"----------------\n"
            << std::left << std::setw(22) << L"Private memory" << (status.privateMemoryBytes / (1024 * 1024)) << L" MB\n"
            << std::left << std::setw(22) << L"Handles" << status.handleCount << L'\n'
            << std::left << std::setw(22) << L"Uptime" << status.uptime << L'\n';
    }

    std::wstring SingleLineCell(std::wstring value)
    {
        for (wchar_t& ch : value)
        {
            if (ch == L'\r' || ch == L'\n' || ch == L'\t')
                ch = L' ';
        }
        return value;
    }

    std::wstring PadCell(const std::wstring& value, std::size_t width, bool rightAlign)
    {
        const std::wstring clean = SingleLineCell(value);
        if (clean.size() >= width)
            return clean;
        const std::wstring padding(width - clean.size(), L' ');
        return rightAlign ? padding + clean : clean + padding;
    }

    std::vector<std::size_t> CalculateColumnWidths(
        const std::vector<std::vector<std::wstring>>& rows,
        std::size_t columnCount,
        const std::vector<std::wstring>* headers = nullptr,
        bool lastColumnFlexible = false)
    {
        std::vector<std::size_t> widths(columnCount, 0);
        if (headers)
        {
            for (std::size_t column = 0; column < columnCount && column < headers->size(); ++column)
                widths[column] = (std::max)(widths[column], SingleLineCell((*headers)[column]).size());
        }

        for (const auto& row : rows)
        {
            for (std::size_t column = 0; column < columnCount && column < row.size(); ++column)
            {
                if (lastColumnFlexible && column + 1 == columnCount)
                    continue;
                widths[column] = (std::max)(widths[column], SingleLineCell(row[column]).size());
            }
        }
        return widths;
    }

    void AppendAlignedRow(
        std::wostringstream& out,
        const std::vector<std::wstring>& row,
        const std::vector<std::size_t>& widths,
        const std::vector<bool>& rightAligned,
        bool lastColumnFlexible)
    {
        const std::size_t columnCount = widths.size();
        for (std::size_t column = 0; column < columnCount; ++column)
        {
            if (column != 0)
                out << L" | ";

            const std::wstring value = column < row.size() ? row[column] : L"";
            if (lastColumnFlexible && column + 1 == columnCount)
                out << SingleLineCell(value);
            else
                out << PadCell(value, widths[column], column < rightAligned.size() && rightAligned[column]);
        }
        out << L'\n';
    }

    std::size_t FixedTableWidth(const std::vector<std::size_t>& widths, bool lastColumnFlexible)
    {
        if (widths.empty())
            return 0;
        std::size_t width = 0;
        for (std::size_t column = 0; column < widths.size(); ++column)
        {
            if (lastColumnFlexible && column + 1 == widths.size())
                break;
            width += widths[column];
            if (column + 1 < widths.size())
                width += 3; // " | "
        }
        return width;
    }

    void AppendAlignedSectionRows(
        std::wostringstream& out,
        const TrackerBridgeSection& section,
        bool lastColumnFlexible)
    {
        if (section.rows.empty())
        {
            out << L"(no rows)\n";
            return;
        }

        const std::size_t columnCount = section.expectedColumns > 0
            ? section.expectedColumns
            : section.rows.front().size();
        const std::vector<std::size_t> widths = CalculateColumnWidths(
            section.rows, columnCount, nullptr, lastColumnFlexible);
        const std::vector<bool> alignments(columnCount, false);

        for (const auto& row : section.rows)
            AppendAlignedRow(out, row, widths, alignments, lastColumnFlexible);
    }

    bool ParseLocalizedNumber(const std::wstring& raw, double& value)
    {
        std::wstring cleaned;
        cleaned.reserve(raw.size());
        for (const wchar_t ch : raw)
        {
            if ((ch >= L'0' && ch <= L'9') || ch == L'+' || ch == L'-' || ch == L',' || ch == L'.')
                cleaned.push_back(ch);
        }
        if (cleaned.empty())
            return false;

        const std::size_t lastComma = cleaned.find_last_of(L',');
        const std::size_t lastDot = cleaned.find_last_of(L'.');
        const bool hasComma = lastComma != std::wstring::npos;
        const bool hasDot = lastDot != std::wstring::npos;

        wchar_t decimalSeparator = 0;
        if (hasComma && hasDot)
            decimalSeparator = lastComma > lastDot ? L',' : L'.';
        else if (hasComma)
            decimalSeparator = L',';
        else if (hasDot)
            decimalSeparator = L'.';

        std::wstring normalized;
        normalized.reserve(cleaned.size());
        for (std::size_t index = 0; index < cleaned.size(); ++index)
        {
            const wchar_t ch = cleaned[index];
            if (ch == L',' || ch == L'.')
            {
                if (ch == decimalSeparator)
                    normalized.push_back(L'.');
                continue;
            }
            normalized.push_back(ch);
        }

        wchar_t* end = nullptr;
        errno = 0;
        const double parsed = std::wcstod(normalized.c_str(), &end);
        if (errno == ERANGE || end == normalized.c_str() || *end != L'\0')
            return false;
        value = parsed;
        return true;
    }

    std::wstring FormatReportNumber(double value, bool showPlusForPositive = false)
    {
        const bool negative = value < 0.0;
        const double magnitude = std::fabs(value);
        std::wostringstream raw;
        raw << std::fixed << std::setprecision(2) << magnitude;
        std::wstring text = raw.str();

        const std::size_t decimalPoint = text.find(L'.');
        std::wstring integerPart = decimalPoint == std::wstring::npos ? text : text.substr(0, decimalPoint);
        const std::wstring decimalPart = decimalPoint == std::wstring::npos ? L"00" : text.substr(decimalPoint + 1);

        std::wstring grouped;
        for (std::size_t index = 0; index < integerPart.size(); ++index)
        {
            if (index != 0 && (integerPart.size() - index) % 3 == 0)
                grouped.push_back(L' ');
            grouped.push_back(integerPart[index]);
        }

        std::wstring result;
        if (negative)
            result.push_back(L'-');
        else if (showPlusForPositive && magnitude > 0.0000001)
            result.push_back(L'+');
        result += grouped;
        result += L',';
        result += decimalPart;
        return result;
    }

    bool IsAsciiLetter(wchar_t ch)
    {
        return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
    }

    bool TryExtractKnownCurrency(const std::wstring& raw, std::wstring& currency)
    {
        currency.clear();
        const bool hasEuroSign = raw.find(L'\u20ac') != std::wstring::npos;
        std::wstring explicitCurrency;

        // Accept an explicit three-letter code such as EUR, USD or CHF. Symbols
        // such as '$', 'kr' and the yen/yuan sign are deliberately not inferred:
        // without an ISO code they can represent more than one currency.
        for (std::size_t begin = 0; begin < raw.size();)
        {
            while (begin < raw.size() && !IsAsciiLetter(raw[begin]))
                ++begin;
            std::size_t end = begin;
            while (end < raw.size() && IsAsciiLetter(raw[end]))
                ++end;

            if (end - begin == 3 &&
                raw[begin] >= L'A' && raw[begin] <= L'Z' &&
                raw[begin + 1] >= L'A' && raw[begin + 1] <= L'Z' &&
                raw[begin + 2] >= L'A' && raw[begin + 2] <= L'Z')
            {
                const std::wstring candidate = raw.substr(begin, 3);
                if (!explicitCurrency.empty() && explicitCurrency != candidate)
                    return false;
                explicitCurrency = candidate;
            }
            begin = end;
        }

        // Reject contradictory evidence instead of choosing one representation.
        if (hasEuroSign && !explicitCurrency.empty() && explicitCurrency != L"EUR")
            return false;
        if (hasEuroSign)
        {
            currency = L"EUR";
            return true;
        }
        if (!explicitCurrency.empty())
        {
            currency = explicitCurrency;
            return true;
        }
        return false;
    }

    void AppendOpenPositionsTable(std::wostringstream& out, const TrackerBridgeSection& section)
    {
        if (section.rows.empty())
        {
            out << L"(no rows)\n";
            return;
        }

        const std::vector<std::wstring> headers = {
            L"Profile", L"Account", L"Symbol", L"Side", L"Qty", L"Average Price",
            L"Native Value", L"Open P/L", L"Last Update"
        };
        std::vector<std::vector<std::wstring>> rows;
        rows.reserve(section.rows.size());
        std::map<std::wstring, double> openPlTotalsByCurrency;
        std::map<std::wstring, std::size_t> openPlRowsByCurrency;
        std::size_t openPlRowsNotTotaled = 0;

        for (const auto& source : section.rows)
        {
            std::vector<std::wstring> row(9);
            row[0] = source.size() > 0 ? source[0] : L"";
            row[1] = source.size() > 1 ? source[1] : L"";
            row[2] = source.size() > 2 ? source[2] : L"";
            row[3] = source.size() > 3 ? source[3] : L"";
            row[4] = source.size() > 4 ? source[4] : L"";
            row[5] = source.size() > 5 ? source[5] : L"";
            row[7] = source.size() > 6 ? source[6] : L"";
            row[8] = source.size() > 7 ? source[7] : L"";

            double quantity = 0.0;
            double averagePrice = 0.0;
            const bool quantityOk = ParseLocalizedNumber(row[4], quantity);
            const bool averagePriceOk = ParseLocalizedNumber(row[5], averagePrice);

            if (quantityOk && averagePriceOk)
            {
                const double nativeValue = std::fabs(quantity) * averagePrice;
                row[6] = FormatReportNumber(nativeValue);
            }
            else
            {
                row[6] = L"n/a";
            }

            double openPl = 0.0;
            std::wstring openPlCurrency;
            if (ParseLocalizedNumber(row[7], openPl) &&
                TryExtractKnownCurrency(row[7], openPlCurrency))
            {
                openPlTotalsByCurrency[openPlCurrency] += openPl;
                ++openPlRowsByCurrency[openPlCurrency];
            }
            else
            {
                ++openPlRowsNotTotaled;
            }

            rows.push_back(std::move(row));
        }

        std::vector<std::vector<std::wstring>> totalRows;
        for (const auto& total : openPlTotalsByCurrency)
        {
            std::vector<std::wstring> totalRow(headers.size());
            totalRow[6] = L"Total Open P/L";
            totalRow[7] = total.first + L" " + FormatReportNumber(total.second, true);
            totalRow[8] = L"[" + std::to_wstring(openPlRowsByCurrency[total.first]) + L" rows]";
            totalRows.push_back(std::move(totalRow));
        }

        std::vector<std::vector<std::wstring>> rowsForWidth = rows;
        rowsForWidth.insert(rowsForWidth.end(), totalRows.begin(), totalRows.end());
        const std::vector<std::size_t> widths =
            CalculateColumnWidths(rowsForWidth, headers.size(), &headers, false);
        const std::vector<bool> rightAligned = {
            false, false, false, false, true, true, true, true, false
        };

        AppendAlignedRow(out, headers, widths, std::vector<bool>(headers.size(), false), false);
        out << std::wstring(FixedTableWidth(widths, false), L'-') << L'\n';
        for (const auto& row : rows)
            AppendAlignedRow(out, row, widths, rightAligned, false);
        out << std::wstring(FixedTableWidth(widths, false), L'-') << L'\n';
        for (const auto& totalRow : totalRows)
            AppendAlignedRow(out, totalRow, widths, rightAligned, false);
        if (openPlTotalsByCurrency.empty())
            out << L"TOTAL OPEN P/L: not calculated - no row had an unambiguous currency.\n";
        if (openPlRowsNotTotaled != 0)
        {
            out << L"OPEN P/L ROWS NOT TOTALLED: " << openPlRowsNotTotaled
                << L" - value or currency was not unambiguous.\n";
        }
    }
}

std::wstring BuildStatusReport(const mcst::WatchdogSystemStatus& status, const TrackerStatusSnapshot& snapshot)
{
    std::wostringstream out;
    out << L"MCST-Watchdog Status Report\n"
        << L"===========================\n"
        << L"Watchdog version       1.114-R21\n"
        << L"Tracker Bridge         MCST Tracker Bridge 1.0 (internal V" << snapshot.bridgeVersion << L", protocol V" << snapshot.protocolVersion << L")\n"
        << L"MultiCharts            " << (status.multiChartsVersion.empty() ? L"Unknown" : status.multiChartsVersion) << L"\n"
        << L"MC executable          " << (status.multiChartsExecutable.empty() ? L"Unknown" : status.multiChartsExecutable) << L"\n"
        << L"AutoTrading profile    " << (status.multiChartsCompatibilityProfile.empty() ? L"Unknown" : status.multiChartsCompatibilityProfile) << L"\n"
        << L"Tracker profile        " << (status.trackerCompatibilityProfile.empty() ? L"Unknown" : status.trackerCompatibilityProfile) << L"\n";
    if (snapshot.atonpTrackerLoaded)
    {
        out << L"ATOnPTracker fingerprint 0x" << std::hex << std::uppercase << snapshot.atonpTrackerPeTimestamp
            << std::dec << L" / " << snapshot.atonpTrackerImageSize << L" bytes\n";
    }
    out << L"\n"
        << L"OVERALL STATUS\n"
        << L"--------------\n"
        << StateCell(status.overall) << L"\n\n"
        << L"SYSTEM STATUS\n"
        << L"-------------\n"
        << MonitorLine(L"Bridge", status.bridge)
        << MonitorLine(L"Tracker Snapshot", status.trackerSnapshot)
        << MonitorLine(L"AutoTrading", status.autoTrading)
        << MonitorLine(L"Broker", status.broker)
        << MonitorLine(L"Recent Logs", status.recentLogs)
        << MonitorLine(L"Status Reports", status.statusReports)
        << MonitorLine(L"Email", status.email)
        << MonitorLine(L"Heartbeat", status.heartbeat)
        << L"\nLATEST ACTIVITY\n"
        << L"---------------\n"
        << std::left << std::setw(22) << L"Last system update" << status.lastSnapshot << L'\n'
        << std::left << std::setw(22) << L"Last Snapshot" << status.lastSnapshot << L'\n'
        << std::left << std::setw(22) << L"Last AutoTrading Read" << (status.lastAutoTradingRead.empty() ? L"Never" : status.lastAutoTradingRead) << L'\n'
        << std::left << std::setw(22) << L"Last Status Report" << status.lastReport << L'\n'
        << std::left << std::setw(22) << L"Last Alert" << status.lastAlert << L"\n\n";

    AppendSystemResources(out, status);

    if (!status.lastError.empty())
        out << L"\nLATEST ERROR\n------------\n" << status.lastError << L'\n';

    if (status.trackerDataStale)
    {
        out << L"\nTRACKER TABLE DATA\n"
            << L"------------------\n"
            << L"WARNING: STALE TRACKER TABLE DATA - one or more failed sections use the last complete snapshot from "
            << (status.trackerDataTimestamp.empty() ? L"an earlier update" : status.trackerDataTimestamp)
            << L" because the current Tracker read failed. Current health remains CRITICAL.\n";
    }

    out << L"\nACCOUNTS\n--------\n";
    AppendAlignedSectionRows(out, snapshot.accounts, false);
    out << L"\nOPEN POSITIONS\n--------------\n";
    AppendOpenPositionsTable(out, snapshot.openPositions);
    out << L"\nRECENT LOGS\n-----------\n";
    AppendAlignedSectionRows(out, snapshot.recentLogs, true);
    return out.str();
}

std::wstring BuildStatusReportHtml(const std::wstring& plainText)
{
    auto escapeHtml = [](const std::wstring& value)
    {
        std::wstring escaped;
        escaped.reserve(value.size() + 256);
        for (const wchar_t ch : value)
        {
            switch (ch)
            {
            case L'&': escaped += L"&amp;"; break;
            case L'<': escaped += L"&lt;"; break;
            case L'>': escaped += L"&gt;"; break;
            case L'\"': escaped += L"&quot;"; break;
            default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    };

    auto trimRight = [](std::wstring value)
    {
        while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' || value.back() == L'\r'))
            value.pop_back();
        return value;
    };

    auto trim = [&](std::wstring value)
    {
        value = trimRight(std::move(value));
        std::size_t first = 0;
        while (first < value.size() && (value[first] == L' ' || value[first] == L'\t'))
            ++first;
        return value.substr(first);
    };

    auto stateHtml = [&](const std::wstring& stateCell)
    {
        struct StateStyle
        {
            const wchar_t* marker;
            const wchar_t* label;
            const wchar_t* dotColor;
            const wchar_t* textColor;
        };

        static const StateStyle styles[] = {
            { L"[OK]", L"OK", L"#16A34A", L"#15803D" },
            { L"[!]", L"WARNING", L"#E6A700", L"#B77900" },
            { L"[X]", L"CRITICAL", L"#D13438", L"#B4232A" },
            { L"[?]", L"UNKNOWN", L"#8B949E", L"#687078" }
        };

        for (const auto& style : styles)
        {
            if (stateCell.find(style.marker) != std::wstring::npos)
            {
                std::wstring result =
                    L"<span style=\"display:inline-block;width:10px;height:10px;background-color:";
                result += style.dotColor;
                result += L";border-radius:50%;margin-right:7px;vertical-align:middle;\"></span>";
                result += L"<span style=\"color:";
                result += style.textColor;
                result += L";font-weight:600;vertical-align:middle;\">";
                result += style.label;
                result += L"</span>";
                return result;
            }
        }
        return escapeHtml(trim(stateCell));
    };

    auto splitAlignedTableRow = [](const std::wstring& line)
    {
        const std::wstring separator = L" | ";
        std::vector<std::wstring> cells;
        std::size_t begin = 0;
        for (;;)
        {
            const std::size_t end = line.find(separator, begin);
            cells.push_back(end == std::wstring::npos
                ? line.substr(begin)
                : line.substr(begin, end - begin));
            if (end == std::wstring::npos)
                break;
            begin = end + separator.size();
        }
        return cells;
    };

    auto openPositionCellHtml = [&](const std::wstring& rawCell, std::size_t column)
    {
        constexpr std::size_t openPlColumn = 7;
        const std::wstring cell = trim(rawCell);
        if (cell.empty())
            return std::wstring(L"&nbsp;");

        if (column != openPlColumn)
            return escapeHtml(cell);

        double profit = 0.0;
        if (!ParseLocalizedNumber(cell, profit) ||
            std::fabs(profit) < 0.0000001)
        {
            return escapeHtml(cell);
        }

        std::wstring result = profit > 0.0
            ? L"<span style=\"color:#15803D;font-weight:600;\">"
            : L"<span style=\"color:#B4232A;font-weight:600;\">";
        result += escapeHtml(cell);
        result += L"</span>";
        return result;
    };

    std::vector<std::wstring> lines;
    {
        std::wistringstream input(plainText);
        std::wstring line;
        while (std::getline(input, line))
            lines.push_back(trimRight(line));
    }

    std::wstring html;
    html += L"<!doctype html>\r\n";
    html += L"<html><head><meta charset=\"utf-8\">";
    html += L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">";
    html += L"<style>body,table,tbody,tr,td,th,div,span,pre,p{font-family:Consolas,\'Courier New\',monospace !important;}";
    html += L"body,table,td,th,pre{-webkit-text-size-adjust:100% !important;-ms-text-size-adjust:100% !important;}";
    html += L"</style></head>\r\n";
    html += L"<body style=\"margin:0;padding:16px;background:#ffffff;color:#202020;font-family:Consolas,'Courier New',monospace;font-size:15px;line-height:1.28;-webkit-text-size-adjust:100%;-ms-text-size-adjust:100%;\">\r\n";

    bool inOverall = false;
    bool inSystemStatus = false;
    bool systemTableOpen = false;
    bool preOpen = false;
    bool inOpenPositions = false;
    bool openPositionsTableOpen = false;

    auto openPre = [&]()
    {
        if (!preOpen)
        {
            html += L"<pre style=\"font-family:Consolas,'Courier New',monospace;font-size:15px;line-height:1.28;white-space:pre;margin:0;\">";
            preOpen = true;
        }
    };

    auto closePre = [&]()
    {
        if (preOpen)
        {
            html += L"</pre>\r\n";
            preOpen = false;
        }
    };

    auto closeSystemTable = [&]()
    {
        if (systemTableOpen)
        {
            html += L"</table>\r\n";
            systemTableOpen = false;
        }
    };

    auto openOpenPositionsTable = [&]()
    {
        if (openPositionsTableOpen)
            return;
        closePre();
        html += L"<div class=\"mcst-horizontal-scroll\" style=\"display:block;width:100%;max-width:100%;overflow-x:auto;-webkit-overflow-scrolling:touch;margin:0 0 2px 0;\">\r\n";
        html += L"<table class=\"mcst-open-positions\" role=\"table\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"border-collapse:collapse;width:100%;min-width:max-content;table-layout:auto;font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;line-height:1.28 !important;-webkit-text-size-adjust:100% !important;-ms-text-size-adjust:100% !important;\"><tbody>\r\n";
        openPositionsTableOpen = true;
    };

    auto closeOpenPositionsTable = [&]()
    {
        if (!openPositionsTableOpen)
            return;
        html += L"</tbody></table>\r\n</div>\r\n";
        openPositionsTableOpen = false;
    };

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::wstring& line = lines[i];

        if (line == L"OPEN POSITIONS")
            inOpenPositions = true;
        else if (line == L"RECENT LOGS")
        {
            closeOpenPositionsTable();
            inOpenPositions = false;
        }

        if (line == L"OVERALL STATUS")
        {
            closePre();
            closeSystemTable();
            inOverall = true;
            inSystemStatus = false;
            html += L"<div style=\"font-size:15px;font-weight:600;letter-spacing:.2px;margin-top:18px;margin-bottom:5px;\">OVERALL STATUS</div>";
            continue;
        }
        if (inOverall && line == L"--------------")
            continue;
        if (inOverall && !line.empty())
        {
            // Size the first two columns by their monospaced content instead of by
            // percentages. This keeps Component and Status readable on narrow mail
            // clients while allowing Description to consume and wrap in all remaining
            // space. The hidden Tracker Snapshot label gives Overall Status exactly the
            // same Component-column width as the System Status table.
            html += L"<table role=\"presentation\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"border-collapse:collapse;font-family:Consolas,'Courier New',monospace;font-size:15px;line-height:1.32;margin:3px 0 18px 0;width:100%;max-width:100%;table-layout:auto;\">";
            html += L"<tr>";
            html += L"<td width=\"1\" style=\"width:1px;padding:2px 18px 2px 0;vertical-align:top;white-space:nowrap;font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;\"><span style=\"visibility:hidden;white-space:nowrap;\">Tracker Snapshot</span></td>";
            html += L"<td width=\"1\" style=\"width:1px;padding:2px 18px 2px 0;vertical-align:top;white-space:nowrap;font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;\">" + stateHtml(line) + L"</td>";
            html += L"<td style=\"padding:2px 0;vertical-align:top;font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;white-space:normal;\"></td>";
            html += L"</tr></table>";
            inOverall = false;
            continue;
        }

        if (line == L"SYSTEM STATUS")
        {
            closePre();
            closeSystemTable();
            inSystemStatus = true;
            inOverall = false;
            html += L"<div style=\"font-size:15px;font-weight:600;letter-spacing:.2px;margin-top:4px;margin-bottom:7px;\">SYSTEM STATUS</div>";
            html += L"<table role=\"presentation\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"border-collapse:collapse;font-family:Consolas,'Courier New',monospace;font-size:15px;line-height:1.32;margin:0 0 18px 0;width:100%;max-width:100%;table-layout:auto;\">";
            systemTableOpen = true;
            continue;
        }
        if (inSystemStatus && line == L"-------------")
            continue;

        if (inSystemStatus)
        {
            if (line.empty())
            {
                closeSystemTable();
                inSystemStatus = false;
                continue;
            }

            // BuildStatusReport() formats every monitor row with fixed 22- and
            // 18-character fields. Preserve that semantic layout here, but use
            // real HTML columns so OK, WARNING, CRITICAL and UNKNOWN never move
            // the description column horizontally. Keep the component column wide enough for labels such as Tracker Snapshot,
            // while the description column may wrap naturally on narrow mail clients.
            std::wstring component = line.substr(0, std::min<std::size_t>(22, line.size()));
            std::wstring stateCell;
            std::wstring description;
            if (line.size() > 22)
                stateCell = line.substr(22, std::min<std::size_t>(18, line.size() - 22));
            if (line.size() > 40)
                description = line.substr(40);

            component = trim(component);
            stateCell = trim(stateCell);
            description = trim(description);

            html += L"<tr>";
            // The first two cells are content-sized (effectively character-sized in
            // this monospaced report). They never wrap. The Description cell has no
            // fixed width and therefore receives all remaining space and wraps naturally.
            html += L"<td width=\"1\" style=\"width:1px;padding:2px 18px 2px 0;vertical-align:top;white-space:nowrap;font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;font-weight:400;\">" + escapeHtml(component) + L"</td>";
            html += L"<td width=\"1\" style=\"width:1px;padding:2px 18px 2px 0;vertical-align:top;white-space:nowrap;font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;\">" + stateHtml(stateCell) + L"</td>";
            html += L"<td style=\"padding:2px 0;vertical-align:top;font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;font-weight:400;white-space:normal;word-break:normal;overflow-wrap:break-word;\">" + escapeHtml(description) + L"</td>";
            html += L"</tr>\r\n";
            continue;
        }

        if (line.rfind(L"WARNING: STALE TRACKER TABLE DATA", 0) == 0)
        {
            closePre();
            html += L"<div class=\"mcst-stale-tracker-warning\" style=\"margin:10px 0 14px 0;padding:10px 12px;border:1px solid #D13438;background:#FFF1F1;color:#B4232A;font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;font-weight:600;line-height:1.32;-webkit-text-size-adjust:100% !important;-ms-text-size-adjust:100% !important;\">";
            html += escapeHtml(line);
            html += L"</div>\r\n";
            continue;
        }

        if (inOpenPositions)
        {
            const std::vector<std::wstring> cells = splitAlignedTableRow(line);
            if (cells.size() == 9)
            {
                openOpenPositionsTable();
                const bool headerRow = trim(cells[0]) == L"Profile" &&
                    trim(cells[7]) == L"Open P/L";
                const bool totalRow = trim(cells[6]) == L"Total Open P/L";
                html += totalRow
                    ? L"<tr style=\"border-top:1px solid #A8A8A8;font-weight:600;\">"
                    : L"<tr>";
                for (std::size_t column = 0; column < cells.size(); ++column)
                {
                    const bool rightAligned = column >= 4 && column <= 7;
                    const bool wrapCell = column == 0 || column == 8;
                    const wchar_t* tag = headerRow ? L"th" : L"td";
                    html += L"<";
                    html += tag;
                    if (headerRow)
                        html += L" scope=\"col\"";
                    html += L" style=\"padding:2px 10px 2px 0;vertical-align:top;text-align:";
                    html += rightAligned ? L"right" : L"left";
                    html += L";white-space:";
                    html += wrapCell ? L"normal" : L"nowrap";
                    html += L";overflow-wrap:anywhere;font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;line-height:1.28 !important;-webkit-text-size-adjust:100% !important;-ms-text-size-adjust:100% !important;";
                    if (headerRow)
                        html += L"font-weight:600;border-bottom:1px solid #A8A8A8;";
                    html += L"\">";
                    html += openPositionCellHtml(cells[column], column);
                    html += L"</";
                    html += tag;
                    html += L">";
                }
                html += L"</tr>\r\n";
                continue;
            }

            const bool separatorLine = !line.empty() &&
                std::all_of(line.begin(), line.end(), [](wchar_t ch) { return ch == L'-'; });
            if (openPositionsTableOpen && separatorLine)
                continue;
            if (openPositionsTableOpen)
                closeOpenPositionsTable();
        }

        // Keep data-heavy sections such as SYSTEM RESOURCES, ACCOUNTS,
        // and RECENT LOGS monospaced. OPEN POSITIONS is rendered as a real HTML
        // table above so mobile mail clients cannot shrink a wide preformatted
        // line to fit the viewport.
        openPre();
        html += escapeHtml(line);
        html += L"\n";
    }

    closeOpenPositionsTable();
    closeSystemTable();
    closePre();
    html += L"</body></html>\r\n";
    return html;
}

std::wstring BuildAlertWithStatusReportHtml(
    const std::wstring& alertIntroduction,
    const std::wstring& statusReportPlainText)
{
    // Always let BuildStatusReportHtml render the report itself. Alert messages only
    // prepend their own introduction to that finished HTML. Therefore scheduled reports,
    // AutoTrading alerts, Broker alerts, Log alerts and Heartbeats cannot accidentally
    // maintain different copies of the Status Report layout.
    std::wstring html = BuildStatusReportHtml(statusReportPlainText);

    auto escapeHtml = [](const std::wstring& value)
    {
        std::wstring escaped;
        escaped.reserve(value.size() + 64);
        for (const wchar_t ch : value)
        {
            switch (ch)
            {
            case L'&': escaped += L"&amp;"; break;
            case L'<': escaped += L"&lt;"; break;
            case L'>': escaped += L"&gt;"; break;
            case L'\"': escaped += L"&quot;"; break;
            default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    };

    if (alertIntroduction.empty())
        return html;

    const std::size_t bodyStart = html.find(L"<body");
    if (bodyStart == std::wstring::npos)
        return html;
    const std::size_t bodyTagEnd = html.find(L'>', bodyStart);
    if (bodyTagEnd == std::wstring::npos)
        return html;

    std::wstring intro;
    intro += L"\r\n<div style=\"margin:0 0 18px 0;font-family:Consolas,'Courier New',monospace;font-size:15px;line-height:1.32;\">";
    intro += L"<div style=\"white-space:pre-wrap;overflow-wrap:break-word;\">";
    intro += escapeHtml(alertIntroduction);
    intro += L"</div></div>\r\n";
    html.insert(bodyTagEnd + 1, intro);
    return html;
}

bool WriteUtf8TextFile(const std::wstring& path, const std::wstring& text, std::wstring& diagnostic)
{
    diagnostic.clear();
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        diagnostic = L"UTF-8 conversion failed.";
        return false;
    }
    std::string bytes(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), required, nullptr, nullptr);

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        diagnostic = L"Could not create report file. Windows error=" + std::to_wstring(GetLastError());
        return false;
    }
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    bool ok = WriteFile(file, bom, sizeof(bom), &written, nullptr) != FALSE && written == sizeof(bom);
    if (ok)
    {
        written = 0;
        ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE && written == bytes.size();
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    diagnostic = ok ? L"Status report written to " + path : L"Writing status report failed.";
    return ok;
}
