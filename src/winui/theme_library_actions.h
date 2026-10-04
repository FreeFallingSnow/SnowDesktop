#pragma once
#include "../theme_library.h"
#include <cstdint>
#include <functional>

namespace snowdesktop::winui
{
// Private settings UI commands; independent of package and bridge protocols.
enum class ThemeLibraryCommand { Refresh, SaveAs, Update, Apply, Detach, Remove, Export, Preview, Share, ChooseCover, ChooseBackground, Regenerate, SyncSubscriptions, Import, CopyLocal, BindWorkshop };
struct ThemeLibraryRequest
{
    ThemeLibraryCommand command = ThemeLibraryCommand::Refresh;
    std::string target = "global", id, name, quickPanel, popup, replacement;
    unsigned scopes = themes::Dock | themes::StatusBar | themes::Taskbar;
    std::vector<std::string> tags;
    bool chooseCover = false;
    bool chooseBackground = false;
};
struct ThemeLibraryResult
{
    bool succeeded = false;
    themes::Library library;
    std::string savedId;
    std::wstring message;
    bool sharingAvailable = false;
    std::map<std::string, std::string> publishedUrls;
};
using ThemeLibraryAction = std::function<ThemeLibraryResult(std::uint64_t, const ThemeLibraryRequest&)>;
using ThemeLibraryAsyncAction = std::function<void(std::uint64_t, ThemeLibraryRequest, std::function<void(ThemeLibraryResult)>)>;
}
