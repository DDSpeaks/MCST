# MCST Architecture

## Purpose

MCST-Watchdog provides an operational view of MultiCharts health at a glance.

## Main layers

1. **MultiCharts** — the monitored trading platform.
2. **MCST-TrackerBridge** — the stable tracker snapshot boundary using Bridge Protocol V155.
3. **MCST-Watchdog Core** — combines tracker, AutoTrading, scheduling, email, alerts, and resource state.
4. **Dashboard** — presents the fixed-layout operational status.
5. **Developer and Research Tools** — support diagnostics and future compatibility work without changing the production bridge protocol.

## Stability rule

The bridge protocol should remain stable unless new information must cross the bridge boundary. Features that can be implemented inside Watchdog should not force a bridge update.
