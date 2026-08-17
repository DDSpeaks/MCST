# MCST 1.114-R21 Extended Broker History

Tracker Bridge internal build: V173  
Bridge protocol: V2

R21 fixes a Broker status error observed after Watchdog was started later than
the Order and Position Tracker. The Tracker's newest successful Saxo connection
event could already be below Watchdog's ten displayed log rows. Watchdog then
reported Broker as UNKNOWN even though the Tracker showed a newer successful
connection after an older disconnect event.

## R21 changes

- Bridge V173 reads the Logs grid once, bounded to 200 rows.
- The existing `recent_logs` section remains capped at ten rows for the
  Dashboard and Status Report.
- A new optional `monitoring_logs` section carries the same live capture up to
  200 rows for BrokerMonitor and LogAlertEngine.
- Broker events are still processed oldest to newest, so the latest relevant
  connection or disconnection event wins. Unrelated newer warnings do not erase
  the last confirmed state.
- Monitoring uses only the current live snapshot. R20's retained stale snapshot
  remains display-only and cannot create false Broker or log-alert decisions.
- Protocol V2 and the existing command set are unchanged; older readers ignore
  the optional section.

## Regression covered

The automated test reproduces the reported ordering: an older “No connection”
event, a newer successful Saxo connection, and more than ten still newer
unrelated UIC warnings. The expected Broker result is Connected.

## Retained corrections

- R20 Tracker snapshot self-recovery, retry, cooldown, and visible stale-data
  warning remain included.
- R19's mobile Status Report correction remains included: semantic Open
  Positions table, 15-pixel monospaced text, text-size protection, and
  horizontal scrolling.
- R18 Open P/L total placement and complete green/red value styling remain
  included.

Normal production snapshots still have a V156 protocol minimum. R21's extended
Broker history requires the included V173 Bridge. After replacing the DLL,
MultiCharts must be restarted once so that V173 is loaded.
