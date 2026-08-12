#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "TrackerBridgeReader.h"
#include "MCBridgeClient.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
            return {};

        const int required = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (required <= 0)
            return {};

        std::wstring result(static_cast<std::size_t>(required), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                result.data(), required) != required)
        {
            return {};
        }
        return result;
    }

    std::vector<std::string> SplitTabs(const std::string& line)
    {
        std::vector<std::string> fields;
        std::size_t start = 0;
        for (;;)
        {
            const std::size_t tab = line.find('\t', start);
            if (tab == std::string::npos)
            {
                fields.push_back(line.substr(start));
                break;
            }
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        return fields;
    }

    std::string UnescapeBytes(const std::string& value)
    {
        std::string result;
        result.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            const char ch = value[i];
            if (ch != '\\' || i + 1 >= value.size())
            {
                result.push_back(ch);
                continue;
            }

            const char next = value[++i];
            switch (next)
            {
            case '\\': result.push_back('\\'); break;
            case 't': result.push_back('\t'); break;
            case 'r': result.push_back('\r'); break;
            case 'n': result.push_back('\n'); break;
            default:
                result.push_back('\\');
                result.push_back(next);
                break;
            }
        }
        return result;
    }

    std::wstring DecodeField(const std::string& value)
    {
        return Utf8ToWide(UnescapeBytes(value));
    }

    bool ParseUnsigned(const std::string& text, std::size_t& value)
    {
        try
        {
            std::size_t consumed = 0;
            const unsigned long long parsed = std::stoull(text, &consumed, 10);
            if (consumed != text.size())
                return false;
            value = static_cast<std::size_t>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    TrackerBridgeSection* SectionByName(TrackerStatusSnapshot& snapshot, const std::string& name)
    {
        if (name == "accounts") return &snapshot.accounts;
        if (name == "open_positions") return &snapshot.openPositions;
        if (name == "recent_logs") return &snapshot.recentLogs;
        return nullptr;
    }

    bool ParsePayload(const std::string& payload, TrackerStatusSnapshot& snapshot, std::wstring& diagnostic)
    {
        snapshot = TrackerStatusSnapshot{};
        snapshot.rawPayload = payload;

        std::istringstream input(payload);
        std::string line;
        bool markerSeen = false;
        bool endSeen = false;
        std::size_t lineNumber = 0;

        while (std::getline(input, line))
        {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;

            if (!markerSeen)
            {
                if (line != "MC_TRACKER_STATUS_V1")
                {
                    diagnostic = L"Bridge payload marker is not MC_TRACKER_STATUS_V1";
                    return false;
                }
                markerSeen = true;
                continue;
            }

            const std::vector<std::string> fields = SplitTabs(line);
            if (fields.empty())
                continue;

            if (fields[0] == "META")
            {
                if (fields.size() < 3)
                {
                    diagnostic = L"Malformed META line in Bridge payload";
                    return false;
                }
                const std::string& key = fields[1];
                const std::string& value = fields[2];
                try
                {
                    if (key == "bridge_version") snapshot.bridgeVersion = std::stoi(value);
                    else if (key == "protocol_version") snapshot.protocolVersion = std::stoi(value);
                    else if (key == "process_id") snapshot.processId = std::stoul(value);
                    else if (key == "captured_utc") snapshot.capturedUtc = DecodeField(value);
                    else if (key == "tracker_found") snapshot.trackerFound = value == "true";
                    else if (key == "tracker_same_process") snapshot.trackerSameProcess = value == "true";
                    else if (key == "atonptracker_loaded") snapshot.atonpTrackerLoaded = value == "true";
                    else if (key == "atonptracker_pe_timestamp") snapshot.atonpTrackerPeTimestamp = static_cast<std::uint32_t>(std::stoull(value));
                    else if (key == "atonptracker_image_size") snapshot.atonpTrackerImageSize = std::stoull(value);
                    else if (key == "tracker_compatibility_matched") snapshot.trackerCompatibilityMatched = value == "true";
                    else if (key == "tracker_compatibility_mode") snapshot.trackerCompatibilityMode = DecodeField(value);
                    else if (key == "tracker_compatibility_profile") snapshot.trackerCompatibilityProfile = DecodeField(value);
                    else if (key == "tracker_compatibility_source") snapshot.trackerCompatibilitySource = DecodeField(value);
                    else if (key == "tracker_compatibility_diagnostic") snapshot.trackerCompatibilityDiagnostic = DecodeField(value);
                }
                catch (...)
                {
                    diagnostic = L"Invalid numeric META value in Bridge payload";
                    return false;
                }
                continue;
            }

            if (fields[0] == "SECTION")
            {
                if (fields.size() < 10)
                {
                    diagnostic = L"Malformed SECTION line in Bridge payload";
                    return false;
                }
                TrackerBridgeSection* section = SectionByName(snapshot, fields[1]);
                if (!section)
                    continue;

                std::size_t expectedColumns = 0;
                std::size_t declaredRows = 0;
                std::size_t reportedCapacity = 0;
                std::size_t callsAttempted = 0;
                std::size_t callsSucceeded = 0;
                std::size_t sehFailures = 0;
                if (!ParseUnsigned(fields[3], expectedColumns) ||
                    !ParseUnsigned(fields[4], declaredRows) ||
                    !ParseUnsigned(fields[5], reportedCapacity) ||
                    !ParseUnsigned(fields[6], callsAttempted) ||
                    !ParseUnsigned(fields[7], callsSucceeded) ||
                    !ParseUnsigned(fields[8], sehFailures))
                {
                    diagnostic = L"Invalid SECTION counters in Bridge payload";
                    return false;
                }

                section->present = true;
                section->ok = fields[2] == "OK";
                section->expectedColumns = expectedColumns;
                section->declaredRows = declaredRows;
                section->reportedCapacity = static_cast<std::uint32_t>(reportedCapacity);
                section->callsAttempted = callsAttempted;
                section->callsSucceeded = callsSucceeded;
                section->sehFailures = sehFailures;
                section->diagnostic = DecodeField(fields[9]);
                continue;
            }

            if (fields[0] == "ROW")
            {
                if (fields.size() < 2)
                {
                    diagnostic = L"Malformed ROW line in Bridge payload";
                    return false;
                }
                TrackerBridgeSection* section = SectionByName(snapshot, fields[1]);
                if (!section)
                    continue;

                std::vector<std::wstring> row;
                row.reserve(fields.size() - 2);
                for (std::size_t index = 2; index < fields.size(); ++index)
                    row.push_back(DecodeField(fields[index]));
                section->rows.push_back(std::move(row));
                continue;
            }

            if (fields[0] == "SUMMARY")
            {
                for (std::size_t index = 1; index + 1 < fields.size(); index += 2)
                {
                    std::size_t value = 0;
                    if (!ParseUnsigned(fields[index + 1], value))
                        continue;
                    if (fields[index] == "pages_ok") snapshot.pagesOk = value;
                    else if (fields[index] == "pages_failed") snapshot.pagesFailed = value;
                    else if (fields[index] == "seh_failures") snapshot.sehFailures = value;
                }
                continue;
            }

            if (fields[0] == "END")
            {
                endSeen = true;
                break;
            }
        }

        if (!markerSeen || !endSeen)
        {
            diagnostic = L"Bridge payload was incomplete";
            return false;
        }
        if (snapshot.bridgeVersion < kTrackerBridgeInternalBuildVersion)
        {
            diagnostic = L"Tracker Bridge internal build V" + std::to_wstring(snapshot.bridgeVersion) +
                L" is older than the required V" + std::to_wstring(kTrackerBridgeInternalBuildVersion) + L".";
            return false;
        }
        if (!snapshot.accounts.present || !snapshot.openPositions.present || !snapshot.recentLogs.present)
        {
            diagnostic = L"Bridge payload did not contain all three status sections";
            return false;
        }

        auto validateRows = [&](const TrackerBridgeSection& section, std::size_t requiredColumns, const wchar_t* name) -> bool {
            if (section.expectedColumns != requiredColumns || section.declaredRows != section.rows.size())
            {
                diagnostic = std::wstring(L"Bridge section counters do not match for ") + name;
                return false;
            }
            for (const auto& row : section.rows)
            {
                if (row.size() != requiredColumns)
                {
                    diagnostic = std::wstring(L"Bridge row column count does not match for ") + name;
                    return false;
                }
            }
            return true;
        };

        if (!validateRows(snapshot.accounts, 12, L"Accounts") ||
            !validateRows(snapshot.openPositions, 8, L"Open Positions") ||
            !validateRows(snapshot.recentLogs, 6, L"Recent Logs"))
        {
            return false;
        }

        std::wostringstream out;
        out << L"Bridge V" << snapshot.bridgeVersion
            << L" payload parsed; accounts=" << snapshot.accounts.rows.size()
            << L" positions=" << snapshot.openPositions.rows.size()
            << L" recent_logs=" << snapshot.recentLogs.rows.size()
            << L" pages_ok=" << snapshot.pagesOk
            << L" seh_failures=" << snapshot.sehFailures;
        diagnostic = out.str();
        return true;
    }
}

bool ReadTrackerStatusSnapshot(
    TrackerStatusSnapshot& snapshot,
    std::wstring& diagnostic,
    unsigned long connectTimeoutMilliseconds)
{
    snapshot = TrackerStatusSnapshot{};
    diagnostic.clear();

    MCBridgeClient client;
    std::wstring connectDiagnostic;
    if (!client.Connect(connectTimeoutMilliseconds, connectDiagnostic))
    {
        diagnostic = L"MCTrackerBridge connection failed: " + connectDiagnostic;
        return false;
    }

    mcbridge::Status responseStatus{};
    std::string payload;
    std::wstring requestDiagnostic;
    if (!client.Request(
            mcbridge::Command::GetStatusReportSnapshot,
            {},
            responseStatus,
            payload,
            requestDiagnostic))
    {
        diagnostic = L"MCTrackerBridge request failed: " + requestDiagnostic;
        return false;
    }

    std::wstring parseDiagnostic;
    if (!ParsePayload(payload, snapshot, parseDiagnostic))
    {
        diagnostic = L"MCTrackerBridge payload parse failed: " + parseDiagnostic;
        return false;
    }

    std::wostringstream out;
    out << parseDiagnostic
        << L"; response_status=" << static_cast<unsigned int>(responseStatus)
        << L"; " << requestDiagnostic;
    diagnostic = out.str();

    // ExtractorCallFailed can still carry a valid partial snapshot. The caller
    // decides section-by-section whether the status report is complete.
    return responseStatus == mcbridge::Status::Ok ||
           responseStatus == mcbridge::Status::ExtractorCallFailed;
}

bool WriteTrackerStatusRawPayload(
    const TrackerStatusSnapshot& snapshot,
    const std::wstring& path,
    std::wstring& diagnostic)
{
    diagnostic.clear();
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        diagnostic = L"Could not create raw Bridge snapshot file: " + path +
            L" error=" + std::to_wstring(GetLastError());
        return false;
    }

    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    bool ok = WriteFile(file, bom, static_cast<DWORD>(sizeof(bom)), &written, nullptr) != FALSE &&
        written == sizeof(bom);

    if (ok && !snapshot.rawPayload.empty())
    {
        written = 0;
        ok = WriteFile(
            file,
            snapshot.rawPayload.data(),
            static_cast<DWORD>(snapshot.rawPayload.size()),
            &written,
            nullptr) != FALSE &&
            written == snapshot.rawPayload.size();
    }

    FlushFileBuffers(file);
    CloseHandle(file);

    if (!ok)
    {
        diagnostic = L"Writing raw Bridge snapshot failed: " + path;
        return false;
    }

    diagnostic = L"Raw Bridge snapshot written to " + path;
    return true;
}

bool CaptureTrackerResearchBundle(
    std::wstring& responseSummary,
    std::wstring& diagnostic,
    unsigned long connectTimeoutMilliseconds)
{
    responseSummary.clear();
    diagnostic.clear();

    MCBridgeClient client;
    std::wstring connectDiagnostic;
    if (!client.Connect(connectTimeoutMilliseconds, connectDiagnostic))
    {
        diagnostic = L"MCTrackerBridge connection failed: " + connectDiagnostic;
        return false;
    }

    mcbridge::Status responseStatus{};
    std::string payload;
    std::wstring requestDiagnostic;
    if (!client.Request(
            mcbridge::Command::CaptureResearchBundle,
            {},
            responseStatus,
            payload,
            requestDiagnostic))
    {
        diagnostic = L"MCTrackerBridge research request failed: " + requestDiagnostic;
        return false;
    }

    responseSummary = Utf8ToWide(payload);
    if (responseStatus != mcbridge::Status::Ok)
    {
        diagnostic = L"Tracker research bundle failed; Bridge status=" +
            std::to_wstring(static_cast<unsigned int>(responseStatus)) + L"; " + requestDiagnostic;
        return false;
    }

    diagnostic = L"Tracker research bundle captured; " + requestDiagnostic;
    return true;
}



bool CapturePositionCurrencyResearch(
    std::wstring& responseSummary,
    std::wstring& diagnostic,
    unsigned long connectTimeoutMilliseconds)
{
    responseSummary.clear();
    diagnostic.clear();

    MCBridgeClient client;
    std::wstring connectDiagnostic;
    if (!client.Connect(connectTimeoutMilliseconds, connectDiagnostic))
    {
        diagnostic = L"MCTrackerBridge connection failed: " + connectDiagnostic;
        return false;
    }

    mcbridge::Status responseStatus{};
    std::string payload;
    std::wstring requestDiagnostic;
    if (!client.Request(
            mcbridge::Command::CapturePositionCurrencyDirectResearch,
            {},
            responseStatus,
            payload,
            requestDiagnostic))
    {
        diagnostic = L"Position Currency research request failed: " + requestDiagnostic;
        return false;
    }

    responseSummary = Utf8ToWide(payload);
    if (responseStatus != mcbridge::Status::Ok)
    {
        diagnostic = L"Position Currency research capture failed; Bridge status=" +
            std::to_wstring(static_cast<unsigned int>(responseStatus)) + L"; " + requestDiagnostic;
        return false;
    }

    diagnostic = L"Position Currency direct research capture completed; " + requestDiagnostic;
    return true;
}
