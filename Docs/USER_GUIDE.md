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
