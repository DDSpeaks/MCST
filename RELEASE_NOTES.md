# MCST-Watchdog 1.01 Release Notes

## Broker Monitor Foundation

Version 1.01 introduces the first production-oriented broker connection monitor. It
uses the Recent Logs rows already delivered by Bridge V155 and applies a state machine
instead of alerting on a single log line.

When a disconnect or reconnect attempt is detected, the Dashboard enters a yellow
`Reconnecting` state. The program waits for the configured grace period, 60 seconds by
default. If a recognized recovery event arrives during that period, no outage email is
sent. If the connection is still unavailable when the grace period expires, the Broker
row becomes red and one alert email is sent. A single recovery email is sent after an
alerted outage recovers.

The default patterns are intentionally configurable because broker and MultiCharts log
wording can differ. Review and adapt them using real log lines from the production
broker profile.

## Universal Application Mapper

The MCST release now documents the Universal Application Mapper as an optional,
standalone Developer Tool. Its default runtime path is
`C:\MCExtras\UniversalApplicationMapper.exe`. The executable is not part of the
Watchdog source solution and must be copied separately when its build is available.

## Compatibility

Bridge Protocol V155 and the verified AutoTrading compatibility profile remain
unchanged.
