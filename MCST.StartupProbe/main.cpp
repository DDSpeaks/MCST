#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace
{
    void WriteProbeLog() noexcept
    {
        CreateDirectoryW(L"C:\\Temp", nullptr);
        const wchar_t* path = L"C:\\Temp\\MCST-StartupProbe.log";
        HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        const char text[] = "MCST Startup Probe 1.108 reached wWinMain successfully.\r\n";
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(sizeof(text) - 1), &written, nullptr);
        CloseHandle(file);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    WriteProbeLog();
    MessageBoxW(nullptr,
        L"MCST Startup Probe 1.108 started successfully.\n\n"
        L"This confirms that a minimal Release x64 /MT Windows executable can start on this computer.\n\n"
        L"A marker was also written to C:\\Temp\\MCST-StartupProbe.log.",
        L"MCST Startup Probe 1.108", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    return 0;
}
