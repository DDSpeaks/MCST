#pragma once

#include <string>
#include "../MCST.Shared/WatchdogSystemStatus.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"

std::wstring BuildStatusReport(
    const mcst::WatchdogSystemStatus& status,
    const TrackerStatusSnapshot& snapshot);

std::wstring BuildStatusReportHtml(const std::wstring& plainText);

// Builds an alert/heartbeat message by embedding the exact current Status Report HTML
// below a short alert introduction. This prevents alert emails from drifting to an
// older or separately formatted Status Report layout.
std::wstring BuildAlertWithStatusReportHtml(
    const std::wstring& alertIntroduction,
    const std::wstring& statusReportPlainText);

bool WriteUtf8TextFile(const std::wstring& path, const std::wstring& text, std::wstring& diagnostic);
