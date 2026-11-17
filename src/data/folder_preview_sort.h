#pragma once

#include <map>
#include <string>

namespace snowdesktop::folder_preview
{
struct Sort
{
    int mode;
    bool ascending;

    bool operator==(const Sort&) const = default;
};

// Paths are the same normalized directory keys used by Shell refresh.
class SortPreferences
{
public:
    Sort Get(const std::wstring& key);
    bool Set(const std::wstring& key, Sort sort);

private:
    void Load();
    bool loaded_ = false;
    std::map<std::wstring, Sort> entries_;
};
}
