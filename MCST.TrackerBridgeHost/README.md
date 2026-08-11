# MCST Tracker Bridge 1.0

MCST Tracker Bridge is the MultiCharts-side runtime component that provides Order and Position Tracker snapshot data and Tracker compatibility metadata to MCST-Watchdog.

## Product identity

```text
Public product name: MCST Tracker Bridge
Product version:     1.0
DLL file:            MCST-TrackerBridge.dll
Internal build:      V156
Bridge protocol:     V2
Runtime location:    C:\MCExtras\MCST-TrackerBridge.dll
```

The internal V156 identifier describes this Bridge implementation. It is separate from the public product version and separate from the exact MultiCharts module fingerprints used by the compatibility framework. MCST-Watchdog 1.113 requires V156 or newer for production Tracker snapshots.

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

Bridge Protocol V2 remains the stable Watchdog/Bridge boundary. Internal layout discovery and profile selection belong inside the Bridge and do not require a wire-protocol revision when the existing payload format can carry backward-compatible metadata.
