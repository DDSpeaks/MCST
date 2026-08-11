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
Internal build:  V156
Protocol:        V2
```

The internal Bridge build is not a MultiCharts compatibility fingerprint. V156 adds profile-driven Tracker compatibility handling while keeping the wire protocol at V2.

### MCST-Watchdog

Watchdog combines Tracker snapshots, AutoTrading state, broker/log monitoring, scheduled reporting, email, resources, and compatibility diagnostics into one operational state.

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
