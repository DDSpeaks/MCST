# MCST-src — MCST-Watchdog 0.571 AutoTrading Compatibility Diagnostics

This package contains the first graphical and practically runnable MCST-Watchdog development build with passive AutoTrading monitoring.

## Included

- Visual Studio 2022 solution for x64 Debug and Release
- Existing MCBridge protocol, client and validated Tracker status snapshot reader
- Fixed-layout Win32 Dashboard with stable row positions
- Prominent traffic-light overall status
- Common `WatchdogSystemStatus` model
- Accounts, Open Positions and Recent Logs counters from Bridge V155
- Automatic snapshot refresh with retry logic
- Passive production AutoTrading reader derived from the proven first-generation Watchdog V84/V85 logic
- Dashboard-style text status report
- Raw snapshot diagnostic file
- INI configuration beside the executable
- Process memory, handle count and uptime display
- Recent activity list

## Version 0.571 changes

- Added real AutoTrading strategy counting from MultiCharts processes.
- The reader uses module enumeration, `VirtualQueryEx`, and `ReadProcessMemory` only.
- It performs no UI activation, clicking, input, or process-memory writes.
- AutoTrading is GREEN at or above `minimum_active_strategies`.
- AutoTrading changes directly to RED below the configured minimum; no yellow warning band is used.
- A failed AutoTrading read is shown as UNKNOWN, never as a false zero count.
- `check_interval_minutes` controls how often the heavier memory scan runs; Dashboard refreshes use the cached result between scans.
- Overall health now includes the AutoTrading result.
- Bridge V155 remains fully compatible and unchanged.

## AutoTrading settings

The executable creates these values in `MCST-Watchdog.ini`:

```ini
[AutoTrading]
enabled=true
minimum_active_strategies=65
check_interval_minutes=5
```

The minimum can be edited directly in the INI file. GUI editing will be added later.

## Build

1. Open `MCST.sln` in Visual Studio 2022.
2. Select `Debug | x64`.
3. Build the solution.
4. Set `MCST.Watchdog` as the startup project.

## Runtime requirement

MCTrackerBridge V155 must be running and accessible through its existing named-pipe protocol.

The AutoTrading reader currently uses the verified MC16-era `Charting.dll` strategy-object layout from the first-generation Watchdog. If MultiCharts changes this layout, the AutoTrading row will become UNKNOWN rather than reporting a guessed value.

## Current limitations

Broker, email, scheduled reports and heartbeat are represented in the common status model and Dashboard, but their production implementations have not yet been integrated. They are shown explicitly as `UNKNOWN`, `DISABLED`, or `CONFIGURED`, never as false success states.

## Generated files

The executable creates these beside itself unless paths are overridden in `MCST-Watchdog.ini`:

- `MCST-Watchdog.ini`
- `MCST-Watchdog-StatusReport.txt`
- `MCST-Watchdog-LatestSnapshot.tsv`

## Output artifact names

Visual Studio project and folder names retain dot notation internally, while generated binaries use hyphenated product names:

- `MCST-Watchdog.exe`
- `MCST-TrackerBridge.lib`
- `MCST-Shared.lib`
- `MCST-Tests.exe`


## Version 0.571

AutoTrading monitoring is now completed for the first operational dashboard test: the latest successful AutoTrading read time is shown on the Dashboard and in the status report, and the Refresh button forces a fresh AutoTrading scan instead of returning a cached value. Automatic scans still use the configured cache interval. The operational rule remains binary: green at or above the configured minimum, red below it, and gray when the read is unavailable.


## AutoTrading compatibility diagnostics

Use the **AT Diagnostics** button after starting MultiCharts. The scan is passive and may take some time. It writes and opens:

`C:\Temp\MCST-Watchdog-AutoTrading-Compatibility-0.571.txt`

The report lists the new Charting.dll identity, old signature results, repeated Charting.dll pointer candidates and possible mixed boolean offsets. Send the report back for analysis. Candidate values are diagnostic only and are not used automatically for production AutoTrading status.
