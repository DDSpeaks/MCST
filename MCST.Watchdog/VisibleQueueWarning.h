#pragma once

// Lightweight visible-only check of the MC16 queue-warning field (part 4).
// No text extraction, OCR, chart activation or full-statusbar pixel scan.
void ReadVisibleQueueWarning(HWND root, bool& checked, bool& redVisible, bool& rendered,
    std::wstring& diagnostic)
{
    checked = false;
    redVisible = false;
    rendered = false;
    const auto start = std::chrono::steady_clock::now();
    std::wostringstream out;
    struct FieldCache { HWND bar = nullptr; int width = 0; int height = 0; RECT field{}; };
    static std::map<HWND, FieldCache> cache;
    static std::set<DWORD> pointerReadDisabled;
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    auto finish = [&]() {
        out << L" elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        diagnostic = out.str();
    };
    if (!IsWindowVisible(root) || IsIconic(root))
    {
        out << L"visible-warning=not-checked (hidden/minimized)";
        finish(); return;
    }
    auto& entry = cache[root];
    DWORD barPid = 0;
    GetWindowThreadProcessId(entry.bar, &barPid);
    if (!IsWindow(entry.bar) || barPid != pid || GetAncestor(entry.bar, GA_ROOT) != root)
    {
        StatusBarSearch search;
        search.root = root;
        EnumChildWindows(root, CollectStatusBars, reinterpret_cast<LPARAM>(&search));
        entry = FieldCache{};
        if (!search.windows.empty()) entry.bar = search.windows.front();
    }
    RECT client{};
    if (!entry.bar || !IsWindowVisible(entry.bar) || !GetClientRect(entry.bar, &client))
    {
        out << L"visible-warning=not-checked (statusbar unavailable)";
        finish(); return;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (entry.width != width || entry.height != height || entry.field.right <= entry.field.left)
    {
        entry.width = entry.height = 0;
        entry.field = RECT{};
        if (pointerReadDisabled.count(pid))
        {
            out << L"visible-warning=not-checked (geometry disabled after message failure)";
            finish(); return;
        }
        DWORD_PTR parts = 0;
        SetLastError(0);
        if (!SendMessageTimeoutW(entry.bar, SB_GETPARTS, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 125, &parts) || parts != 7)
        {
            out << L"visible-warning=not-checked (expected 7-part MC16 statusbar; parts="
                << parts << L" error=" << GetLastError() << L")";
            finish(); return;
        }
        HANDLE target = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, pid);
        void* remote = target ? VirtualAllocEx(target, nullptr, sizeof(RECT),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : nullptr;
        DWORD_PTR result = 0;
        SIZE_T bytes = 0;
        bool success = false;
        if (remote)
        {
            SetLastError(0);
            const bool replied = SendMessageTimeoutW(entry.bar, SB_GETRECT, 4,
                reinterpret_cast<LPARAM>(remote), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                125, &result) != 0;
            const DWORD messageError = GetLastError();
            if (replied)
            {
                success = result && ReadProcessMemory(target, remote, &entry.field,
                    sizeof(RECT), &bytes) && bytes == sizeof(RECT);
                VirtualFreeEx(target, remote, 0, MEM_RELEASE);
            }
            else
            {
                // Receiver might still write later; keep one allocation and
                // disable pointer queries for this PID until Watchdog exits.
                pointerReadDisabled.insert(pid);
                out << L"geometry-message failed error=" << messageError << L"; ";
            }
        }
        if (target) CloseHandle(target);
        if (!success || entry.field.left < 0 || entry.field.top < 0 ||
            entry.field.right > width || entry.field.bottom > height ||
            entry.field.right <= entry.field.left || entry.field.bottom <= entry.field.top)
        {
            entry.field = RECT{};
            out << L"visible-warning=not-checked (field geometry unavailable)";
            finish(); return;
        }
        entry.width = width; entry.height = height;
    }
    RECT field = entry.field;
    POINT origin{field.left, field.top};
    if (!ClientToScreen(entry.bar, &origin))
    {
        out << L"visible-warning=not-checked (coordinate conversion failed)";
        finish(); return;
    }
    const int fieldWidth = field.right - field.left;
    const int fieldHeight = field.bottom - field.top;
    if (fieldWidth < 12 || fieldWidth > 512 || fieldHeight < 6 || fieldHeight > 128)
    {
        out << L"visible-warning=not-checked (unsupported field size)";
        finish(); return;
    }
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = screen ? CreateCompatibleBitmap(screen, fieldWidth, fieldHeight) : nullptr;
    HGDIOBJ previous = memory && bitmap ? SelectObject(memory, bitmap) : nullptr;
    unsigned int samples = 0, visible = 0, red = 0;
    const bool captured = previous && previous != HGDI_ERROR &&
        BitBlt(memory, 0, 0, fieldWidth, fieldHeight, screen, origin.x, origin.y, SRCCOPY);
    if (captured)
    {
        for (int row = 1; row <= 2; ++row)
            for (int x = 2; x < fieldWidth - 2; x += 2)
            {
                ++samples;
                const int y = fieldHeight * row / 3;
                const HWND hit = WindowFromPoint(POINT{origin.x + x, origin.y + y});
                if (hit != entry.bar) continue;
                const COLORREF color = GetPixel(memory, x, y);
                if (color == CLR_INVALID) continue;
                ++visible;
                if (GetRValue(color) >= 150 && GetGValue(color) <= 90 && GetBValue(color) <= 90)
                    ++red;
            }
        // Require every sampled point to belong to this statusbar. Any detected
        // occlusion makes the check unavailable, never a negative reading.
        checked = samples >= 12 && visible == samples;
        redVisible = mcst::IsVisibleRedWarning(samples, visible, red);
    }
    if (previous && previous != HGDI_ERROR) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    out << L"field=4 visible-warning=" << (!checked ? L"not-checked" : redVisible ? L"red" : L"not-red")
        << L" samples=" << samples << L" visible=" << visible << L" red=" << red;
    if (!checked && samples > visible)
    {
        std::wstring probeDiagnostic;
        if (mcstprobe::TryCovered(entry.bar, entry.field, probeDiagnostic))
        {
            checked = redVisible = rendered = true;
        }
        out << L" " << probeDiagnostic;
    }
    finish();
}
