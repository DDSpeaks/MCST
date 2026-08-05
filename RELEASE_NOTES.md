# MCST-Watchdog 1.06

## Configurable Log Alert Engine

This release adds email alerts for selected messages in MultiCharts Recent Logs.

The engine is configured through the `[LogAlerts]` section in `MCST-Watchdog.ini`. Keyword lists use `|` as the separator and matching is case-insensitive.

Default critical keywords include rejected orders and invalid stop prices. Existing log rows are treated as a startup baseline and are not emailed by default. New matching rows are grouped into one HTML email per snapshot. Duplicate messages are suppressed for 60 minutes by default.

Example configuration:

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

Bridge Protocol V155, Broker State detection and the AutoTrading Compatibility Framework are unchanged.
