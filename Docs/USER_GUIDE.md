# MCST-Watchdog User Guide

## Purpose

The Dashboard is designed to answer one operational question quickly: **Is the monitored MultiCharts environment healthy?**

Normal operation should require no Developer Mode tools.

## Starting the system

1. Start the required MultiCharts64 instances.
2. Ensure the supplied Tracker Bridge PowerLanguage host is active in each monitored instance.
3. Start `C:\MCExtras\MCST-Watchdog.exe`.
4. Confirm that the Dashboard reaches the expected state.

## Reading the Dashboard

MCST uses four operational states:

- `HEALTHY` — the subsystem is operating as expected.
- `WARNING` — attention may be required, but the condition is not yet treated as critical.
- `CRITICAL` — a configured production condition requires attention.
- `UNKNOWN` — Watchdog does not have enough verified information to claim a valid state.

`UNKNOWN` is intentional fail-safe behavior. MCST prefers an explicit unknown state over guessing.

The System Status area includes Bridge, Tracker Snapshot, AutoTrading, Broker, Recent Logs, Status Reports, Email, and Heartbeat state. Latest Activity and process/resource information provide additional context.

The Dashboard also displays the detected MultiCharts version and compatibility information where available. A readable MultiCharts version does not by itself authorize internal-memory access; build-dependent readers require a verified compatibility profile.

## Normal controls

The normal production control row includes actions such as:

- `Refresh`
- `Save Report`
- `Open Folder`
- `Open Settings`
- `Reload Settings`

System Status rows with a `...` button provide focused configuration and subsystem-specific actions.

## Developer Mode controls

Developer Mode is disabled by default:

```ini
[Developer]
enabled=false
```

When enabled, research controls appear as a **compact, lower-height toolbar** that is visually distinct from the normal production buttons. The compact layout intentionally leaves room for future MultiCharts compatibility-research tools.

Current AutoTrading research controls include:

- `Start AT Research`
- `Capture AT Snapshot`
- `Finish AT Research`

These controls are not required for normal production monitoring.

## Self-documenting configuration

`MCST-Watchdog.ini` is stored beside the executable. MCST intentionally treats it as a **self-documenting configuration file**.

When a known setting is missing, Watchdog writes the setting with its safe built-in default. Invalid Boolean or numeric values are normalized where appropriate. This allows an existing INI file to grow automatically when new supported settings are introduced.

User-specific values and secrets are not invented. For example, email addresses, SMTP user names, and SMTP passwords may remain empty until the user configures them.

Configuration normalization changes are recorded in:

```text
MCST-Watchdog-ConfigNormalization.log
```

The `[DetectedMultiCharts]` section is different from normal settings: it is diagnostic information written by Watchdog and should not be maintained manually.

## Email settings

The Email settings panel separates alert and report recipients. Alerts are intended for urgent operational events, while routine Status Reports and Heartbeats can be sent to a different mailbox.

### Password / App Password

The Email settings field is labelled **Password / App Password** because the correct credential depends on the email provider.

Use the SMTP credential required by the provider. When an application-specific password is supported or required, use an **App Password** instead of the normal interactive account password. Gmail normally requires a Google App Password for this type of SMTP authentication. Other providers may use a different mechanism.

Leaving the Password / App Password field empty when editing Email settings preserves the currently stored value.

MCST does not intentionally include the configured credential in status reports, alert messages, or diagnostic logs.

## Status Reports

Status Reports provide a consolidated operational snapshot containing:

- Watchdog and Tracker Bridge versions
- detected MultiCharts version and executable
- selected compatibility profile
- overall and per-subsystem status
- latest activity
- system resources
- Watchdog process resources
- Accounts
- Open Positions
- Recent Logs

HTML reports use the same status semantics as the Dashboard.

Status Report scheduling is configured under `[StatusReport]`, including interval, startup behavior, weekdays, sending window, and recipient.

## Heartbeat

Heartbeat messages confirm that the monitoring system is still operating. They use the report email channel rather than the urgent alert channel.

The Heartbeat row menu can send a manual heartbeat immediately.

## AutoTrading monitor

AutoTrading monitoring reads MultiCharts internal state passively. Production reading is allowed only when the detected `Charting.dll` build matches a verified compatibility profile.

If the build is unknown, AutoTrading remains `UNKNOWN` rather than using an old address.

The configured minimum number of active strategies determines when the AutoTrading state becomes critical.

## Broker Monitor

The Broker Monitor consumes recognized events from MultiCharts Recent Logs and also supports configured browser-based authentication detection.

A disconnect or reconnect event starts a grace period. If recovery is recognized before the grace period expires, an outage alert is not sent. If the grace period expires, Broker state becomes critical and an alert can be sent. A recovery message is sent only after an alerted outage recovers.

A configured broker authentication/login page is treated as stronger current evidence than an older successful connection log.

Typical configuration:

```ini
[BrokerMonitor]
enabled=true
disconnect_grace_seconds=60
state_cache_max_age_minutes=1440
alert_email=true
recovery_email=true
disconnect_patterns=connection lost|connection disconnected|broker disconnected|connection closed
reconnecting_patterns=reconnecting|reconnect attempt|trying to connect
connected_patterns=connection restored|reconnected|connection established|logged on
```

Patterns are case-insensitive and separated with `|`.

## Browser-based broker authentication

Broker authentication profiles are data-driven. Example:

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

Prefer a stable `url_contains` value. Title and text terms are useful as additional or fallback evidence. Avoid overly generic match terms when a broker-specific URL is available.

The detector is passive. It does not enter credentials or interact with the page.

## Recent Logs alerts

The Log Alert Engine watches newly observed Tracker Logs rows and can send an alert when configured keywords are found.

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

Matching is case-insensitive and keywords are separated with `|`. Existing rows normally establish a startup baseline rather than generating historical alerts.

The Log Alert Engine is separate from the Broker State Engine.

## System resources

Status Reports include system-wide RAM, CPU, and system-drive information. Watchdog-specific private memory, handle count, and uptime are reported separately.

## Startup troubleshooting

If Watchdog closes before showing the Dashboard, inspect:

```text
C:\MCExtras\MCST-Watchdog-Startup.log
C:\Temp\MCST-Watchdog-Startup.log
```

If Watchdog starts but a subsystem is `UNKNOWN` or `CRITICAL`, use the row description, Latest Activity, Status Report, and the relevant configuration section to determine the cause before changing compatibility data.
