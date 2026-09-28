# MCST-Watchdog portable Windows package

This package is for a user who wants to run MCST without installing Visual
Studio or compiling the source code. It contains the Release x64 Watchdog,
Tracker Bridge, PowerLanguage host files, and these instructions.

MCST is distributed under the MIT License included as `LICENSE` in this
package. Copyright (c) 2026 Mika Tättäläinen. MultiCharts, Saxo, Windows, and
other third-party products remain subject to their own terms.

## New installation

1. Close MCST-Watchdog and MultiCharts.
2. Create `C:\MCExtras` if it does not exist.
3. Copy the contents of the package's `MCExtras` folder to `C:\MCExtras`.
4. In MultiCharts PowerLanguage, import or create the studies from the two text
   files in the package's `PowerLanguage` folder.
5. Add `MCST_Tracker_Bridge_Host` to one chart in each MultiCharts64 instance
   whose Order and Position Tracker MCST should monitor.
6. Start MultiCharts and confirm the host study is active.
7. Start `C:\MCExtras\MCST-Watchdog.exe`.

MCST writes missing safe defaults to `MCST-Watchdog.ini` on first start. Email
addresses, SMTP credentials, account identifiers, and other user-specific
values must be supplied by the user when required.

The package's `Examples` folder contains inert reference templates ending in
`.ini.example`. They are never loaded under those names and cannot overwrite
an active configuration. `MCST-Watchdog.ini.example` is a conservative starter
reference with email sending disabled. `MCST-Compatibility.ini.example` is a
disabled schema example only; never enable its placeholder candidate or copy
unverified addresses into production.

## Updating an existing installation

1. Keep your existing `MCST-Watchdog.ini` and `MCST-Compatibility.ini` files.
2. Close MCST-Watchdog.
3. Replace `C:\MCExtras\MCST-Watchdog.exe` with the packaged EXE.
4. If you also replace `MCST-TrackerBridge.dll`, close and restart MultiCharts;
   restarting Watchdog alone cannot reload a DLL already loaded by MultiCharts.

The package intentionally contains no active INI files, so installing it cannot
overwrite the user's settings or credentials. Version 1.20.17 includes Tracker Bridge V181
and retains Protocol V2. Replace both the Watchdog executable and Bridge DLL, then restart
MultiCharts so V181 is loaded into its process.

## Package verification

The GitHub Release includes a `SHA256SUMS.txt` file. In PowerShell, verify the
downloaded ZIP with:

```powershell
Get-FileHash .\MCST-Watchdog-1.20.17-Windows-x64.zip -Algorithm SHA256
```

Compare the displayed hash with the value in the checksum file before
extracting the package.
