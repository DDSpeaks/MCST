# MCST

## Package: 1.114-R30 Status Report Alignment Cleanup

This source package retains R29's fixed System Status columns, unified `(no rows)` output, account-specific monthly P/L, Tracker recovery, and Saxo authentication detection. R30 moves Overall bolding and dot enlargement into nested elements so the outer `ch` column widths remain identical to ordinary rows in iOS Mail.

MCST is a Windows monitoring suite for MultiCharts. Its production application, **MCST-Watchdog**, provides an at-a-glance operational view of MultiCharts health, Tracker data, AutoTrading state, broker connectivity, recent-log alerts, scheduled status reports, heartbeat reporting, system resources, and MultiCharts compatibility.

## Current production versions

- MCST-Watchdog: **1.114-R30**
- MCST Tracker Bridge: **1.0**
- Tracker Bridge internal build: **V176**
- Bridge protocol: **V2**
- Build target: **Release x64**
- C/C++ runtime linkage: **static `/MT`**

The Tracker Bridge product version, internal build, and protocol version are separate identifiers. V175 added optional `position_history`; V176 adds the exact verified CATPTTabView vtable anchor and structural validation used by bounded self-recovery. Protocol V2 is unchanged. The current-month account totals and improved recovery require the V176 DLL in this package. The retained R16 `Position CCY` action still requires V171 or newer.

## Runtime installation

Install the production runtime files in:

```text
C:\MCExtras
```

Principal files:

```text
C:\MCExtras\MCST-Watchdog.exe
C:\MCExtras\MCST-TrackerBridge.dll
C:\MCExtras\MCST-Watchdog.ini
C:\MCExtras\MCST-Compatibility.ini
```

The source tree and documentation may be stored anywhere.

## MultiCharts integration

MultiCharts loads `MCST-TrackerBridge.dll` through the supplied PowerLanguage host. Add `MCST_Tracker_Bridge_Host.txt` as an indicator to one chart in each MultiCharts64 instance that owns an Order and Position Tracker monitored by MCST-Watchdog.

The production Bridge path is:

```text
C:\MCExtras\MCST-TrackerBridge.dll
```

MCST-Watchdog does not load this DLL as a normal EXE dependency. The Bridge runs inside MultiCharts and communicates with Watchdog through Bridge Protocol V2.

## Self-documenting configuration

`MCST-Watchdog.ini` follows a **self-documenting configuration** model. When a known setting is missing, Watchdog writes the setting with its safe built-in default. Invalid Boolean or bounded numeric values are normalized where the configuration contract defines a valid range. This makes the installed INI file a practical reference for the settings supported by that MCST version.

User-specific values and secrets are never invented. Addresses, account identifiers, SMTP usernames, passwords, and App Passwords must be supplied by the user when required.

Position History dates are read with an automatic DMY/MDY/YMD detector. Ambiguous rows fall back to the Windows user locale. A deployment can override that decision explicitly:

```ini
[Tracker]
date_order=auto
```

Accepted values are `auto`, `dmy`, `mdy`, and `ymd`. Invalid or unparseable dates are excluded rather than guessed.

The default Saxo authentication profile also contains:

```ini
[BrokerAuth.Saxo]
title_only_contains=MultiCharts (OpenAPI Web App)
```

This exact dedicated-window title complements URL and page-text matching when
modern Edge does not expose its address bar through ordinary Win32 text. Only
configured match patterns are reported; complete OAuth URLs and credentials are
not retained.

Watchdog also maintains generated diagnostic information such as:

```ini
[DetectedMultiCharts]
product_version=...
file_version=...
product_name=...
executable=MultiCharts64.exe
process_id=...
charting_pe_timestamp=...
charting_image_size=...
atonptracker_pe_timestamp=...
atonptracker_image_size=...
autotrading_compatibility_profile=...
tracker_compatibility_profile=...
last_detected=...
```

`[DetectedMultiCharts]` is application-generated diagnostic output, not a user-maintained settings section.

## Email password / App Password

The `smtp_password` setting is the SMTP credential required by the selected email provider. When the provider supports or requires application-specific passwords, use an **App Password** instead of the normal account password. Gmail normally uses a Google App Password for this style of SMTP authentication. Other providers may use a normal SMTP password, an application-specific password, or another provider-specific credential.

MCST does not intentionally include configured credentials in status reports, alert messages, startup diagnostics, research output, or normal logs.

## MultiCharts Internal Compatibility Framework

MCST separates human-readable version detection from authorization to read MultiCharts internal structures.

- The MultiCharts product/file version is useful diagnostic information.
- `Charting.dll` fingerprinting authorizes build-dependent AutoTrading data.
- `ATOnPTracker.dll` fingerprinting authorizes build-dependent Tracker layout data.
- A changed or unknown fingerprint never authorizes reuse of an unverified profile.

The same `MCST-Compatibility.ini` database is used by Watchdog and the Tracker Bridge. For a new `ATOnPTracker.dll` build, the Bridge records a disabled `Candidate.*` section containing the exact fingerprint. Candidate sections are **never production profiles**. Developer research is used to discover and verify the required layout values, after which an enabled `Profile.*` section can be added.

The **Reload Compat** Developer action forces Watchdog to re-evaluate AutoTrading compatibility and requests a fresh Tracker snapshot. The Bridge re-reads Tracker compatibility data for each snapshot request, so a verified profile can be put into service without recompiling MCST.

See `Docs/COMPATIBILITY.md` for the full profile schema and verification workflow.

## Developer Mode

Developer Mode is disabled by default:

```ini
[Developer]
enabled=false
```

When enabled, a compact toolbar exposes research actions without competing visually with normal production controls:

```text
AT Start | AT Capture | AT Finish | Tracker Capture | Position CCY | Open Compat | Reload Compat
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
