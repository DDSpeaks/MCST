# Changelog

## 1.114-R23

- Replaced the wide HTML Open Positions table with normal-width cards after an
  iOS Mail capture confirmed that it scaled the fixed 1100-pixel table down.
- Kept every card and field at the report's 15-pixel monospaced size and allowed
  values to wrap instead of widening the message viewport.
- Preserved all nine position fields, known-currency Open P/L totals, and full
  green/red profit-value coloring.
- Retained R22 Tracker recovery and stale-state corrections unchanged. Tracker
  Bridge remains internal V174 and Protocol V2 remains unchanged.

## 1.114-R22

- Scoped Bridge recovery mode to the active bounded exact-profile scan; a
  failed scan no longer suppresses normal CATPTTabView discovery indefinitely.
- Added a 30-second positive/negative candidate-cache lifetime so restored
  normal discovery cannot become a repeated process-wide memory scan.
- Preserved all Watchdog retry/recovery outcomes instead of reporting only the
  final `cooldown` result.
- Added a configurable 10-minute stale-data Attention interval before Tracker
  monitoring escalates to Critical.
- Separated Last Tracker attempt from Last complete snapshot in the Dashboard
  and Status Report.
- Gave the HTML Open Positions table an explicit 1100-pixel width, non-wrapping
  cells, and horizontal scrolling so iOS Mail does not shrink its 15-pixel text.
- Bumped Tracker Bridge internal build to V174; Protocol V2 is unchanged.

## 1.114-R21

- Added an optional 200-row `monitoring_logs` Bridge section while retaining the
  existing ten-row Recent Logs display section.
- Changed BrokerMonitor and LogAlertEngine to consume the extended live history.
- Added a regression test matching the observed Saxo sequence: an older
  disconnection, a newer successful connection, and more than ten later
  unrelated UIC warnings must result in Broker `Connected`.
- Bumped Tracker Bridge internal build to V173; Protocol V2 is unchanged.
- Retained R20 self-recovery and stale-data labeling plus the complete R19 report
  appearance correction.

## 1.114-R20

- Added one bounded Bridge-local cache refresh and fresh scan for incomplete,
  compatibility-authorized Tracker snapshots; Bridge internal build is V172.
- Changed Watchdog retry handling so parsed partial snapshots no longer count as
  completed reads.
- Added in-memory last-good Tracker tables with explicit STALE timestamping while
  preserving current Critical health.
- Prevented stale Recent Logs from entering broker and log-alert engines.
- Added a prominent stale-data warning to plain-text and HTML reports.
- Retained the complete R19 mobile Open Positions table appearance correction,
  R18 Open P/L placement/colors, Protocol V2, and the R16 diagnostic.

## 1.114-R19

- Replaced the Open Positions `<pre>` block in HTML emails with a real HTML
  table to prevent mobile clients from shrinking a wide fixed-width line.
- Enforced the report's 15-pixel monospaced font on the table and every cell.
- Added mobile viewport metadata and disabled automatic mobile text resizing.
- Added a width-constrained horizontal-scroll container around the table.
- Allowed Profile and Last Update cells to wrap while preserving non-wrapping
  numeric cells and column alignment.
- Retained R18 Open P/L placement and green/red coloring unchanged.
- Added mobile table structure and typography regression tests.
- Retained Bridge V171 and Protocol V2.

## 1.114-R18

- Moved every known-currency Open P/L total into the Open Positions table so
  the amount is directly below the individual Open P/L values.
- Added green HTML styling for complete positive Open P/L cells, including the
  currency symbol/code and optional plus sign.
- Added red HTML styling for complete negative Open P/L cells, including the
  currency symbol/code and minus sign.
- Applied the same styling to individual values and totals in Status Reports
  and in status reports embedded in alert messages.
- Removed the Native Value total explanation line from the report.
- Added regression coverage for column placement and positive/negative colors.
- Retained the R17 currency aggregation rules, Bridge V171, and Protocol V2.

## 1.114-R17

- Ended the search for the unresolved Average Price/native currency.
- Added per-currency Open P/L totals based only on unambiguous currency evidence
  already present in each visible Open Positions cell.
- Added EUR recognition from the euro sign and support for explicit three-letter
  currency codes.
- Added fail-closed handling for ambiguous currency symbols: affected rows are
  excluded from totals and reported as not totalled.
- Kept Native Value as a per-row value and intentionally left its total disabled.
- Added a Status Report regression test for EUR grouping and dollar ambiguity.
- Retained Bridge V171, Protocol V2, and the R16 Position CCY diagnostic action.

## 1.114-R16 (research)

- Bumped Tracker Bridge internal build from V170 to V171; Protocol V2 and command 50 remain unchanged.
- Replaced the active broad ABI candidate search with fingerprint-scoped verification of ATCenterProxy vtable RVA `0x44D518`.
- Added exact target-RVA and code-signature gates for Quantity, Average Price, Open P/L, CurrencyCode/CurrencyLetter, CurrencyLetterRPL, and Realized P/L.
- Added direct reads of object fields `+0x1A8`, `+0x1B0`, `+0x1B8`, and `+0x1C8` only after the fingerprint gate passes.
- Added bounded MSVC x64 `std::wstring` decoding and strict currency validation at object offsets `+0x308` and `+0x328`.
- Added unique row correlation using Quantity and Average Price, plus Open P/L delta reporting.
- Added staged exact-vtable discovery: known-anchor direct scan, anchor pointer-reference scan, and bounded process-data fallback.
- Corrected PriceScaleCode receiver semantics: it uses a separately queried interface, not the position interface's own `+0x60` slot.
- Kept the action read-only; no undocumented MultiCharts function is called.

## 1.114-R15 (research)

- Bumped Tracker Bridge internal build from V169 to V170; Protocol V2 and command 50 remain unchanged.
- Replaced object-instance candidate limiting with unique-vtable grouping.
- Added all-module target capture, bounded direct-thunk following, module-relative RVAs, and generic runtime-function boundaries.
- Added `RDX` output-pointer ABI evidence and target classifications: `COMPATIBLE`, `PLAUSIBLE`, `UNKNOWN`, and `REJECTED`.
- Added explicit rejection of the R14 false-match pattern: trivial `this`-field getters that ignore the required output pointer.
- Added ABI-aware vtable ranking, bounded source-path reporting, reference samples, and limit-sensitive `PARTIAL` checkpoints.
- Kept the action read-only; no undocumented MultiCharts function is called.

## 1.114-R14 (research)

- Bumped Tracker Bridge internal build from V168 to V169; Protocol V2 and command 50 remain unchanged.
- Added automatic verification of the seven extractor vtable-dispatch offsets.
- Added a bounded known-anchor interface-object search using the six confirmed semantic slots.
- Added target-module, RVA, runtime-boundary, and unique target-code evidence.
- Kept the action read-only; no undocumented MultiCharts function is called.

## 1.114-R7 (research)

- Bumped Tracker Bridge internal build from V161 to V162; Protocol V2 and command 50 remain unchanged.
- Added an unconditional process-wide stride-table scan with a 3 GiB byte budget and 150-second deadline.
- Kept the R6 symbol-bearing independent position records as a parallel research route.
- Added process-wide deduplicated back-reference scanning with a 3 GiB budget, 120-second deadline, and 4096 unique-hit cap.
- Expanded record-neighborhood inspection substantially for strings, pointers, integers, and numeric fields.
- Corrected checkpoint status to `PARTIAL` when the validated table phase remains incomplete.
- Preserved the read-only safety boundary and prohibition on unknown MultiCharts calls.

## 1.114-R6 (research)

- Bumped Tracker Bridge internal build from V160 to V161; Protocol V2 and command 50 remain unchanged.
- Added strict validation of the live stride-`0x30` position table across every visible row.
- Deduplicate ownership references by field address and pointer value before result limiting.
- Added complete per-row owner-pointer coverage and stride-`0x90` owner-record grouping.
- Added bounded pointer/string inspection around each unique owner field.
- Preserved the read-only safety boundary and prohibition on unknown MultiCharts calls.

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
