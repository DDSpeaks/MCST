# MCST 1.114-R24 Aligned Open P/L Lines

Tracker Bridge internal build: V174
Bridge protocol: V2

R24 uses the same preformatted HTML flow for Open Positions as Accounts and
Recent Logs. This responds to the observed iOS Mail result where both a wide
HTML table and a later card layout failed the intended at-a-glance presentation.

## Open Positions HTML layout

- Every position occupies exactly one non-wrapping line and may continue to the
  right beyond the initially visible message area.
- The column order is Symbol, Open P/L, Side, Qty, Average Price, Native Value,
  Account, Profile, and Last Update.
- Open P/L is the second column. Every known-currency total is printed directly
  beneath that same column.
- Open Positions shares the same explicitly protected 15-pixel monospaced
  `<pre>` styling as the other data-heavy report sections.
- The rejected multi-line card layout and fixed-width HTML table are absent.
- Positive profit values remain fully green and negative values fully red,
  including currency signs/codes and plus/minus signs.
- The plain-text/local Status Report uses the identical one-line column order.

## Retained R22 behavior

- Bridge V174 recovery mode remains non-sticky after both success and failure.
- Watchdog preserves every configured Tracker retry result.
- Recent last-good Tracker data begins as Attention and escalates at
  `[TrackerMonitor] stale_critical_after_minutes` (10 minutes by default).
- Last Tracker attempt and Last complete snapshot remain separate timestamps.
- R21's bounded 200-row Broker/log-alert monitoring history is retained.

The R24 change is Watchdog-only. If Bridge V174 is already installed from R22
or R23, only the rebuilt R24 Watchdog executable needs to be replaced.
