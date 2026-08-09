#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include "ExtractorSeh.h"

typedef struct MCBridgeIUnknownVtable
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** object);
    ULONG (STDMETHODCALLTYPE *AddRef)(void* self);
    ULONG (STDMETHODCALLTYPE *Release)(void* self);
} MCBridgeIUnknownVtable;

typedef struct MCBridgeIUnknownObject { MCBridgeIUnknownVtable* vtable; } MCBridgeIUnknownObject;

int MCBridge_SafeCopyMemory(void* destination, const void* source, size_t bytes, DWORD* sehCode)
{
    if (sehCode != NULL) *sehCode = 0;
    if (destination == NULL || source == NULL || bytes == 0) return 0;
    __try { memcpy(destination, source, bytes); }
    __except (sehCode != NULL ? (*sehCode = GetExceptionCode()) : 0, EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

HRESULT MCBridge_QueryInterfaceWithSeh(void* unknownPointer, const GUID* interfaceId, void** result, DWORD* sehCode)
{
    MCBridgeIUnknownObject* object;
    if (result != NULL) *result = NULL;
    if (sehCode != NULL) *sehCode = 0;
    if (unknownPointer == NULL || interfaceId == NULL || result == NULL) return E_POINTER;
    object = (MCBridgeIUnknownObject*)unknownPointer;
    __try
    {
        if (object->vtable == NULL || object->vtable->QueryInterface == NULL) return E_NOINTERFACE;
        return object->vtable->QueryInterface(object, interfaceId, result);
    }
    __except (sehCode != NULL ? (*sehCode = GetExceptionCode()) : 0, EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
}

ULONG MCBridge_ReleaseWithSeh(void* unknownPointer, DWORD* sehCode, int* succeeded)
{
    MCBridgeIUnknownObject* object;
    ULONG result = 0;
    if (sehCode != NULL) *sehCode = 0;
    if (succeeded != NULL) *succeeded = 0;
    if (unknownPointer == NULL) return 0;
    object = (MCBridgeIUnknownObject*)unknownPointer;
    __try
    {
        if (object->vtable == NULL || object->vtable->Release == NULL) return 0;
        result = object->vtable->Release(object);
        if (succeeded != NULL) *succeeded = 1;
        return result;
    }
    __except (sehCode != NULL ? (*sehCode = GetExceptionCode()) : 0, EXCEPTION_EXECUTE_HANDLER) { return 0; }
}


typedef void (*MCBridgeFlexGridGetTextFn)(
    void* self, unsigned int row, unsigned int column,
    wchar_t* buffer, unsigned int bufferCharacters);

int MCBridge_CallFlexGridGetTextWithSeh(
    void* gridObject, void* functionAddress,
    unsigned int row, unsigned int column,
    wchar_t* buffer, unsigned int bufferCharacters, DWORD* sehCode)
{
    MCBridgeFlexGridGetTextFn functionPointer;
    if (sehCode != NULL) *sehCode = 0;
    if (gridObject == NULL || functionAddress == NULL || buffer == NULL || bufferCharacters == 0) return 0;
    buffer[0] = L'\0';
    functionPointer = (MCBridgeFlexGridGetTextFn)functionAddress;
    __try
    {
        functionPointer(gridObject, row, column, buffer, bufferCharacters);
        buffer[bufferCharacters - 1] = L'\0';
        return 1;
    }
    __except (sehCode != NULL ? (*sehCode = GetExceptionCode()) : 0, EXCEPTION_EXECUTE_HANDLER)
    {
        buffer[0] = L'\0';
        return 0;
    }
}
