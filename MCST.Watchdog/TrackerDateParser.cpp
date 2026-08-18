#include "TrackerDateParser.h"

#include <algorithm>
#include <cwctype>

namespace
{
    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    bool ExtractDateNumbers(const std::wstring& value, int (&numbers)[3])
    {
        std::size_t position = 0;
        for (int part = 0; part < 3; ++part)
        {
            while (position < value.size() && !std::iswdigit(value[position]))
                ++position;
            if (position == value.size())
                return false;

            int number = 0;
            int digits = 0;
            while (position < value.size() && std::iswdigit(value[position]))
            {
                if (++digits > 4)
                    return false;
                number = number * 10 + (value[position] - L'0');
                ++position;
            }
            numbers[part] = number;

            if (part < 2)
            {
                if (position == value.size())
                    return false;
                const wchar_t separator = value[position];
                if (separator != L'/' && separator != L'.' &&
                    separator != L'-' && !std::iswspace(separator))
                {
                    return false;
                }
            }
        }
        return true;
    }

    TrackerDateOrder StructuralOrder(const std::wstring& value)
    {
        int parts[3]{};
        if (!ExtractDateNumbers(value, parts))
            return TrackerDateOrder::Auto;
        if (parts[0] >= 1000)
            return TrackerDateOrder::YearMonthDay;
        if (parts[2] < 1000)
            return TrackerDateOrder::Auto;
        if (parts[0] > 12 && parts[1] >= 1 && parts[1] <= 12)
            return TrackerDateOrder::DayMonthYear;
        if (parts[1] > 12 && parts[0] >= 1 && parts[0] <= 12)
            return TrackerDateOrder::MonthDayYear;
        return TrackerDateOrder::Auto;
    }

    bool IsLeapYear(int year)
    {
        return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    }

    bool IsValidDate(const TrackerCalendarDate& date)
    {
        if (date.year < 1601 || date.year > 9999 || date.month < 1 || date.month > 12)
            return false;
        static const int daysPerMonth[] = {
            31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
        };
        int days = daysPerMonth[date.month - 1];
        if (date.month == 2 && IsLeapYear(date.year))
            ++days;
        return date.day >= 1 && date.day <= days;
    }
}

TrackerDateOrder ParseTrackerDateOrder(const std::wstring& value)
{
    const std::wstring normalized = Lower(value);
    if (normalized == L"dmy" || normalized == L"day-month-year")
        return TrackerDateOrder::DayMonthYear;
    if (normalized == L"mdy" || normalized == L"month-day-year")
        return TrackerDateOrder::MonthDayYear;
    if (normalized == L"ymd" || normalized == L"year-month-day")
        return TrackerDateOrder::YearMonthDay;
    return TrackerDateOrder::Auto;
}

const wchar_t* TrackerDateOrderName(TrackerDateOrder order)
{
    switch (order)
    {
    case TrackerDateOrder::DayMonthYear: return L"DMY";
    case TrackerDateOrder::MonthDayYear: return L"MDY";
    case TrackerDateOrder::YearMonthDay: return L"YMD";
    default: return L"AUTO";
    }
}

TrackerDateOrder DetectTrackerDateOrder(
    const std::vector<std::wstring>& values,
    TrackerDateOrder fallbackOrder)
{
    TrackerDateOrder detected = TrackerDateOrder::Auto;
    for (const auto& value : values)
    {
        const TrackerDateOrder current = StructuralOrder(value);
        if (current == TrackerDateOrder::Auto)
            continue;
        if (detected != TrackerDateOrder::Auto && detected != current)
            return TrackerDateOrder::Auto;
        detected = current;
    }
    return detected == TrackerDateOrder::Auto ? fallbackOrder : detected;
}

bool TryParseTrackerCalendarDate(
    const std::wstring& value,
    TrackerDateOrder order,
    TrackerCalendarDate& result)
{
    result = TrackerCalendarDate{};
    int parts[3]{};
    if (!ExtractDateNumbers(value, parts))
        return false;

    if (order == TrackerDateOrder::Auto)
        order = StructuralOrder(value);
    switch (order)
    {
    case TrackerDateOrder::DayMonthYear:
        result = { parts[2], parts[1], parts[0] };
        break;
    case TrackerDateOrder::MonthDayYear:
        result = { parts[2], parts[0], parts[1] };
        break;
    case TrackerDateOrder::YearMonthDay:
        result = { parts[0], parts[1], parts[2] };
        break;
    default:
        return false;
    }
    return IsValidDate(result);
}
