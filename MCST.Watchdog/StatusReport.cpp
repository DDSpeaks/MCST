#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "StatusReport.h"

#include <iomanip>
#include <sstream>
#include <vector>
#include <algorithm>

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

    void AppendRows(std::wostringstream& out, const TrackerBridgeSection& section)
    {
        if (section.rows.empty())
        {
            out << L"(no rows)\n";
            return;
        }
        for (const auto& row : section.rows)
        {
            for (std::size_t i = 0; i < row.size(); ++i)
            {
                if (i != 0) out << L" | ";
                out << row[i];
            }
            out << L'\n';
        }
    }
}

std::wstring BuildStatusReport(const mcst::WatchdogSystemStatus& status, const TrackerStatusSnapshot& snapshot)
{
    std::wostringstream out;
    out << L"MCST-Watchdog Status Report\n"
        << L"===========================\n"
        << L"Watchdog version       1.113\n"
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

    out << L"\nACCOUNTS\n--------\n";
    AppendRows(out, snapshot.accounts);
    out << L"\nOPEN POSITIONS\n--------------\n";
    AppendRows(out, snapshot.openPositions);
    out << L"\nRECENT LOGS\n-----------\n";
    AppendRows(out, snapshot.recentLogs);
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
    html += L"<style>body,table,tbody,tr,td,th,div,span,pre,p{font-family:Consolas,\'Courier New\',monospace !important;}</style></head>\r\n";
    html += L"<body style=\"margin:0;padding:16px;background:#ffffff;color:#202020;font-family:Consolas,'Courier New',monospace;\">\r\n";

    bool inOverall = false;
    bool inSystemStatus = false;
    bool systemTableOpen = false;
    bool preOpen = false;

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

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::wstring& line = lines[i];

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

        // Keep data-heavy sections such as SYSTEM RESOURCES, ACCOUNTS,
        // OPEN POSITIONS and RECENT LOGS monospaced. This retains the proven
        // fixed-column readability while keeping one consistent monospaced
        // typeface throughout the entire report.
        openPre();
        html += escapeHtml(line);
        html += L"\n";
    }

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
