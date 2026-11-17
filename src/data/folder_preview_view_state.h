#pragma once

#include <map>
#include <optional>
#include <string>

namespace snowdesktop::folder_preview
{
struct ViewPage
{
    std::wstring path;
    int scroll = 0;
    bool operator==(const ViewPage&) const = default;
};

struct SavedView
{
    std::wstring sourceWidgetId;
    std::optional<ViewPage> page;
    bool listMode = false;
    bool dateHeaders = false;
    bool operator==(const SavedView&) const = default;
};

class ViewPreferences
{
public:
    std::optional<SavedView> Get(const std::wstring& key);
    bool Set(const std::wstring& key, SavedView view);

private:
    void Load();
    bool loaded_ = false;
    std::map<std::wstring, SavedView> entries_;
};
}
