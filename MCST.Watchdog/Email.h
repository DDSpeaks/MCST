#pragma once
#include "AppConfig.h"
#include <windows.h>
#include <string>

struct EmailSendResult
{
    bool ok = false;
    bool alert = false;
    std::wstring message;
    std::wstring eventText;
};

class EmailSender
{
public:
    explicit EmailSender(const AppConfig& config);
    bool IsConfigured(std::wstring* reason = nullptr, const std::wstring& recipientOverride = L"") const;
    bool Send(const std::wstring& subject, const std::wstring& body, bool bodyAsHtml = false, std::wstring* errorOut = nullptr, const std::wstring& recipientOverride = L"") const;
private:
    AppConfig config_;
};

void SendEmailAsync(HWND targetWindow, UINT completionMessage, const AppConfig& config,
    const std::wstring& subject, const std::wstring& body, bool alert, const std::wstring& eventText, bool bodyAsHtml = false, const std::wstring& recipientOverride = L"");
