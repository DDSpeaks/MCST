#pragma once
#include <algorithm>

namespace mcst
{
    inline bool IsVisibleRedWarning(unsigned int samples, unsigned int visible, unsigned int red)
    {
        return samples >= 12 && visible == samples && red >= 12 && red * 100 >= visible * 25;
    }

    inline void UpdateVisibleWarning(bool checked, bool red, int& redSamples,
        int& clearSamples, bool& confirmed)
    {
        if (!checked)
        {
            redSamples = clearSamples = 0;
            return; // Unknown visibility never clears a confirmed warning.
        }
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
