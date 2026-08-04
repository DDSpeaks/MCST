# MCST-Watchdog 1.0 Release Notes

## Status

MCST-Watchdog 1.0 is the first production-candidate release intended for sustained testing in the real MultiCharts environment.

## Included capabilities

- Operational dashboard with fixed status ordering and traffic-light states.
- Tracker Bridge V155 integration.
- Verified AutoTrading reader with profile-based MultiCharts compatibility matching.
- Centralized INI normalization.
- AutoTrading alerts and recovery notifications.
- SMTP email, test email, scheduled status reports and heartbeat reports.
- Resource monitoring and system-health summary.
- Developer research tools and compatibility diagnostics.
- English-only source comments, interface text and documentation.

## Build corrections from 0.588

- Removed a duplicate `ReadRemotePeTimestamp` function body that caused compiler error C2084.
- Removed the remaining local-variable shadowing warning in `main.cpp`.
- Updated all active product version strings to 1.0.

## Installation convention

Install runtime EXE and DLL files in:

```text
C:\MCExtras
```

Source code and documentation may be stored anywhere selected by the user.

## Required validation

Run **Rebuild Solution** with **Debug / x64** in Visual Studio. After a clean build, run the application in the production environment and record any stability, display, alerting or compatibility observations.
