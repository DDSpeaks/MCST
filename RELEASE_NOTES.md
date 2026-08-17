# MCST 1.114-R23 Mobile Position Cards

Tracker Bridge internal build: V174
Bridge protocol: V2

R23 corrects the Open Positions size problem confirmed by an iOS Mail capture
from R22. Other preformatted report sections remained at the intended size, but
iOS Mail scaled the fixed 1100-pixel position table as one visual object. The
overflow container did not prevent that client-specific scaling.

## Open Positions HTML layout

- The wide nine-column HTML table and horizontal-scroll wrapper are removed.
- Every position is rendered as a normal-width card with four compact lines:
  symbol/side/quantity, profile/account, Average Price/Native Value, and
  Open P/L/Last Update.
- Cards, child fields, and totals explicitly retain the report's 15-pixel
  monospaced typography.
- Values use normal wrapping and `overflow-wrap:anywhere`; no Open Positions
  child can force the message viewport wider.
- Known-currency Open P/L totals remain immediately after the position cards.
- Positive profit values remain fully green and negative values fully red,
  including currency signs/codes and plus/minus signs.
- The plain-text/local Status Report keeps its aligned nine-column table.

## Retained R22 behavior

- Bridge V174 recovery mode remains non-sticky after both success and failure.
- Watchdog preserves every configured Tracker retry result.
- Recent last-good Tracker data begins as Attention and escalates at
  `[TrackerMonitor] stale_critical_after_minutes` (10 minutes by default).
- Last Tracker attempt and Last complete snapshot remain separate timestamps.
- R21's bounded 200-row Broker/log-alert monitoring history is retained.

The R23 change is Watchdog-only. If Bridge V174 is already installed from R22,
only the rebuilt R23 Watchdog executable needs to be replaced.
