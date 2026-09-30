# MCST

## Package: 1.21.2 — MultiCharts 16 + 17 AutoTrading

Version 1.21.2 adds click-to-read detail bubbles for truncated System Status
and Latest Activity text. It also retains the visible MC compatibility state
and conservative automatic adaptation for nearby MC16/MC17 AutoTrading layouts. A family candidate is
accepted only after passive live validation finds at least two strategy objects,
valid boolean states and no field-read failures. Successful results are cached
under the exact new DLL fingerprint and revalidated on every read. Uncertain
layouts remain unavailable and show `UPDATE REQUIRED` instead of guessed data.

The ordinary `Release|x64` build contains no accessible research controls and
provides user-focused Help. The separate `ReleaseDeveloper|x64` configuration
produces `MCST-Watchdog-Developer.exe` with the retained passive investigation
tools. The verified MC16 and MC17 profiles, Tracker Bridge V181, Protocol V2,
embedded logo and bounded queue-warning expiry remain included.

The logo is compiled into the executable and requires no separate installed file.
This source package has not been built or run on Windows during preparation.

### Retained features

MCST-Watchdog includes Tracker Bridge V181. When a Tracker object can no
longer be read through its cached route, it first revalidates the most recent
object address, then scans its allocator neighborhood, and finally performs a
bounded process-wide RTTI fallback. Long failure streaks use progressively
longer retry intervals to keep monitoring overhead low.

This source package fixes duplicate Latest Activity events while preserving
genuine events created during an in-flight refresh. Exact duplicates are
identified by timestamp, state, and text; the resulting history is sorted
newest-first and limited to ten entries. The Latest Activity timestamp column
now uses the same starting coordinate as the System Status state column. The
Latest Activity detail column remains aligned to the exact same
starting coordinate as the System Status description column. The shared
alignment applies to Accounts/Uptime details and retained activity-history
descriptions. It retains the original Latest Activity structure: a compact
description on the left, full local date and time in the adjacent value column,
and a wide right-hand detail column. Because activity events do not currently
have a separate detail field, their description is repeated in that wide
column so longer text remains readable. The Developer tools panel is raised ten
pixels to leave a clearly visible gap above the production buttons. Activity
rows remain text-only, uniformly 20 pixels high, and backed by the existing
bounded ten-event history. Monitoring behavior is unchanged.

The new first dashboard row reports process count, UI responsiveness, the
visible `q / s` processing backlog, per-process CPU use, private memory,
handles, GDI/USER objects, and unexpected process disappearance. Checks are
sampled during the existing Watchdog refresh cycle and use bounded reads; no
external heartbeat service or active interaction with MultiCharts is required.

MCST is a Windows monitoring suite for MultiCharts. Its production application, **MCST-Watchdog**, provides an at-a-glance operational view of MultiCharts health, Tracker data, AutoTrading state, broker connectivity, recent-log alerts, scheduled status reports, heartbeat reporting, system resources, and MultiCharts compatibility.

## Current production versions

- MCST-Watchdog: **1.21.2**
- MCST Tracker Bridge: **1.0**
- Tracker Bridge internal build: **V181**
- Bridge protocol: **V2**
- Build target: **Release x64**
- C/C++ runtime linkage: **static `/MT`**

The Tracker Bridge product version, internal build, and protocol version are
separate identifiers. V175 added optional `position_history`; V176 added the
exact verified CATPTTabView vtable anchor; V177 added progressive fast, expanded,
and wide recovery tiers, retained allocation hints, and detailed recovery
diagnostics. V178 moved the tier decision into a shared policy. V179 replaces
the slow expanded/wide targeted retries with validated-hint, optimized targeted,
and bounded process-wide stages. Protocol V2 is unchanged. The retained R16
`Position CCY` action still requires V171 or newer.

## Prebuilt GitHub Release

Pushing tag `v1.21.2` runs the Windows Release x64 build, executes
`MCST-LogicTests.exe`, and publishes a portable ZIP plus its SHA-256 checksum.
The ZIP contains only the Watchdog EXE, Tracker Bridge DLL, PowerLanguage host
files, inert `.ini.example` templates, installation instructions, release
notes, the MIT License, and a package manifest. It contains no active user INI
files, credentials, source code, test binary, or compiler artifacts.

A manual **Build Windows release** Actions run performs the same validation and
uploads a temporary artifact without publishing a GitHub Release. See
`Docs/FIRST_GITHUB_PUBLICATION.md`, `Docs/GITHUB_RELEASES.md`, and
`Docs/PORTABLE_INSTALL.md`.

## License

MCST is released under the permissive [MIT License](LICENSE).

Copyright (c) 2026 Mika Tättäläinen

The license covers this MCST source code and the binaries built from it. It does
not grant rights to MultiCharts, Saxo software, Microsoft Windows, or other
third-party products, trademarks, services, or dependencies.

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

## Separate Developer build

The ordinary `Release|x64` user build does not expose Developer Mode or any
research buttons. For maintainer investigations, build:

```text
Configuration: ReleaseDeveloper
Platform:      x64
```

This produces `MCST-Watchdog-Developer.exe`. Its optional Developer Mode
toolbar exposes:

```text
AT Start | AT Capture | AT Finish | Tracker Capture | Position CCY | Open Compat | Reload Compat | Help
```

Ordinary users receive user-focused Help and never need these controls.
Developer Help opens a two-pane selectable button-by-button guide. The AutoTrading
research sequence is `AT Start` → change only one chart strategy's AutoTrading
ON/OFF state → `AT Capture` (repeat as needed) → `AT Finish`.
Tracker and compatibility actions are documented individually in `Docs/USER_GUIDE.md`.

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

To create the portable package after a successful local build:

```powershell
.\Tools\Build-PortableRelease.ps1
```

## Documentation

- `Docs/INSTALLATION.md`
- `Docs/USER_GUIDE.md`
- `Docs/DEVELOPER_GUIDE.md`
- `Docs/ARCHITECTURE.md`
- `Docs/COMPATIBILITY.md`
- `Docs/FIRST_GITHUB_PUBLICATION.md`
- `Docs/GITHUB_RELEASES.md`
- `Docs/PORTABLE_INSTALL.md`
- `CODING_STANDARD.md`
- `RELEASE_NOTES.md`
- `CHANGELOG.md`
- `BUILD_INFO.txt`

## Engineering language policy

Source comments, identifiers, UI strings, log and error messages, documentation, release notes, and Git commit messages are written in English.
