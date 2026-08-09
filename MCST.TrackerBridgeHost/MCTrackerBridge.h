#pragma once

#include <windows.h>

#if defined(MCTRACKERBRIDGE_EXPORTS)
#define MCBRIDGE_API extern "C" __declspec(dllexport)
#else
#define MCBRIDGE_API extern "C" __declspec(dllimport)
#endif

// PowerLanguage uses Int return values. On x64 __stdcall is accepted but ignored
// by the ABI; extern "C" plus the .def file keeps the exported names stable.
MCBRIDGE_API int __stdcall MCBridge_Initialize();
MCBRIDGE_API int __stdcall MCBridge_Heartbeat();
MCBRIDGE_API int __stdcall MCBridge_GetState();
MCBRIDGE_API int __stdcall MCBridge_Shutdown();
MCBRIDGE_API int __stdcall MCBridge_GetVersion();

enum MCBridgeResult : int
{
    MC_BRIDGE_OK = 1,
    MC_BRIDGE_STARTED_WAITING_FOR_TRACKER = 2,
    MC_BRIDGE_ALREADY_INITIALIZED = 3,

    MC_BRIDGE_ERROR = -1,
    MC_BRIDGE_ANOTHER_PROCESS_OWNS_SINGLETON = -2,
    MC_BRIDGE_NOT_MULTICHARTS_PROCESS = -3,
    MC_BRIDGE_THREAD_CREATE_FAILED = -4,
    MC_BRIDGE_NOT_INITIALIZED = -5,
    MC_BRIDGE_SHUTDOWN_TIMEOUT = -6,
    MC_BRIDGE_SELF_REFERENCE_FAILED = -7
};
