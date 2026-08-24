#pragma once

#include <string>
#include <vector>

struct DeveloperHelpTopic
{
    std::wstring buttonLabel;
    std::wstring content;
};

const std::vector<DeveloperHelpTopic>& GetDeveloperHelpTopics();

