# MCST Architecture

## Purpose

MCST-Watchdog provides an operational view of MultiCharts health at a glance while keeping build-dependent MultiCharts internals behind explicit compatibility controls.

## Runtime data flow

```text
MultiCharts64
    |
    | PowerLanguage host
    v
MCST-TrackerBridge.dll
    |
    | Bridge Protocol V2
    v
TrackerBridgeClient
    |
    +-- Accounts
    +-- Open Positions
    +-- Recent Logs
    +-- Bridge / Tracker compatibility metadata
            |
            v
       MCST-Watchdog
            |
            +-- MultiCharts Health Monitor
            +-- Broker State Engine
            +-- Log Alert Engine
            +-- AutoTrading Reader
            +-- MultiCharts Version Detector
            +-- Compatibility Manager
            +-- Email / Alert Service
            +-- Scheduler
            +-- Resource Monitor
            |
            v
       Dashboard / Status Reports / Alerts
```

## Main components

### MultiCharts

The monitored trading platform. MCST does not modify MultiCharts process memory.

### MCST Tracker Bridge

The Bridge runs inside MultiCharts and provides a stable Tracker snapshot boundary.

```text
Product version: 1.0
Internal build:  V180
Protocol:        V2
```

The internal Bridge build is not a MultiCharts compatibility fingerprint. V156 introduced profile-driven Tracker compatibility handling, V171 added fingerprint-scoped currency verification, V172 added bounded Tracker recovery, V173 added monitoring-only Logs history, V174 made failed recovery non-sticky, V175 added optional Position History capture, V176 added fingerprint-scoped structural recovery, V177 added progressive diagnostic recovery, V178 shared its tier-selection policy with LogicTests, V179 added validated-hint and process-wide fallback recovery, and V180 adds passive dynamic Tracker-locator evidence for changed MC17 structures. Protocol V2 remains unchanged.

Recovery is deliberately read-only. V179 validates the most recent object hint first, scans remembered allocation neighborhoods in 256 KiB blocks, and runs a time/byte-bounded process-wide RTTI fallback after a targeted miss. Every candidate must match the exact verified profile vtable and the retained structural acceptance rules. Failed recovery attempts are spaced at 30 seconds initially, 60 seconds after three failures, and five minutes after ten failures. Each stage logs candidates, scores, limits, elapsed time, and its decision. V176's exact V147 vtable RVA `0x1D78C8` remains unchanged.

If live data is still incomplete, the last complete tables may be displayed with an explicit stale timestamp. A stale snapshot remains Attention during the configured grace period (10 minutes by default) and escalates to Critical afterwards; a wholly unreadable current snapshot with no last-good data is immediately Critical. Stale Recent Logs are never sent to broker or log-alert evaluation.

R21 reads the Logs grid only once per snapshot. The first ten rows cross the established `recent_logs` display boundary, while up to 200 rows are also supplied in the optional `monitoring_logs` section. State engines consume the latter so unrelated warning traffic cannot hide a slightly older Broker connection event. Reports still render only the ten-row display section.

V175 and later also supply optional `position_history` rows. This fourth table is not
part of `pages_ok`, `pages_failed`, core SEH totals, or the three-section Tracker
health decision. Watchdog uses it only to recalculate current-month,
known-currency Realized P/L. A missing or failed history section is reported as
unavailable instead of changing Broker/Tracker health.

### MCST-Watchdog

Watchdog combines Tracker snapshots, AutoTrading state, broker/log monitoring, scheduled reporting, email, resources, and compatibility diagnostics into one operational state.

`MultiChartsHealthMonitor` independently enumerates all running
`MultiCharts.exe` and `MultiCharts64.exe` processes. During the existing refresh
cycle it samples process resources, sends a bounded non-mutating `WM_NULL`
responsiveness probe to each main window, and reads the visible `q / s` backlog
from status controls. It never clicks, types, places orders, or modifies process
memory. A process-set change invalidates the cached aggregate AutoTrading count
once, allowing the existing minimum-active rule to determine whether a vanished
instance carried trading strategies.

### Developer and research tools

Research tools are deliberately separated from normal production operation. Developer Mode is used to discover and verify values for a new MultiCharts build before those values become production profile data.

## Compatibility architecture

The shared database is:

```text
C:\MCExtras\MCST-Compatibility.ini
```

Two module fingerprints are currently important:

```text
Charting.dll
    -> AutoTrading build-dependent profile values

ATOnPTracker.dll
    -> Tracker build-dependent profile values
```

Watchdog and Bridge consume the same database independently:

```text
                 MCST-Compatibility.ini
                       /       \
                      /         \
                     v           v
             MCST-Watchdog   Tracker Bridge
                  |              |
             Charting.dll   ATOnPTracker.dll
                  |              |
             AutoTrading       Tracker
```

A human-readable MultiCharts version helps identify the installed release, but it does not authorize internal memory access. Exact module fingerprints and verified profile values provide that authorization.

Developer Mode is not part of the normal monitoring path. Its research tools
are normally entered only when an update changes an exact module fingerprint
and no verified profile matches, or when a developer explicitly requests a
controlled evidence capture.

## Unknown-build policy

The compatibility policy is fail-safe:

```text
fingerprint changed
        |
        v
exact verified profile available? ---- yes ---> use verified values
        |
        no
        v
affected reader UNKNOWN / unavailable
        |
        v
Developer Mode research
        |
        v
verified Profile.* added
        |
        v
Reload Compat / fresh snapshot
```

For an unknown `ATOnPTracker.dll`, the Bridge can create a disabled `Candidate.*` section that records the fingerprint. A candidate is not selected by production code and contains no fabricated verified offsets.

## Bridge stability rule

Bridge Protocol V2 should remain stable unless the information crossing the Bridge boundary fundamentally requires a protocol revision. Adding internal metadata fields that existing parsers safely ignore, or changing how the Bridge resolves its own internal layout, does not by itself require a protocol change.

Watchdog-only features should not force a Bridge protocol revision.

## Tracker snapshot safety

For an externally verified Tracker profile, the production reader checks the configured CATPTTabView/CFlexGrid identities and layout values before calling the grid text reader. Failures are reported as specific diagnostics such as:

- `ATOnPTracker.dll is not loaded.`
- no verified Tracker profile for the exact fingerprint;
- CATPTTabView not found for the selected profile;
- Tracker profile layout read failure;
- verified FlexGrid identity mismatch.

This distinguishes compatibility failure from an actual critical Recent Logs message.

## Broker authentication signal priority

The Broker State Engine prefers current evidence over stale historical evidence:

1. configured browser authentication/login evidence;
2. explicit Recent Logs disconnect/reconnect events;
3. reconnect grace-period state;
4. unknown when no reliable evidence is available.

A confirmed login page cannot be cleared merely by an older successful connection log.

## Configuration architecture

`MCST-Watchdog.ini` is self-documenting for normal settings: known missing settings are written with safe defaults and bounded values are normalized. Generated diagnostics and verified compatibility data are treated separately so the program never invents secrets or verified internal addresses.
