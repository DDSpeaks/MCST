# MCST Developer Guide


## Beginner Developer Help build

MCST-Watchdog 1.20.11 retains R33's two-pane Win32 topic selector backed by
`DeveloperHelpContent` and adds a persistent operating-boundary notice. It
states that these controls are normally used only after a MultiCharts update,
after an exact Charting.dll or ATOnPTracker.dll fingerprint change, or at a
developer's explicit request. Normal monitoring does not require them. The
notice appears both above the topic selector and inside every topic so it
cannot be missed when a user skips Overview. Each Developer action has
Goal, Purpose, When, Preparation, Action, Success, Next step, Failure, and
Safety sections. The same content is linked into `MCST-LogicTests.exe`, which
rejects a missing topic or missing required guidance.

The AT Capture contract is intentionally specific: on one chart, select one
strategy and change only its AutoTrading state from ON to OFF or OFF to ON.
The chart, workspace, strategy settings, and running MultiCharts process set
must stay unchanged between snapshots. The Watchdog records the state; it does
not perform the user's AutoTrading change.

## Status Report alignment retained

MCST-Watchdog 1.20.11 places `MultiCharts Health` first, uses 30-pixel compact status-row spacing, and separates Developer tools into a labelled read-only diagnostics panel with an 18-pixel gap above the production controls. Latest Activity uses uniform 20-pixel text rows and, when Developer mode is off, fills the available space above the production buttons with as many complete entries as fit from the existing bounded ten-event history. Every additional row follows the original summary-row order: event description at left and full local timestamp in the adjacent value column. The timestamp and detail columns share the exact starting coordinates used by the System Status state and description columns. Because activity events have no separate detail field, the description is repeated in the wide right-hand detail column so long text remains readable. Activity history is merged by exact timestamp, state, and text identity, preventing refresh-time duplication while retaining genuine concurrent events. Activity rows use no health-state indicators or System Status styling. When Developer mode is on, the original three-row summary remains compact. It retains R30's emphasized `OVERALL STATUS`, protected 15-pixel preformatted System Status and Open Positions flows, and fixed outer `ch` widths. It retains R32's enlarged report component status dots at `1.5em`, the `2em` Overall dot, and the fixed dot column. Every status and position row remains one line and may continue to the right without iOS Mail changing the column geometry.

Version 1.20.11 retains optional Position History and Protocol V2. Realized P/L aggregation is keyed by the account values currently present in `snapshot.accounts`; history rows for all other accounts are discarded before date inference or arithmetic. Empty Open Positions sections use `(no rows)`, matching Accounts and Recent Logs, before any monthly Realized P/L rows. Bridge V179 validates the latest object hint, searches remembered allocations in blocks, and performs a bounded process-wide RTTI fallback after a targeted miss. BrokerMonitor and LogAlertEngine use only current live monitoring history; reports continue to show ten log rows.

The Broker regression sequence is newest-first: many unrelated UIC warnings, a successful `Connection to Saxo Group has been established` row, and an older `No connection to Saxo Group trading system` row. BrokerMonitor processes oldest-to-newest and must finish Connected because the successful connection is the newest Broker-state evidence.

Recovery remains read-only. Bridge first revalidates recent object hints, then scans the remembered allocator neighborhood with a 100 ms / 8 MiB cap. A miss triggers a process-wide RTTI fallback bounded to 3 seconds / 512 MiB. Retries use a 30-second cooldown initially, 60 seconds after three failures, and five minutes after ten failures. The exact V147 fingerprint supplies vtable RVA `0x1D78C8`; accepted candidates still require strong structural evidence. Bridge does not write MultiCharts memory, manipulate Tracker windows, send synthetic input, or call an unknown target function.

If recovery is not immediate, Watchdog retains the last complete Tracker table snapshot for operational context. Tracker health is Attention during `stale_critical_after_minutes` (10 by default), then Critical. The Dashboard/status report show both the last Tracker attempt and the last complete snapshot. BrokerMonitor and LogAlertEngine always receive the current live Recent Logs result, never the retained stale rows.

R46 retains the nine-column order and all R30 report formatting. Total Open P/L and each visible account's current-month Realized P/L place their labels in the Symbol column and amounts in the Open P/L column. Their third/comment cells remain empty so row counts and currency explanations cannot widen the report. There is no combined monthly account total.

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

GitHub Actions performs this same MSVC build on `windows-2022`, runs
`MCST-LogicTests.exe`, and invokes `Tools\Build-PortableRelease.ps1`. A manual
workflow run uploads a temporary artifact; a matching version tag publishes a
GitHub Release. See `GITHUB_RELEASES.md` for the release-maintainer workflow.

## Project roles

### MCST.Watchdog

The production dashboard and monitoring coordinator. It owns configuration normalization, AutoTrading monitoring, broker/log state, scheduling, email, status reporting, system resources, MultiCharts version detection, and user-facing compatibility diagnostics.

### MCST.TrackerBridgeHost

Builds `MCST-TrackerBridge.dll`, which runs inside MultiCharts. It locates and reads Order and Position Tracker data and exposes snapshots/research operations through the Bridge boundary.

Current identity:

```text
Product version: 1.0
Internal build:  V179
Protocol:        V2
```

V156 added Tracker compatibility profiles, V171 added the retained R16 position-interface verification, V172 added bounded Bridge-local cache refresh, V173 added extended monitoring history, V174 made recovery failure non-sticky, V175 added optional Position History, V176 added targeted structural recovery, V177 added progressive diagnostic recovery, V178 shared its tier policy with LogicTests, and V179 adds validated-hint, optimized targeted, and bounded process-wide fallback recovery without changing Protocol V2.

### MCST.TrackerBridge

Watchdog-side Bridge client and Tracker snapshot parser.

### MCST.Shared

Shared protocol and status types used across projects.

### MCST.LogicTests

`MCST-LogicTests.exe` is the Release-build regression executable for logic that can be tested independently from a live MultiCharts process. It returns exit code `0` only when every test passes and prints the failing regression plus exit code `1` otherwise.

Current coverage includes report layout and coloring, Open and Realized P/L aggregation, localized Tracker dates, stale-data escalation, Saxo authentication and broker-history decisions, and the shared V179 recovery budgets and retry backoff policy. It deliberately does not connect to or manipulate a running MultiCharts process. A future live integration-test executable should remain separate because it depends on actual windows, modules, Bridge IPC, and runtime state.

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

The intended trigger is an unrecognized exact module fingerprint or a specific
developer investigation request. A visible MultiCharts product-version change
is the common case, but fingerprint authorization remains authoritative: an
updated module can require research even if the marketing version string does
not visibly change.

The main window exposes `[Developer] enabled` as a persistent **Developer
mode** checkbox immediately to the right of Reload Settings. The normalized
default remains false. The research toolbar uses a 22-pixel button height and
8-point labels, versus the normal production row's 34-pixel buttons.

Current controls are `AT Start`, `AT Capture`, `AT Finish`, `Tracker Capture`, `Position CCY`, `Open Compat`, `Reload Compat`, and `Help`.

### Developer control contract

- `AT Start` creates/replaces the controlled AutoTrading research baseline.
- `AT Capture` appends a settled post-change state. Before each capture the
  user changes only one strategy's AutoTrading ON/OFF state on one chart;
  multiple controlled captures are expected.
- `AT Finish` analyzes the active session and writes `C:\Temp\MCST-Watchdog\AutoTradingResearch.txt`.
- `Tracker Capture` requests the Bridge's passive read-only Tracker research bundle.
- `Position CCY` runs the retained optional currency research against representative visible positions.
- `Open Compat` opens the shared compatibility database but never validates or enables a candidate.
- `Reload Compat` invalidates the Watchdog compatibility/read cache and requests fresh AutoTrading and Tracker evaluation. It cannot reload a DLL already hosted by MultiCharts.
- `Help` opens a two-pane, selectable, non-mutating beginner guide containing
  the purpose, preparation, output, recovery, and safety rules for every action.

The AutoTrading research sequence is `AT Start`, change only the AutoTrading
ON/OFF state of one strategy on one chart, then `AT Capture`; repeat that exact
pair as needed and finally press `AT Finish`. These controls do not enable or
disable production AutoTrading.

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
