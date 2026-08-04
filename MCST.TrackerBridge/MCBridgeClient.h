#pragma once

#include "../MCST.Shared/MCBridgeProtocol.h"

#include <windows.h>

#include <cstdint>
#include <string>

class MCBridgeClient
{
public:
    MCBridgeClient() = default;
    ~MCBridgeClient();

    MCBridgeClient(const MCBridgeClient&) = delete;
    MCBridgeClient& operator=(const MCBridgeClient&) = delete;

    bool Connect(DWORD timeoutMilliseconds, std::wstring& diagnostic);
    void Disconnect();
    bool IsConnected() const noexcept { return pipe_ != INVALID_HANDLE_VALUE; }

    bool Request(
        mcbridge::Command command,
        const std::string& requestPayload,
        mcbridge::Status& responseStatus,
        std::string& responsePayload,
        std::wstring& diagnostic);

private:
    bool ReadExact(void* buffer, DWORD bytes, std::wstring& diagnostic);
    bool WriteExact(const void* buffer, DWORD bytes, std::wstring& diagnostic);

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::uint32_t nextRequestId_ = 1;
};
