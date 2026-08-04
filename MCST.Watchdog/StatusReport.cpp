#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "StatusReport.h"

#include <iomanip>
#include <sstream>

namespace
{
    std::wstring MonitorLine(const wchar_t* name, const mcst::MonitorStatus& item)
    {
        std::wostringstream out;
        out << std::left << std::setw(22) << name
            << std::setw(11) << mcst::HealthStateText(item.state)
            << item.value;
        if (!item.detail.empty())
            out << L"  " << item.detail;
        out << L'\n';
        return out.str();
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
        << L"===========================\n\n"
        << L"OVERALL STATUS\n"
        << L"--------------\n"
        << mcst::HealthStateText(status.overall) << L"\n\n"
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
        << std::left << std::setw(22) << L"Last Alert" << status.lastAlert << L'\n'
        << L"\nSYSTEM RESOURCES\n"
        << L"----------------\n"
        << std::left << std::setw(22) << L"Private memory" << (status.privateMemoryBytes / (1024 * 1024)) << L" MB\n"
        << std::left << std::setw(22) << L"Handles" << status.handleCount << L'\n'
        << std::left << std::setw(22) << L"Uptime" << status.uptime << L'\n';

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
