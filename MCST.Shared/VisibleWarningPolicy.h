#pragma once
#include <algorithm>

namespace mcst
{
    inline bool IsConfirmedRenderedWarning(bool completed, unsigned int redPasses)
    {
        return completed && redPasses == 2;
    }

    inline bool IsVisibleRedWarning(unsigned int samples, unsigned int visible, unsigned int red)
    {
        return samples >= 12 && visible == samples && red >= 12 && red * 100 >= visible * 25;
    }

    inline void UpdateVisibleWarning(bool checked, bool red, int& redSamples,
        int& clearSamples, int& uncheckedSamples, bool& confirmed)
    {
        if (!checked)
        {
            redSamples = clearSamples = 0;
            if (confirmed)
            {
                uncheckedSamples = (std::min)(uncheckedSamples + 1, 3);
                if (uncheckedSamples >= 3)
                    confirmed = false;
            }
            else
            {
                uncheckedSamples = 0;
            }
            return; // Keep two grace samples, then expire stale visual evidence.
        }
        uncheckedSamples = 0;
        if (red)
        {
            redSamples = (std::min)(redSamples + 1, 2);
            clearSamples = 0;
            if (redSamples >= 2) confirmed = true;
        }
        else
        {
            redSamples = 0;
            clearSamples = (std::min)(clearSamples + 1, 2);
            if (clearSamples >= 2) confirmed = false;
        }
    }
}
