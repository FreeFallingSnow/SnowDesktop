#pragma once

#include <string>

namespace snowdesktop::app_fonts
{
struct Selection
{
    std::string package = "system";
    std::string family;
    bool operator==(const Selection&) const = default;
};

struct Choice
{
    Selection selection;
    std::wstring name;
    std::wstring xamlSource;
};


}
