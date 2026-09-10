#pragma once

#include "WatchdogSystemStatus.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace mcst
{
    inline bool IsSameActivity(const ActivityItem& left, const ActivityItem& right)
    {
        return left.time == right.time &&
            left.state == right.state &&
            left.text == right.text;
    }

    inline void MergeActivityHistory(std::vector<ActivityItem>& current,
        const std::vector<ActivityItem>& previous, std::size_t maximumItems = 10)
    {
        std::vector<ActivityItem> merged;
        merged.reserve(current.size() + previous.size());

        const auto appendUnique = [&merged](const ActivityItem& candidate)
        {
            const bool alreadyPresent = std::any_of(
                merged.begin(), merged.end(),
                [&candidate](const ActivityItem& existing)
                {
                    return IsSameActivity(existing, candidate);
                });
            if (!alreadyPresent)
                merged.push_back(candidate);
        };

        for (const auto& item : current)
            appendUnique(item);
        for (const auto& item : previous)
            appendUnique(item);

        std::stable_sort(merged.begin(), merged.end(),
            [](const ActivityItem& left, const ActivityItem& right)
            {
                return left.time > right.time;
            });

        if (merged.size() > maximumItems)
            merged.resize(maximumItems);
        current.swap(merged);
    }
}
