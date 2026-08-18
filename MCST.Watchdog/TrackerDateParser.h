#pragma once

#include <string>
#include <vector>

enum class TrackerDateOrder
{
    Auto,
    DayMonthYear,
    MonthDayYear,
    YearMonthDay
};

struct TrackerCalendarDate
{
    int year = 0;
    int month = 0;
    int day = 0;
};

TrackerDateOrder ParseTrackerDateOrder(const std::wstring& value);
const wchar_t* TrackerDateOrderName(TrackerDateOrder order);

// Resolves the order from unambiguous dates in the supplied rows. If every date
// is ambiguous, fallbackOrder is used. Conflicting structural evidence fails
// closed and returns Auto.
TrackerDateOrder DetectTrackerDateOrder(
    const std::vector<std::wstring>& values,
    TrackerDateOrder fallbackOrder);

// Reads only the calendar portion. Time text may follow the date and is ignored.
// Numeric DMY, MDY and YMD dates with '/', '.', '-', or spaces are supported.
bool TryParseTrackerCalendarDate(
    const std::wstring& value,
    TrackerDateOrder order,
    TrackerCalendarDate& result);
