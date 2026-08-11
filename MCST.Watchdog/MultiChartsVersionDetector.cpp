#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>
#include <tlhelp32.h>

#include "MultiChartsVersionDetector.h"
#include "AppConfig.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace
{
    std::wstring NumericVersion(const VS_FIXEDFILEINFO& info, bool product)
    {
        const DWORD ms = product ? info.dwProductVersionMS : info.dwFileVersionMS;
        const DWORD ls = product ? info.dwProductVersionLS : info.dwFileVersionLS;
        std::wostringstream out;
        out << HIWORD(ms) << L'.' << LOWORD(ms) << L'.' << HIWORD(ls) << L'.' << LOWORD(ls);
        return out.str();
    }

    std::wstring QueryStringValue(const std::vector<unsigned char>& data, WORD language, WORD codePage, const wchar_t* key)
    {
        wchar_t block[256]{};
        swprintf_s(block, L"\\StringFileInfo\\%04x%04x\\%s", language, codePage, key);
        LPVOID value = nullptr;
        UINT chars = 0;
        if (VerQueryValueW(data.data(), block, &value, &chars) && value && chars > 0)
            return std::wstring(static_cast<const wchar_t*>(value));
        return {};
    }

    std::wstring ReadVersionString(const std::vector<unsigned char>& data, const wchar_t* key)
    {
        struct LangCodePage { WORD language; WORD codePage; };
        LPVOID translationPtr = nullptr;
        UINT translationBytes = 0;
        if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &translationPtr, &translationBytes) &&
            translationPtr && translationBytes >= sizeof(LangCodePage))
        {
            const auto* translations = static_cast<const LangCodePage*>(translationPtr);
            const UINT count = translationBytes / sizeof(LangCodePage);
            for (UINT i = 0; i < count; ++i)
            {
                const std::wstring value = QueryStringValue(data, translations[i].language, translations[i].codePage, key);
                if (!value.empty()) return value;
            }
        }
        for (const auto fallback : { LangCodePage{0x0409,1200}, LangCodePage{0x0409,1252}, LangCodePage{0x0000,1200} })
        {
            const std::wstring value = QueryStringValue(data, fallback.language, fallback.codePage, key);
            if (!value.empty()) return value;
        }
        return {};
    }

    bool ReadPeTimestampFromFile(const std::wstring& path, DWORD& timestamp)
    {
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        if (!input)
            return false;

        IMAGE_DOS_HEADER dos{};
        input.read(reinterpret_cast<char*>(&dos), sizeof(dos));
        if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
            return false;

        input.seekg(dos.e_lfanew, std::ios::beg);
        DWORD signature = 0;
        input.read(reinterpret_cast<char*>(&signature), sizeof(signature));
        if (!input || signature != IMAGE_NT_SIGNATURE)
            return false;

        IMAGE_FILE_HEADER fileHeader{};
        input.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
        if (!input)
            return false;

        timestamp = fileHeader.TimeDateStamp;
        return true;
    }

    void DetectInternalModuleFingerprints(DWORD processId, MultiChartsVersionInfo& result)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;

        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (Module32FirstW(snapshot, &module))
        {
            do
            {
                DWORD timestamp = 0;
                if (_wcsicmp(module.szModule, L"Charting.dll") == 0)
                {
                    if (ReadPeTimestampFromFile(module.szExePath, timestamp))
                    {
                        result.chartingDetected = true;
                        result.chartingPeTimestamp = timestamp;
                        result.chartingImageSize = static_cast<unsigned long long>(module.modBaseSize);
                    }
                }
                else if (_wcsicmp(module.szModule, L"ATOnPTracker.dll") == 0)
                {
                    if (ReadPeTimestampFromFile(module.szExePath, timestamp))
                    {
                        result.atonpTrackerDetected = true;
                        result.atonpTrackerPeTimestamp = timestamp;
                        result.atonpTrackerImageSize = static_cast<unsigned long long>(module.modBaseSize);
                    }
                }
            } while (Module32NextW(snapshot, &module));
        }
        CloseHandle(snapshot);
    }

    std::wstring HexTimestamp(DWORD value)
    {
        std::wostringstream out;
        out << L"0x" << std::hex << std::uppercase << value;
        return out.str();
    }

    void WriteIni(const wchar_t* key, const std::wstring& value)
    {
        WritePrivateProfileStringW(L"DetectedMultiCharts", key, value.c_str(), GetConfigPath().c_str());
    }
}

MultiChartsVersionInfo DetectMultiChartsVersion(DWORD processId)
{
    MultiChartsVersionInfo result;
    result.processId = processId;
    if (processId == 0)
    {
        result.diagnostic = L"MultiCharts process id is not available.";
        return result;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
    {
        result.diagnostic = L"OpenProcess failed, Windows error=" + std::to_wstring(GetLastError());
        return result;
    }

    std::wstring path(32768, L'\0');
    DWORD pathChars = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &pathChars))
    {
        const DWORD error = GetLastError();
        CloseHandle(process);
        result.diagnostic = L"QueryFullProcessImageName failed, Windows error=" + std::to_wstring(error);
        return result;
    }
    CloseHandle(process);
    path.resize(pathChars);
    result.executablePath = path;
    result.executableName = std::filesystem::path(path).filename().wstring();

    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (bytes == 0)
    {
        result.diagnostic = L"MultiCharts executable has no readable Windows version resource.";
        return result;
    }
    std::vector<unsigned char> versionData(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, versionData.data()))
    {
        result.diagnostic = L"GetFileVersionInfo failed, Windows error=" + std::to_wstring(GetLastError());
        return result;
    }

    LPVOID fixedPtr = nullptr;
    UINT fixedBytes = 0;
    if (VerQueryValueW(versionData.data(), L"\\", &fixedPtr, &fixedBytes) && fixedPtr && fixedBytes >= sizeof(VS_FIXEDFILEINFO))
    {
        const auto& fixed = *static_cast<const VS_FIXEDFILEINFO*>(fixedPtr);
        if (fixed.dwSignature == 0xFEEF04BD)
        {
            result.fileVersion = NumericVersion(fixed, false);
            result.productVersion = NumericVersion(fixed, true);
        }
    }

    const std::wstring productVersionText = ReadVersionString(versionData, L"ProductVersion");
    const std::wstring fileVersionText = ReadVersionString(versionData, L"FileVersion");
    result.productName = ReadVersionString(versionData, L"ProductName");
    if (!productVersionText.empty()) result.productVersion = productVersionText;
    if (!fileVersionText.empty()) result.fileVersion = fileVersionText;

    result.displayVersion = !result.productVersion.empty() ? result.productVersion : result.fileVersion;
    if (result.displayVersion.empty()) result.displayVersion = L"version unavailable";
    DetectInternalModuleFingerprints(processId, result);
    result.detected = true;
    result.diagnostic = L"Detected from " + result.executableName;
    return result;
}

void WriteDetectedMultiChartsInfoToIni(
    const MultiChartsVersionInfo& info,
    const std::wstring& autoTradingCompatibilityProfile,
    const std::wstring& trackerCompatibilityProfile)
{
    if (!info.detected) return;
    WriteIni(L"product_version", info.productVersion);
    WriteIni(L"file_version", info.fileVersion);
    WriteIni(L"product_name", info.productName);
    WriteIni(L"executable", info.executableName);
    WriteIni(L"process_id", std::to_wstring(info.processId));
    WriteIni(L"charting_pe_timestamp", info.chartingDetected ? HexTimestamp(info.chartingPeTimestamp) : L"");
    WriteIni(L"charting_image_size", info.chartingDetected ? std::to_wstring(info.chartingImageSize) : L"");
    WriteIni(L"atonptracker_pe_timestamp", info.atonpTrackerDetected ? HexTimestamp(info.atonpTrackerPeTimestamp) : L"");
    WriteIni(L"atonptracker_image_size", info.atonpTrackerDetected ? std::to_wstring(info.atonpTrackerImageSize) : L"");
    WriteIni(L"autotrading_compatibility_profile", autoTradingCompatibilityProfile);
    WriteIni(L"tracker_compatibility_profile", trackerCompatibilityProfile);
    // Keep the historical key as an alias for AutoTrading so existing diagnostics remain readable.
    WriteIni(L"compatibility_profile", autoTradingCompatibilityProfile);

    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t timeText[32]{};
    swprintf_s(timeText, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    WriteIni(L"last_detected", timeText);
}
