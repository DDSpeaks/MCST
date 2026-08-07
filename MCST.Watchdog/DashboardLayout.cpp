#define NOMINMAX
#include "DashboardLayout.h"

#include <algorithm>

DashboardRowLayout CalculateDashboardRowLayout(int clientWidth)
{
    DashboardRowLayout layout;

    // Keep one reserved overflow-menu column for every System Status row.
    // Rows without a menu intentionally leave this area empty so all
    // descriptions terminate on the same visual boundary.
    layout.overflowButtonX = (std::max)(690, clientWidth - 58);
    layout.descriptionRight = (std::max)(layout.descriptionLeft + 40, layout.overflowButtonX - 14);
    return layout;
}
