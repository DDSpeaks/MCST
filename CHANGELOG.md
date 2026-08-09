# Changelog

## 1.111

- Removed the redundant `RECENT ACTIVITY` section from the Dashboard.
- Kept `LATEST ACTIVITY` as the single user-facing activity summary.
- Preserved internal activity/event tracking and production monitoring logic.
- Updated the User Guide and release metadata to match the simplified Dashboard.
- Preserved Bridge Protocol V2, Tracker Bridge internal build V155, and established production Release settings.

## 1.110

- Added a compact, lower-height Developer Mode toolbar with centralized layout geometry for future compatibility research controls.
- Added a smaller font for Developer Mode research buttons to distinguish them from production controls.
- Changed the Email settings credential label to `Password / App Password` and added provider-specific guidance without renaming the existing `smtp_password` INI key.
- Documented MCST's self-documenting INI behavior: missing known settings are written with safe defaults while user-specific values and secrets are not invented.
- Expanded the compatibility documentation from an AutoTrading-specific view to a general MultiCharts Internal Compatibility Framework.
- Clarified that the human-readable MultiCharts version is diagnostic information, while internal-memory access requires an exact verified module fingerprint.
- Added detected `Charting.dll` PE timestamp and image size to the generated `[DetectedMultiCharts]` INI diagnostics when available.
- Documented that future MC-internal readers must add their build-dependent values to a verified compatibility profile/schema before production use.
- Updated the Tracker Bridge documentation for product version 1.0, internal build V155, and Bridge Protocol V2.
- Reworked README, Installation Guide, User Guide, Developer Guide, Architecture, Compatibility, Coding Standard, build information, release notes, and release validation for public distribution.
- Removed historical per-version changelog files from the production source package in favor of this single public changelog.
- Preserved Bridge Protocol V2, Tracker Bridge internal build V155, and established Watchdog production Release settings.
