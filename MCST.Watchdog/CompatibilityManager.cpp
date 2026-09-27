#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "CompatibilityManager.h"
#include "AppConfig.h"

#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <cwctype>
#include <sstream>
#include <vector>

namespace
{
    constexpr wchar_t kDatabaseFileName[] = L"MCST-Compatibility.ini";
    constexpr wchar_t kKnownMc16Section[] = L"Profile.MC16-Charting-6A5684BF";
    constexpr wchar_t kKnownMc17Section[] = L"Profile.MC17-Charting-6AB57EE6";

    std::wstring Trim(std::wstring value)
    {
        const auto notSpace = [](wchar_t ch) { return iswspace(ch) == 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    bool TryParseUnsigned(const std::wstring& text, unsigned long long& value)
    {
        const std::wstring normalized = Trim(text);
        if (normalized.empty())
            return false;

        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::wcstoull(normalized.c_str(), &end, 0);
        if (errno == ERANGE || end == normalized.c_str() || *end != L'\0')
            return false;
        value = parsed;
        return true;
    }

    std::wstring ReadValue(const std::wstring& path, const std::wstring& section,
        const wchar_t* key, const wchar_t* fallback = L"")
    {
        wchar_t buffer[2048]{};
        GetPrivateProfileStringW(section.c_str(), key, fallback, buffer,
            static_cast<DWORD>(std::size(buffer)), path.c_str());
        return buffer;
    }

    std::vector<std::wstring> ReadSections(const std::wstring& path)
    {
        std::vector<wchar_t> buffer(32768, L'\0');
        const DWORD count = GetPrivateProfileSectionNamesW(buffer.data(),
            static_cast<DWORD>(buffer.size()), path.c_str());
        std::vector<std::wstring> sections;
        if (count == 0)
            return sections;

        const wchar_t* current = buffer.data();
        while (*current != L'\0')
        {
            sections.emplace_back(current);
            current += sections.back().size() + 1;
        }
        return sections;
    }

    bool WriteKnownMc16Profile(const std::wstring& path)
    {
        bool ok = true;
        ok = ok && WritePrivateProfileStringW(L"Compatibility", L"schema_version", L"2", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(L"Compatibility", L"unknown_build_policy", L"reject", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc16Section, L"name", L"MC16 verified Charting.dll 0x6A5684BF", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc16Section, L"enabled", L"true", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc16Section, L"charting_pe_timestamp", L"0x6A5684BF", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc16Section, L"charting_image_size", L"18493440", path.c_str()) != FALSE;
        // Legacy aliases remain readable by older MCST builds.
        ok = ok && WritePrivateProfileStringW(kKnownMc16Section, L"pe_timestamp", L"0x6A5684BF", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc16Section, L"image_size", L"18493440", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc16Section, L"strategy_vtable_rva", L"0xA457B8", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc16Section, L"autotrading_offset", L"0x142", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc16Section, L"verification", L"Controlled research session: 8/8 exact toggle responses", path.c_str()) != FALSE;
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
        return ok;
    }

    bool WriteKnownMc17Profile(const std::wstring& path)
    {
        bool ok = true;
        ok = ok && WritePrivateProfileStringW(L"Compatibility", L"schema_version", L"2", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(L"Compatibility", L"unknown_build_policy", L"reject", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc17Section, L"name", L"MC17 verified Charting.dll 0x6AB57EE6", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc17Section, L"enabled", L"true", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc17Section, L"charting_pe_timestamp", L"0x6AB57EE6", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc17Section, L"charting_image_size", L"18624512", path.c_str()) != FALSE;
        // Legacy aliases keep this database readable by older MCST builds.
        ok = ok && WritePrivateProfileStringW(kKnownMc17Section, L"pe_timestamp", L"0x6AB57EE6", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc17Section, L"image_size", L"18624512", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc17Section, L"strategy_vtable_rva", L"0xB74CF0", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc17Section, L"autotrading_offset", L"0x18", path.c_str()) != FALSE;
        ok = ok && WritePrivateProfileStringW(kKnownMc17Section, L"verification", L"Controlled dynamic research: 4/4 exact responses with both toggle directions", path.c_str()) != FALSE;
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
        return ok;
    }

    bool HasSection(const std::vector<std::wstring>& sections, const wchar_t* expected)
    {
        return std::any_of(sections.begin(), sections.end(), [expected](const std::wstring& section) {
            return _wcsicmp(section.c_str(), expected) == 0;
        });
    }

    bool WriteBundledProfiles(const std::wstring& path)
    {
        return WriteKnownMc16Profile(path) && WriteKnownMc17Profile(path);
    }
}

std::wstring GetCompatibilityDatabasePath()
{
    return (std::filesystem::path(GetApplicationDirectory()) / kDatabaseFileName).wstring();
}

bool EnsureCompatibilityDatabase(std::wstring& diagnostic)
{
    const std::wstring path = GetCompatibilityDatabasePath();
    if (std::filesystem::exists(path))
    {
        // Schema metadata is safe to normalize. Verified address/offset values are never fabricated.
        WritePrivateProfileStringW(L"Compatibility", L"schema_version", L"2", path.c_str());
        WritePrivateProfileStringW(L"Compatibility", L"unknown_build_policy", L"reject", path.c_str());

        // The Tracker Bridge can legitimately create the shared database first when it
        // records an unknown ATOnPTracker candidate. Ensure that this does not prevent
        // the bundled, already-verified AutoTrading profile from being installed later.
        const auto sections = ReadSections(path);
        if (!HasSection(sections, kKnownMc16Section) && !WriteKnownMc16Profile(path))
        {
            diagnostic = L"Compatibility database exists but the bundled verified MC16 AutoTrading profile could not be added: " + path;
            return false;
        }
        if (!HasSection(sections, kKnownMc17Section) && !WriteKnownMc17Profile(path))
        {
            diagnostic = L"Compatibility database exists but the bundled verified MC17 AutoTrading profile could not be added: " + path;
            return false;
        }

        diagnostic = L"Compatibility database available: " + path;
        return true;
    }

    if (!WriteBundledProfiles(path))
    {
        diagnostic = L"Failed to create compatibility database: " + path;
        return false;
    }

    diagnostic = L"Created compatibility database with the verified MC16 and MC17 profiles: " + path;
    return true;
}

CompatibilityProfile ResolveCompatibilityProfile(DWORD peTimestamp, unsigned long long imageSize)
{
    CompatibilityProfile profile;
    std::wstring ensureDiagnostic;
    if (!EnsureCompatibilityDatabase(ensureDiagnostic))
    {
        profile.diagnostic = ensureDiagnostic;
        return profile;
    }

    const std::wstring path = GetCompatibilityDatabasePath();
    for (const auto& section : ReadSections(path))
    {
        if (section.rfind(L"Profile.", 0) != 0)
            continue;

        const std::wstring enabled = ReadValue(path, section, L"enabled", L"true");
        if (_wcsicmp(enabled.c_str(), L"false") == 0 || enabled == L"0")
            continue;

        unsigned long long candidateTimestamp = 0;
        unsigned long long candidateImageSize = 0;
        unsigned long long candidateRva = 0;
        unsigned long long candidateOffset = 0;
        std::wstring timestampText = ReadValue(path, section, L"charting_pe_timestamp");
        if (timestampText.empty()) timestampText = ReadValue(path, section, L"pe_timestamp");
        std::wstring imageSizeText = ReadValue(path, section, L"charting_image_size");
        if (imageSizeText.empty()) imageSizeText = ReadValue(path, section, L"image_size");
        if (!TryParseUnsigned(timestampText, candidateTimestamp) ||
            !TryParseUnsigned(imageSizeText, candidateImageSize) ||
            !TryParseUnsigned(ReadValue(path, section, L"strategy_vtable_rva"), candidateRva) ||
            !TryParseUnsigned(ReadValue(path, section, L"autotrading_offset"), candidateOffset))
        {
            continue;
        }

        if (candidateTimestamp != peTimestamp || candidateImageSize != imageSize)
            continue;

        profile.matched = true;
        profile.name = ReadValue(path, section, L"name", section.c_str());
        profile.chartingPeTimestamp = static_cast<DWORD>(candidateTimestamp);
        profile.chartingImageSize = candidateImageSize;
        profile.strategyVtableRva = static_cast<ULONG_PTR>(candidateRva);
        profile.autoTradingOffset = static_cast<SIZE_T>(candidateOffset);
        profile.source = path + L" [" + section + L"]";
        profile.diagnostic = L"Verified compatibility profile selected: " + profile.name;
        return profile;
    }

    std::wostringstream message;
    message << L"No verified compatibility profile for Charting.dll timestamp 0x"
            << std::hex << std::uppercase << peTimestamp << std::dec
            << L", image size " << imageSize
            << L". AutoTrading remains UNKNOWN until a profile is verified.";
    profile.diagnostic = message.str();
    return profile;
}
