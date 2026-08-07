#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "BrokerAuthDetector.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    std::wstring GetWindowTextSafe(HWND hwnd)
    {
        const int length = GetWindowTextLengthW(hwnd);
        if (length <= 0)
            return L"";

        std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
        text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
        return text;
    }

    std::wstring GetClassNameSafe(HWND hwnd)
    {
        wchar_t buffer[512]{};
        GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer)));
        return buffer;
    }

    bool LooksLikeBrowserWindow(const std::wstring& className)
    {
        const std::wstring value = ToLower(className);
        return value.find(L"chrome_widgetwin") != std::wstring::npos
            || value.find(L"mozilla") != std::wstring::npos
            || value.find(L"firefox") != std::wstring::npos
            || value.find(L"applicationframewindow") != std::wstring::npos;
    }

    struct ChildTextCollectData
    {
        std::wstring text;
        int count = 0;
    };

    BOOL CALLBACK CollectChildTextsProc(HWND hwnd, LPARAM lParam)
    {
        auto* data = reinterpret_cast<ChildTextCollectData*>(lParam);
        if (!data)
            return TRUE;
        if (data->count >= 250 || data->text.size() >= 16384)
            return FALSE;

        const std::wstring text = GetWindowTextSafe(hwnd);
        if (!text.empty())
        {
            data->text += L" ";
            data->text += text;
            ++data->count;
        }
        return TRUE;
    }

    std::wstring GetWindowAndChildText(HWND hwnd)
    {
        ChildTextCollectData data;
        data.text = GetWindowTextSafe(hwnd);
        EnumChildWindows(hwnd, CollectChildTextsProc, reinterpret_cast<LPARAM>(&data));
        return data.text;
    }

    bool ContainsAny(const std::wstring& lowerText, const std::vector<std::wstring>& patterns, std::wstring* matched = nullptr)
    {
        for (const auto& pattern : patterns)
        {
            const std::wstring normalized = ToLower(pattern);
            if (!normalized.empty() && lowerText.find(normalized) != std::wstring::npos)
            {
                if (matched)
                    *matched = pattern;
                return true;
            }
        }
        return false;
    }

    struct DetectionContext
    {
        const AppConfig* config = nullptr;
        BrokerAuthenticationDetection result;
    };

    BOOL CALLBACK EnumBrowserWindow(HWND hwnd, LPARAM lParam)
    {
        auto* context = reinterpret_cast<DetectionContext*>(lParam);
        if (!context)
            return TRUE;
        if (context->result.detected)
            return FALSE;
        if (!IsWindowVisible(hwnd))
            return TRUE;

        const std::wstring title = GetWindowTextSafe(hwnd);
        const std::wstring className = GetClassNameSafe(hwnd);
        if (title.empty() || !LooksLikeBrowserWindow(className))
            return TRUE;

        // This intentionally reuses the proven BrokerWatch technique from the
        // original production Watchdog: browser title plus child-window text.
        // In the supported browser UI this includes the visible address-bar URL.
        const std::wstring combined = GetWindowAndChildText(hwnd);
        const std::wstring lowerCombined = ToLower(combined);
        const std::wstring lowerTitle = ToLower(title);

        for (const auto& profile : context->config->brokerAuthProfiles)
        {
            if (!profile.enabled)
                continue;

            std::wstring matched;
            const bool urlConfigured = !profile.urlContains.empty();
            const bool titleConfigured = !profile.titleContains.empty();
            const bool textConfigured = !profile.textContains.empty();

            const bool urlMatch = urlConfigured && ContainsAny(lowerCombined, profile.urlContains, &matched);
            const bool titleMatch = !titleConfigured || ContainsAny(lowerTitle, profile.titleContains, &matched);
            const bool textMatch = !textConfigured || ContainsAny(lowerCombined, profile.textContains, &matched);

            // A configured login URL is sufficient and is the preferred signal.
            // Without a URL, require all configured fallback title/text criteria.
            const bool fallbackMatch = !urlConfigured && titleMatch && textMatch && (titleConfigured || textConfigured);
            if (!urlMatch && !fallbackMatch)
                continue;

            context->result.detected = true;
            context->result.profileName = profile.name;
            context->result.browserTitle = title;
            context->result.matchedPattern = matched;
            context->result.alertAfterSeconds = profile.alertAfterSeconds;

            // Never copy the browser's full URL into diagnostics. OAuth URLs can
            // contain request IDs or tokens. Report only the configured pattern.
            if (urlMatch)
                context->result.sanitizedUrl = matched;
            return FALSE;
        }
        return TRUE;
    }
}

BrokerAuthenticationDetection DetectBrokerAuthentication(const AppConfig& config)
{
    BrokerAuthenticationDetection result;
    if (!config.brokerMonitoringEnabled || config.brokerAuthProfiles.empty())
        return result;

    DetectionContext context;
    context.config = &config;
    EnumWindows(EnumBrowserWindow, reinterpret_cast<LPARAM>(&context));
    return context.result;
}
