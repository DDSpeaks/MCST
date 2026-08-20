# MCST 1.114-R27 UI Automation Header Fix

Tracker Bridge internal build: V176  
Bridge protocol: V2

## Windows SDK compilation fix

- `BrokerAuthDetector.cpp` now includes `ole2.h` and `oleauto.h` before
  `UIAutomation.h`, ensuring the COM interface declarations are available.
- `WIN32_LEAN_AND_MEAN` is no longer defined in this translation unit because
  it hid declarations required by the Windows 10.0.26100 UI Automation headers.
- The authentication behavior, privacy limits, account totals, report layout,
  Tracker recovery, Bridge V176, and Protocol V2 are otherwise unchanged.

## Account-specific monthly Realized P/L

- Position History is totalled only for every account visible in Accounts.
  Rows belonging to any other account are excluded.
- Each visible account receives its own `Current month Realized P/L <account>`
  line. No potentially misleading combined total across accounts is shown.
- Known currencies remain separate, and the amount, currency code, and sign use
  the same green/red styling as Open P/L.
- The locale-aware Position History date parser supports numeric DMY, MDY, and
  YMD formats. `[Tracker] date_order=auto|dmy|mdy|ymd` remains available.

## Report layout

- Overall is now the first aligned row inside System Status instead of a
  separate section.
- System Status and Open Positions retain protected, non-wrapping,
  15-pixel monospaced rendering. Every logical row stays on one line and may
  continue to the right on a narrow mail display.
- Total Open P/L and each account's monthly Realized P/L remain directly below
  the Open P/L column.

## Tracker self-recovery

- The exact verified V147 `ATOnPTracker.dll` fingerprint now supplies the known
  CATPTTabView primary vtable RVA `0x1D78C8` to bounded fresh recovery scans.
- Recovery candidates must also pass structural validation: secondary vtable,
  Tracker layout signature, or at least five credible page pointers.
- The existing 30-second cooldown remains, so a long-lived stale state receives
  new bounded attempts without restarting MCST-Watchdog.

## Saxo authentication alert

- Modern Edge/Chromium browser contents are inspected through bounded Windows
  UI Automation in addition to ordinary child-window text.
- The dedicated `MultiCharts (OpenAPI Web App)` title can identify the known
  Saxo authentication window even when the address bar is not Win32 text.
- The detector stores only the configured matching pattern, never a complete
  OAuth URL, request identifier, token, user ID, or password.

## Installation note

Rebuild and replace `MCST-Watchdog.exe`. Tracker Bridge remains V176 and does
not need replacement when upgrading directly from R26. When upgrading from R25
or older, replace the included `MCST-TrackerBridge.dll` too and restart
MultiCharts. Protocol V2 is unchanged.
