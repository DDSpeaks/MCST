# MCST Installation Guide

## Requirements

- 64-bit Windows
- MultiCharts64 for the monitored trading environment
- Visual Studio 2022 with the v143 C++ toolset only when building from source
- An SMTP account only if email alerts, status reports, or heartbeats are enabled

## Build the production binaries

Open `MCST.sln` in Visual Studio 2022 and build `Release|x64`. The production source package intentionally contains no Debug solution configuration.

Normal outputs are written to:

```text
bin\Release
```

The principal production binaries are:

```text
MCST-Watchdog.exe
MCST-TrackerBridge.dll
```

The build uses static Microsoft C/C++ runtime linkage (`/MT`), so a separate Visual C++ Redistributable is not intended to be an MCST runtime requirement. Windows system DLLs remain normal operating-system dependencies.

## Runtime directory

Copy the runtime files to:

```text
C:\MCExtras
```

Typical installed files are:

```text
C:\MCExtras\MCST-Watchdog.exe
C:\MCExtras\MCST-TrackerBridge.dll
C:\MCExtras\MCST-Watchdog.ini
C:\MCExtras\MCST-Compatibility.ini
```

`MCST-Watchdog.ini` and `MCST-Compatibility.ini` are created or completed by the application when required. Source code and documentation can be stored anywhere and do not need to be installed in `C:\MCExtras`.

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

Restart MultiCharts after installing or replacing the Bridge DLL so the intended DLL build is loaded cleanly.

## First Watchdog start

Start MultiCharts and its Bridge host first, then start:

```text
C:\MCExtras\MCST-Watchdog.exe
```

On startup Watchdog normalizes its configuration and fills in missing known settings. This is intentional.

### Self-documenting INI behavior

MCST uses a **self-documenting INI** approach:

- a missing known setting is written with the built-in safe default;
- invalid Boolean and numeric values are normalized where a valid range is defined;
- the normalization is recorded in `MCST-Watchdog-ConfigNormalization.log`;
- user-specific values and secrets are not invented;
- automatically detected information, such as `[DetectedMultiCharts]`, is written by the application and is not a user setting.

As a result, the installed `MCST-Watchdog.ini` becomes a useful reference for the configuration keys supported by that version.

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

### Important: password / App Password

`smtp_password` means the **SMTP credential required by the email provider**. It does not always mean the normal password used to sign in interactively to the email account.

When the provider supports or requires application-specific passwords, create and use an **App Password**. Gmail normally requires a Google App Password for this type of SMTP authentication. Other providers may use an App Password, an app-specific password, a dedicated SMTP password, or another provider-specific credential.

Do not assume that the normal account password is the correct SMTP credential. Follow the current instructions of the email provider.

MCST does not intentionally include the configured password or App Password in status reports, alert emails, or diagnostic logs.

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

`MCST-Compatibility.ini` is stored next to the Watchdog executable. It contains verified internal MultiCharts build profiles. Do not copy internal addresses from one MultiCharts build to another without verification.

The human-readable MultiCharts version shown by Watchdog is diagnostic information. Internal compatibility requires an exact verified fingerprint.

## Optional Universal Application Mapper

The Universal Application Mapper is a separate Developer Mode research tool. If it is available, its default configured path is:

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

Watchdog writes startup stages to both locations when possible and displays a startup error dialog for failures that can be reported safely.

## Release validation

Before distributing a build, run:

```powershell
.\Tools\Validate-Release.ps1
```

Then perform a real `Release|x64` rebuild in Visual Studio and verify normal Watchdog and MultiCharts Bridge operation on Windows.
