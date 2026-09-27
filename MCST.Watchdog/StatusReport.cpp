#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "StatusReport.h"
#include "../MCST.Shared/ReportReadPolicy.h"
#include "TrackerDateParser.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cwchar>
#include <map>
#include <utility>

namespace
{
    constexpr std::size_t kMonitorNameWidth = 18;
    constexpr std::size_t kMonitorStateWidth = 14;
    constexpr std::size_t kMonitorValueWidth = 26;

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

    std::wstring MonitorLine(
        const std::wstring& name,
        const mcst::MonitorStatus& item,
        std::size_t nameWidth,
        std::size_t stateWidth,
        std::size_t valueWidth)
    {
        std::wostringstream out;
        out << std::left << std::setw(static_cast<int>(nameWidth)) << name
            << std::setw(static_cast<int>(stateWidth)) << StateCell(item.state)
            << std::setw(static_cast<int>(valueWidth)) << item.value;
        if (!item.detail.empty())
            out << item.detail;
        out << L'\n';
        return out.str();
    }

    void AppendMonitorLines(std::wostringstream& out, const mcst::WatchdogSystemStatus& status)
    {
        const std::vector<std::pair<std::wstring, mcst::MonitorStatus>> rows = {
            { L"OVERALL STATUS", { status.overall, L"",
                status.multiChartsProcesses.visibleQueueUncheckedCount > 0
                    ? L"Queue visual check incomplete" : L"" } },
            { L"MultiCharts Health", status.multiChartsHealth },
            { L"Bridge", status.bridge },
            { L"Tracker Snapshot", status.trackerSnapshot },
            { L"AutoTrading", status.autoTrading },
            { L"Broker", status.broker },
            { L"Recent Logs", status.recentLogs },
            { L"Status Reports", status.statusReports },
            { L"Email", status.email },
            { L"Heartbeat", status.heartbeat }
        };

        out << MonitorLine(
            rows.front().first,
            rows.front().second,
            kMonitorNameWidth,
            kMonitorStateWidth,
            kMonitorValueWidth)
            << L'\n';
        for (std::size_t index = 1; index < rows.size(); ++index)
        {
            out << MonitorLine(
                rows[index].first,
                rows[index].second,
                kMonitorNameWidth,
                kMonitorStateWidth,
                kMonitorValueWidth);
        }
    }

    void AppendKeyValueRows(
        std::wostringstream& out,
        const std::vector<std::pair<std::wstring, std::wstring>>& rows)
    {
        std::size_t labelWidth = 0;
        for (const auto& row : rows)
            labelWidth = (std::max)(labelWidth, row.first.size());
        labelWidth += 2;

        for (const auto& row : rows)
            out << std::left << std::setw(static_cast<int>(labelWidth)) << row.first
                << row.second << L'\n';
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

    void AppendMultiChartsProcesses(std::wostringstream& out, const mcst::WatchdogSystemStatus& status)
    {
        out << L"\nMULTICHARTS PROCESSES\n"
            << L"--------------------------------------------------------------------------------\n";
        if (status.multiChartsProcesses.processes.empty())
        {
            out << L"(no MultiCharts processes found)\n";
            return;
        }

        out << std::left << std::setw(10) << L"PID"
            << std::setw(14) << L"State"
            << std::right << std::setw(11) << L"CPU/core"
            << std::setw(12) << L"Private MB"
            << std::setw(10) << L"Handles"
            << std::setw(8) << L"GDI"
            << std::setw(9) << L"USER"
            << std::setw(10) << L"Queue"
            << std::setw(9) << L"Delay" << L'\n'
            << L"--------------------------------------------------------------------------------\n";

        for (const auto& process : status.multiChartsProcesses.processes)
        {
            std::wstring cpu = L"n/a";
            if (process.cpuAvailable)
            {
                std::wostringstream formatted;
                formatted << std::fixed << std::setprecision(0) << process.cpuCorePercent << L"%";
                cpu = formatted.str();
            }
            const std::wstring queue = process.queueIndicatorFound
                ? std::to_wstring(process.queueCount) + L" q"
                : L"unread";
            const std::wstring delay = process.queueIndicatorFound
                ? std::to_wstring(process.queueAgeSeconds) + L" s"
                : L"n/a";

            out << std::left << std::setw(10) << process.processId
                << std::setw(14) << mcst::HealthStateText(process.state)
                << std::right << std::setw(11) << cpu
                << std::setw(12) << (process.privateMemoryBytes / (1024 * 1024))
                << std::setw(10) << process.handleCount
                << std::setw(8) << process.gdiObjects
                << std::setw(9) << process.userObjects
                << std::setw(10) << queue
                << std::setw(9) << delay << L'\n';
        }

        if (status.multiChartsProcesses.recentlyDisappearedProcessId != 0)
            out << L"Recently terminated PID: " << status.multiChartsProcesses.recentlyDisappearedProcessId << L'\n';
        out << L"CPU/core uses 100% to mean one fully occupied logical processor.\n";
        out << L"\nVISIBLE QUEUE WARNINGS (field 4; bounded covered-window trial; no numeric values)\n";
        for (const auto& process : status.multiChartsProcesses.processes)
            out << L"PID " << process.processId << L": check="
                << (!process.visibleQueueWarningChecked ? L"not checked" :
                    process.visibleQueueWarningRendered ? L"RED-rendered-experimental" :
                    process.visibleQueueWarningRed ? L"RED" : L"not red")
                << L"; confirmed-warning=" << (process.visibleQueueWarningConfirmed ? L"yes" : L"no")
                << L"; " << process.queueReadDiagnostic << L'\n';
        out << L"\nQUEUE READ DIAGNOSTICS (visible pixels + bounded covered-window rendering)\n";
        for (const auto& process : status.multiChartsProcesses.processes)
            out << L"PID " << process.processId << L": found="
                << (process.queueIndicatorFound ? L"yes" : L"no") << L" "
                << process.queueReadDiagnostic << L'\n';
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

    std::wstring TrimCell(std::wstring value)
    {
        const std::size_t begin = value.find_first_not_of(L" \t\r\n");
        if (begin == std::wstring::npos)
            return L"";
        const std::size_t end = value.find_last_not_of(L" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    std::wstring PadCell(const std::wstring& value, std::size_t width, bool rightAlign)
    {
        const std::wstring clean = TrimCell(SingleLineCell(value));
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
                widths[column] = (std::max)(widths[column], TrimCell(SingleLineCell((*headers)[column])).size());
        }

        for (const auto& row : rows)
        {
            for (std::size_t column = 0; column < columnCount && column < row.size(); ++column)
            {
                if (lastColumnFlexible && column + 1 == columnCount)
                    continue;
                widths[column] = (std::max)(widths[column], TrimCell(SingleLineCell(row[column])).size());
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

            const std::wstring value = column < row.size() ? TrimCell(row[column]) : L"";
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
        bool lastColumnFlexible,
        bool alignNumericColumns)
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
        std::vector<bool> alignments(columnCount, false);
        if (alignNumericColumns)
        {
            for (std::size_t column = 0; column < columnCount; ++column)
            {
                bool sawNumber = false;
                bool onlyNumbers = true;
                for (const auto& row : section.rows)
                {
                    const std::wstring value = column < row.size() ? TrimCell(row[column]) : L"";
                    if (value.empty())
                        continue;

                    bool hasDigit = false;
                    for (const wchar_t ch : value)
                    {
                        if (ch >= L'0' && ch <= L'9')
                        {
                            hasDigit = true;
                            continue;
                        }
                        if (ch != L' ' && ch != L'\u00A0' && ch != L'+' && ch != L'-' &&
                            ch != L',' && ch != L'.' && ch != L'\'')
                        {
                            onlyNumbers = false;
                            break;
                        }
                    }
                    if (!onlyNumbers)
                        break;
                    sawNumber = sawNumber || hasDigit;
                }
                alignments[column] = onlyNumbers && sawNumber;
            }
        }

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

    TrackerDateOrder WindowsDateOrderFallback()
    {
#ifdef _WIN32
        wchar_t value[8]{};
        if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IDATE, value,
                static_cast<int>(std::size(value))) > 0)
        {
            if (value[0] == L'0') return TrackerDateOrder::MonthDayYear;
            if (value[0] == L'1') return TrackerDateOrder::DayMonthYear;
            if (value[0] == L'2') return TrackerDateOrder::YearMonthDay;
        }
#endif
        return TrackerDateOrder::DayMonthYear;
    }

    void CurrentLocalYearMonth(int& year, int& month)
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        year = local.tm_year + 1900;
        month = local.tm_mon + 1;
    }

    std::vector<std::vector<std::wstring>> BuildPositionHistoryTotalRows(
        const TrackerBridgeSection& section,
        const TrackerBridgeSection& accounts,
        const std::wstring& configuredDateOrder)
    {
        constexpr std::size_t outputColumns = 9;
        std::vector<std::vector<std::wstring>> rows;

        std::vector<std::wstring> visibleAccounts;
        for (const auto& accountRow : accounts.rows)
        {
            if (accountRow.size() <= 1)
                continue;
            const std::wstring account = TrimCell(accountRow[1]);
            if (!account.empty() &&
                std::find(visibleAccounts.begin(), visibleAccounts.end(), account) == visibleAccounts.end())
            {
                visibleAccounts.push_back(account);
            }
        }

        auto unavailable = [&](const std::wstring& account)
        {
            std::vector<std::wstring> row(outputColumns);
            row[0] = account.empty()
                ? L"Current month Realized P/L"
                : L"Current month Realized P/L " + account;
            row[1] = L"not available";
            rows.push_back(std::move(row));
        };

        if (visibleAccounts.empty())
        {
            unavailable(L"");
            return rows;
        }
        if (!section.present)
        {
            for (const auto& account : visibleAccounts)
                unavailable(account);
            return rows;
        }
        if (!section.ok)
        {
            for (const auto& account : visibleAccounts)
                unavailable(account);
            return rows;
        }

        std::vector<std::wstring> dateValues;
        dateValues.reserve(section.rows.size());
        for (const auto& row : section.rows)
        {
            if (row.size() > 2 &&
                std::find(visibleAccounts.begin(), visibleAccounts.end(), TrimCell(row[2])) != visibleAccounts.end())
            {
                dateValues.push_back(row[0]);
            }
        }

        const TrackerDateOrder configured = ParseTrackerDateOrder(configuredDateOrder);
        const TrackerDateOrder order = configured == TrackerDateOrder::Auto
            ? DetectTrackerDateOrder(dateValues, WindowsDateOrderFallback())
            : configured;
        if (order == TrackerDateOrder::Auto)
        {
            for (const auto& account : visibleAccounts)
                unavailable(account);
            return rows;
        }

        int currentYear = 0;
        int currentMonth = 0;
        CurrentLocalYearMonth(currentYear, currentMonth);

        std::map<std::wstring, std::map<std::wstring, double>> totalsByAccount;
        std::size_t datesNotParsed = 0;
        for (const auto& row : section.rows)
        {
            if (row.size() < 8)
                continue;

            const std::wstring account = TrimCell(row[2]);
            if (std::find(visibleAccounts.begin(), visibleAccounts.end(), account) == visibleAccounts.end())
                continue;

            TrackerCalendarDate date;
            if (!TryParseTrackerCalendarDate(row[0], order, date))
            {
                ++datesNotParsed;
                continue;
            }
            if (date.year != currentYear || date.month != currentMonth)
                continue;

            double realizedPl = 0.0;
            std::wstring currency;
            if (ParseLocalizedNumber(row[7], realizedPl) &&
                TryExtractKnownCurrency(row[7], currency))
            {
                totalsByAccount[account][currency] += realizedPl;
            }
        }

        for (const auto& account : visibleAccounts)
        {
            const auto totals = totalsByAccount.find(account);
            if (totals == totalsByAccount.end() || totals->second.empty())
            {
                std::vector<std::wstring> row(outputColumns);
                row[0] = L"Current month Realized P/L " + account;
                row[1] = L"not calculated";
                rows.push_back(std::move(row));
                continue;
            }

            for (const auto& total : totals->second)
            {
                std::vector<std::wstring> row(outputColumns);
                row[0] = L"Current month Realized P/L " + account;
                row[1] = total.first + L" " + FormatReportNumber(total.second, true);
                rows.push_back(std::move(row));
            }
        }

        if (datesNotParsed != 0)
        {
            std::vector<std::wstring> row(outputColumns);
            row[0] = L"Position History dates skipped";
            row[1] = std::to_wstring(datesNotParsed);
            rows.push_back(std::move(row));
        }
        return rows;
    }

    void AppendOpenPositionsTable(
        std::wostringstream& out,
        const TrackerBridgeSection& section,
        const TrackerBridgeSection& positionHistory,
        const TrackerBridgeSection& accounts,
        const std::wstring& configuredDateOrder)
    {
        const std::vector<std::wstring> headers = {
            L"Symbol", L"Open P/L", L"Side", L"Qty", L"Average Price",
            L"Native Value", L"Account", L"Profile", L"Last Update"
        };
        std::vector<std::vector<std::wstring>> rows;
        rows.reserve(section.rows.size());
        std::map<std::wstring, double> openPlTotalsByCurrency;
        std::size_t openPlRowsNotTotaled = 0;

        for (const auto& source : section.rows)
        {
            std::vector<std::wstring> row(9);
            row[0] = source.size() > 2 ? source[2] : L"";
            row[1] = source.size() > 6 ? source[6] : L"";
            row[2] = source.size() > 3 ? source[3] : L"";
            row[3] = source.size() > 4 ? source[4] : L"";
            row[4] = source.size() > 5 ? source[5] : L"";
            row[6] = source.size() > 1 ? source[1] : L"";
            row[7] = source.size() > 0 ? source[0] : L"";
            row[8] = source.size() > 7 ? source[7] : L"";

            double quantity = 0.0;
            double averagePrice = 0.0;
            const bool quantityOk = ParseLocalizedNumber(row[3], quantity);
            const bool averagePriceOk = ParseLocalizedNumber(row[4], averagePrice);

            if (quantityOk && averagePriceOk)
            {
                const double nativeValue = std::fabs(quantity) * averagePrice;
                row[5] = FormatReportNumber(nativeValue);
            }
            else
            {
                row[5] = L"n/a";
            }

            double openPl = 0.0;
            std::wstring openPlCurrency;
            if (ParseLocalizedNumber(row[1], openPl) &&
                TryExtractKnownCurrency(row[1], openPlCurrency))
            {
                openPlTotalsByCurrency[openPlCurrency] += openPl;
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
            totalRow[0] = L"Total Open P/L";
            totalRow[1] = total.first + L" " + FormatReportNumber(total.second, true);
            totalRows.push_back(std::move(totalRow));
        }
        const auto historyTotalRows =
            BuildPositionHistoryTotalRows(positionHistory, accounts, configuredDateOrder);
        totalRows.insert(totalRows.end(), historyTotalRows.begin(), historyTotalRows.end());

        if (rows.empty())
        {
            out << L"(no rows)\n";
            if (!totalRows.empty())
            {
                out << L'\n';
                const std::vector<std::size_t> compactWidths =
                    CalculateColumnWidths(totalRows, 3, nullptr, false);
                const std::vector<bool> compactAlignment = { false, true, false };
                for (const auto& totalRow : totalRows)
                {
                    const std::vector<std::wstring> compactRow = {
                        totalRow[0], totalRow[1], totalRow[2]
                    };
                    AppendAlignedRow(out, compactRow, compactWidths, compactAlignment, false);
                }
            }
            return;
        }

        std::vector<std::vector<std::wstring>> rowsForWidth = rows;
        rowsForWidth.insert(rowsForWidth.end(), totalRows.begin(), totalRows.end());
        const std::vector<std::size_t> widths =
            CalculateColumnWidths(rowsForWidth, headers.size(), &headers, false);
        const std::vector<bool> rightAligned = {
            false, true, false, true, true, true, false, false, false
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
        << L"===========================\n";
    std::vector<std::pair<std::wstring, std::wstring>> identityRows = {
        { L"Watchdog version", L"1.20.12" },
        { L"Tracker Bridge", L"MCST Tracker Bridge 1.0 (internal V" +
            std::to_wstring(snapshot.bridgeVersion) + L", protocol V" +
            std::to_wstring(snapshot.protocolVersion) + L")" },
        { L"MultiCharts", status.multiChartsVersion.empty() ? L"Unknown" : status.multiChartsVersion },
        { L"MC executable", status.multiChartsExecutable.empty() ? L"Unknown" : status.multiChartsExecutable },
        { L"AutoTrading profile", status.multiChartsCompatibilityProfile.empty() ? L"Unknown" : status.multiChartsCompatibilityProfile },
        { L"Tracker profile", status.trackerCompatibilityProfile.empty() ? L"Unknown" : status.trackerCompatibilityProfile }
    };
    if (snapshot.atonpTrackerLoaded)
    {
        std::wostringstream fingerprint;
        fingerprint << L"0x" << std::hex << std::uppercase << snapshot.atonpTrackerPeTimestamp
            << std::dec << L" / " << snapshot.atonpTrackerImageSize << L" bytes";
        identityRows.push_back({ L"ATOnPTracker fingerprint", fingerprint.str() });
    }
    AppendKeyValueRows(out, identityRows);
    out << L"\nSYSTEM STATUS\n"
        << L"-------------\n";
    AppendMonitorLines(out, status);
    out << L"\nLATEST ACTIVITY\n"
        << L"---------------\n";
    AppendKeyValueRows(out, {
        { L"Last Tracker attempt", status.lastTrackerAttempt },
        { L"Last complete snapshot", status.lastCompleteTrackerSnapshot },
        { L"Last AutoTrading Read", status.lastAutoTradingRead.empty() ? L"Never" : status.lastAutoTradingRead },
        { L"Last Status Report", status.lastReport },
        { L"Last Alert", status.lastAlert }
    });
    out << L'\n';

    AppendSystemResources(out, status);
    AppendMultiChartsProcesses(out, status);

    if (!status.lastError.empty())
        out << L"\nLATEST ERROR\n------------\n" << status.lastError << L'\n';

    if (status.trackerDataStale)
    {
        out << L"\nTRACKER TABLE DATA\n"
            << L"------------------\n"
            << (status.trackerDataStaleCritical
                ? L"CRITICAL: STALE TRACKER TABLE DATA - "
                : L"ATTENTION: STALE TRACKER TABLE DATA - ")
            << L"one or more failed sections use the last complete snapshot from "
            << (status.trackerDataTimestamp.empty() ? L"an earlier update" : status.trackerDataTimestamp)
            << L" because the current Tracker read failed. Stale age is "
            << status.trackerDataStaleAgeMinutes << L" min; current monitoring health is "
            << (status.trackerDataStaleCritical ? L"CRITICAL" : L"ATTENTION")
            << L" (CRITICAL threshold " << status.trackerDataStaleCriticalAfterMinutes << L" min).\n";
    }

    out << L"\nACCOUNTS\n--------\n";
    AppendAlignedSectionRows(out, snapshot.accounts, false, true);
    out << L"\nOPEN POSITIONS\n--------------\n";
    AppendOpenPositionsTable(
        out, snapshot.openPositions, snapshot.positionHistory, snapshot.accounts, status.trackerDateOrder);
    out << L"\nRECENT LOGS\n-----------\n";
    AppendAlignedSectionRows(out, snapshot.recentLogs, true, false);
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

    auto statusLineHtml = [&](const std::wstring& rawLine)
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

        auto slice = [&](std::size_t begin, std::size_t width)
        {
            if (begin >= rawLine.size())
                return std::wstring();
            return rawLine.substr(begin, (std::min)(width, rawLine.size() - begin));
        };

        if (rawLine.size() < kMonitorNameWidth)
            return escapeHtml(rawLine);

        const std::wstring nameCell = slice(0, kMonitorNameWidth);
        const std::wstring stateCell = slice(kMonitorNameWidth, kMonitorStateWidth);
        const std::wstring valueCell = slice(
            kMonitorNameWidth + kMonitorStateWidth,
            kMonitorValueWidth);
        const std::wstring detailCell = rawLine.size() >
            kMonitorNameWidth + kMonitorStateWidth + kMonitorValueWidth
            ? rawLine.substr(kMonitorNameWidth + kMonitorStateWidth + kMonitorValueWidth)
            : L"";

        for (const auto& style : styles)
        {
            const std::size_t marker = stateCell.find(style.marker);
            if (marker != std::wstring::npos)
            {
                const bool overall = TrimCell(nameCell) == L"OVERALL STATUS";
                std::wstring result = L"<span class=\"mcst-status-name-cell\" style=\"display:inline-block;width:" +
                    std::to_wstring(kMonitorNameWidth) + L"ch;\">";
                if (overall)
                    result += L"<span style=\"font-weight:700;\">" + escapeHtml(TrimCell(nameCell)) + L"</span>";
                else
                    result += escapeHtml(TrimCell(nameCell));
                result += L"</span>";

                // Every status segment owns an explicit character width. Bold
                // fonts and the different source markers ([OK] versus [X]/[?]/[!])
                // can therefore no longer move any following column in iOS Mail.
                result += L"<span class=\"mcst-status-dot-cell\" style=\"display:inline-block;width:4ch;text-align:center;color:";
                result += style.dotColor;
                result += L";font-weight:700;\">";
                if (overall)
                {
                    result += L"<span class=\"mcst-status-overall-dot\" style=\"font-size:2em;line-height:0.5;vertical-align:-0.12em;\">&#9679;</span>";
                }
                else
                {
                    result += L"<span class=\"mcst-status-dot\" style=\"font-size:1.5em;line-height:0.65;vertical-align:-0.08em;\">&#9679;</span>";
                }
                result += L"</span>";

                result += L"<span class=\"mcst-status-state-cell\" style=\"display:inline-block;width:" +
                    std::to_wstring(kMonitorStateWidth - 4) + L"ch;color:" +
                    std::wstring(style.textColor) + L";\">";
                result += overall
                    ? L"<span style=\"font-weight:700;\">"
                    : L"<span style=\"font-weight:600;\">";
                // Keep a real separator inside the fixed-width cell. CSS width
                // alone does not create text whitespace, so iOS Mail otherwise
                // concatenates e.g. "OK", "Ready" and the following address
                // and detects the whole string as one email address.
                result += std::wstring(style.label) + L"&#160;</span></span>";
                result += L"<span class=\"mcst-status-value-cell\" style=\"display:inline-block;width:" +
                    std::to_wstring(kMonitorValueWidth) + L"ch;\">" +
                    escapeHtml(TrimCell(valueCell)) + L"&#160;</span>";
                result += escapeHtml(detailCell);
                return result;
            }
        }
        return escapeHtml(rawLine);
    };

    auto splitAlignedTableRow = [](const std::wstring& line)
    {
        const std::wstring separator = L" | ";
        std::vector<std::wstring> cells;
        std::size_t begin = 0;
        for (;;)
        {
            const std::size_t end = line.find(separator, begin);
            if (end == std::wstring::npos)
            {
                // trimRight() removes the last space from a trailing " | "
                // delimiter when the final aligned cell is empty. Preserve that
                // final empty field so total rows still have all nine columns.
                if (line.size() >= 2 && line.compare(line.size() - 2, 2, L" |") == 0 &&
                    line.size() - 2 >= begin)
                {
                    cells.push_back(line.substr(begin, line.size() - 2 - begin));
                    cells.emplace_back();
                }
                else
                {
                    cells.push_back(line.substr(begin));
                }
                break;
            }
            cells.push_back(line.substr(begin, end - begin));
            begin = end + separator.size();
        }
        return cells;
    };

    auto openPositionCellHtml = [&](const std::wstring& rawCell, std::size_t column)
    {
        constexpr std::size_t openPlColumn = 1;
        if (column != openPlColumn)
            return escapeHtml(rawCell);

        const std::size_t begin = rawCell.find_first_not_of(L" \t");
        if (begin == std::wstring::npos)
            return escapeHtml(rawCell);
        const std::size_t end = rawCell.find_last_not_of(L" \t");
        const std::wstring cell = rawCell.substr(begin, end - begin + 1);

        double profit = 0.0;
        const bool parsedProfit = ParseLocalizedNumber(cell, profit);
        const bool explicitNegative = cell.find(L'-') != std::wstring::npos;
        const bool explicitPositive = cell.find(L'+') != std::wstring::npos;
        if ((!parsedProfit || std::fabs(profit) < 0.0000001) &&
            !explicitNegative && !explicitPositive)
        {
            return escapeHtml(rawCell);
        }

        std::wstring result = escapeHtml(rawCell.substr(0, begin));
        result += explicitNegative || (parsedProfit && profit < 0.0)
            ? L"<span style=\"color:#B4232A;font-weight:600;\">"
            : L"<span style=\"color:#15803D;font-weight:600;\">";
        result += escapeHtml(cell);
        result += L"</span>";
        result += escapeHtml(rawCell.substr(end + 1));
        return result;
    };

    std::vector<std::wstring> lines;
    {
        std::wistringstream input(mcst::WithoutQueueDiagnostics(plainText));
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

    bool inStatusLines = false;
    bool preOpen = false;
    bool inOpenPositions = false;

    const std::wstring preStyle =
        L"font-family:Consolas,'Courier New',monospace !important;"
        L"font-size:15px !important;line-height:1.28 !important;"
        L"white-space:pre;margin:0;-webkit-text-size-adjust:100% !important;"
        L"-ms-text-size-adjust:100% !important;";

    auto openPre = [&]()
    {
        if (!preOpen)
        {
            html += L"<pre style=\"" + preStyle + L"\">";
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

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::wstring& line = lines[i];

        if (line == L"OPEN POSITIONS")
        {
            closePre();
            inOpenPositions = true;
            html += L"<pre class=\"mcst-open-positions-lines\" style=\"" + preStyle + L"\">";
            preOpen = true;
        }
        else if (line == L"RECENT LOGS")
        {
            closePre();
            inOpenPositions = false;
        }

        if (line == L"SYSTEM STATUS")
        {
            closePre();
            inStatusLines = true;
            html += L"<pre class=\"mcst-system-status-lines\" style=\"" + preStyle + L"\">";
            preOpen = true;
            html += escapeHtml(line) + L"\n";
            continue;
        }

        if (inStatusLines && line == L"LATEST ACTIVITY")
        {
            closePre();
            inStatusLines = false;
        }
        if (inStatusLines)
        {
            html += statusLineHtml(line) + L"\n";
            continue;
        }

        if (line.rfind(L"ATTENTION: STALE TRACKER TABLE DATA", 0) == 0 ||
            line.rfind(L"CRITICAL: STALE TRACKER TABLE DATA", 0) == 0)
        {
            closePre();
            const bool staleCritical = line.rfind(L"CRITICAL:", 0) == 0;
            html += L"<div class=\"mcst-stale-tracker-warning ";
            html += staleCritical ? L"mcst-stale-critical" : L"mcst-stale-attention";
            html += L"\" style=\"margin:10px 0 14px 0;padding:10px 12px;border:1px solid ";
            html += staleCritical ? L"#D13438;background:#FFF1F1;color:#B4232A;" : L"#D99A00;background:#FFF8E1;color:#8A5A00;";
            html += L"font-family:Consolas,'Courier New',monospace !important;font-size:15px !important;font-weight:600;line-height:1.32;-webkit-text-size-adjust:100% !important;-ms-text-size-adjust:100% !important;\">";
            html += escapeHtml(line);
            html += L"</div>\r\n";
            continue;
        }

        if (inOpenPositions)
        {
            const std::vector<std::wstring> cells = splitAlignedTableRow(line);
            if (cells.size() >= 2)
            {
                for (std::size_t column = 0; column < cells.size(); ++column)
                {
                    if (column != 0)
                        html += L" | ";
                    html += openPositionCellHtml(cells[column], column);
                }
                html += L"\n";
                continue;
            }
        }

        // Keep every data-heavy section in the same 15-pixel preformatted flow.
        // Open Positions deliberately remains one logical line per position and
        // may continue to the right, matching Accounts and Recent Logs instead
        // of becoming a separate table that iOS Mail scales down.
        openPre();
        html += escapeHtml(line);
        html += L"\n";
    }

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
