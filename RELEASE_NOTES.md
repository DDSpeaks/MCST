# MCST-Watchdog 1.03 Release Notes

Version 1.03 fixes broker recovery detection observed during production testing.

The Broker Monitor now recognizes provider-specific successful connection messages,
including `Connection with TradeStation established.`, by using semantic token matching
in addition to configurable INI patterns. A successful recovery event immediately clears
an existing reconnecting or critical state. If the connection returned during the grace
period, no alert is sent. If a critical alert was already sent, one recovery email is sent.

Negative messages such as reconnect failures, timeouts and disconnected states are
explicitly excluded from recovery classification.

Bridge Protocol V155, the AutoTrading reader, Compatibility Framework and HTML Status
Report remain unchanged.
