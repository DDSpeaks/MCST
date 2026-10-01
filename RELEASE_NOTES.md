# MCST 1.21.8 GitHub Publication-Ready Package

MultiCharts 16 + 17 AutoTrading support

Tracker Bridge internal build: V181  
Bridge protocol: Protocol V2

## Usable Activity and Email history scrolling

- Replaces the narrow painted scroll marker with a native 18-pixel Windows
  vertical scrollbar on `Latest Activity` and `Latest Emails`.
- Supports dragging the thumb, line-up/line-down arrows, page-up/page-down
  track clicks and continuous thumb tracking.
- Mouse-wheel scrolling works over both the selected tab and its content.
- The control is hidden automatically when all session entries fit on screen
  and is never shown on the Developer Tools tab.

## Dedicated compatibility strip and main header

- Places the compatibility state in a dedicated narrow strip at the very top
  of the dashboard.
- Draws a light separator across the full inner width below that strip.
- Keeps the logo, `MCST-Watchdog 1.21.8` title and overall-health headline
  together in the main header below the separator.
- Retains the shared horizontal center of the differently sized compatibility
  and overall-health indicators while leaving room for the complete text.
- Moves the update row, System Status rows, history panel and their fixed row
  menu controls down together, preserving their visual and click alignment.

## Dashboard visual refinement

- History-tab outlines now stop exactly at the horizontal panel separator.
- The small compatibility indicator and larger overall-health indicator use
  one shared horizontal center in the upper-right header.
- The default dashboard height is now 960 pixels and its minimum height is
  880 pixels, providing substantially more visible Activity and Email rows.
- Existing smaller saved window heights are normalized to the new usable
  default when settings are loaded.

## Visual Studio compilation correction

- Corrects the `std::max` template type mismatch reported at the new
  `visibleRows` calculation.
- Both Win32 `RECT` coordinates are now converted explicitly to `int` before
  the row-count expression, removing the C2672/C2737 error cascade.

## Session history tabs and separated Developer tools

- Replaces the fixed Latest Activity block with `Latest Activity` and
  `Latest Emails` tabs.
- Retains up to 1,000 events of each type from the current Watchdog startup and
  provides mouse-wheel scrolling plus a native draggable scrollbar.
- `Latest Emails` lists Watchdog send results only; it does not connect to or
  read the user's mailbox.
- Adds `Developer Tools` as a third tab only in
  `MCST-Watchdog-Developer.exe`. The normal user build never displays it.
- Keeps research controls out of the normal monitoring view and shows them
  only while the Developer Tools tab is selected.
- Gives ordinary `? Help` the same light-blue, bordered and circled-question-
  mark appearance as Developer Help.
- Long Activity and Email rows retain the rounded click-to-read detail bubble.

## Click-to-read long dashboard text

- Corrects the MSVC type mismatch found in the initial 1.21.1 implementation;
  popup dimensions now use explicit `int` values throughout Win32 layout.
- Truncated descriptions in `SYSTEM STATUS` and `LATEST ACTIVITY` can now be
  clicked to open a lightweight rounded detail bubble.
- The bubble shows the complete wrapped text; Latest Activity details also
  include the event timestamp.
- Text can be selected and copied. The bubble closes by pressing Esc or by
  clicking elsewhere and is automatically kept inside the current screen.
- Only genuinely truncated rows use the hand pointer and open a bubble.

## Automatic compatibility status and ordinary-user interface

- Adds an always-visible compatibility banner and an `MC Compatibility`
  System Status row with `Verified`, `Auto-adapted`, `Checking...`, or
  `Update required` results.
- For an unknown but nearby MC16/MC17 `Charting.dll` build, tests the closest
  known family layout using passive reads. It accepts the candidate only when
  at least two strategy objects are found, every state is a valid boolean and
  no field read fails.
- Stores a successful automatic result under the exact DLL fingerprint in
  `MCST-Compatibility.ini`. Cached automatic profiles remain subject to the
  same live validation on every read.
- Keeps an uncertain layout unavailable and displays `UPDATE REQUIRED`; it
  never reports a guessed AutoTrading count.
- The normal `Release|x64` build contains no accessible research controls or
  Developer Mode selector. Its `? Help` window covers normal startup,
  compatibility, updates, warnings, safety and report privacy.
- `ReleaseDeveloper|x64` creates `MCST-Watchdog-Developer.exe` with the
  existing passive research controls for the maintainer.

## Bounded queue-warning retention

- A confirmed red MultiCharts queue warning remains active through two
  consecutive unchecked refreshes, protecting against momentary window
  coverage or an incomplete visual probe.
- If the field remains unchecked for a third consecutive refresh, the old
  visual evidence expires instead of leaving processes in WARNING forever.
- Any new checked red or clear result resets that expiry counter and is handled
  by the existing two-sample confirmation policy.
- When every monitored item is green, an unchecked visual probe no longer
  replaces the dashboard headline: it again reads `SYSTEM HEALTHY`.
- This change affects only Watchdog queue-warning state. MC16/MC17 profiles,
  Tracker Bridge V181 and Protocol V2 are unchanged.

## MultiCharts 17 production Tracker support

- Adds the exact verified MC17 `ATOnPTracker.dll` fingerprint
  `0x6AB58F4C / 3534848`.
- Selects CATPTTabView primary vtable RVA `0x1D78D8` only for that exact
  fingerprint. The verified MC16 RVA `0x1D78C8` remains unchanged.
- Retains Accounts and Open Positions extractor RVAs `0x10FAE6` and
  `0x10FF56`, which the passive MC17 capture independently confirmed.
- Unknown later MC17 fingerprints inherit no fixed address. They may proceed
  only through bounded RTTI discovery and structural validation; incompatible
  layouts remain unavailable instead of returning guessed data.
- Raises Tracker Bridge to internal V181 while retaining Protocol V2.

## Retained MC17 Tracker research and clearer Help access

- Extends the existing `Tracker Capture` action; no additional Developer Mode
  button is required.
- Writes `MCST_Tracker_Dynamic_Locator_<pid>.txt` with the exact
  `ATOnPTracker.dll` fingerprint, RTTI evidence, bounded process-wide candidate
  results and all discovered RTTI type names.
- Keeps the investigation passive and read-only: it performs no clicks, input,
  unknown function calls or writes to MultiCharts memory.
- Returns the useful research bundle even when the old MC16 table extractors
  cannot read the changed MC17 structure.
- Highlights the rightmost `? Help` button with a light-blue background,
  stronger blue border and circled question mark.
- The passive research tools remain available for later MultiCharts updates.

## Embedded Watchdog header logo

- Adds the selected doctor-and-patient artwork before the program name in the
  main dashboard header.
- Embeds the transparent PNG in `MCST-Watchdog.exe`; users do not need to copy
  or retain a separate logo file.
- Keeps the compact Windows title bar with the descriptive text
  `MCST-Watchdog 1.21.8 - MC16 + MC17 AutoTrading`.

## MultiCharts 17 production AutoTrading support

- Added a production profile for exact MC17 `Charting.dll` fingerprint
  `0x6AB57EE6 / 18624512`.
- Uses `strategy_vtable_rva=0xB74CF0` and `autotrading_offset=0x18`, selected
  from five controlled snapshots and four exact transitions.
- The selected field matched all `4/4` transitions, including two `1 -> 0`
  and two `0 -> 1` changes, with no unchanged or ambiguous transition.
- Preserved the existing MC16 fingerprint, RVA and offset unchanged, so MC16
  and this exact MC17 build are supported side by side.
- A nearby unknown fingerprint may use this MC17 family layout only after the
  new live structural validation succeeds. Otherwise it remains unavailable.

## Correct MultiCharts process identity

- Replaced window-title substring process discovery with an exact executable
  check for `MultiCharts64.exe` or `MultiCharts.exe`.
- Explicitly excludes the current Watchdog process.
- Prevents the Watchdog research window from being counted as a MultiCharts
  instance merely because its title mentions MultiCharts.

## Retained dynamic compatibility research

- `AT Start` now discovers repeated pointers into the active `Charting.dll`
  instead of testing only hard-coded MC16 RVA/offset pairs.
- `AT Capture` compares stable candidate objects after one controlled chart
  AutoTrading change and records exact single-object boolean transitions.
- `AT Finish` ranks fields across the complete session and requires both
  toggle directions for the strongest research confidence.
- Every snapshot records the `Charting.dll` PE timestamp and image size.
- Sessions without a real response report `NO CANDIDATE FOUND` rather than
  recommending the first legacy entry.
- Developer Mode retains passive dynamic discovery for future unknown builds.

## Retained iOS Mail status-cell separator correction

- Adds a non-breaking text separator inside the fixed-width status and value
  cells of HTML email reports.
- Prevents iOS Mail from concatenating `OK`, `Ready` and a following address
  into a false address such as `OKReadyalerts@example.com`.
- Preserves the existing status-column widths and alignment.
- The legitimate email address remains detectable and clickable.
- Changes only Watchdog; Bridge V179 and Protocol V2 are unchanged. Users
  upgrading from 1.20.10 may replace only `MCST-Watchdog.exe`.

This source package retains the GitHub Release workflow, SHA-256 checksum,
MIT License, `.gitignore`, inert `.ini.example` files, no active INI files,
Overall status reporting, Position History limited to accounts visible in
Accounts, UI Automation with `ole2.h`, the verified `0x1D78C8` anchor,
`date_order` handling, and the 15-pixel monospaced report layout.

## Retained 1.20.10 healthy-headline correction

Tracker Bridge internal build: V179  
Bridge protocol: Protocol V2

## Healthy Dashboard headline correction

- A Healthy overall state now always displays `SYSTEM HEALTHY`, including when
  some optional queue fields could not be visually inspected.
- `INITIALIZING` is retained before the first completed monitoring update.
- A genuinely Unknown overall state after monitoring starts displays
  `CHECK INCOMPLETE`.
- Warning and critical states remain `ATTENTION REQUIRED` and
  `CRITICAL CONDITION`.
- The saved report states `Queue visual check incomplete` only while a
  confirmed warning is still inside its bounded unchecked grace period; it
  does not rename a fully Healthy overall state.
- This release changes only Watchdog. Tracker Bridge V179 and Protocol V2 are
  unchanged, so users upgrading from 1.20.9 may replace only the EXE.

This source package retains the GitHub Release workflow, SHA-256 checksum,
MIT License, `.gitignore`, inert `.ini.example` files, no active INI files,
Overall status reporting, Position History limited to accounts visible in
Accounts, UI Automation with `ole2.h`, the verified `0x1D78C8` anchor,
`date_order` handling, and the 15-pixel monospaced report layout.

## Retained 1.20.9 covered queue-warning trial

Adds a bounded, experimental statusbar-rendering fallback for covered MC16
queue-warning fields. Two rendered red images can confirm WARNING; non-red,
blank, unsupported or timed-out output remains unknown. Windows use is still
unverified. See `COVERED_QUEUE_PROBE_TRIAL.txt` and `BUILD_VALIDATION_1.20.9.txt`.
The V179 Bridge and earlier log/report corrections are unchanged.

## Retained Tracker recovery behavior

Tracker Bridge internal build: V179  
Bridge protocol: V2

## Tracker recovery correction

- Revalidates the last accepted CATPTTabView address before scanning memory.
- Searches remembered allocator neighborhoods first and reads them in 256 KiB
  blocks instead of issuing one protected read for every pointer-sized value.
- Runs a bounded process-wide RTTI fallback immediately after a targeted miss.
- Applies the verified Tracker profile and strict structural score requirements
  before accepting a candidate from any recovery stage.
- Removes the persistent-recovery lock-in that previously suppressed the only
  search proven to find the still-live Tracker object.
- Uses 30-second retries initially, 60-second retries after three failures, and
  five-minute retries after ten failures to limit long-running recovery load.
- Adds stage-specific execution diagnostics for validated-hint, targeted, and
  process-wide recovery results.
- Keeps recovery fully read-only and retains Bridge Protocol V2.

## GitHub Actions compatibility fix

- Updated `actions/checkout` from v4 to v5 for its Node.js 24 runtime.
- Updated `microsoft/setup-msbuild` from v2 to v3 for its Node.js 24 runtime.
- The warning-producing Node.js 20 action generations are no longer used.
- Corrected a stale regression assertion that expected the removed `[10 rows]`
  text after the current Saxo Open P/L total. The production report was already
  correct and remains unchanged.
- Version `1.20.9` is based on `1.20.1`; Watchdog monitoring and Dashboard
  behavior are retained while Tracker recovery is updated in Bridge V179.
- Public release tags use the standard `vX.Y.Z` form. The package build still
  rejects any tag that does not exactly match the source version.

## Deduplicated and fully aligned Latest Activity

- Fixed repeated activity groups caused by merging an already-carried history
  back into itself after each refresh.
- Exact duplicates are removed by timestamp, state, and text while genuine
  events created during an in-flight refresh are retained.
- Aligned the Latest Activity timestamp column with the System Status state
  column using the same shared Dashboard layout coordinate.

- Aligned the Latest Activity detail column with the System Status description
  column by using the same shared Dashboard layout coordinate.
- The alignment applies to both Accounts/Uptime summary details and the long
  descriptions repeated for activity-history rows.

- When Developer mode is off, Latest Activity uses the available space above
  the main production buttons to show the existing bounded ten-event history.
- Every row uses the original compact text layout: event description on the
  left and full local date and time in the adjacent value column.
- When no separate activity detail exists, the event description is repeated
  in the wide right-hand detail column so the complete text is easier to read.
- Raised the complete Developer tools panel ten pixels, leaving an 18-pixel
  visual gap before the production-button row.
- Removed the status-colored indicators and separator styling mistakenly used
  for these activity rows in R41.
- When Developer mode is on, the existing three compact summary rows remain
  unchanged and the Developer tools panel retains its dedicated space.
- The existing history is reused, so the change adds no new polling,
  background work, or MultiCharts monitoring load.

## Retained refined Dashboard layout

- Reduced System Status row spacing slightly while keeping every status line
  readable and aligned.
- Substantially reduced Latest Activity row spacing.
- Moved Developer controls into a dedicated pale blue-grey panel labelled
  `DEVELOPER TOOLS · READ-ONLY DIAGNOSTICS`.
- Increased Developer button height and font slightly while keeping them
  visually distinct from normal production controls.
- Raised the minimum/default window height so persisted undersized layouts are
  normalized and Developer controls cannot cover activity text.
- Replaced user-facing AutoTrading `object/objects` wording with
  `chart/charts`. Internal C++ object terminology remains unchanged where it
  refers to actual memory objects.

## MultiCharts Health

- Added `MultiCharts Health` as the first dashboard status row and moved the
  header upward to preserve the compact layout.
- Monitors every running `MultiCharts.exe` and `MultiCharts64.exe` process,
  including empty auxiliary instances, without clicking or modifying anything
  in MultiCharts.
- Detects repeated UI non-response, a growing or old visible `q / s` backlog,
  high handle/GDI/USER-object counts, low available system memory, and a
  recently disappeared MultiCharts process.
- Reports per-process PID, state, CPU use (100% equals one logical core),
  private memory, handles, GDI/USER objects, queue depth, and queue age in the
  scheduled status report.
- A disappeared process triggers one immediate AutoTrading rescan. Loss of an
  empty auxiliary instance is a warning; loss of a trading instance becomes
  critical when the existing minimum-active-strategy requirement is missed.
- Sends an alert on WARNING/CRITICAL transitions and a recovery message when
  MultiCharts Health returns to OK, using the existing email configuration.
- Uses the existing refresh cycle, a 250 ms bounded responsiveness probe, and
  bounded accessibility reads. No external heartbeat was added.

## Compact Open Positions P/L summaries

- Removed the bracketed row-count comment from `Total Open P/L`.
- Removed row-count, excluded-row, and ambiguous-currency comments from each
  account-specific `Current month Realized P/L` row.
- The report still displays the calculated amount, `not calculated`, or
  `not available`; only the width-expanding explanation column was removed.
- P/L arithmetic, currency validation, visible-account filtering, colors, and
  alignment remain unchanged.
- Added regression checks requiring the summary comment column to remain empty.
- Tracker Bridge Protocol V2 is unchanged.

## Retained MIT License

- MCST is now published under the permissive MIT License.
- The copyright line is `Copyright (c) 2026 Mika Tättäläinen`.
- The same `LICENSE` file is committed at the repository root and included in
  every generated portable Windows package.
- The license applies to MCST, not to MultiCharts, Saxo, Windows, or other
  third-party products and services.

## Retained publication safeguards

- Added `.gitignore` protection for active INI files, credentials, private
  keys, Visual Studio output, runtime reports, logs, and research captures.
- Added `.gitattributes` so Windows projects, PowerShell scripts, source files,
  Markdown, and workflow YAML use deterministic line endings.
- Added safe `MCST-Watchdog.ini.example` and
  `MCST-Compatibility.ini.example` reference templates. Both remain inert
  under their packaged names; the compatibility candidate is disabled and has
  no placeholder address that could be authorized accidentally.
- Extended the portable Release ZIP with an `Examples` directory while still
  rejecting every active `.ini`, credential, test executable, compiler output,
  and source file.
- Added `Docs/FIRST_GITHUB_PUBLICATION.md`, covering repository creation,
  Actions permissions, the non-publishing test build, the exact release tag,
  Release verification, and MIT License verification.

The validated GitHub build and Release automation is retained, with its tag
filter updated for public `vX.Y.Z` releases.

## GitHub Release and portable user package

- Added `.github/workflows/release.yml`, which validates the source, builds
  `Release|x64` with Visual Studio 2022 tooling, and runs
  `MCST-LogicTests.exe` on a GitHub Windows runner.
- A manual Actions run uploads a 30-day package artifact for testing but does
  not create a public Release.
- Pushing the exact tag `v1.20.9` creates a GitHub Release from
  `RELEASE_NOTES.md` and attaches the portable Windows x64 ZIP and its SHA-256
  checksum.
- `Tools/Build-PortableRelease.ps1` verifies the version/tag match, packages
  only the required EXE, DLL, PowerLanguage files, MIT License, and user documentation, and
  rejects INI, credential-bearing configuration, PDB/LIB/OBJ, source, and test
  files.
- The portable package preserves existing user settings by containing no
  active INI files. A new user can install it without Visual Studio or a C++
  build environment.
- Added release-maintainer and end-user instructions in
  `Docs/GITHUB_RELEASES.md` and `Docs/PORTABLE_INSTALL.md`.

All R34 Developer Help behavior and the following tested production behavior
are retained unchanged.

R27's Windows SDK header-order fix is retained: `ole2.h` and `oleauto.h`
remain before `UIAutomation.h` in `BrokerAuthDetector.cpp`.

## Logic and regression tests

- The Release test executable is now named `MCST-LogicTests.exe`; the Visual
  Studio solution displays the project as `MCST.LogicTests`.
- Test failures are reported with a nonzero exit code and a readable diagnostic.
- Tracker retry/load limits live in shared `TrackerRecoveryPolicy.h`. Both
  Bridge V179 and LogicTests use the same policy.
- LogicTests verifies the targeted and process-wide budgets plus the 30-second,
  60-second, and five-minute retry thresholds.

## Developer Mode help

- R34 added a prominent rule above the Help topic selector and repeats it inside
  every topic: Developer tools are normally needed only after a MultiCharts
  update, after a Charting.dll/ATOnPTracker.dll fingerprint change, or when a
  developer explicitly requests compatibility evidence. They are not part of
  normal monitoring.
- Help now opens a resizable two-pane window. The left pane selects `Overview`,
  `AT Start`, `AT Capture`, `AT Finish`, `Tracker Capture`, `Position CCY`,
  `Open Compat`, or `Reload Compat`; the right pane shows the selected guide.
- Every action explains its goal, purpose, use case, prerequisites, exact user
  action, success evidence, next step, failure recovery, and safety boundary.
- `AT Capture` explicitly instructs the user to select one strategy on one
  chart and change only that strategy's AutoTrading state from ON to OFF or
  OFF to ON. It warns against changing settings, charts, workspaces, or the
  MultiCharts process set between captures.
- Help content is shared with LogicTests so missing topics and essential
  beginner instructions fail the regression test.
- A default-off **Developer mode** checkbox now appears to the right of Reload
  Settings and saves `[Developer] enabled`. Research buttons are visibly
  shorter than the normal production controls (22 pixels versus 34 pixels).

## More visible Status Report indicators

- Normal System Status dots are enlarged from the base 15-pixel glyph to a
  nested `1.5em` glyph. Their color is now easier to recognize on mobile mail.
- The `OVERALL STATUS` dot remains the largest at `2em`; both sizes remain
  inside the same fixed-width Dot column, so row alignment does not change.

## Progressive CATPTTabView recovery

- A verified exact-profile locator now runs before the expensive process-wide
  RTTI walk. Normal startup and object recreation can therefore recover through
  the known V147 vtable without a broad scan.
- After a confirmed failure, repeated Watchdog retries no longer launch the
  20-40 second process-wide search observed in the R30 execution trace.
- Recovery uses three read-only tiers: fast (16 MiB / 0.9 s), expanded
  (64 MiB / 1.8 s), and wide (128 MiB / 2.8 s). Expanded and wide attempts have
  separate two- and five-minute cooldowns; fast attempts retain the 30-second
  cooldown.
- Up to eight previously accepted CATPTTabView addresses are retained as safe
  allocator-neighborhood hints. Each hint is revalidated with `VirtualQuery`
  before its allocation is inspected.
- Execution trace entries now report tier, candidate count, scores, exact
  vtable hits, invalid hits, structural rejections, byte/time limits, elapsed
  time, and the final decision (`accepted`, `no_candidates`,
  `insufficient_structure`, or `ambiguous_candidates`).
- The parsed CATPTTabView diagnostic is also included in Watchdog Tracker and
  Recent Logs details.

## Retained column alignment

- Identity rows and Latest Activity now use a shared calculated label width.
- System Status HTML uses explicit fixed-width Name, Dot, State, and Value spans.
  Different marker lengths and iOS Mail's bold-font metrics can no longer move
  the following columns. The large Overall dot occupies the same Dot column as
  every normal-size component dot.
- R34 retains the fixed-width Overall Name, Dot, and State elements at the normal
  font size. Bold text and the double-size dot live in nested elements, so CSS
  no longer doubles the physical width of the `4ch` Dot column.
- Accounts cells are trimmed before layout and numeric columns are right-aligned.
  Positive and negative values therefore end at exactly the same character column.

## Account-specific monthly Realized P/L

- Position History is totalled only for every account visible in Accounts.
  Rows belonging to any other account are excluded.
- Each visible account receives its own `Current month Realized P/L <account>`
  line. No potentially misleading combined total across accounts is shown.
- Known currencies remain separate, and the amount, currency code, and sign use
  the same green/red styling as Open P/L.
- The locale-aware Position History date parser supports numeric DMY, MDY, and
  YMD formats. `[Tracker] date_order=auto|dmy|mdy|ymd` remains available.

## Report layout

- `OVERALL STATUS` is bold, separated from component rows, and uses a double-size
  colored dot while retaining the same fixed marker-cell width. Its state is no
  longer printed twice.
- System Status and Open Positions retain protected, non-wrapping,
  15-pixel monospaced rendering. Every logical row stays on one line and may
  continue to the right on a narrow mail display.
- Total Open P/L and each account's monthly Realized P/L remain directly below
  the Open P/L column.
- When there are no open positions, position column headings and separators are
  omitted and `(no rows)` is shown, matching Accounts and Recent Logs.
  Account-specific monthly Realized
  P/L lines remain available below the message.

## Tracker compatibility

- The exact verified V147 `ATOnPTracker.dll` fingerprint now supplies the known
  CATPTTabView primary vtable RVA `0x1D78C8` to bounded fresh recovery scans.
- Recovery candidates must also pass structural validation: secondary vtable,
  Tracker layout signature, or at least five credible page pointers.
- Protocol V2, the V147 fingerprint, the structural acceptance requirements,
  Position History, and Position Currency research are unchanged.

## Saxo authentication alert

- Modern Edge/Chromium browser contents are inspected through bounded Windows
  UI Automation in addition to ordinary child-window text.
- The dedicated `MultiCharts (OpenAPI Web App)` title can identify the known
  Saxo authentication window even when the address bar is not Win32 text.
- The detector stores only the configured matching pattern, never a complete
  OAuth URL, request identifier, token, user ID, or password.

## Installation note

Install the prebuilt GitHub Release ZIP or rebuild and replace both
`MCST-Watchdog.exe` and `MCST-TrackerBridge.dll`. Restart MultiCharts so Bridge
V179 is loaded; restarting Watchdog alone does not replace the DLL running
inside MultiCharts. Protocol V2 is unchanged.
# 1.20.9 — truthful Logs status and clean email reports

Distinguishes successful reads, unchanged events, new observed rows and cached
display after failed reads. Successful empty Logs are OK. Queue diagnostics
are hidden from HTML emails while warnings and coverage limitations remain.
See `REPORT_STATUS_CHANGES.txt`. Windows/live behavior remains unverified here.

## Inherited visible-warning notes

Active queue monitoring checks only field 4's visible red background. Two
consecutive red observations raise a warning through the existing health/email
path. Unchecked fields never clear a confirmed warning. No numeric extraction.
See `VISIBLE_QUEUE_WARNING.txt`. Windows/live behavior remains unverified here.

## Historical field-retry notes (no longer the active queue path)

Adds field geometry, red-field priority, continuation after length-query
failure, failure codes and bounded priority retry. Current instructions:
`QUEUE_FIELD_RETRY_TRIAL.txt`. Windows/live behavior remains unverified here.

## Inherited queue-area locator notes

Adds statusbar-area window geometry and temporary on-screen red-pixel probing
to identify the component drawing the queue warning. No color-based alerts.
See `QUEUE_AREA_LOCATOR_TRIAL.txt` for the current test procedure.

## Inherited direct-reader trial notes

Direct statusbar-part reading replaces broad accessibility traversal in the
active queue monitor. Reports record field types, text and read duration.
See `DIRECT_STATUSBAR_TRIAL.txt`. The following notes describe inherited
features; they do not establish successful live queue detection in this trial.
