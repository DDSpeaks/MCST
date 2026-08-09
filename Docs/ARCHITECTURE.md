# MCST Architecture

## Purpose

MCST-Watchdog provides an operational view of MultiCharts health at a glance while keeping invasive research functions separate from normal production monitoring.

## System overview

```text
MultiCharts64
    |
    | PowerLanguage host
    v
MCST-TrackerBridge.dll
    |
    | Bridge Protocol V2
    v
MCST TrackerBridgeClient
    |
    +-- Accounts
    +-- Open Positions
    +-- Recent Logs
    +-- Tracker metadata
    |
    v
MCST-Watchdog
    |
    +-- Broker State Engine
    +-- Broker Authentication Detector
    +-- Log Alert Engine
    +-- AutoTrading Reader
    +-- MultiCharts Version Detector
    +-- Compatibility Manager
    +-- Schedule Service
    +-- Email / Alert Service
    +-- Resource Monitor
    |
    +--> Dashboard
    +--> Status Reports
    +--> Alerts
    +--> Heartbeats
```

## Component boundaries

### MultiCharts64

The monitored trading platform. One or more MultiCharts instances may be active.

### MCST Tracker Bridge 1.0

`MCST-TrackerBridge.dll` runs inside MultiCharts and exposes Tracker snapshot information through Bridge Protocol V2. It is loaded by the supplied PowerLanguage host.

Identifiers:

```text
Product version: 1.0
Internal build:  V155
Protocol:        V2
```

The internal V155 build identifier is a Tracker Bridge implementation identifier. It is not a MultiCharts compatibility fingerprint.

### TrackerBridgeClient

The Watchdog-side static client library that requests and parses Bridge snapshots.

### MCST-Watchdog

Combines independent monitor inputs into the Dashboard and report state. Subsystems should preserve uncertainty rather than converting missing evidence into a false healthy state.

## Bridge stability rule

Bridge Protocol V2 should remain stable unless new information must cross the MultiCharts/Watchdog boundary. A Watchdog-only feature should not cause a Bridge protocol change.

## MultiCharts version and internal compatibility

The architecture deliberately separates the visible MultiCharts version from internal-memory compatibility.

### MultiCharts Version Detector

Reads the running MultiCharts executable path and Windows version resource. The result is useful to users, Status Reports, and diagnostics and is written to `[DetectedMultiCharts]`.

### Compatibility Manager

Selects a verified profile only when the internal module fingerprint matches exactly. The current AutoTrading profile uses:

- `Charting.dll` PE timestamp
- `Charting.dll` image size

A profile currently supplies the build-dependent vtable and field offset needed by the AutoTrading reader.

The compatibility architecture is intentionally broader than AutoTrading. Any future production reader that depends on MultiCharts internal layout must obtain its build-dependent values from a verified profile or equivalent exact build authorization. A new product-version string alone must never authorize old offsets.

## Fail-safe state model

For internal readers:

```text
fingerprint known + profile verified -> reader may operate
fingerprint unknown                -> reader reports UNKNOWN
required profile value missing     -> affected reader reports UNKNOWN
```

This policy prevents an apparently plausible but incorrect value from being treated as production truth.

## AutoTrading Reader

The AutoTrading reader performs passive process-memory inspection. It does not write to MultiCharts memory. Research and production modes are separate: research can generate candidate evidence, while production requires a verified compatibility profile.

## Broker State Engine

Broker state combines multiple types of evidence. Current evidence is preferred over stale historical evidence.

Priority is conceptually:

1. configured current broker-authentication/login evidence;
2. explicit Recent Logs connection/disconnection evidence;
3. reconnect grace-timer state;
4. `UNKNOWN` when reliable evidence is unavailable.

A confirmed authentication/login page therefore cannot be cleared merely by an older historical success log.

## Log Alert Engine

Recent Logs alert classification is independent of Broker connection state. Keyword matches can generate Fatal, Critical, or Warning events without changing the Broker State Engine rules.

## Scheduling and email

Status Reports and Heartbeats use the report channel. Operational alerts use the alert channel. Scheduling logic maintains independent state so a report operation does not suppress heartbeat or alert delivery.

## Status Report rendering

Scheduled Status Reports, Heartbeats, and alert messages that embed system status share the same HTML Status Report renderer. This avoids layout drift between message types.

## Configuration architecture

`MCST-Watchdog.ini` follows a self-documenting configuration model. Known missing keys are materialized with safe defaults. Invalid bounded values are normalized. User-specific values and secrets are not fabricated.

`[DetectedMultiCharts]` is program-generated diagnostic output rather than configuration input.

`MCST-Compatibility.ini` is separate because verified internal build data has different trust requirements from normal user settings. A verified profile must never be created by guessing or ordinary default normalization.

## Developer and research tools

Developer Mode is hidden by default. Its compact toolbar is intentionally separated from production controls and is designed to accept additional compatibility-research actions as new MC-internal readers are developed.

The Universal Application Mapper remains a separate application-independent developer tool rather than a Watchdog runtime dependency.
