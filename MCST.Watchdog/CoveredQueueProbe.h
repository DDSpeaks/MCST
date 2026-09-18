#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <map>
#include <set>
#include <iterator>
#include <chrono>
#include <cwchar>
#include "../MCST.Shared/VisibleWarningPolicy.h"

namespace mcstprobe
{
    struct Payload
    {
        DWORD magic = 0x4D435150;
        DWORD targetPid = 0;
        UINT_PTR bar = 0;
        RECT field{};
        DWORD redPasses = 0;
        DWORD completed = 0;
    };
    inline unsigned int& Budget() { static unsigned int value = 0; return value; }
    inline std::set<DWORD>& Eligible() { static std::set<DWORD> value; return value; }
    inline void ResetBudget(const std::set<DWORD>& processes)
    {
        static std::size_t cursor = 0;
        Eligible().clear(); Budget() = 2;
        if (processes.empty()) return;
        auto it = processes.begin();
        std::advance(it, cursor % processes.size());
        for (std::size_t i = 0; i < (std::min)(std::size_t(2), processes.size()); ++i)
        {
            Eligible().insert(*it);
            if (++it == processes.end()) it = processes.begin();
        }
        cursor = (cursor + 2) % processes.size();
    }

    inline int Worker(const wchar_t* name)
    {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        auto* payload = mapping ? static_cast<Payload*>(MapViewOfFile(mapping,
            FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Payload))) : nullptr;
        if (!payload) { if (mapping) CloseHandle(mapping); return 2; }
        HWND bar = reinterpret_cast<HWND>(payload->bar);
        DWORD pid = 0;
        GetWindowThreadProcessId(bar, &pid);
        RECT client{};
        wchar_t windowClass[64]{};
        GetClassNameW(bar, windowClass, 64);
        const RECT field = payload->field;
        bool valid = payload->magic == 0x4D435150 && pid == payload->targetPid &&
            wcscmp(windowClass, L"msctls_statusbar32") == 0 && GetClientRect(bar, &client) &&
            client.right > 0 && client.right <= 4096 && client.bottom > 0 && client.bottom <= 128 &&
            field.left >= 0 && field.top >= 0 && field.right <= client.right && field.bottom <= client.bottom &&
            field.right - field.left >= 12 && field.bottom - field.top >= 6;
        HDC screen = valid ? GetDC(nullptr) : nullptr;
        HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
        HBITMAP bitmap = screen ? CreateCompatibleBitmap(screen, client.right, client.bottom) : nullptr;
        HGDIOBJ previous = memory && bitmap ? SelectObject(memory, bitmap) : nullptr;
        if (previous && previous != HGDI_ERROR)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                // PrintWindow is synchronous: only this expendable child calls it.
                PatBlt(memory, 0, 0, client.right, client.bottom, BLACKNESS);
                if (!PrintWindow(bar, memory, PW_CLIENTONLY)) break;
                unsigned int samples = 0, red = 0;
                bool pixelsValid = true;
                for (int row = 1; row <= 2; ++row)
                    for (int x = field.left + 2; x < field.right - 2; x += 2)
                    {
                        const int y = field.top + (field.bottom - field.top) * row / 3;
                        const COLORREF color = GetPixel(memory, x, y);
                        ++samples;
                        if (color == CLR_INVALID) { pixelsValid = false; continue; }
                        if (GetRValue(color) >= 150 && GetGValue(color) <= 90 && GetBValue(color) <= 90) ++red;
                    }
                if (pixelsValid && mcst::IsVisibleRedWarning(samples, samples, red)) ++payload->redPasses;
                else break; // Non-red output cannot prove a covered warning is absent.
            }
            SelectObject(memory, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        payload->completed = 1;
        UnmapViewOfFile(payload);
        CloseHandle(mapping);
        return 0;
    }

    inline bool TryWorkerMode(int& result)
    {
        int argc = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &argc);
        const bool mode = args && argc >= 2 && wcscmp(args[1], L"--mcst-covered-queue-probe") == 0;
        if (mode) result = argc == 3 ? Worker(args[2]) : 2;
        if (args) LocalFree(args);
        return mode;
    }

    inline bool TryCovered(HWND bar, const RECT& field, std::wstring& diagnostic)
    {
        DWORD targetPid = 0;
        GetWindowThreadProcessId(bar, &targetPid);
        if (!Eligible().count(targetPid))
        { diagnostic = L"covered-probe=deferred (rotating process budget)"; return false; }
        static std::map<HWND, std::chrono::steady_clock::time_point> attempts;
        const auto now = std::chrono::steady_clock::now();
        if (!Budget()) { diagnostic = L"covered-probe=refresh-budget-exhausted"; return false; }
        if (attempts.count(bar) && now - attempts[bar] < std::chrono::seconds(60))
        { diagnostic = L"covered-probe=cooldown"; return false; }
        --Budget(); attempts[bar] = now;
        static unsigned long serial = 0;
        const std::wstring name = L"Local\\MCSTQueueProbe_" + std::to_wstring(GetCurrentProcessId()) +
            L"_" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(++serial);
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Payload), name.c_str());
        if (mapping && GetLastError() == ERROR_ALREADY_EXISTS)
        { CloseHandle(mapping); diagnostic = L"covered-probe=mapping-collision"; return false; }
        auto* payload = mapping ? static_cast<Payload*>(MapViewOfFile(mapping,
            FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Payload))) : nullptr;
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        bool positive = false;
        diagnostic = L"covered-probe=setup-failed";
        wchar_t executable[32768]{};
        const DWORD pathLength = GetModuleFileNameW(nullptr, executable, 32768);
        if (payload && job && pathLength && pathLength < 32768 &&
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            *payload = Payload{};
            GetWindowThreadProcessId(bar, &payload->targetPid);
            payload->bar = reinterpret_cast<UINT_PTR>(bar);
            payload->field = field;
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --mcst-covered-queue-probe \"" + name + L"\"";
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                    CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            {
                if (AssignProcessToJobObject(job, process.hProcess) && ResumeThread(process.hThread) != static_cast<DWORD>(-1))
                {
                    const DWORD wait = WaitForSingleObject(process.hProcess, 300);
                    if (wait == WAIT_OBJECT_0)
                    {
                        DWORD expectedPid = 0;
                        GetWindowThreadProcessId(bar, &expectedPid);
                        positive = payload->magic == 0x4D435150 && payload->targetPid == expectedPid &&
                            payload->bar == reinterpret_cast<UINT_PTR>(bar) &&
                            mcst::IsConfirmedRenderedWarning(payload->completed == 1, payload->redPasses);
                        diagnostic = positive ? L"covered-probe=RED (two rendered images; experimental)"
                            : L"covered-probe=inconclusive (non-red/blank/unsupported output)";
                    }
                    else diagnostic = L"covered-probe=timeout-or-wait-failure (300 ms)";
                }
                else diagnostic = L"covered-probe=job-or-start-failed";
                // This handle belongs only to the child just created, never MC.
                if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0)
                    TerminateProcess(process.hProcess, 3);
                CloseHandle(process.hThread); CloseHandle(process.hProcess);
            }
            else diagnostic = L"covered-probe=launch-failed";
        }
        if (job) CloseHandle(job);
        if (payload) UnmapViewOfFile(payload);
        if (mapping) CloseHandle(mapping);
        return positive;
    }
}
