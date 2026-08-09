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

DeveloperToolbarLayout CalculateDeveloperToolbarLayout(int productionButtonY)
{
    DeveloperToolbarLayout layout;
    // Keep an 8 px visual gap between the compact research toolbar and
    // the normal 34 px production button row.
    layout.top = productionButtonY - layout.buttonHeight - 8;
    return layout;
}

RECT CalculateDeveloperToolbarButtonRect(const DeveloperToolbarLayout& layout, int buttonIndex)
{
    const int left = layout.left + buttonIndex * (layout.buttonWidth + layout.horizontalGap);
    return RECT{ left, layout.top, left + layout.buttonWidth, layout.top + layout.buttonHeight };
}
