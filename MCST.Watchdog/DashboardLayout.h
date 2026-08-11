#pragma once

#include <windows.h>

/**
 * @brief Fixed horizontal geometry shared by every System Status row.
 *
 * The description column always ends before the overflow-menu column, even
 * on rows that do not currently expose a menu button. This keeps the table
 * visually aligned and prevents long status text from entering the menu area.
 */
struct DashboardRowLayout
{
    int indicatorX = 42;
    int labelLeft = 71;
    int labelRight = 250;
    int stateLeft = 250;
    int stateRight = 410;
    int descriptionLeft = 405;
    int descriptionRight = 0;
    int overflowButtonX = 0;
    int overflowButtonWidth = 30;
};

DashboardRowLayout CalculateDashboardRowLayout(int clientWidth);
/**
 * @brief Compact geometry for Developer Mode research controls.
 *
 * Developer controls are intentionally smaller than normal production buttons.
 * Keeping their geometry centralized leaves room for additional compatibility
 * research actions without changing the production button row.
 */
struct DeveloperToolbarLayout
{
    int left = 28;
    int top = 0;
    int buttonWidth = 116;
    int buttonHeight = 24;
    int horizontalGap = 6;
};

DeveloperToolbarLayout CalculateDeveloperToolbarLayout(int productionButtonY);
RECT CalculateDeveloperToolbarButtonRect(const DeveloperToolbarLayout& layout, int buttonIndex);

