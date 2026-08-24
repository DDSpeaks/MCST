# MCST 1.114-R34 Update-Only Developer Guidance

Tracker Bridge internal build: V178  
Bridge protocol: V2

R27's Windows SDK header-order fix is retained: `ole2.h` and `oleauto.h`
remain before `UIAutomation.h` in `BrokerAuthDetector.cpp`.

## Logic and regression tests

- The Release test executable is now named `MCST-LogicTests.exe`; the Visual
  Studio solution displays the project as `MCST.LogicTests`.
- Test failures are reported with a nonzero exit code and a readable diagnostic.
- The production fast/expanded/wide Tracker recovery decision now lives in the
  shared `TrackerRecoveryPolicy.h`. Both Bridge V178 and LogicTests call that
  exact policy instead of maintaining duplicate decision logic.
- LogicTests verifies thresholds, budgets, cooldowns, and expanded fallback
  when the wide tier remains inside its cooldown.

## Developer Mode help

- R34 adds a prominent rule above the Help topic selector and repeats it inside
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
  shorter than the normal production controls (20 pixels versus 34 pixels).

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

Rebuild and replace `MCST-Watchdog.exe`. Bridge V178 is unchanged from R33, so
it does not need replacement for this Help-only update. If the DLL is replaced,
restart MultiCharts so Bridge V178 is loaded; restarting Watchdog alone does not
replace the DLL running inside MultiCharts. Protocol V2 is unchanged.
