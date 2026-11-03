#pragma once

#include <string>
#include <vector>

struct CategoryRule
{
    std::wstring id;
    std::wstring customLabel;
    std::wstring extensions;
    bool enabled = true;
};

struct CategorySettings
{
    float tabFontSize = 15.0f;
    bool collectProgramsEnabled = false;
    std::vector<CategoryRule> rules;

    static CategorySettings Defaults();
};
