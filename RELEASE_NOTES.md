# MCST 1.111 Production Release

MCST-Watchdog 1.111 is a production correction release that simplifies the Dashboard by removing a redundant activity section while preserving the monitoring logic, public-release documentation, compact Developer Mode UI, and established Bridge interfaces.

## Dashboard activity cleanup

The redundant **Recent Activity** section has been removed from the Dashboard. **Latest Activity** remains the single user-facing activity summary, avoiding duplicated information and preserving vertical space for the production controls. Internal activity/event tracking used by monitoring logic is unchanged.

## Developer Mode UI

Developer research controls now use a compact, lower-height toolbar with a smaller UI font. This deliberately distinguishes research actions from normal production controls and reserves horizontal space for future MultiCharts compatibility-research tools.

The compact layout is centralized in `DashboardLayout` so future Developer Mode buttons can use the same geometry.

## Email credential clarity

The Email settings UI now labels the credential as **Password / App Password** and explains that the correct value is provider-specific. Gmail normally requires a Google App Password for this SMTP authentication style; other providers may require an App Password, another SMTP credential, or a normal SMTP password according to their policy.

No SMTP configuration key was renamed, so existing `smtp_password` settings remain compatible.

## Self-documenting INI model

The public documentation now explicitly defines the existing configuration behavior as a self-documenting INI model. Missing known settings are written with safe defaults and invalid bounded settings are normalized. User-specific values and secrets are never invented.

`[DetectedMultiCharts]` is documented as program-generated diagnostic output rather than a user-maintained setting. MultiCharts detection now also records the detected `Charting.dll` PE timestamp and image size when available, making the human-readable version and the exact compatibility fingerprint visible together in the INI diagnostics.

## MultiCharts compatibility model

The documentation now describes the compatibility system as a general **MultiCharts Internal Compatibility Framework** rather than an AutoTrading-only concept.

The visible MultiCharts product/file version is diagnostic information. Production internal-memory readers require an exact verified module fingerprint. The current production profile provides the build-dependent values used by the AutoTrading reader; future internal readers must extend the verified profile/schema for their own build-dependent values.

Unknown builds remain fail-safe and do not authorize old offsets.

## Tracker Bridge

- Product: **MCST Tracker Bridge 1.0**
- Internal build: **V155**
- Protocol: **V2**
- Runtime DLL: `C:\MCExtras\MCST-TrackerBridge.dll`

Bridge Protocol V2 and the V155 implementation remain unchanged in this release.

## Production build

- Configuration: `Release|x64`
- C/C++ runtime: static `/MT`
- Watchdog optimization: `/O2` with the established production linker/code-generation settings
- Debug solution configuration: not included

## Documentation

README, Installation Guide, User Guide, Developer Guide, Architecture, Compatibility, Coding Standard, Tracker Bridge documentation, Mapper integration documentation, build information, release notes, changelog, and release validation have been updated for public distribution.
