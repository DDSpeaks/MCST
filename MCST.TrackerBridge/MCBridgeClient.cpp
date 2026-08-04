#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "MCBridgeClient.h"

#include <algorithm>
#include <sstream>

namespace
{
    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\MCTrackerBridge";
    constexpr DWORD kRetryDelayMs = 100;
}

MCBridgeClient::~MCBridgeClient()
{
    Disconnect();
}

bool MCBridgeClient::Connect(DWORD timeoutMilliseconds, std::wstring& diagnostic)
{
    Disconnect();
    diagnostic.clear();

    const ULONGLONG started = GetTickCount64();
    DWORD attempts = 0;
    DWORD lastError = ERROR_SUCCESS;

    for (;;)
    {
        ++attempts;
        pipe_ = CreateFileW(
            kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (pipe_ != INVALID_HANDLE_VALUE)
        {
            std::wostringstream out;
            out << L"connected attempts=" << attempts;
            diagnostic = out.str();
            return true;
        }

        lastError = GetLastError();
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= timeoutMilliseconds)
            break;

        const DWORD remaining = static_cast<DWORD>(timeoutMilliseconds - elapsed);
        const DWORD waitSlice = (std::min)(remaining, kRetryDelayMs);
        if (lastError == ERROR_PIPE_BUSY)
            WaitNamedPipeW(kPipeName, waitSlice);
        else
            Sleep(waitSlice);
    }

    std::wostringstream out;
    out << L"pipe connect timed out attempts=" << attempts
        << L" last_error=" << lastError;
    diagnostic = out.str();
    pipe_ = INVALID_HANDLE_VALUE;
    return false;
}

void MCBridgeClient::Disconnect()
{
    if (pipe_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool MCBridgeClient::ReadExact(void* buffer, DWORD bytes, std::wstring& diagnostic)
{
    auto* cursor = static_cast<unsigned char*>(buffer);
    DWORD remaining = bytes;
    while (remaining > 0)
    {
        DWORD read = 0;
        if (!ReadFile(pipe_, cursor, remaining, &read, nullptr) || read == 0)
        {
            const DWORD error = GetLastError();
            std::wostringstream out;
            out << L"ReadFile failed error=" << error;
            diagnostic = out.str();
            Disconnect();
            return false;
        }
        cursor += read;
        remaining -= read;
    }
    return true;
}

bool MCBridgeClient::WriteExact(const void* buffer, DWORD bytes, std::wstring& diagnostic)
{
    const auto* cursor = static_cast<const unsigned char*>(buffer);
    DWORD remaining = bytes;
    while (remaining > 0)
    {
        DWORD written = 0;
        if (!WriteFile(pipe_, cursor, remaining, &written, nullptr) || written == 0)
        {
            const DWORD error = GetLastError();
            std::wostringstream out;
            out << L"WriteFile failed error=" << error;
            diagnostic = out.str();
            Disconnect();
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool MCBridgeClient::Request(
    mcbridge::Command command,
    const std::string& requestPayload,
    mcbridge::Status& responseStatus,
    std::string& responsePayload,
    std::wstring& diagnostic)
{
    diagnostic.clear();
    responsePayload.clear();
    responseStatus = mcbridge::Status::InternalError;

    if (!IsConnected())
    {
        diagnostic = L"bridge pipe is not connected";
        return false;
    }
    if (requestPayload.size() > mcbridge::kMaximumPayloadBytes)
    {
        diagnostic = L"request payload exceeds protocol limit";
        return false;
    }

    mcbridge::MessageHeader request;
    request.command = static_cast<std::uint16_t>(command);
    request.requestId = nextRequestId_++;
    request.payloadBytes = static_cast<std::uint32_t>(requestPayload.size());

    if (!WriteExact(&request, sizeof(request), diagnostic))
        return false;
    if (!requestPayload.empty() &&
        !WriteExact(requestPayload.data(), request.payloadBytes, diagnostic))
        return false;

    mcbridge::MessageHeader response{};
    if (!ReadExact(&response, sizeof(response), diagnostic))
        return false;
    if (response.magic != mcbridge::kMagic ||
        response.protocolVersion != mcbridge::kProtocolVersion ||
        response.requestId != request.requestId ||
        response.command != request.command)
    {
        diagnostic = L"response header validation failed";
        Disconnect();
        return false;
    }
    if (response.payloadBytes > mcbridge::kMaximumPayloadBytes)
    {
        diagnostic = L"response payload exceeds protocol limit";
        Disconnect();
        return false;
    }

    responsePayload.assign(response.payloadBytes, '\0');
    if (response.payloadBytes > 0 &&
        !ReadExact(responsePayload.data(), response.payloadBytes, diagnostic))
        return false;

    responseStatus = static_cast<mcbridge::Status>(response.status);
    diagnostic = L"request completed on persistent connection";
    return true;
}
