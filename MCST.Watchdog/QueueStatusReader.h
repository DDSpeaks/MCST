#pragma once

// Included inside MultiChartsHealthMonitor.cpp's anonymous namespace.
// Only standard status bars are queried. No chart UIA/MSAA traversal.
struct StatusBarSearch
{
    HWND root = nullptr;
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectStatusBars(HWND window, LPARAM parameter)
{
    auto& search = *reinterpret_cast<StatusBarSearch*>(parameter);
    if (search.windows.size() >= 8)
        return FALSE;
    if (LowerCopy(WindowClass(window)) == L"msctls_statusbar32" &&
        GetAncestor(window, GA_ROOT) == search.root)
        search.windows.push_back(window);
    return TRUE;
}

bool ReadStatusBarQueue(HWND root, unsigned long& count, unsigned long& age,
    std::wstring& diagnostic)
{
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(500);
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    static std::map<HWND, std::vector<HWND>> cache;
    // A timed-out SB_GETTEXT may still write later. Never free/reuse that
    // buffer in this target process. At most one 128 KiB quarantine per PID
    // during this Watchdog run; Windows reclaims it when the target exits.
    static std::set<DWORD> quarantined;
    auto& bars = cache[root];
    bool valid = !bars.empty();
    for (HWND bar : bars)
    {
        DWORD barPid = 0;
        GetWindowThreadProcessId(bar, &barPid);
        valid = valid && IsWindow(bar) && barPid == pid &&
            GetAncestor(bar, GA_ROOT) == root &&
            LowerCopy(WindowClass(bar)) == L"msctls_statusbar32";
    }
    if (!valid)
    {
        StatusBarSearch search;
        search.root = root;
        EnumChildWindows(root, CollectStatusBars, reinterpret_cast<LPARAM>(&search));
        bars = search.windows;
    }
    std::wostringstream out;
    out << L"statusbars=" << bars.size();
    HANDLE process = nullptr;
    void* remote = nullptr;
    bool timedOutWrite = false;
    bool found = false;
    constexpr SIZE_T bufferBytes = 65536 * sizeof(wchar_t);
    auto send = [&](HWND bar, UINT message, WPARAM part, LPARAM buffer, DWORD_PTR& result) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        SetLastError(0);
        return SendMessageTimeoutW(bar, message, part, buffer,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result) != 0;
    };
    for (HWND bar : bars)
    {
        if (found || timedOutWrite || std::chrono::steady_clock::now() >= deadline)
            break;
        DWORD_PTR partsResult = 0;
        if (!send(bar, SB_GETPARTS, 0, 0, partsResult))
        {
            out << L" [SB_GETPARTS failed error=" << GetLastError() << L"]";
            continue;
        }
        const unsigned int parts = (std::min)(static_cast<unsigned int>(partsResult), 64U);
        out << L" [HWND=0x" << std::hex << reinterpret_cast<UINT_PTR>(bar)
            << std::dec << L" parts=" << parts << L"]";
        for (unsigned int part = 0; part < parts; ++part)
        {
            DWORD_PTR lengthResult = 0;
            if (!send(bar, SB_GETTEXTLENGTHW, part, 0, lengthResult))
            {
                out << L" [part=" << part << L" length-query failed]";
                break;
            }
            const bool ownerDraw = (HIWORD(lengthResult) & SBT_OWNERDRAW) != 0;
            out << L" [part=" << part << L" len=" << LOWORD(lengthResult)
                << L" ownerdraw=" << (ownerDraw ? L"yes" : L"no");
            if (!ownerDraw && LOWORD(lengthResult) == 0)
            {
                out << L" text=(empty)]";
                continue;
            }
            if (quarantined.count(pid))
            {
                out << L" read=disabled-after-timeout]";
                continue;
            }
            if (!process)
                process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ |
                    PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process && !remote)
                remote = VirtualAllocEx(process, nullptr, bufferBytes,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!remote)
            {
                out << L" buffer-access failed error=" << GetLastError() << L"]";
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                out << L" read=budget-exhausted]";
                break;
            }
            DWORD_PTR textResult = 0;
            if (!send(bar, SB_GETTEXTW, part, reinterpret_cast<LPARAM>(remote), textResult))
            {
                // Failure cannot prove the receiver has finished using lParam.
                timedOutWrite = true;
                quarantined.insert(pid);
                out << L" read=failed-buffer-quarantined]";
                break;
            }
            if (ownerDraw)
            {
                out << L" item-data=0x" << std::hex << textResult << std::dec
                    << L" (not interpreted as text)]";
                continue;
            }
            std::vector<wchar_t> text(65536, L'\0');
            SIZE_T bytesRead = 0;
            const SIZE_T required = (static_cast<SIZE_T>(LOWORD(textResult)) + 1) * sizeof(wchar_t);
            if (!ReadProcessMemory(process, remote, text.data(), required, &bytesRead) || bytesRead != required)
            {
                out << L" memory-read failed]";
                continue;
            }
            text.back() = L'\0';
            text[LOWORD(textResult)] = L'\0';
            const std::wstring value(text.data());
            out << L" text=" << value.substr(0, 512) << L"]";
            if (ParseQueueText(value, count, age))
            {
                found = true;
                break;
            }
        }
    }
    if (remote && !timedOutWrite)
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (process)
        CloseHandle(process);
    out << L" elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    diagnostic = out.str();
    return found;
}
