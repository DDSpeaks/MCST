# MCST Tracker Bridge 1.0

MCST Tracker Bridge is the MultiCharts-side runtime component that provides Order and Position Tracker snapshot data to MCST-Watchdog.

## Product identity

```text
Public product name: MCST Tracker Bridge
Product version:     1.0
DLL file:            MCST-TrackerBridge.dll
Internal build:      V155
Bridge protocol:     V2
Runtime location:    C:\MCExtras\MCST-TrackerBridge.dll
```

The internal V155 build identifier describes this Bridge implementation. It is separate from the public product version and separate from MultiCharts internal compatibility fingerprints used by Watchdog.

## How the Bridge is loaded

MultiCharts loads `MCST-TrackerBridge.dll` through the supplied PowerLanguage host:

```text
PowerLanguage\MCST_Tracker_Bridge_Host.txt
```

Add the host as an indicator to one chart in each MultiCharts64 instance that owns the Order and Position Tracker monitored by MCST-Watchdog.

The host initializes the Bridge and maintains its heartbeat. A shutdown helper is supplied as:

```text
PowerLanguage\MCST_Tracker_Bridge_Stop.txt
```

Both scripts reference:

```text
C:\MCExtras\MCST-TrackerBridge.dll
```

## Relationship to Watchdog

`MCST-Watchdog.exe` does not load this DLL as a normal executable dependency. The DLL executes inside MultiCharts; Watchdog uses the separate TrackerBridgeClient to communicate through Bridge Protocol V2.

## Data boundary

The Bridge provides the stable Tracker snapshot boundary used for information such as:

- Accounts
- Open Positions
- Recent Logs
- Tracker/Bridge metadata

Keep Bridge Protocol V2 stable unless information crossing this boundary must change. Watchdog-only features should not require a Bridge protocol revision.
