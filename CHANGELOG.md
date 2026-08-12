# Changelog

## 1.114-R3 (research)

- Replaced R2's null `ITC_TradeInfo -> +0x98 -> +0x10E0` record-root dependency with dynamic read-only row correlation.
- Bumped MCST Tracker Bridge internal build from V157 to V158; public product version remains 1.0 and Bridge Protocol remains V2.
- Kept protocol command 50 unchanged while changing its implementation to R3 dynamic correlation.
- Added bounded pointer-graph discovery starting from Tracker roots, Open Positions page, and grid objects.
- Added bounded fallback scanning of readable private/mapped process data regions when root-reachable regions do not correlate all rows.
- Candidate discovery now starts from the visible Average Price and requires the visible Quantity nearby; displayed Open P/L is additional confidence evidence.
- Added per-candidate small-integer, pointer-string, indirect-string, and nearby-double diagnostics to help identify native and P/L currency fields.
- Added cross-row relative-offset frequency reporting so stable layouts can be distinguished from one-row coincidences.
- Replaced full-image diagnostic-string scanning with PE section-by-section scanning for `CurrencyCode`, `CurrencyLetter`, `CurrencyLetterRPL`, and related Open Positions getter names.
- Added raw RIP-relative reference diagnostics for the discovered extractor-name strings without calling any unknown MultiCharts internal function.
- Preserved the fail-closed fingerprint gate and research-safe suppression of multi-currency aggregate totals.

## 1.114-R2 (research)

- Upgraded Developer Mode `Position CCY` from the broad R1 structure probe to focused row/record correlation.
- Added Bridge Protocol V2 command 50, `CapturePositionCurrencyDirectResearch`.
- Bumped MCST Tracker Bridge internal build from V156 to V157; public product version remains 1.0 and Protocol remains V2.
- Correlates visible Open Positions rows with the mapped `ITC_TradeInfo -> +0x98 -> +0x10E0` position-record storage using quantity and Average Price.
- Records separate evidence for native/instrument currency and displayed Open P/L currency rather than assuming they are the same.
- Inspects the current record hypothesis around `Qty +0x60`, `Average Price +0x68`, `Open P/L +0x70`, and currency candidates beginning at `+0x78`.
- Records diagnostic method-name evidence for `CurrencyCode`, `CurrencyLetter`, and `CurrencyLetterRPL` without calling unknown MultiCharts functions.
- Preserves the R1 safety behavior: per-position `Native Value` only, with aggregate Position Value and Open P/L totals disabled until currency normalization is verified.

## 1.114

- Corrected the Tracker Bridge Host compatibility-database path construction so the Release x64 source compiles cleanly with `std::filesystem::path`.
- Removed the Watchdog local-variable shadowing warning in the Tracker Capture command handler.
- Added exact-fingerprint Tracker compatibility-profile support for `ATOnPTracker.dll` in the production Tracker Bridge reader.
- Added Tracker layout fields to the shared `MCST-Compatibility.ini` schema and kept unknown-build policy fail-safe.
- Added automatic disabled `Candidate.ATOnPTracker-*` sections for new unverified Tracker module fingerprints; candidate sections are never selected by production code.
- Added `ATOnPTracker.dll` PE timestamp and image size to detected MultiCharts diagnostics.
- Added separate AutoTrading and Tracker compatibility-profile names to `[DetectedMultiCharts]` and Status Report diagnostics.
- Added compact Developer actions: `Tracker Capture`, `Open Compat`, and `Reload Compat` alongside the existing AutoTrading research controls.
- Added runtime profile reload behavior so a verified Tracker profile can be put into service without recompiling MCST.
- Replaced the ambiguous combined CATPTTabView/ATOnPTracker read error with subsystem-specific compatibility and layout diagnostics.
- Updated Tracker Snapshot health/activity handling so a partial or unavailable Tracker reader is not reported as a fully successful snapshot.
- Bumped MCST Tracker Bridge internal build from V155 to V156 while preserving public product version 1.0 and Bridge Protocol V2.
- Updated public documentation and release validation for the generalized MultiCharts Internal Compatibility Framework.
- Refreshed the Windows INI profile view before runtime configuration reloads so manually edited settings are read immediately.
- Added configuration revision tracking to prevent an in-flight refresh that started before Reload Settings from restoring stale AutoTrading limits or other settings.
- Applied the same generation-safe reload behavior after configuration-panel saves.
- Preserved AutoTrading alert state across manual INI reloads to avoid duplicate alerts while still recognizing threshold-crossing transitions.
- Added calculated-width formatting for Accounts, Open Positions, and Recent Logs Status Report rows.
- Kept the final Recent Logs message field flexible while preserving a consistent aligned starting position.
- Added the derived `Position Value` column to Open Positions using absolute quantity multiplied by average price.
- Added a final `TOTALS` row with combined Position Value and combined Open P/L.

## 1.111

- Removed the redundant `RECENT ACTIVITY` section from the Dashboard.
- Kept `LATEST ACTIVITY` as the single user-facing activity summary.
- Preserved internal activity/event tracking and production monitoring logic.
- Updated the User Guide and release metadata to match the simplified Dashboard.

## 1.110

- Added a compact, lower-height Developer Mode toolbar with centralized layout geometry for future compatibility research controls.
- Added provider-aware `Password / App Password` guidance without renaming the existing `smtp_password` INI key.
- Documented MCST's self-documenting INI behavior: missing known settings are written with safe defaults while user-specific values and secrets are not invented.
- Expanded compatibility documentation from an AutoTrading-specific view to a general MultiCharts Internal Compatibility Framework.
- Added detected `Charting.dll` fingerprint information to `[DetectedMultiCharts]` diagnostics.
- Reworked public documentation for distribution and consolidated release history into this changelog.
