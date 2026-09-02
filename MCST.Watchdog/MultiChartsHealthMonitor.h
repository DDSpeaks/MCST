#pragma once

#include "../MCST.Shared/WatchdogSystemStatus.h"

// Lightweight, read-only MultiCharts process monitor.
// The function enumerates MultiCharts desktop processes, samples inexpensive
// Win32 resource counters, checks the main window with a bounded timeout and
// reads the visible q/s queue indicator without clicking or using Clipboard.
mcst::MultiChartsHealthSnapshot ReadMultiChartsHealth();

