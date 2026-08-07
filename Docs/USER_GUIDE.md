# MCST-Watchdog User Guide

## Main objective

The Dashboard answers one operational question: **Is MultiCharts healthy?**

## Normal operation

1. Start MultiCharts and the required instances.
2. Start `C:\MCExtras\MCST-Watchdog.exe`.
3. Confirm that Bridge, Tracker Snapshot, AutoTrading, Email, Heartbeat, Status Reports, and resource status show the expected state.
4. Leave the Dashboard visible during trading operation.

## Configuration

`MCST-Watchdog.ini` is stored beside the executable. Missing known settings are written with safe defaults. User-specific SMTP addresses, usernames, and passwords are never invented.

## Diagnostics

Developer and research functions are intended for troubleshooting and MultiCharts compatibility work. They are not required for ordinary production use.

## Broker Monitor

The Broker row is driven by recognized events in MultiCharts Recent Logs. A detected
connection loss starts a recovery grace period. During the grace period the row is
shown as `Reconnecting` and no email is sent. The default delay is 60 seconds.

If a recognized recovery event appears before the delay expires, the event is recorded
but no outage alert is sent. If the delay expires first, the Broker row becomes
`Disconnected` and one alert email is sent. A recovery email is sent only when an
alerted outage later recovers.

Configure the monitor in `MCST-Watchdog.ini`:

```ini
[BrokerMonitor]
enabled=true
disconnect_grace_seconds=60
alert_email=true
recovery_email=true
disconnect_patterns=connection lost|connection disconnected|broker disconnected|connection closed
reconnecting_patterns=reconnecting|reconnect attempt|trying to connect
connected_patterns=connection restored|reconnected|connection established|logged on
```

Pattern text is case-insensitive. Use the vertical bar (`|`) to separate patterns.
The defaults are a safe starting point, but production patterns should be verified
against the actual broker log wording.


## Recent Logs email alerts

Version 1.06 adds a dedicated Log Alert Engine. It watches only newly observed rows in the Order and Position Tracker **Logs** tab and can send an HTML email when a configured keyword is found.

Configuration is stored in `MCST-Watchdog.ini`:

```ini
[LogAlerts]
enabled=true
email_enabled=true
notify_existing_on_startup=false
deduplication_minutes=60
fatal_keywords=fatal|unhandled exception|access violation|application crash
critical_keywords=status: rejected|order: rejected|invalid stop price|order failed|boxed positions are not permitted
warning_keywords=
ignore_keywords=simulated trades are not shown on historical data
```

Keywords are separated with `|` and matching is case-insensitive. Any one matching keyword is sufficient. The default critical list catches rejected orders such as `Invalid Stop Price`. Existing rows are used only as a startup baseline unless `notify_existing_on_startup=true`.

The Log Alert Engine is separate from the Broker State Engine. Changing Fatal, Critical or Warning keywords does not change broker connection recovery detection.


## Email channels (1.07)

Use separate recipients for alerts and routine reports:

```ini
[Email]
alert_to=alerts@example.com
report_to=reports@example.com

[StatusReport]
to=reports@example.com
```

AutoTrading, Broker and Log Alert messages use `alert_to`. Status Reports and Heartbeats use the Status Report recipient.

The **Status Settings** button edits the Status Report recipient, interval, startup behavior, weekdays and sending window. AutoTrading research buttons are shown only in Developer Mode.


## Dashboard row menus (1.09)

The AutoTrading, Status Reports, Email, and Heartbeat rows include a compact `...` button. Use it to open the focused settings panel or run a subsystem-specific action. AutoTrading research commands are shown only when `[Developer] enabled=true`.

## Browser-based Broker Authentication Detection

Broker authentication pages are treated as stronger current evidence than historical Recent Logs.
The Watchdog passively reads supported browser address bars by the proven Win32 browser window/child-text inspection used by the original production Watchdog. It does not
interact with the page or credentials.

The default Saxo profile is written automatically when missing:

```ini
[BrokerAuth.Saxo]
enabled=true
name=Saxo
url_contains=developer.saxobank.com/login
title_contains=MultiCharts (OpenAPI Web App)|Saxo
text_contains=login|account authentication
recovery_log_contains=saxo|saxo group
alert_after_seconds=10
```

If the configured login URL remains visible for the configured delay, Broker status becomes
CRITICAL and the normal broker alert channel is used. A login page therefore overrides an older
"connection established" log entry. URL query strings and fragments are deliberately removed from
Watchdog diagnostics and email.

To add another broker, create another section such as `[BrokerAuth.MyBroker]`. Prefer a stable
`url_contains` value. Multiple alternatives can be separated with `|`. If no stable URL is
available, `title_contains` and `text_contains` can be used as a fallback.

## Status Report System Resources

The Status Report SYSTEM RESOURCES section reports system-wide RAM, CPU and the Windows system
Drive. RAM and disk rows include total capacity, free capacity and used percentage. CPU includes
logical processor capacity and current usage. Watchdog-specific private memory, handle count and
uptime are shown separately under WATCHDOG PROCESS.

In HTML email, SYSTEM STATUS values include the same green/amber/red/gray visual status-dot
language used by the Dashboard.

Example for a second broker profile:

```ini
[BrokerAuth.TradeStation]
enabled=true
name=TradeStation
url_contains=auth.tradestation.com|signin.tradestation.com|login.tradestation.com
title_contains=TradeStation
text_contains=login|sign in|authentication
recovery_log_contains=tradestation
alert_after_seconds=10
```

The exact URL terms should be based on the broker's real authentication page observed in the
production environment. A configured URL match is preferred over generic words such as `login`
because it avoids false alerts from unrelated browser tabs.
