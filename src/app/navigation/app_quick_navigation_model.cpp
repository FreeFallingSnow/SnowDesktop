#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "common/l10n.h"
#include "widget/runtime/widget_engine.h"
#include "common/utils.h"
#include "icons/icon_render_rules.h"
#include "quick_navigation_helpers.h"
#include "navigation/quick_navigation_rules.h"
#include "navigation/search_match.h"
#include <commoncontrols.h>

// Quick-navigation search caches, app indexing and content-model construction.

std::vector<DesktopApp::QuickNavigationAppEntry>
DesktopApp::BuildQuickNavigationAppIndex(HWND ownerHwnd,
    HIMAGELIST& systemImageListSmall, int iconSourceSize)
{
    std::vector<QuickNavigationAppEntry> entries;
    systemImageListSmall = nullptr;
    iconSourceSize = snowdesktop::icon_render_rules::
        SourcePixelsForTarget(iconSourceSize);

    ComPtr<IImageList> iconImageList;
    HRESULT imageListHr = SHGetImageList(
        SHIL_EXTRALARGE, IID_IImageList,
        reinterpret_cast<void**>(iconImageList.GetAddressOf()));
    if (FAILED(imageListHr) || !iconImageList)
    {
        iconImageList.Reset();
        SHGetImageList(
            SHIL_LARGE, IID_IImageList,
            reinterpret_cast<void**>(iconImageList.GetAddressOf()));
    }

    PIDLIST_ABSOLUTE rawAppsPidl = nullptr;
    if (FAILED(SHParseDisplayName(L"shell:AppsFolder", nullptr, &rawAppsPidl, 0, nullptr)) ||
        rawAppsPidl == nullptr)
    {
        return entries;
    }
    Pidl appsPidl;
    appsPidl.reset(rawAppsPidl);

    ComPtr<IShellFolder> appsFolder;
    if (FAILED(SHBindToObject(nullptr, appsPidl.get(), nullptr,
        IID_IShellFolder, reinterpret_cast<void**>(appsFolder.GetAddressOf()))) ||
        !appsFolder)
    {
        return entries;
    }

    ComPtr<IEnumIDList> enumerator;
    HWND enumOwner = IsWindow(ownerHwnd) ? ownerHwnd : nullptr;
    if (FAILED(appsFolder->EnumObjects(enumOwner, SHCONTF_NONFOLDERS, &enumerator)) || !enumerator)
        return entries;

    std::unordered_set<std::wstring> seen;
    PITEMID_CHILD child = nullptr;
    ULONG fetched = 0;
    while (enumerator->Next(1, &child, &fetched) == S_OK)
    {
        PIDLIST_ABSOLUTE absolute = ILCombine(appsPidl.get(), child);
        if (!absolute)
        {
            ILFree(child);
            continue;
        }

        SHFILEINFOW info{};
        DWORD_PTR imageList = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(absolute), 0,
            &info, sizeof(info), SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_DISPLAYNAME);
        if (imageList)
            systemImageListSmall = reinterpret_cast<HIMAGELIST>(imageList);

        std::wstring name = info.szDisplayName[0]
            ? info.szDisplayName
            : StrRetToString(appsFolder.Get(), child, SHGDN_NORMAL);
        std::wstring parsingName = StrRetToString(appsFolder.Get(), child, SHGDN_FORPARSING);
        std::wstring key = ToUpperInvariant(parsingName.empty() ? name : parsingName);
        if (name.empty() || key.empty() || seen.contains(key))
        {
            ILFree(absolute);
            ILFree(child);
            continue;
        }
        seen.insert(std::move(key));

        QuickNavigationAppEntry entry;
        entry.name = std::move(name);
        entry.parsingName = std::move(parsingName);
        entry.iconCacheIdentity =
            snowdesktop::quick_navigation_rules::
                ApplicationIconCacheIdentity(
                    entry.parsingName, entry.name);
        entry.absolutePidl.reset(absolute);
        entry.systemIconIndex = imageList ? info.iIcon : -1;
        if (iconImageList && entry.systemIconIndex >= 0)
        {
            HICON icon = nullptr;
            if (SUCCEEDED(iconImageList->GetIcon(
                    entry.systemIconIndex,
                    ILD_TRANSPARENT | ILD_PRESERVEALPHA,
                    &icon)) && icon)
            {
                entry.iconBitmap = CreateAlphaBitmapFromIcon(
                    icon, iconSourceSize, iconSourceSize,
                    entry.iconBitmapSize);
                DestroyIcon(icon);
            }
        }
        entries.push_back(std::move(entry));

        ILFree(child);
    }

    std::stable_sort(entries.begin(), entries.end(),
        [](const QuickNavigationAppEntry& a, const QuickNavigationAppEntry& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
    return entries;
}

void DesktopApp::StartQuickNavigationAppIndexing()
{
    if (quickNavigation_->appsIndexed || quickNavigation_->appIndexing) return;
    const auto serial = ++quickNavigation_->appIndexSerial;
    const int size = QuickNavScale(28);
    quickNavigation_->appIndexing = appIndexWork_.Submit(L"apps", [serial, size] {
        auto result = std::make_shared<QuickNavigationAppIndexResult>();
        result->serial = serial;
        result->entries = BuildQuickNavigationAppIndex(nullptr,
            result->systemImageListSmall, size);
        return result;
    }, [this](std::shared_ptr<QuickNavigationAppIndexResult> result) {
        if (result) OnQuickNavigationAppsIndexed(0,
            reinterpret_cast<LPARAM>(new QuickNavigationAppIndexResult(std::move(*result))));
        else quickNavigation_->appIndexing = false;
    }, hwnd_, kBackgroundShellReadyMessage);
}

void DesktopApp::StopQuickNavigationAppIndexing()
{
    ++quickNavigation_->appIndexSerial;
    appIndexWork_.Stop();
    quickNavigation_->appIndexing = false;
}

void DesktopApp::OnQuickNavigationAppsIndexed(WPARAM /*wParam*/, LPARAM lParam)
{
    std::unique_ptr<QuickNavigationAppIndexResult> result(
        reinterpret_cast<QuickNavigationAppIndexResult*>(lParam));


    quickNavigation_->appIndexing = false;

    if (!result || result->serial != quickNavigation_->appIndexSerial)
        return;

    quickNavigation_->appIconCache.clear();
    quickNavigation_->appEntries = std::move(result->entries);
    if (result->systemImageListSmall)
        quickNavigation_->systemImageListSmall = result->systemImageListSmall;
    quickNavigation_->appsIndexed = true;
    quickNavigation_->appsExpanded = false;
    if (widgetEngine_)
        widgetEngine_->NotifyDesktopChanged("applications");

    if (!GetQuickNavigationEffectiveSearchText().empty() ||
        navigationSettings_.favoritesEnabled)
    {
        RefreshQuickNavigationAppResults();
        quickNavigation_->scrollOffset = std::clamp(quickNavigation_->scrollOffset, 0,
            GetQuickNavigationMaxScrollOffset(quickNavigation_->rect));
        if (quickNavigation_->open)
        {
            if (quickNavigation_->collapsed) PositionQuickNavigationWindow();
            InvalidateQuickNavigationWindow();
        }
    }
}

void DesktopApp::RefreshQuickNavigationAppResults()
{
    quickNavigation_->appResultIndices.clear();
    const std::wstring query = GetQuickNavigationEffectiveSearchText();
    if (quickNavigation_->searchType != QuickNavigationSearchType::All && quickNavigation_->searchType != QuickNavigationSearchType::App) return;
    if (query.empty())
        return;
    if (IsLuaLogicalSlotPickerOpen() &&
        !LuaLogicalSlotPickerAccepts("app.reference"))
        return;

    StartQuickNavigationAppIndexing();
    if (!quickNavigation_->appsIndexed)
        return;

    for (size_t i = 0; i < quickNavigation_->appEntries.size(); ++i)
    {
        const QuickNavigationAppEntry& entry = quickNavigation_->appEntries[i];
        const std::wstring displayName =
            !IsLuaLogicalSlotPickerOpen() &&
            generalSettings_.demoModeEnabled &&
            demoIdentityAssetsAvailable_
            ? GetDemoIdentityTitle(entry.parsingName)
            : entry.name;
        if (!NameMatchesQuery(displayName, query))
            continue;
        quickNavigation_->appResultIndices.push_back(i);
    }

    std::stable_sort(quickNavigation_->appResultIndices.begin(), quickNavigation_->appResultIndices.end(),
        [&](size_t a, size_t b) {
            const auto displayName = [&](size_t index) {
                const auto& entry = quickNavigation_->appEntries[index];
                return !IsLuaLogicalSlotPickerOpen() &&
                    generalSettings_.demoModeEnabled &&
                    demoIdentityAssetsAvailable_
                    ? GetDemoIdentityTitle(entry.parsingName)
                    : entry.name;
            };
            return NameSearchMatchRank(displayName(a), query) <
                NameSearchMatchRank(displayName(b), query);
        });

    if (quickNavigation_->appResultIndices.size() > kQuickNavigationAppResultLimit)
        quickNavigation_->appResultIndices.resize(kQuickNavigationAppResultLimit);
}

const DesktopApp::QuickNavigationAppEntry*
DesktopApp::FindQuickNavigationEverythingAppEntry() const
{
    if (!quickNavigation_->appsIndexed)
        return nullptr;

    const QuickNavigationAppEntry* bestEntry = nullptr;
    int bestRank = kNameSearchNoMatchRank;
    for (const auto& entry : quickNavigation_->appEntries)
    {
        const int rank = NameSearchMatchRank(entry.name, L"Everything");
        if (rank < bestRank)
        {
            bestRank = rank;
            bestEntry = &entry;
            if (rank == 0)
                break;
        }
    }
    return bestEntry;
}

const DesktopApp::QuickNavigationAppEntry*
DesktopApp::FindQuickNavigationAppEntryByParsingName(
    const std::wstring& parsingName) const
{
    if (!quickNavigation_->appsIndexed || parsingName.empty())
        return nullptr;
    const std::wstring normalized = ToUpperInvariant(parsingName);
    for (const auto& entry : quickNavigation_->appEntries)
    {
        if (ToUpperInvariant(entry.parsingName) == normalized)
            return &entry;
    }
    return nullptr;
}

std::wstring DesktopApp::GetQuickNavigationEverythingNoticeText() const
{
    if (!quickNavigation_->appsIndexed)
        return _LW("app.interact.everything_not_running");
    return FindQuickNavigationEverythingAppEntry()
        ? _LW("app.interact.everything_click_start")
        : _LW("app.nav.everything_download");
}

bool DesktopApp::TryLaunchQuickNavigationEverythingApp()
{
    const QuickNavigationAppEntry* entry = FindQuickNavigationEverythingAppEntry();
    return entry &&
        LaunchQuickNavigationAppEntry(*entry);
}

std::vector<size_t> DesktopApp::GetQuickNavigationCollectionIndices() const
{
    return snowdesktop::quick_navigation_rules::
        OrderTabWidgetIndices(quickNavigation_->tabOrder, widgets_);
}

std::vector<std::wstring> DesktopApp::GetQuickNavigationItemKeys() const
{
    std::vector<std::wstring> result;
    std::unordered_set<std::wstring> seen;
    auto appendKey = [&](const std::wstring& key) {
        if (FindItemIndexByKey(key) == static_cast<size_t>(-1))
            return;
        std::wstring normalized = ToUpperInvariant(key);
        if (normalized.empty() || seen.contains(normalized))
            return;
        seen.insert(std::move(normalized));
        result.push_back(key);
    };

    if (quickNavigation_->activeWidgetIndex == static_cast<size_t>(-1))
    {
        std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
        for (size_t ci : collectionIndices)
        {
            if (widgets_[ci].type == DesktopWidgetType::FolderMapping) continue;
            const auto& widget = widgets_[ci];
            for (const auto& itemKey : widget.itemKeys)
                appendKey(itemKey);
        }
        return result;
    }
    if (quickNavigation_->activeWidgetIndex == static_cast<size_t>(-2))
    {
        std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
        for (size_t ci : collectionIndices)
        {
            if (widgets_[ci].type != DesktopWidgetType::FolderMapping) continue;
            for (const auto& fe : widgets_[ci].folderEntries)
                appendKey(fe.fullPath);
        }
        return result;
    }

    if (quickNavigation_->activeWidgetIndex < widgets_.size() &&
        (widgets_[quickNavigation_->activeWidgetIndex].type == DesktopWidgetType::Collection ||
         widgets_[quickNavigation_->activeWidgetIndex].type == DesktopWidgetType::FileCategories))
    {
        const auto& widget = widgets_[quickNavigation_->activeWidgetIndex];
        for (const auto& itemKey : widget.itemKeys)
            appendKey(itemKey);
        return result;
    }
    if (quickNavigation_->activeWidgetIndex < widgets_.size() &&
        widgets_[quickNavigation_->activeWidgetIndex].type == DesktopWidgetType::FolderMapping)
    {
        for (const auto& fe : widgets_[quickNavigation_->activeWidgetIndex].folderEntries)
            appendKey(fe.fullPath);
        return result;
    }

    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    for (size_t ci : collectionIndices)
    {
        if (widgets_[ci].type == DesktopWidgetType::FolderMapping)
        {
            for (const auto& fe : widgets_[ci].folderEntries)
                appendKey(fe.fullPath);
        }
        else
        {
            const auto& widget = widgets_[ci];
            for (const auto& itemKey : widget.itemKeys)
                appendKey(itemKey);
        }
    }
    return result;
}

DesktopApp::QuickNavigationContentModel
DesktopApp::BuildQuickNavigationContentModel(const RECT* overlay) const
{
    QuickNavigationContentModel model;
    if (quickNavigation_->searchType != QuickNavigationSearchType::All && quickNavigation_->searchType != QuickNavigationSearchType::File) return model;
    std::unordered_set<std::wstring> seenDesktop;
    std::unordered_set<std::wstring> dockDesktopKeys;
    for (const DockEntry& dockEntry : dockEntries_)
    {
        if (dockEntry.type == DockEntryType::DesktopItem)
            dockDesktopKeys.insert(ToUpperInvariant(dockEntry.reference));
    }
    const std::wstring query = GetQuickNavigationEffectiveSearchText();
    auto matches = [&](const std::wstring& name) {
        return query.empty() || NameMatchesQuery(name, query);
    };
    auto appendDesktop = [&](
        std::vector<QuickNavigationEntry>& destination,
        size_t itemIndex, const std::wstring& source,
        size_t demoCollectionIndex = static_cast<size_t>(-1),
        std::unordered_set<std::wstring>* seen = nullptr) {
        if (itemIndex >= items_.size()) return;
        auto& visibleKeys = seen ? *seen : seenDesktop;
        const DesktopItem& item = items_[itemIndex];
        std::wstring key = ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
        if (key.empty() || visibleKeys.contains(key)) return;
        if (demoCollectionIndex >= widgets_.size() &&
            generalSettings_.demoModeEnabled &&
            demoIdentityAssetsAvailable_)
        {
            for (size_t widgetIndex = 0;
                widgetIndex < widgets_.size(); ++widgetIndex)
            {
                const auto& candidate = widgets_[widgetIndex];
                if (candidate.type != DesktopWidgetType::Collection)
                    continue;
                auto itemIt = std::find_if(candidate.itemKeys.begin(),
                    candidate.itemKeys.end(), [&](const auto& candidateKey) {
                        return ToUpperInvariant(candidateKey) == key;
                    });
                if (itemIt == candidate.itemKeys.end())
                    continue;
                demoCollectionIndex = widgetIndex;
                break;
            }
        }
        const std::wstring_view identity = item.layoutKey.empty()
            ? std::wstring_view(item.parsingName)
            : std::wstring_view(item.layoutKey);
        const DesktopWidget* demoCollection =
            demoCollectionIndex < widgets_.size()
            ? &widgets_[demoCollectionIndex] : nullptr;
        const std::wstring displayName =
            !IsLuaLogicalSlotPickerOpen() &&
                ShouldUseDemoCollectionIdentity(demoCollection)
            ? GetDemoCollectionIdentityTitle(*demoCollection, identity)
            : (!IsLuaLogicalSlotPickerOpen() && ShouldUseDemoIdentity(item)
                ? GetDemoIdentityTitle(identity) : item.name);
        if (!matches(displayName)) return;
        visibleKeys.insert(std::move(key));

        QuickNavigationEntry entry;
        entry.kind = QuickNavigationEntry::Kind::DesktopItem;
        entry.itemIndex = itemIndex;
        entry.demoCollectionIndex = demoCollectionIndex;
        entry.name = displayName;
        entry.path = item.parsingName;
        entry.source = source;
        if (!CanPickLuaLogicalSlotEntry(entry)) return;
        destination.push_back(std::move(entry));
    };
    auto appendDockDesktopItems =
        [&](std::vector<QuickNavigationEntry>& destination,
            std::unordered_set<std::wstring>* seen = nullptr) {
        for (const DockEntry& dockEntry : dockEntries_)
        {
            if (dockEntry.type != DockEntryType::DesktopItem)
                continue;
            appendDesktop(
                destination,
                FindItemIndexByKey(dockEntry.reference),
                _LW("app.nav.source_dock"),
                static_cast<size_t>(-1), seen);
        }
    };
    auto appendSection = [&](
        const std::wstring& label,
        std::vector<QuickNavigationEntry>& entries,
        bool showWhenEmpty = false,
        bool favoriteSection = false) {
        if (entries.empty() && !showWhenEmpty) return;
        const size_t first = model.entries.size();
        model.entries.insert(
            model.entries.end(),
            std::make_move_iterator(entries.begin()),
            std::make_move_iterator(entries.end()));
        model.sections.push_back({
            label, first, model.entries.size() - first,
            showWhenEmpty, favoriteSection});
    };
    auto widgetTitle = [&](const DesktopWidget& widget) {
        if (ShouldUseDemoCollectionIdentity(&widget))
            return GetDemoCollectionCategoryTitle(widget);
        if (!widget.title.empty()) return widget.title;
        if (widget.type ==
            DesktopWidgetType::FileCategories)
            return std::wstring(
                _LW("widget.desktop_files"));
        if (widget.type ==
            DesktopWidgetType::FolderMapping)
            return std::wstring(
                _LW("widget.folder_mapping"));
        return std::wstring(_LW("widget.collection"));
    };
    auto orderedWidgetIndices =
        [&](auto predicate) {
        std::vector<std::wstring> widgetIds;
        std::vector<bool> eligible;
        widgetIds.reserve(widgets_.size());
        eligible.reserve(widgets_.size());
        for (const auto& widget : widgets_)
        {
            widgetIds.push_back(widget.id);
            eligible.push_back(predicate(widget));
        }
        return snowdesktop::quick_navigation_rules::
            OrderIndicesByTabIds(
                quickNavigation_->tabOrder, widgetIds, eligible);
    };
    const auto groupByInitial = [&] {
        auto entries = std::move(model.entries);
        model.entries.clear();
        model.sections.clear();
        std::array<std::vector<QuickNavigationEntry>, 27> buckets;
        for (auto& entry : entries)
        {
            const wchar_t initial = snowdesktop::quick_navigation_rules::InitialBucket(entry.name);
            buckets[initial >= L'A' && initial <= L'Z' ? static_cast<size_t>(initial - L'A') : 26]
                .push_back(std::move(entry));
        }
        for (size_t i = 0; i < buckets.size(); ++i)
        {
            auto& bucket = buckets[i];
            std::stable_sort(bucket.begin(), bucket.end(), [](const auto& lhs, const auto& rhs) {
                return snowdesktop::quick_navigation_rules::InitialNameLess(lhs.name, rhs.name);
            });
            appendSection(i < 26 ? std::wstring(1, static_cast<wchar_t>(L'A' + i)) : L"#", bucket);
        }
    };
    const auto groupWidgetEntriesByInitial = [&] {
        if (GetQuickNavigationViewMode() == QuickNavigationDesktopViewMode::Initial)
            groupByInitial();
    };

    if (query.empty())
    {
        if (quickNavigation_->activeWidgetIndex == static_cast<size_t>(-1))
        {
            const auto mode = navigationSettings_.desktopViewMode;
            const bool showSuggestions = snowdesktop::quick_navigation_rules::ShowAppSuggestions(
                navigationSettings_, true, false, IsLuaLogicalSlotPickerOpen(), UseQuickNavigationList());
            const bool showFavorites = snowdesktop::quick_navigation_rules::ShowFavorites(
                navigationSettings_, true, false, IsLuaLogicalSlotPickerOpen(), UseQuickNavigationList());
            std::vector<QuickNavigationEntry> tileEntries;
            if (mode == QuickNavigationDesktopViewMode::Tile ||
                mode == QuickNavigationDesktopViewMode::Frequency || showSuggestions)
            {
                appendDockDesktopItems(tileEntries);
                for (const auto& key :
                    GetQuickNavigationItemKeys())
                    appendDesktop(
                        tileEntries,
                        FindItemIndexByKey(key),
                        _LW("widget.collection"));

                std::unordered_set<std::wstring>
                    desktopKeys;
                for (const auto& widget : widgets_)
                {
                    if (widget.type !=
                            DesktopWidgetType::Collection &&
                        widget.type !=
                            DesktopWidgetType::FileCategories)
                        continue;
                    for (const auto& key :
                        widget.itemKeys)
                        desktopKeys.insert(
                            ToUpperInvariant(key));
                }

                auto isLnkOrUrl =
                    [](const std::wstring& path) {
                    if (path.size() < 4)
                        return false;
                    std::wstring ext =
                        path.substr(path.size() - 4);
                    for (auto& c : ext)
                        c = static_cast<wchar_t>(
                            towupper(c));
                    return ext == L".LNK" ||
                        ext == L".URL";
                };
                for (size_t i = 0;
                    i < items_.size(); ++i)
                {
                    const DesktopItem& item = items_[i];
                    const std::wstring key =
                        ToUpperInvariant(
                            item.layoutKey.empty()
                            ? item.parsingName
                            : item.layoutKey);
                    if (desktopKeys.contains(key) ||
                        !isLnkOrUrl(
                            item.parsingName))
                        continue;
                    appendDesktop(
                        tileEntries, i,
                        _LW("app.interact.free_desktop"));
                }
                // Source and Initial build their own complete, deduplicated lists.
                seenDesktop.clear();
            }
            std::vector<int> launchCounts;
            launchCounts.reserve(tileEntries.size());
            for (const auto& entry : tileEntries)
            {
                const auto& item = items_[entry.itemIndex];
                const auto key = ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
                const auto record = dockUsageStats_.find(key);
                launchCounts.push_back(IsDockUsageEligibleItem(item) && record != dockUsageStats_.end()
                    ? record->second.launchCount : 0);
            }
            if (mode == QuickNavigationDesktopViewMode::Frequency)
            {
                for (size_t index : snowdesktop::quick_navigation_rules::LaunchFrequencyOrder(launchCounts))
                    model.entries.push_back(std::move(tileEntries[index]));
                return model;
            }
            const auto finishDesktop = [&](QuickNavigationContentModel content) {
                std::vector<QuickNavigationEntry> prefixEntries;
                std::vector<QuickNavigationSection> prefixSections;
                if (showSuggestions)
                {
                    const auto order = snowdesktop::quick_navigation_rules::LaunchFrequencyOrder(
                        launchCounts, true, static_cast<size_t>(GetQuickNavigationColumnCount(
                            overlay ? *overlay : quickNavigation_->rect)));
                    if (!order.empty())
                    {
                        const size_t first = prefixEntries.size();
                        for (size_t index : order)
                            prefixEntries.push_back(tileEntries[index]);
                        prefixSections.push_back({
                            _LW("app.nav.app_suggestions"),
                            first, prefixEntries.size() - first});
                    }
                }
                if (showFavorites)
                {
                    const size_t first = prefixEntries.size();
                    const std::wstring favoriteLabel = _LW("app.nav.favorites");
                    for (const auto& favorite : navigationSettings_.favorites)
                    {
                        QuickNavigationEntry entry;
                        entry.source = favoriteLabel;
                        const std::wstring target = Utf8ToWide(favorite.target);
                        switch (favorite.type)
                        {
                        case QuickNavigationFavoriteType::File:
                        case QuickNavigationFavoriteType::Folder:
                            if (target.empty())
                                break;
                            entry.kind = QuickNavigationEntry::Kind::PathFavorite;
                            entry.path = target;
                            entry.isDirectory =
                                favorite.type == QuickNavigationFavoriteType::Folder;
                            entry.name = Utf8ToWide(favorite.name);
                            if (entry.name.empty())
                                entry.name = FileNameFromPath(target);
                            if (!entry.name.empty())
                                prefixEntries.push_back(std::move(entry));
                            break;
                        case QuickNavigationFavoriteType::DesktopItem:
                        {
                            size_t itemIndex = FindItemIndexByKey(
                                Utf8ToWide(favorite.desktopKey));
                            if (itemIndex >= items_.size())
                            {
                                const std::wstring normalizedTarget =
                                    ToUpperInvariant(target);
                                itemIndex = static_cast<size_t>(-1);
                                for (size_t index = 0;
                                    index < items_.size(); ++index)
                                {
                                    if (ToUpperInvariant(
                                            items_[index].parsingName) ==
                                        normalizedTarget)
                                    {
                                        itemIndex = index;
                                        break;
                                    }
                                }
                            }
                            if (itemIndex >= items_.size())
                                break;
                            entry.kind = QuickNavigationEntry::Kind::DesktopItem;
                            entry.itemIndex = itemIndex;
                            entry.name = items_[itemIndex].name;
                            entry.path = items_[itemIndex].parsingName;
                            if (entry.name.empty())
                                entry.name = Utf8ToWide(favorite.name);
                            if (!entry.name.empty())
                                prefixEntries.push_back(std::move(entry));
                            break;
                        }
                        case QuickNavigationFavoriteType::Application:
                        {
                            const auto* app =
                                FindQuickNavigationAppEntryByParsingName(target);
                            if (!app)
                                break;
                            entry.kind =
                                QuickNavigationEntry::Kind::ApplicationFavorite;
                            entry.name = app->name;
                            entry.path = app->parsingName;
                            if (entry.name.empty())
                                entry.name = Utf8ToWide(favorite.name);
                            if (!entry.name.empty())
                                prefixEntries.push_back(std::move(entry));
                            break;
                        }
                        }
                    }
                    prefixSections.push_back({
                        favoriteLabel, first,
                        prefixEntries.size() - first, true, true});
                }
                if (prefixSections.empty())
                    return content;
                if (content.sections.empty() && !content.entries.empty())
                    content.sections.push_back({
                        _LW("app.nav.tab_desktop"), 0,
                        content.entries.size()});
                const size_t prefixCount = prefixEntries.size();
                for (auto& section : content.sections)
                    section.firstEntryIndex += prefixCount;
                prefixEntries.insert(
                    prefixEntries.end(),
                    std::make_move_iterator(content.entries.begin()),
                    std::make_move_iterator(content.entries.end()));
                prefixSections.insert(
                    prefixSections.end(),
                    std::make_move_iterator(content.sections.begin()),
                    std::make_move_iterator(content.sections.end()));
                content.entries = std::move(prefixEntries);
                content.sections = std::move(prefixSections);
                return content;
            };
            if (navigationSettings_.desktopViewMode ==
                QuickNavigationDesktopViewMode::Source)
            {
                const auto sourceWidgets =
                    orderedWidgetIndices(
                        [](const DesktopWidget& widget) {
                            return widget.type ==
                                    DesktopWidgetType::Collection ||
                                widget.type ==
                                    DesktopWidgetType::FileCategories;
                        });

                // Source view keeps one identity per source section. An item
                // may intentionally appear in several sections, including
                // both Dock and its original collection.
                std::unordered_set<std::wstring> classifiedKeys;
                for (const DockEntry& dockEntry : dockEntries_)
                {
                    if (dockEntry.type == DockEntryType::DesktopItem)
                        classifiedKeys.insert(
                            ToUpperInvariant(dockEntry.reference));
                }
                for (size_t sourceIndex = 0;
                    sourceIndex < sourceWidgets.size();
                    ++sourceIndex)
                {
                    for (const auto& key :
                        widgets_[sourceWidgets[sourceIndex]].itemKeys)
                        classifiedKeys.insert(ToUpperInvariant(key));
                }

                std::vector<QuickNavigationEntry>
                    dockEntries;
                std::unordered_set<std::wstring> dockSeen;
                appendDockDesktopItems(dockEntries, &dockSeen);
                appendSection(
                    _LW("app.nav.source_dock"),
                    dockEntries);

                for (size_t sourceIndex = 0;
                    sourceIndex <
                        sourceWidgets.size();
                    ++sourceIndex)
                {
                    const size_t widgetIndex =
                        sourceWidgets[sourceIndex];
                    const DesktopWidget& widget =
                        widgets_[widgetIndex];
                    std::vector<
                        QuickNavigationEntry>
                        entries;
                    std::unordered_set<std::wstring> widgetSeen;
                    for (const auto& key : widget.itemKeys)
                    {
                        const size_t itemIndex =
                            FindItemIndexByKey(key);
                        appendDesktop(
                            entries, itemIndex,
                            widgetTitle(widget), widgetIndex,
                            &widgetSeen);
                    }
                    appendSection(
                        widgetTitle(widget),
                        entries);
                }

                std::vector<QuickNavigationEntry>
                    looseEntries;
                for (size_t i = 0;
                    i < items_.size(); ++i)
                {
                    const DesktopItem& item = items_[i];
                    const std::wstring key = ToUpperInvariant(
                        item.layoutKey.empty()
                            ? item.parsingName
                            : item.layoutKey);
                    if (classifiedKeys.contains(key))
                        continue;
                    appendDesktop(
                        looseEntries, i,
                        _LW("app.interact.free_desktop"));
                }
                appendSection(
                    _LW("app.interact.free_desktop"),
                    looseEntries);
                return finishDesktop(std::move(model));
            }

            if (navigationSettings_.desktopViewMode ==
                QuickNavigationDesktopViewMode::Initial)
            {
                for (size_t i = 0;
                    i < items_.size(); ++i)
                    appendDesktop(
                        model.entries, i,
                        _LW("app.nav.tab_desktop"));
                groupByInitial();
                return finishDesktop(std::move(model));
            }

            if (showSuggestions) model.entries = tileEntries;
            else model.entries = std::move(tileEntries);
            return finishDesktop(std::move(model));
        }

        if (quickNavigation_->activeWidgetIndex == static_cast<size_t>(-2))
        {
            const auto mappingWidgets =
                orderedWidgetIndices(
                    [](const DesktopWidget& widget) {
                        return widget.type ==
                            DesktopWidgetType::
                                FolderMapping;
                    });
            for (size_t wi : mappingWidgets)
            {
                std::vector<QuickNavigationEntry>
                    entries;
                const std::wstring source =
                    widgetTitle(widgets_[wi]);
                for (size_t ei = 0; ei < widgets_[wi].folderEntries.size(); ++ei)
                {
                    const FolderEntry& entryData = widgets_[wi].folderEntries[ei];
                    QuickNavigationEntry entry;
                    entry.kind = QuickNavigationEntry::Kind::FolderEntry;
                    entry.widgetIndex = wi;
                    entry.folderEntryIndex = ei;
                    entry.name = entryData.DisplayName();
                    entry.path = entryData.fullPath;
                    entry.source = source;
                    if (CanPickLuaLogicalSlotEntry(entry))
                        entries.push_back(std::move(entry));
                }
                if (navigationSettings_.mappingViewMode == QuickNavigationDesktopViewMode::Source)
                    appendSection(source, entries);
                else
                    model.entries.insert(model.entries.end(), std::make_move_iterator(entries.begin()),
                        std::make_move_iterator(entries.end()));
            }
            if (navigationSettings_.mappingViewMode == QuickNavigationDesktopViewMode::Initial)
                groupByInitial();
            return model;
        }

        if (quickNavigation_->activeWidgetIndex < widgets_.size() &&
            widgets_[quickNavigation_->activeWidgetIndex].type == DesktopWidgetType::FolderMapping)
        {
            const DesktopWidget& widget = widgets_[quickNavigation_->activeWidgetIndex];
            std::wstring source = widget.title.empty() ? _LW("widget.folder_mapping") : widget.title;
            for (size_t ei = 0; ei < widget.folderEntries.size(); ++ei)
            {
                const FolderEntry& entryData = widget.folderEntries[ei];
                QuickNavigationEntry entry;
                entry.kind = QuickNavigationEntry::Kind::FolderEntry;
                entry.widgetIndex = quickNavigation_->activeWidgetIndex;
                entry.folderEntryIndex = ei;
                entry.name = entryData.DisplayName();
                entry.path = entryData.fullPath;
                entry.source = source;
                if (CanPickLuaLogicalSlotEntry(entry))
                    model.entries.push_back(std::move(entry));
            }
            groupWidgetEntriesByInitial();
            return model;
        }

        for (const auto& key : GetQuickNavigationItemKeys())
            appendDesktop(
                model.entries,
                FindItemIndexByKey(key),
                _LW("widget.collection"),
                quickNavigation_->activeWidgetIndex < widgets_.size()
                    ? quickNavigation_->activeWidgetIndex
                    : static_cast<size_t>(-1));
        groupWidgetEntriesByInitial();
        return model;
    }

    appendDockDesktopItems(model.entries);
    for (size_t i = 0; i < items_.size(); ++i)
        appendDesktop(
            model.entries, i,
            _LW("app.nav.tab_desktop"));

    for (size_t ci : GetQuickNavigationCollectionIndices())
    {
        const DesktopWidget& widget = widgets_[ci];
        if (widget.type == DesktopWidgetType::FolderMapping) continue;
        std::wstring source = widgetTitle(widget);
        for (const auto& key : widget.itemKeys)
        {
            appendDesktop(
                model.entries,
                FindItemIndexByKey(key), source, ci);
        }
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        const DesktopWidget& widget = widgets_[wi];
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;
        std::wstring source = widget.title.empty() ? _LW("widget.folder_mapping") : widget.title;
        for (size_t ei = 0; ei < widget.folderEntries.size(); ++ei)
        {
            const FolderEntry& entryData = widget.folderEntries[ei];
            if (!matches(entryData.name))
                continue;

            QuickNavigationEntry entry;
            entry.kind = QuickNavigationEntry::Kind::FolderEntry;
            entry.widgetIndex = wi;
            entry.folderEntryIndex = ei;
            entry.name = entryData.DisplayName();
            entry.path = entryData.fullPath;
            entry.source = source;
            if (CanPickLuaLogicalSlotEntry(entry))
                model.entries.push_back(std::move(entry));
        }
    }

    std::stable_sort(
        model.entries.begin(),
        model.entries.end(),
        [&](const QuickNavigationEntry& a, const QuickNavigationEntry& b) {
            auto isDockEntry = [&](const QuickNavigationEntry& entry) {
                if (entry.kind != QuickNavigationEntry::Kind::DesktopItem ||
                    entry.itemIndex >= items_.size())
                    return false;
                const DesktopItem& item = items_[entry.itemIndex];
                return dockDesktopKeys.contains(ToUpperInvariant(
                    item.layoutKey.empty() ? item.parsingName : item.layoutKey));
            };
            const bool aIsDockEntry = isDockEntry(a);
            const bool bIsDockEntry = isDockEntry(b);
            if (aIsDockEntry != bIsDockEntry)
                return aIsDockEntry;
            if (aIsDockEntry)
                return false;
            return NameSearchMatchRank(a.name, query) <
                NameSearchMatchRank(b.name, query);
        });
    return model;
}

std::vector<DesktopApp::QuickNavigationEntry>
DesktopApp::GetQuickNavigationEntries() const
{
    return BuildQuickNavigationContentModel().
        entries;
}
