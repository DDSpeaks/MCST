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
