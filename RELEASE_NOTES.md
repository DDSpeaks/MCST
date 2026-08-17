# MCST 1.114-R22 Tracker Recovery and Readable Reports

Tracker Bridge internal build: V174
Bridge protocol: V2

R22 addresses the observed long-lived state where Order and Position Tracker
and its Logs grid were visible, but Watchdog repeatedly reported that the
CATPTTabView object was not found. It also corrects stale-state severity,
snapshot timestamps, and the tiny Open Positions table in iOS Mail.

## Tracker recovery

- Recovery is now non-sticky: a failed pass cannot disable later normal
  discovery.
- Bridge recovery mode now exists only while one bounded exact-profile scan is
  executing. Its scope guard releases the mode after both success and failure.
- A failed exact-profile scan therefore cannot suppress normal RTTI discovery
  on later Watchdog requests.
- Normal candidate discovery caches both positive and empty results for 30
  seconds. This restores recovery opportunities without producing continuous
  process-wide memory scans.
- The existing exact-profile recovery scan remains limited to one per 30-second
  window and remains read-only.
- Watchdog preserves the recovery result from every configured retry, so a
  first `incomplete` scan is not hidden by a final `cooldown` response.

## Stale-data severity and timestamps

- When a complete earlier snapshot exists, a failed current read starts as
  Attention rather than immediately making the whole system Critical.
- The new `[TrackerMonitor] stale_critical_after_minutes` setting defaults to
  10 minutes and may be configured from 1 to 1440 minutes.
- The state escalates to Critical when that interval expires. A Bridge outage,
  wholly unreadable current Tracker snapshot with no last-good data,
  AutoTrading failure, Broker failure, or actual critical Log Alert remains
  immediately Critical.
- Dashboard and Status Report now distinguish Last Tracker attempt from Last
  complete snapshot.
- The stale warning is amber during Attention and red after Critical escalation.

## Open Positions email layout

- The HTML table no longer uses `width:100%` or unsupported
  `min-width:max-content` sizing.
- It has an explicit 1100-pixel width, 15-pixel monospaced text, and non-wrapping
  cells inside the existing horizontal-scroll container.
- This prevents iOS Mail from shrinking the table to tiny text while retaining
  desktop readability and horizontal panning on narrow screens.

R21's 200-row live Broker/log-alert history, R20 retry foundations, R18 Open P/L
placement/colors, and the retained R16 Position CCY diagnostic remain included.
R22 requires the V174 Bridge from this package. Restart MultiCharts once after
replacing the DLL so V174 is loaded.
