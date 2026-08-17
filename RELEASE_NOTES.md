# MCST 1.114-R20 Tracker Snapshot Self-Recovery

Tracker Bridge internal build: V172  
Bridge protocol: V2

R20 addresses a long-lived state in which Bridge, AutoTrading, and Broker were
healthy but Tracker Snapshot and Recent Logs stayed Critical until Watchdog was
restarted. R19's mobile Status Report correction is included in full.

## R20 changes

- Bridge V172 clears only its own CATPTTabView discovery caches when an
  authorized production snapshot is incomplete, captures current process/window
  state again, and performs one bounded fresh read-only scan.
- Expensive fresh scans have a 30-second Bridge-side cooldown; normal lightweight
  snapshot requests and Watchdog retries continue during that interval.
- Watchdog retries parsed-but-incomplete Tracker responses; previously such a
  response ended the retry loop even when all table sections had failed.
- The retry is restricted to a same-process Tracker with a matched compatibility
  profile. An unknown build remains blocked and is not scanned repeatedly.
- Watchdog stores the last complete Tracker snapshot in memory. If the current
  read fails, those tables remain available for context but are explicitly marked
  **STALE** with their timestamp while the current health stays **CRITICAL**.
- Stale Recent Logs are display-only. BrokerMonitor and LogAlertEngine continue
  to evaluate only the current live read result.
- A later complete snapshot automatically clears the stale state and records a
  recovery activity; a Watchdog restart should not normally be necessary.
- The Bridge recovery path does not write MultiCharts memory, manipulate Tracker
  windows, generate input, or invoke unknown functions.

## Included report appearance correction

- Open Positions remains a semantic HTML table with the same explicit 15-pixel
  monospaced font as the rest of the report.
- Mobile viewport and text-size-adjust protections remain enabled.
- The wide table remains inside a horizontal-scroll container.
- R18's Open P/L total placement and complete green/red profit styling remain.
- Stale table data receives a prominent red warning in HTML reports.

The retained R16 Position CCY diagnostic requires Bridge V171 or newer. Normal
production snapshots still have a V156 protocol minimum, while this R20
self-recovery behavior requires the included V172 Bridge.
