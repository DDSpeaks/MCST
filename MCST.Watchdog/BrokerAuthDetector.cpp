#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <UIAutomation.h>

#include "BrokerAuthDetector.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    template <typename T>
    void ReleaseCom(T*& value)
    {
        if (value)
        {
            value->Release();
            value = nullptr;
        }
    }

    struct ComApartmentScope
    {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool usable = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
        bool ownsInitialization = result == S_OK || result == S_FALSE;

        ~ComApartmentScope()
        {
            if (ownsInitialization)
                CoUninitialize();
        }
    };

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

    void AppendAutomationBstr(std::wstring& text, BSTR value)
    {
        if (value && value[0] != L'\0' && text.size() < 65536)
        {
            text += L" ";
            text.append(value, SysStringLen(value));
        }
        SysFreeString(value);
    }

    std::wstring GetWindowAutomationText(HWND hwnd)
    {
        ComApartmentScope apartment;
        if (!apartment.usable)
            return L"";

        IUIAutomation* automation = nullptr;
        if (FAILED(CoCreateInstance(
                CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&automation))) || !automation)
        {
            return L"";
        }

        IUIAutomationElement* root = nullptr;
        IUIAutomationCondition* condition = nullptr;
        IUIAutomationElementArray* elements = nullptr;
        std::wstring text;

        if (SUCCEEDED(automation->ElementFromHandle(hwnd, &root)) && root &&
            SUCCEEDED(automation->CreateTrueCondition(&condition)) && condition &&
            SUCCEEDED(root->FindAll(TreeScope_Subtree, condition, &elements)) && elements)
        {
            int length = 0;
            if (SUCCEEDED(elements->get_Length(&length)))
            {
                // A browser can expose thousands of accessibility nodes. The
                // address bar and authentication form occur near the top of the
                // tree, so keep this read bounded on every Watchdog refresh.
                const int boundedLength = (std::min)(length, 1200);
                for (int index = 0; index < boundedLength && text.size() < 65536; ++index)
                {
                    IUIAutomationElement* element = nullptr;
                    if (FAILED(elements->GetElement(index, &element)) || !element)
                        continue;

                    BSTR value = nullptr;
                    if (SUCCEEDED(element->get_CurrentName(&value)))
                        AppendAutomationBstr(text, value);
                    value = nullptr;
                    if (SUCCEEDED(element->get_CurrentAutomationId(&value)))
                        AppendAutomationBstr(text, value);
                    value = nullptr;
                    if (SUCCEEDED(element->get_CurrentHelpText(&value)))
                        AppendAutomationBstr(text, value);

                    IUIAutomationValuePattern* valuePattern = nullptr;
                    if (SUCCEEDED(element->GetCurrentPatternAs(
                            UIA_ValuePatternId, IID_PPV_ARGS(&valuePattern))) && valuePattern)
                    {
                        value = nullptr;
                        if (SUCCEEDED(valuePattern->get_CurrentValue(&value)))
                            AppendAutomationBstr(text, value);
                        ReleaseCom(valuePattern);
                    }
                    ReleaseCom(element);
                }
            }
        }

        ReleaseCom(elements);
        ReleaseCom(condition);
        ReleaseCom(root);
        ReleaseCom(automation);
        return text;
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

        const std::wstring lowerTitle = ToLower(title);
        // The dedicated MultiCharts OAuth browser has an exact configured
        // title. Resolve that inexpensive and privacy-safe signal before asking
        // the browser accessibility provider for any descendant values.
        for (const auto& profile : context->config->brokerAuthProfiles)
        {
            if (!profile.enabled || profile.titleOnlyContains.empty())
                continue;
            std::wstring matchedTitle;
            if (!ContainsAny(lowerTitle, profile.titleOnlyContains, &matchedTitle))
                continue;

            context->result.detected = true;
            context->result.profileName = profile.name;
            context->result.browserTitle = title;
            context->result.matchedPattern = matchedTitle;
            context->result.alertAfterSeconds = profile.alertAfterSeconds;
            return FALSE;
        }

        // This intentionally reuses the proven BrokerWatch technique from the
        // original production Watchdog: browser title plus child-window text.
        // In the supported browser UI this includes the visible address-bar URL.
        std::wstring combined = GetWindowAndChildText(hwnd);
        combined += L" ";
        combined += GetWindowAutomationText(hwnd);
        const std::wstring lowerCombined = ToLower(combined);

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

            // A configured login URL is sufficient and preferred. Modern Edge
            // exposes its address bar through UI Automation instead of ordinary
            // Win32 child text. Broad title patterns still require page text;
            // independently sufficient titles were resolved before this scan.
            const bool fallbackMatch = titleMatch && textMatch && (titleConfigured || textConfigured);
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
