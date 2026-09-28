#pragma once

namespace mcst
{
    inline bool IsAutomaticAutoTradingProfileStructurallyValid(
        int objects, int active, int readFailures)
    {
        return objects >= 2 && active >= 0 && active <= objects && readFailures == 0;
    }
}
