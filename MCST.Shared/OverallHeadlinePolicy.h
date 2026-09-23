#pragma once

#include "WatchdogSystemStatus.h"

namespace mcst
{
    inline const wchar_t* OverallHeadline(HealthState state, bool firstUpdateCompleted)
    {
        switch (state)
        {
        case HealthState::Healthy: return L"SYSTEM HEALTHY";
        case HealthState::Attention: return L"ATTENTION REQUIRED";
        case HealthState::Critical: return L"CRITICAL CONDITION";
        default: return firstUpdateCompleted ? L"CHECK INCOMPLETE" : L"INITIALIZING";
        }
    }
}
