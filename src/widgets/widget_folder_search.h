#pragma once

#include "common/types.h"
#include "navigation/quick_navigation_search_async.h"
#include <memory>
#include <unordered_map>
#include <vector>

class Item;
class DesktopApp;

namespace snowdesktop
{
std::shared_ptr<Item> CaptureSearchDragItem(const Item& item, DesktopApp* app);
inline UINT WidgetFolderSearchReadyMessage()
{
    static const UINT message = RegisterWindowMessageW(L"SnowDesktop.WidgetFolderSearchReady");
    return message;
}
struct WidgetFolderSearchState
{
    struct Source
    {
        std::wstring id;
        std::wstring title;
        std::vector<std::wstring> roots;
    };
    struct Row
    {
        std::wstring sourceId;
        std::wstring desktopKey;
        size_t folderIndex = 0;
        std::shared_ptr<FolderEntry> recursive;
    };
    struct Section
    {
        std::wstring title;
        size_t first = 0;
        size_t count = 0;
        size_t total = 0;
        std::wstring key;
        bool recursive = false;
    };
    std::wstring identity;
    std::wstring sourceIdentity;
    std::wstring modelIdentity;
    std::wstring query;
    std::vector<Source> sources;
    std::vector<Row> rows;
    std::vector<Row> allRows;
    std::vector<Section> sections;
    std::unordered_map<std::wstring, size_t> visibleLimits;
    std::vector<EverythingSearchResult> results;
    std::unique_ptr<QuickNavigationEverythingSearchAsync> worker;
    // Inject only the external SDK boundary in native integration tests.
    QuickNavigationEverythingSearchAsync::SearchFunction search;
    std::uint64_t generation = 0;
    bool available = false;
    bool pending = false;
    bool refreshRequested = false;
};
}
