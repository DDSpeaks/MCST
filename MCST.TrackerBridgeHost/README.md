# MCST Tracker Bridge 1.0

MCST Tracker Bridge is the MultiCharts-side runtime component that provides Order and Position Tracker snapshot data and Tracker compatibility metadata to MCST-Watchdog.

## Product identity

```text
Public product name: MCST Tracker Bridge
Product version:     1.0
DLL file:            MCST-TrackerBridge.dll
Internal build:      V180
Bridge protocol:     V2
Runtime location:    C:\MCExtras\MCST-TrackerBridge.dll
```

The internal V180 identifier describes this Bridge implementation. It is separate from the public product version and from exact MultiCharts module fingerprints. Production Tracker snapshots still have a V156 minimum. V175's optional `position_history`, V176's exact verified V147 CATPTTabView vtable RVA `0x1D78C8`, and V179 recovery remain intact. V180 adds a passive dynamic Tracker locator report for changed MC17 structures. Retry backoff is exercised by `MCST-LogicTests.exe`. The retained Position Currency command remains available from V171 onward.

## How the Bridge is loaded

MultiCharts loads `MCST-TrackerBridge.dll` through:

```text
PowerLanguage\MCST_Tracker_Bridge_Host.txt
```

Add the host as an indicator to one chart in each MultiCharts64 instance that owns the Order and Position Tracker monitored by MCST-Watchdog.

A shutdown helper is supplied as:

```text
PowerLanguage\MCST_Tracker_Bridge_Stop.txt
```

Both scripts use:

```text
C:\MCExtras\MCST-TrackerBridge.dll
```

## Relationship to Watchdog

`MCST-Watchdog.exe` does not load this DLL as a normal executable dependency. The DLL executes inside MultiCharts; Watchdog communicates with it through TrackerBridgeClient and Bridge Protocol V2.

## Data boundary

The Bridge provides:

- Accounts
- Open Positions
- Recent Logs
- Extended monitoring-only Logs history (up to 200 rows)
- Tracker/Bridge metadata
- `ATOnPTracker.dll` fingerprint and selected Tracker compatibility information
- passive Developer Mode research operations

## Tracker compatibility database

The Bridge reads:

```text
C:\MCExtras\MCST-Compatibility.ini
```

for each production Tracker snapshot request. An external Tracker profile must exactly match the running `ATOnPTracker.dll` PE timestamp and image size and contain the required verified layout fields.

When no complete profile exists for a newly detected fingerprint, the Bridge can create a disabled `Candidate.ATOnPTracker-*` section. Candidate sections are not production profiles and are never selected automatically.

This design allows a newly verified `Profile.*` section to be activated with a fresh snapshot/Reload Compat rather than recompiling the Bridge.

## Protocol stability

Bridge Protocol V2 remains the stable Watchdog/Bridge boundary. Command 50 was introduced in V157 and is retained unchanged. V173 added optional `monitoring_logs`; V175 similarly added optional `position_history`. Older parsers ignore unknown sections. V179 changed internal recovery behavior, and V180 extends the existing research-bundle command without changing the message header or command set. Internal layout discovery, recovery, profile selection, and bounded table capture stay inside the Bridge.

The recovery path never writes MultiCharts memory, changes Tracker UI state, sends synthetic input, or calls an unknown function. It is attempted at most once per snapshot request and only when a same-process Tracker has an authorized compatibility profile but fewer than all three table sections were read.
