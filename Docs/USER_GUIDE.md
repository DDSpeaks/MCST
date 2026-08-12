# MCST-Watchdog User Guide

## Purpose

The Dashboard is designed to answer one operational question quickly: **Is MultiCharts healthy?**

The overall status summarizes the subsystem rows. Green/OK represents a verified healthy state, warning indicates attention is required, critical means monitoring or a monitored condition has failed, and unknown means MCST does not have enough verified information to make a safe claim.

## Normal operation

1. Start MultiCharts and make sure the MCST Tracker Bridge host is running in each monitored MultiCharts instance.
2. Start `C:\MCExtras\MCST-Watchdog.exe`.
3. Confirm the expected status for Bridge, Tracker Snapshot, AutoTrading, Broker, Recent Logs, Status Reports, Email, and Heartbeat.
4. Leave Watchdog running during trading operation.

The Dashboard uses **Latest Activity** as its single activity summary. There is no separate Recent Activity section.

## Configuration

`MCST-Watchdog.ini` is stored beside the Watchdog executable.

MCST intentionally uses a **self-documenting INI** model. Missing known settings are written with safe built-in defaults, and invalid Boolean or bounded numeric values are normalized where appropriate. This allows the INI file itself to show the configuration keys known by the installed version.

User-specific addresses, account identifiers, usernames, passwords, and App Passwords are never invented. Generated sections such as `[DetectedMultiCharts]` are diagnostic output and should not be treated as user settings.

Use **Open Settings** on the Dashboard to open the INI file in the system's associated text editor. Use **Reload Settings** after manual configuration changes. Reload Settings refreshes the Windows INI profile view and starts a configuration-generation-safe refresh, so values such as the AutoTrading minimum take effect without restarting the Watchdog. A background result created before the reload is discarded rather than being allowed to restore old settings on the Dashboard.

## Email Password / App Password

The Email settings field is labelled **Password / App Password** because the required credential depends on the email provider.

- If the provider requires an application-specific password, use that App Password.
- Gmail normally uses a Google App Password for this SMTP authentication style.
- Other providers may use a normal SMTP password or another provider-specific credential.

Do not assume that the normal interactive account password is correct. MCST does not intentionally expose the configured credential in reports, alerts, or diagnostic logs.

## Email channels

Urgent alerts and routine reports can be sent to different addresses.

- AutoTrading, Broker, and Log Alert messages use the alert recipient.
- Status Reports and Heartbeats use the report recipient.

This allows the alert mailbox to use stronger notification rules without making every routine status message urgent.

## Broker Monitor

Broker state is derived from recognized MultiCharts Recent Logs events plus optional broker-authentication browser profiles.

A disconnect/reconnect event starts a grace period. A successful recovery during the grace period cancels the pending outage alert. If the grace period expires, the Broker state becomes critical and an alert can be sent. A later verified recovery can produce a recovery message.

A configured browser authentication/login page is considered stronger current evidence than an older successful connection log. This prevents stale historical log evidence from incorrectly clearing a current authentication condition.

Broker authentication profiles are configured using sections such as:

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

Prefer stable URL terms over generic words when possible.

## Recent Logs alerts

The Log Alert Engine evaluates newly observed Recent Logs rows and can send alerts for configured keywords.

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

Matching is case-insensitive and keywords are separated with `|`. The Log Alert Engine is separate from the Broker State Engine.

## Status Reports and Heartbeats

Status Reports use the same operational state as the Dashboard and include MultiCharts/Bridge identity, selected compatibility profiles, system resources, Watchdog process information, Accounts, Open Positions, and Recent Logs. Tracker tables use calculated column widths so values remain vertically aligned. The final Recent Logs message field may extend freely after its aligned starting position. Open Positions also includes a derived `Position Value` field and a final `TOTALS` row for total Position Value and total Open P/L.

Heartbeat messages are routine proof-of-life messages and use the report recipient rather than the alert recipient.

## MultiCharts version and compatibility

Watchdog displays the human-readable MultiCharts version and records module fingerprints in the generated `[DetectedMultiCharts]` section.

The important distinction is:

- a product version tells you which MultiCharts release appears to be running;
- an exact module fingerprint determines whether MCST is authorized to use build-dependent internal values.

AutoTrading and Tracker compatibility are evaluated independently. A new MultiCharts build may therefore leave one subsystem verified while another becomes `UNKNOWN` or unreadable until its module profile is verified.

For Tracker compatibility, a new `ATOnPTracker.dll` build without a verified profile is deliberately blocked rather than read using guessed offsets. The Bridge creates a disabled candidate entry to support later Developer Mode research.

## Developer Mode

Developer Mode is disabled by default:

```ini
[Developer]
enabled=false
```

When enabled, the compact Developer toolbar contains:

- **AT Start** — begins a controlled AutoTrading research session.
- **AT Capture** — captures the current AutoTrading research state.
- **AT Finish** — completes/analyzes the AutoTrading research session.
- **Tracker Capture** — requests the passive Tracker research bundle from the Bridge and opens the research-output location.
- **Open Compat** — opens `MCST-Compatibility.ini`.
- **Reload Compat** — forces a fresh compatibility evaluation for AutoTrading and Tracker.

These controls are intentionally smaller than normal production buttons and are hidden in normal operation.

### Tracker profile workflow

When a MultiCharts update changes `ATOnPTracker.dll`:

1. Watchdog/Bridge records the new fingerprint.
2. The Tracker reader remains safely unavailable if no verified profile matches.
3. A disabled `Candidate.ATOnPTracker-*` section is created automatically.
4. Use **Tracker Capture** and other Developer Mode research tools to discover the new build-dependent values.
5. Verify the values through controlled research.
6. Create or complete an enabled `Profile.*` section with the exact fingerprint and verified values.
7. Use **Reload Compat**.
8. Confirm that Tracker Snapshot, Accounts, Open Positions, and Recent Logs are readable and that the selected profile is reported.

Never enable unverified candidate values simply to remove an `UNKNOWN` state.
