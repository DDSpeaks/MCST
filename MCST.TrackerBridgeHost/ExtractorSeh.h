#pragma once

#include <windows.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

int MCBridge_SafeCopyMemory(void* destination, const void* source, size_t bytes, DWORD* sehCode);

HRESULT MCBridge_QueryInterfaceWithSeh(
    void* unknownPointer,
    const GUID* interfaceId,
    void** result,
    DWORD* sehCode);

ULONG MCBridge_ReleaseWithSeh(void* unknownPointer, DWORD* sehCode, int* succeeded);

int MCBridge_CallFlexGridGetTextWithSeh(
    void* gridObject,
    void* functionAddress,
    unsigned int row,
    unsigned int column,
    wchar_t* buffer,
    unsigned int bufferCharacters,
    DWORD* sehCode);

#ifdef __cplusplus
}
#endif
