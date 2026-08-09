# MCST Developer Guide

## Build environment

- Visual Studio 2022
- Platform toolset v143
- Windows SDK 10
- C++17
- Release x64
- static C/C++ runtime linkage (`/MT`)

Open `MCST.sln` and build `Release|x64`. The production source tree intentionally contains no Debug solution configuration.

## Project responsibilities

### MCST.Watchdog

The production monitoring application. It owns the Dashboard, configuration, scheduling, email, alerts, Broker state, log alerts, resources, AutoTrading reading, MultiCharts version detection, and compatibility selection.

### MCST.TrackerBridge

The Watchdog-side static client library for reading Tracker Bridge snapshots.

### MCST.TrackerBridgeHost

Builds `MCST-TrackerBridge.dll`, which is loaded into MultiCharts by the PowerLanguage host.

Public product identity:

```text
MCST Tracker Bridge 1.0
```

Technical identifiers:

```text
Internal bridge build: V155
Bridge protocol:       V2
```

These identifiers serve different purposes and must not be conflated.

### MCST.Shared

Shared protocol and system-status definitions used across components.

### MCST.Tests

Foundation smoke tests and compile-time protocol checks.

## Bridge architecture and stability

The PowerLanguage host loads:

```text
C:\MCExtras\MCST-TrackerBridge.dll
```

The Bridge runs inside MultiCharts. Watchdog communicates with it through Bridge Protocol V2; Watchdog does not depend on the Bridge DLL as a normal process-loaded runtime DLL.

Keep the bridge protocol stable unless information truly must cross the Bridge boundary. Features that can be implemented entirely in Watchdog must not force a protocol change.

## Developer Mode

Developer Mode is controlled by:

```ini
[Developer]
enabled=false
```

Research controls remain hidden in normal production operation. When enabled, they use a compact low-height toolbar distinct from production controls. `DashboardLayout` owns the compact Developer toolbar geometry so additional compatibility-research actions can be added without redesigning the normal button row.

## MultiCharts internal compatibility framework

Treat every MultiCharts-internal address or layout assumption as build-dependent unless it has been demonstrated otherwise.

The framework separates two concepts:

1. **Human-readable version detection** — product/file version of the running MultiCharts executable. Useful for UI and diagnostics.
2. **Internal compatibility fingerprint** — exact module identity used to authorize a verified internal-memory profile.

The current production fingerprint for AutoTrading is based on `Charting.dll` PE timestamp and image size.

The current production profile contains the verified values required by the AutoTrading reader:

- strategy vtable RVA
- AutoTrading field offset

The architecture is intentionally general. If another production reader later needs MC-build-dependent vtables, RVAs, structure offsets, interface signatures, or semantic locators, those values must be represented in a verified build profile before that reader uses them. Do not silently keep an old value after a fingerprint changes.

## Unknown-build policy

Production behavior is fail-safe:

```text
unknown build -> no verified profile -> affected internal reader = UNKNOWN
```

Do not guess offsets, copy them from a different build, or treat a matching product version string as sufficient evidence.

## AutoTrading research

The AutoTrading reader is passive and may use process enumeration, `VirtualQueryEx`, module inspection, and `ReadProcessMemory`. It must not write to MultiCharts memory or inject input.

The current Developer Mode workflow is:

1. Start an AutoTrading research session.
2. Toggle exactly one known strategy state.
3. Capture a research snapshot.
4. Repeat controlled transitions as required.
5. Finish the research session and review the generated evidence.
6. Add a compatibility value only after controlled verification.

Research output does not automatically authorize a production profile.

## Self-documenting INI model

`NormalizeConfigFile()` implements the self-documenting configuration policy:

- missing known settings are written with built-in defaults;
- invalid Boolean values are replaced with their defined defaults;
- bounded numeric settings are parsed and normalized to allowed ranges;
- normalization changes are logged;
- user-specific values and secrets are not invented.

This policy allows the INI file to document the currently supported settings after the program has run.

Program-generated diagnostic sections such as `[DetectedMultiCharts]` are not user-owned settings and should be clearly treated as output.

When adding a new normal configuration key, add it to normalization with a safe default and document it. Do not use this mechanism to fabricate credentials, verified compatibility profiles, or other values that require external truth.

## Secret handling

`smtp_password` stores the provider-required SMTP credential. Depending on the provider this may be an App Password or another SMTP password.

Secrets must never be intentionally written to:

- startup logs
- configuration-normalization logs
- Status Reports
- alert bodies
- compatibility diagnostics
- research output

Error messages should identify the failed operation without echoing secret values.

## Broker Monitor

The Broker Monitor processes rolling Recent Logs snapshots oldest-to-newest and deduplicates previously observed rows. Disconnect/reconnect evidence starts a grace period; explicit recovery can cancel the pending outage before an alert is generated.

Broker text patterns are configuration data rather than compiled broker-specific rules.

## Broker authentication detector

`BrokerAuthDetector` passively inspects supported browser window/control text. `[BrokerAuth.*]` sections define broker-specific URL, title, text, recovery-log, and delay evidence.

A confirmed authentication page is stronger current evidence than an older successful log event. Full sensitive authentication URLs should not be copied into reports; configured match identifiers are sufficient.

## Universal Application Mapper

The Universal Application Mapper is a separate application-independent research tool. Watchdog stores only its optional path:

```ini
[DeveloperTools]
universal_application_mapper_path=C:\MCExtras\UniversalApplicationMapper.exe
```

Do not start the Mapper automatically in production mode.

## Release policy

Before publishing:

1. Keep the production Watchdog Release settings intentionally controlled.
2. Build `Release|x64` with no Debug solution configuration in the release tree.
3. Keep `/MT` explicit for all production projects.
4. Resolve compiler warnings unless a documented exception has been reviewed.
5. Run `Tools\Validate-Release.ps1`.
6. Perform a real Windows/MSVC rebuild.
7. Verify Watchdog startup and normal Dashboard operation.
8. Verify the MultiCharts PowerLanguage host loads the intended Tracker Bridge DLL.
9. Verify no secret values appear in generated diagnostics or reports.

Static release validation is a guardrail, not a replacement for a real build and runtime test.
