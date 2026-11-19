#pragma once

#include <string>

namespace snowdesktop::category_collection_rules
{

struct ShortcutTarget
{
    bool classified = false;
    bool directory = false;
    bool application = false;
    std::wstring extension;
};

} // namespace snowdesktop::category_collection_rules
