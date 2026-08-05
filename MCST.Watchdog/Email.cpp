#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "Email.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <exception>

namespace {
std::wstring EscapePS(const std::wstring& value) {
    std::wstring out; out.reserve(value.size()+8);
    for (wchar_t c: value) { out.push_back(c); if (c==L'\'') out.push_back(L'\''); }
    return out;
}
bool WriteUtf8(const std::filesystem::path& path, const std::wstring& text) {
    int n=WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0,nullptr,nullptr);
    if(n<0) return false; std::string bytes((size_t)n,'\0');
    if(n) WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),bytes.data(),n,nullptr,nullptr);
    std::ofstream f(path,std::ios::binary|std::ios::trunc); if(!f) return false;
    f.write(bytes.data(),(std::streamsize)bytes.size()); return f.good();
}
}
EmailSender::EmailSender(const AppConfig& config): config_(config) {}
bool EmailSender::IsConfigured(std::wstring* reason) const {
    if(config_.emailEnabledSettingPresent && !config_.emailEnabled){ if(reason)*reason=L"Email is explicitly disabled in INI ([Email] enabled=false)."; return false; }
    if(config_.smtpServer.empty()||config_.smtpUser.empty()||config_.smtpPassword.empty()||config_.emailFrom.empty()||config_.emailTo.empty()){
        if(reason)*reason=L"SMTP server, user, password, from and to must be configured."; return false;
    }
    return true;
}
bool EmailSender::Send(const std::wstring& subject, const std::wstring& body, bool bodyAsHtml, std::wstring* errorOut) const {
    std::wstring reason; if(!IsConfigured(&reason)){ if(errorOut)*errorOut=reason; return false; }
    try {
        auto temp=std::filesystem::temp_directory_path();
        auto stamp=std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64());
        auto ps=temp/(L"mcst_mail_"+stamp+L".ps1"); auto bp=temp/(L"mcst_body_"+stamp+L".txt"); auto sp=temp/(L"mcst_subject_"+stamp+L".txt");
        if(!WriteUtf8(bp,body)||!WriteUtf8(sp,subject)){ if(errorOut)*errorOut=L"Could not create temporary email files."; return false; }
        std::wofstream f(ps,std::ios::trunc); if(!f){ if(errorOut)*errorOut=L"Could not create PowerShell email script."; return false; }
        f<<L"$ErrorActionPreference='Stop'\n";
        f<<L"$server='"<<EscapePS(config_.smtpServer)<<L"'\n$port="<<config_.smtpPort<<L"\n";
        f<<L"$user='"<<EscapePS(config_.smtpUser)<<L"'\n$pass='"<<EscapePS(config_.smtpPassword)<<L"'\n";
        f<<L"$from='"<<EscapePS(config_.emailFrom)<<L"'\n$to='"<<EscapePS(config_.emailTo)<<L"'\n";
        f<<L"$subject=Get-Content -LiteralPath '"<<EscapePS(sp.wstring())<<L"' -Raw -Encoding UTF8\n";
        f<<L"$body=Get-Content -LiteralPath '"<<EscapePS(bp.wstring())<<L"' -Raw -Encoding UTF8\n";
        f<<L"$sec=ConvertTo-SecureString $pass -AsPlainText -Force\n$cred=New-Object System.Management.Automation.PSCredential($user,$sec)\n";
        f<<L"Send-MailMessage -SmtpServer $server -Port $port "<<(config_.smtpUseSsl?L"-UseSsl ":L"")<<L"-Credential $cred -From $from -To $to -Subject $subject -Body $body "<<(bodyAsHtml?L"-BodyAsHtml ":L"")<<L"-Encoding UTF8\n";
        f.close();
        std::wstring cmd=L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+ps.wstring()+L"\"";
        int rc=_wsystem(cmd.c_str()); std::error_code ec; std::filesystem::remove(ps,ec); std::filesystem::remove(bp,ec); std::filesystem::remove(sp,ec);
        if(rc!=0){ if(errorOut)*errorOut=L"PowerShell email send failed, return code "+std::to_wstring(rc)+L"."; return false; }
        return true;
    } catch(const std::exception& e){ if(errorOut){ int n=MultiByteToWideChar(CP_UTF8,0,e.what(),-1,nullptr,0); std::wstring w((size_t)(n>0?n:0),L'\0'); if(n>1){ MultiByteToWideChar(CP_UTF8,0,e.what(),-1,w.data(),n); if(!w.empty() && w.back()==L'\0') w.pop_back(); } *errorOut=w; } return false; }
}

#include <memory>
#include <thread>

void SendEmailAsync(HWND targetWindow, UINT completionMessage, const AppConfig& config,
    const std::wstring& subject, const std::wstring& body, bool alert, const std::wstring& eventText, bool bodyAsHtml)
{
    std::thread([targetWindow, completionMessage, config, subject, body, alert, eventText, bodyAsHtml]() {
        auto result = std::make_unique<EmailSendResult>();
        result->alert = alert;
        result->eventText = eventText;
        EmailSender sender(config);
        std::wstring error;
        result->ok = sender.Send(subject, body, bodyAsHtml, &error);
        result->message = result->ok ? L"Email sent successfully." : error;
        PostMessageW(targetWindow, completionMessage, 0, reinterpret_cast<LPARAM>(result.release()));
    }).detach();
}
