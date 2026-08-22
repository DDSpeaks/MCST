# MCST Developer Guide


## Status Report alignment build

MCST-Watchdog 1.114-R30 renders emphasized `OVERALL STATUS` as the first row of System Status and keeps System Status and Open Positions in protected 15-pixel preformatted HTML flows. It does not use semantic status tables, the R22 fixed-width position table, or the R23 multi-line cards. Every status and position row remains one line and may continue to the right. System Status Name, Dot, State, and Value outer segments use normal-size explicit `ch` widths. Overall bolding and double-size dot styling are applied only to nested content, so neither emphasis changes the physical column width in iOS Mail.

R30 retains R26's optional Position History and Protocol V2. Realized P/L aggregation is keyed by the account values currently present in `snapshot.accounts`; history rows for all other accounts are discarded before date inference or arithmetic. Empty Open Positions sections use `(no rows)`, matching Accounts and Recent Logs, before any monthly Realized P/L rows. Bridge V176 adds a fingerprint-scoped CATPTTabView vtable anchor and structural candidate validation to the existing non-sticky recovery. BrokerMonitor and LogAlertEngine use only current live monitoring history; reports continue to show ten log rows.

The Broker regression sequence is newest-first: many unrelated UIC warnings, a successful `Connection to Saxo Group has been established` row, and an older `No connection to Saxo Group trading system` row. BrokerMonitor processes oldest-to-newest and must finish Connected because the successful connection is the newest Broker-state evidence.

The bounded recovery remains read-only. Bridge clears only its own CATPTTabView discovery caches, takes a fresh snapshot, and rescans. V176 retains the scope-bound recovery state and 30-second cache/cooldown. The exact V147 fingerprint supplies vtable RVA `0x1D78C8`; accepted candidates must also carry a secondary vtable, Tracker layout signature, or at least five credible page pointers. Bridge does not write MultiCharts memory, manipulate Tracker windows, send synthetic input, or call an unknown target function.

If recovery is not immediate, Watchdog retains the last complete Tracker table snapshot for operational context. Tracker health is Attention during `stale_critical_after_minutes` (10 by default), then Critical. The Dashboard/status report show both the last Tracker attempt and the last complete snapshot. BrokerMonitor and LogAlertEngine always receive the current live Recent Logs result, never the retained stale rows.

R30 retains the nine-column order: Symbol, Open P/L, Side, Qty, Average Price, Native Value, Account, Profile, and Last Update. Total Open P/L and each visible account's current-month Realized P/L place their labels in the Symbol column and amounts in the Open P/L column. There is no combined monthly account total. Padding outside each colored P/L span is preserved to keep the monospaced alignment exact.

Identity and Latest Activity rows calculate one shared label width per section.
System Status calculates common Name, State, Value, and Detail widths. Accounts
cells are trimmed before width calculation; columns containing only localized
numbers are right-aligned, while account identifiers and timestamps remain
left-aligned.

`BrokerAuthDetector.cpp` deliberately does not define `WIN32_LEAN_AND_MEAN`.
It includes `ole2.h` and `oleauto.h` before `UIAutomation.h` so Windows SDK
10.0.26100 sees the COM base declarations before UI Automation provider and
client interfaces.

`TrackerDateParser` accepts numeric DMY, MDY, and YMD dates and validates real
calendar days. With `date_order=auto`, snapshot-wide unambiguous evidence is
used first and the Windows short-date order is the fallback. The parser ignores
time text. A contradiction or invalid date is excluded rather than coerced.
The monthly total is recomputed from the current snapshot; no refresh-to-refresh
accumulator is maintained.

The earlier R16 `Position CCY` Developer action is retained. Bridge V171 or newer verifies the exact ATCenterProxy position vtable and machine-code signatures discovered by R15, finds its live objects, correlates Quantity and Average Price with the visible rows, and decodes the bounded currency strings at object offsets `+0x308` and `+0x328`. No candidate target is called. R20 production reporting does not depend on running this research action.

Use at least two simultaneously open positions with different native currencies when practical. See `POSITION_CURRENCY_RESEARCH.md` for the capture files and analysis workflow.

## Build environment

- Visual Studio 2022
- Platform toolset v143
- Windows SDK 10
- C++17
- Release x64
- Static C/C++ runtime linkage (`/MT`)

Open `MCST.sln` and build `Release|x64`. The public production source package intentionally contains no Debug solution configuration.

## Project roles

### MCST.Watchdog

The production dashboard and monitoring coordinator. It owns configuration normalization, AutoTrading monitoring, broker/log state, scheduling, email, status reporting, system resources, MultiCharts version detection, and user-facing compatibility diagnostics.

### MCST.TrackerBridgeHost

Builds `MCST-TrackerBridge.dll`, which runs inside MultiCharts. It locates and reads Order and Position Tracker data and exposes snapshots/research operations through the Bridge boundary.

Current identity:

```text
Product version: 1.0
Internal build:  V176
Protocol:        V2
```

V156 added Tracker compatibility profiles, V171 added the retained R16 position-interface verification, V172 added bounded Bridge-local cache refresh, V173 added extended monitoring history, V174 made recovery failure non-sticky, V175 added optional Position History, and V176 adds targeted structural recovery without changing Protocol V2.

### MCST.TrackerBridge

Watchdog-side Bridge client and Tracker snapshot parser.

### MCST.Shared

Shared protocol and status types used across projects.

### MCST.Tests

Release-build test executable for logic that can be tested independently from a live MultiCharts process.

## PowerLanguage host

MultiCharts loads the Bridge from:

```text
C:\MCExtras\MCST-TrackerBridge.dll
```

through `MCST_Tracker_Bridge_Host.txt`. Watchdog does not link to the Bridge DLL as a normal runtime dependency.

## Developer Mode

Developer Mode is controlled by:

```ini
[Developer]
enabled=false
```

Research controls are hidden in production mode. The compact toolbar is deliberately centralized in `DashboardLayout` so additional compatibility tools can be added without consuming the normal production button row.

Current controls are `AT Start`, `AT Capture`, `AT Finish`, `Tracker Capture`, `Open Compat`, and `Reload Compat`.

## MultiCharts internal-read safety

Production readers may read MultiCharts memory but must not write to MultiCharts process memory. Unknown build-dependent layouts must fail safely rather than reuse stale offsets.

Human-readable MultiCharts version strings are diagnostic metadata. Internal-read authorization is based on exact module fingerprints and verified profile data.

## Shared compatibility database

Both Watchdog and the Bridge use:

```text
C:\MCExtras\MCST-Compatibility.ini
```

The consumers are independent:

- Watchdog matches `Charting.dll` fields for AutoTrading.
- Bridge matches `ATOnPTracker.dll` fields for Tracker layout.

A `Profile.*` section may contain fields for one subsystem or both. Each consumer ignores profiles that do not contain the fields it requires.

### AutoTrading fields

```ini
charting_pe_timestamp=0x...
charting_image_size=...
strategy_vtable_rva=0x...
autotrading_offset=0x...
```

### Tracker fields

```ini
atonptracker_pe_timestamp=0x...
atonptracker_image_size=...
tracker_tabview_vtable_rva=0x...
tracker_accounts_page_offset=0x...
tracker_open_positions_page_offset=0x...
tracker_position_history_page_offset=0x...
tracker_logs_page_offset=0x...
tracker_grid_member_offset=0x...
tracker_rows_offset_1=0x...
tracker_rows_offset_2=0x...
tracker_gettext_slot=...
tracker_flexgrid_vtable_rva=0x...
tracker_gettext_rva=0x...
```

`tracker_position_history_page_offset` is optional. Without it the verified
core profile remains valid, but monthly Realized P/L is unavailable.

Optional research metadata may also include:

```ini
tracker_accounts_extractor_rva=0x...
tracker_open_positions_extractor_rva=0x...
```

The production Tracker snapshot path is profile-driven for external verified builds. Historical research probes may still contain explicitly labelled research constants; those values must not silently become authorization for a new build.

## Candidate profiles

When the Bridge sees an `ATOnPTracker.dll` fingerprint for which no complete verified Tracker profile is available, it may create:

```ini
[Candidate.ATOnPTracker-...]
candidate_created=true
enabled=false
name=Unverified ATOnPTracker build
atonptracker_pe_timestamp=0x...
atonptracker_image_size=...
tracker_tabview_vtable_rva=
...
verification=UNVERIFIED - Developer Mode research required before creating an enabled Profile.* section
```

Candidate sections are deliberately excluded from profile selection. They are a self-documenting research starting point, not a shortcut to production authorization.

## Tracker verification workflow

1. Record the exact `ATOnPTracker.dll` fingerprint.
2. Capture a passive Tracker research bundle.
3. Identify the CATPTTabView and CFlexGridImpl identities for the exact build.
4. Determine page, grid, row-counter, GetText slot, and required RVA values.
5. Verify the values across controlled Tracker states and all required pages: Accounts, Open Positions, and Recent Logs.
6. Create an enabled `Profile.*` section with the exact fingerprint and verified values.
7. Record a meaningful `verification=` note.
8. Use **Reload Compat** or request a fresh snapshot.
9. Confirm the Bridge reports `tracker_compatibility_matched=true` and the expected profile name.
10. Confirm all three Tracker sections read successfully without SEH failures or identity mismatches.

Do not promote a candidate based only on a plausible address or a single successful read.

## Runtime reload behavior

The Bridge resolves the Tracker profile from `MCST-Compatibility.ini` for every production snapshot request. Watchdog's **Reload Compat** action also forces AutoTrading to bypass its compatibility/read cache. A newly verified profile can therefore be activated without recompiling MCST.

If the Bridge DLL itself has been replaced, restart MultiCharts so the new DLL build is loaded.

## Self-documenting configuration

Normal settings belong in centralized normalization with safe defaults. Missing known keys are written to `MCST-Watchdog.ini`, making the file a version-specific configuration reference.

Exceptions are deliberate:

- user-specific values and secrets are not invented;
- generated diagnostic sections are program-owned output;
- verified compatibility values are never fabricated as defaults.

## Secret handling

`smtp_password` is the provider-required SMTP credential and may be an App Password. Never include SMTP passwords, App Passwords, tokens, or similar secrets in logs, diagnostics, research bundles, reports, or error text.

## Release validation

Run:

```powershell
.\Tools\Validate-Release.ps1
```

before packaging. Static validation cannot replace a real Windows/MSVC rebuild and runtime test.
