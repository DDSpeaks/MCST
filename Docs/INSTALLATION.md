# MCST Installation Guide

## Requirements

- 64-bit Windows
- MultiCharts64 for the monitored trading environment
- Visual Studio 2022 with the v143 C++ toolset only when building from source
- An SMTP account only when email alerts, status reports, or heartbeats are enabled

Visual Studio is not required when installing the prebuilt portable ZIP from a
GitHub Release. Follow `PORTABLE_INSTALL.md` in that package. The source-build
instructions below are for developers and release maintainers.

## Build the production binaries

Open `MCST.sln` in Visual Studio 2022 and build `Release|x64`. The production source package intentionally contains no Debug solution configuration.

Normal outputs are written to:

```text
bin\Release
```

The principal runtime binaries are:

```text
MCST-Watchdog.exe
MCST-TrackerBridge.dll
```

MCST uses static Microsoft C/C++ runtime linkage (`/MT`). A separate Visual C++ Redistributable is therefore not intended to be an MCST runtime requirement. Normal Windows system DLLs remain operating-system dependencies.

## Runtime directory

Copy the runtime files to:

```text
C:\MCExtras
```

A normal installation contains:

```text
C:\MCExtras\MCST-Watchdog.exe
C:\MCExtras\MCST-TrackerBridge.dll
C:\MCExtras\MCST-Watchdog.ini
C:\MCExtras\MCST-Compatibility.ini
```

Keeping the Watchdog EXE and Tracker Bridge DLL in the same production directory is important because both components use the same `MCST-Compatibility.ini` database.

Source code and documentation may be stored anywhere.

## Install the PowerLanguage host

The Tracker Bridge DLL is loaded by MultiCharts through the supplied PowerLanguage host, not by `MCST-Watchdog.exe` as a normal DLL dependency.

Use:

```text
MCST.TrackerBridgeHost\PowerLanguage\MCST_Tracker_Bridge_Host.txt
```

Add the host as an indicator to one chart in each MultiCharts64 instance that owns the Order and Position Tracker monitored by MCST-Watchdog. The host references:

```text
C:\MCExtras\MCST-TrackerBridge.dll
```

A stop helper is also supplied:

```text
MCST.TrackerBridgeHost\PowerLanguage\MCST_Tracker_Bridge_Stop.txt
```

Restart MultiCharts after installing or replacing the Bridge DLL so the intended build is loaded cleanly. MCST-Watchdog 1.114-R45 production Tracker snapshots require Bridge V156 or newer; progressive diagnostic self-recovery uses the V178 DLL included in this package. The retained R16 Position Currency research action requires V171 or newer. R45 refines Watchdog's Dashboard and keeps Bridge V178 unchanged, so an installation already using Bridge V178 needs only the rebuilt Watchdog executable. If the Bridge DLL is replaced for any reason, MultiCharts must be restarted because restarting Watchdog alone does not reload the DLL inside MultiCharts.

Keep Developer mode disabled during normal operation. Enable its research
controls only after a MultiCharts/module update has produced an unrecognized
fingerprint or when a developer asks for a specific compatibility capture.

The default Saxo authentication profile includes
`title_only_contains=MultiCharts (OpenAPI Web App)`. Keep this exact match only
for the dedicated MultiCharts OAuth window. The detector uses bounded Windows
UI Automation when the browser does not expose its address bar through Win32;
it never reports the complete OAuth URL or credentials.

## First Watchdog start

Start MultiCharts and the Bridge host first, then start:

```text
C:\MCExtras\MCST-Watchdog.exe
```

Watchdog normalizes its configuration on startup and fills in missing known settings. This is intentional.

## Self-documenting INI behavior

MCST uses a **self-documenting INI** approach:

- a missing known setting is written with the built-in safe default;
- invalid Boolean and bounded numeric values are normalized where a valid range is defined;
- normalization changes are recorded in `MCST-Watchdog-ConfigNormalization.log`;
- user-specific values and secrets are never invented;
- generated diagnostic sections such as `[DetectedMultiCharts]` are maintained by the application and are not user settings;
- verified compatibility addresses and offsets are never fabricated as defaults.

As a result, the installed `MCST-Watchdog.ini` becomes a practical reference for the settings supported by the running MCST version.

The default Position History date handling is locale-aware:

```ini
[Tracker]
date_order=auto
```

Use `dmy`, `mdy`, or `ymd` only when an explicit override is needed. With
`auto`, unambiguous rows are preferred and ambiguous rows fall back to the
Windows user locale.

## Configure email

Email is optional. Configure it through the Dashboard Email settings or directly in `MCST-Watchdog.ini`.

Typical fields include:

```ini
[Email]
enabled=true
smtp_server=smtp.example.com
smtp_port=587
use_ssl=true
smtp_user=user@example.com
smtp_password=
from=user@example.com
alert_to=alerts@example.com
report_to=reports@example.com
```

### Important: Password / App Password

`smtp_password` means the **SMTP credential required by the email provider**. It does not necessarily mean the normal password used to sign in interactively to the email account.

When the provider supports or requires application-specific passwords, create and use an **App Password**. Gmail normally uses a Google App Password for this type of SMTP authentication. Other providers may use an App Password, an app-specific password, a dedicated SMTP password, or another provider-specific credential.

Do not assume that the normal account password is the correct SMTP credential. Follow the current instructions of the email provider.

MCST does not intentionally include configured passwords or App Passwords in reports, alerts, research output, or diagnostic logs.

## Email channels

Routine reports and urgent alerts can use separate recipients:

```ini
[Email]
alert_to=alerts@example.com
report_to=reports@example.com

[StatusReport]
to=reports@example.com
```

AutoTrading, Broker, and Log Alert messages use the alert channel. Status Reports and Heartbeats use the report channel.

## Compatibility database

`MCST-Compatibility.ini` is stored in `C:\MCExtras` beside both production binaries.

The database can contain independent verified data for:

- `Charting.dll` / AutoTrading
- `ATOnPTracker.dll` / Tracker snapshot layout

Watchdog records the detected fingerprints in `[DetectedMultiCharts]` inside `MCST-Watchdog.ini`.

If a new `ATOnPTracker.dll` fingerprint has no verified Tracker profile, the Bridge creates a disabled `Candidate.ATOnPTracker-*` section in `MCST-Compatibility.ini`. The candidate records the fingerprint only; it is never used for production memory access. Developer research is required before the values are promoted to an enabled `Profile.*` section.

See `COMPATIBILITY.md` before editing compatibility profiles.

## Optional Universal Application Mapper

The Universal Application Mapper is a separate Developer Mode research tool. Its default configured path is:

```text
C:\MCExtras\UniversalApplicationMapper.exe
```

It is not required for normal Watchdog operation.

## Startup diagnostics

If the Dashboard does not appear, inspect:

```text
C:\MCExtras\MCST-Watchdog-Startup.log
C:\Temp\MCST-Watchdog-Startup.log
```

When possible, critical startup failures are also shown in a message box.
