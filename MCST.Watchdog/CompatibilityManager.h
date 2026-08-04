#pragma once

#include <windows.h>

#include <string>

struct CompatibilityProfile
{
    bool matched = false;
    std::wstring name;
    DWORD chartingPeTimestamp = 0;
    unsigned long long chartingImageSize = 0;
    ULONG_PTR strategyVtableRva = 0;
    SIZE_T autoTradingOffset = 0;
    std::wstring source;
    std::wstring diagnostic;
};

/**
 * @brief Returns the path of the external compatibility profile database.
 */
std::wstring GetCompatibilityDatabasePath();

/**
 * @brief Creates the compatibility database with the verified built-in profile when missing.
 */
bool EnsureCompatibilityDatabase(std::wstring& diagnostic);

/**
 * @brief Selects a verified profile for the detected Charting.dll build.
 *
 * Exact matching uses the PE timestamp and image size. Unknown builds are
 * rejected safely instead of reusing an unverified memory signature.
 */
CompatibilityProfile ResolveCompatibilityProfile(DWORD peTimestamp, unsigned long long imageSize);
