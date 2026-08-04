#pragma once

#include <string>
#include "../MCST.Shared/WatchdogSystemStatus.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"

std::wstring BuildStatusReport(
    const mcst::WatchdogSystemStatus& status,
    const TrackerStatusSnapshot& snapshot);

bool WriteUtf8TextFile(const std::wstring& path, const std::wstring& text, std::wstring& diagnostic);
