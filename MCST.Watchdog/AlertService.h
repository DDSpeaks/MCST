#pragma once

#include "AppConfig.h"
#include <string>

struct AutoTradingAlertDecision
{
    bool stateChanged = false;
    bool belowMinimum = false;
    bool sendEmail = false;
    std::wstring subject;
    std::wstring eventText;
};

class AutoTradingAlertTracker
{
public:
    AutoTradingAlertDecision Evaluate(int activeCount, const AppConfig& config);
    void Reset();

private:
    bool stateKnown_ = false;
    bool wasBelowMinimum_ = false;
};
