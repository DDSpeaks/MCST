# MCST

MCST is a Windows monitoring suite for MultiCharts. Its production application, **MCST-Watchdog**, provides an at-a-glance operational view of MultiCharts health, Tracker data, AutoTrading state, broker connectivity, recent-log alerts, scheduled status reports, heartbeat reporting, system resources, and compatibility diagnostics.

## Current production versions

- MCST-Watchdog: **1.110**
- MCST Tracker Bridge: **1.0**
- Tracker Bridge internal build: **V155**
- Bridge protocol: **V2**
- Build target: **Release x64**
- C/C++ runtime linkage: **static `/MT`**

The public Tracker Bridge product version, internal bridge build, and bridge protocol are separate identifiers. V155 is not the public product version.

## Runtime installation

Install the runtime files in:

```text
C:\MCExtras
```

Principal runtime files:

```text
C:\MCExtras\MCST-Watchdog.exe
C:\MCExtras\MCST-TrackerBridge.dll
C:\MCExtras\MCST-Watchdog.ini
C:\MCExtras\MCST-Compatibility.ini
```

The source tree and documentation may be stored anywhere.

## MultiCharts integration

MultiCharts loads `MCST-TrackerBridge.dll` through the supplied PowerLanguage host. Add `MCST_Tracker_Bridge_Host.txt` as an indicator to one chart in each MultiCharts64 instance that owns an Order and Position Tracker monitored by MCST-Watchdog.

The production bridge path is:

```text
C:\MCExtras\MCST-TrackerBridge.dll
```

MCST-Watchdog does not load this DLL as a normal EXE dependency. The Bridge runs inside MultiCharts and communicates with Watchdog through Bridge Protocol V2.

## Self-documenting configuration

`MCST-Watchdog.ini` follows a **self-documenting configuration** model. When a known setting is missing, Watchdog writes the setting to the INI file with its safe built-in default. Invalid Boolean or numeric values are normalized to valid values where defined. This makes the INI file itself a practical reference for the settings supported by the installed version.

User-specific values and secrets are never invented. Address, account, SMTP user, and password fields may therefore be created as empty placeholders and must be completed by the user when needed.

Watchdog also maintains a generated diagnostic section similar to:

```ini
[DetectedMultiCharts]
product_version=...
file_version=...
product_name=...
executable=MultiCharts64.exe
process_id=...
charting_pe_timestamp=...
charting_image_size=...
compatibility_profile=...
last_detected=...
```

This section is output from detection, not a user-maintained configuration section.

## Email password / App Password

The `smtp_password` setting is the SMTP credential required by the selected email provider. When the provider supports or requires an application-specific password, use an **App Password** instead of the normal account password. Gmail normally requires a Google App Password for this style of SMTP authentication. Other providers may use a normal SMTP password, an app-specific password, or another authentication policy.

MCST does not intentionally include the configured password or App Password in status reports, alert messages, or diagnostic logs.

## MultiCharts compatibility safety

Watchdog detects the active MultiCharts executable version for human-readable diagnostics. Internal-memory compatibility is validated separately using an exact build fingerprint, currently based on `Charting.dll` PE timestamp and image size.

A readable MultiCharts version number is not, by itself, proof that an internal-memory profile is compatible. Unknown internal builds fail safely: affected readers remain `UNKNOWN` rather than reusing unverified addresses.

The compatibility framework is designed to hold all future MultiCharts-build-dependent internal values. The current production profile supplies the verified values required by the AutoTrading reader; future internal readers must add and verify their own build-dependent values before production use.

## Developer Mode

Developer Mode is disabled by default. When enabled, research controls are shown as a compact toolbar that is visually distinct from normal production controls and leaves room for future compatibility-research actions.

```ini
[Developer]
enabled=false
```

Developer tools are not required for normal monitoring.

## Building

Open `MCST.sln` in Visual Studio 2022 and build:

```text
Configuration: Release
Platform:      x64
```

The production source tree intentionally contains no Debug solution configuration. Before packaging a release, run:

```powershell
.\Tools\Validate-Release.ps1
```

Static validation does not replace a real Windows/MSVC build.

## Documentation

- `Docs/INSTALLATION.md`
- `Docs/USER_GUIDE.md`
- `Docs/DEVELOPER_GUIDE.md`
- `Docs/ARCHITECTURE.md`
- `Docs/COMPATIBILITY.md`
- `CODING_STANDARD.md`
- `RELEASE_NOTES.md`
- `CHANGELOG.md`
- `BUILD_INFO.txt`

## Engineering language policy

Source comments, identifiers, UI strings, log and error messages, documentation, release notes, and Git commit messages are written in English.
