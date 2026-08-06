# MCST-Watchdog 1.091

## UI Polish Build Fix

This maintenance release corrects the GDI+ header integration and removes accidental duplicate GDI+ initialization fragments. All UI changes from 1.09 remain included.


## Dashboard Settings Panels

The AutoTrading, Status Reports, Email, and Heartbeat rows now include compact three-dot menus. Each menu opens a focused settings panel and offers actions relevant to that subsystem.

AutoTrading research actions remain hidden unless Developer Mode is enabled. Alert email and report email recipients remain separate. Settings are stored in `MCST-Watchdog.ini` and reloaded without triggering startup deliveries.

The runtime EXE and DLL files are intended for `C:\MCExtras`. Documentation and source code can be stored anywhere.
