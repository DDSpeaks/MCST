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
    // Developer tools occupy their own labelled panel. The button row remains
    // clearly separated from the normal 34 px production controls below it.
    layout.panelTop = productionButtonY - 74;
    layout.panelBottom = productionButtonY - 18;
    layout.labelTop = layout.panelTop + 6;
    layout.top = productionButtonY - layout.buttonHeight - 22;
    return layout;
}

RECT CalculateDeveloperToolbarButtonRect(const DeveloperToolbarLayout& layout, int buttonIndex)
{
    const int left = layout.left + buttonIndex * (layout.buttonWidth + layout.horizontalGap);
    return RECT{ left, layout.top, left + layout.buttonWidth, layout.top + layout.buttonHeight };
}
