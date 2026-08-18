# MCST 1.114-R25 Status Lines and Monthly Realized P/L

Tracker Bridge internal build: V175  
Bridge protocol: V2

## Status Report layout

- Overall Status and System Status now use the same non-wrapping, preformatted
  HTML flow as the other report sections.
- Every status component remains on one logical line and may continue to the
  right on a narrow display.
- The colored state dot occupies a fixed four-character cell, so the status and
  description columns remain vertically aligned.
- Semantic status tables were removed because iOS Mail could scale them to a
  much smaller type size.
- Overall Status, System Status, and Open Positions all enforce the same
  15-pixel monospaced typography.

## Current-month Realized P/L

- Bridge V175 reads the Positions History grid through the existing validated,
  read-only FlexGrid path and adds an optional `position_history` payload
  section.
- The page offset `0x78` is enabled only for the exact verified V147
  `ATOnPTracker.dll` fingerprint. External compatibility profiles can provide
  `tracker_position_history_page_offset`; existing profiles remain valid when
  the optional key is absent.
- Watchdog recalculates the current month's Realized P/L from the complete
  captured history on every report. It never accumulates refreshes, so repeated
  rows cannot be double-counted across snapshots.
- Only values with an unambiguous currency sign or ISO code are totalled.
- The amount is aligned directly below the Open P/L column and uses the same
  full-value green/red styling, including currency and sign.
- The monthly total remains visible even when the Open Positions grid has no
  current rows.
- A capture that reaches the 5,000-row safety limit is rejected as incomplete;
  Watchdog never presents a potentially partial monthly total as complete.
- Position History failure is report enrichment failure only. It does not alter
  the three existing Tracker health sections, recovery count, Broker state, or
  overall Critical classification.

## Locale-aware dates

- Numeric DMY, MDY, and YMD dates are supported with `/`, `.`, `-`, or spaces.
- Automatic detection uses unambiguous Position History rows and then the
  Windows user locale for ambiguous dates.
- `[Tracker] date_order=auto|dmy|mdy|ymd` provides an explicit override.
- Invalid dates fail closed and are reported as skipped; time text is ignored.

## Installation note

Both `MCST-Watchdog.exe` and `MCST-TrackerBridge.dll` must be rebuilt and
replaced. Restart MultiCharts after replacing the DLL. Protocol V2 is retained.
