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

## Broker Authentication Signal Priority

The Broker State Engine uses current evidence before historical evidence:

1. A configured browser authentication URL/profile match.
2. Explicit Recent Logs disconnect/reconnect events.
3. Reconnect grace timer state.
4. Unknown when no reliable evidence is available.

`BrokerAuthDetector` reuses the original production Watchdog's passive Win32 browser-window and
child-control text inspection. Profiles are data-driven through `[BrokerAuth.*]` INI sections.
A confirmed login page cannot be overridden by an older successful connection log. Recovery from
an authentication alert can also require broker-specific log terms from the same profile, which
prevents a different broker's connection message from clearing the alert.
