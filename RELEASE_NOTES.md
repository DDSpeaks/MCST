# MCST-Watchdog 1.104

## Content-Sized Status Columns

The Status Report no longer divides SYSTEM STATUS into percentage-based columns. The Component and Status columns now size themselves to their non-wrapping monospaced content, while the Description column receives all remaining width and wraps naturally. This follows the same robust behavior used by the report's lower data sections and works better across desktop, tablet, and narrow phone mail views.

OVERALL STATUS uses the same content-sized geometry. A hidden `Tracker Snapshot` sizing label keeps its status indicator on the same vertical line as the SYSTEM STATUS indicators without exposing extra text.

The unified Consolas / Courier New / monospace typography, colored HTML indicators, and all monitoring logic are unchanged.

See `CHANGELOG_1.104.txt` for details.
