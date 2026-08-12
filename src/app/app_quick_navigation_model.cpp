#include "app.h"
#include "quick_navigation_helpers.h"
#include "quick_navigation_rules.h"
#include "search_match.h"

// Quick-navigation search caches, app indexing and content-model construction.

std::vector<EverythingSearchResult> DesktopApp::SearchEverythingCached(
    const std::wstring& query, DWORD maxResults) const
{
    if (query.empty() || maxResults == 0)
        return {};

    const DWORD now = GetTickCount();
    const bool cacheFresh = everythingSearchCacheQuery_ == query &&
        everythingSearchCacheMaxResults_ >= maxResults &&
        now - everythingSearchCacheTick_ < 2000;
    if (cacheFresh)
    {
        const size_t count = std::min<size_t>(everythingSearchCacheResults_.size(), maxResults);
        std::vector<EverythingSearchResult> cached;
        cached.reserve(count);
        for (size_t i = 0; i < count; ++i)
            cached.push_back(everythingSearchCacheResults_[i]);
        return cached;
    }

    everythingSearchCacheQuery_ = query;
    everythingSearchCacheMaxResults_ = maxResults;
    everythingSearchCacheTick_ = now;
    everythingSearchCacheResults_ = everythingSearch_.Search(query, maxResults);
    everythingSearchAvailable_ = (everythingSearch_.LastError() != 2); // 2 = EVERYTHING_ERROR_IPC, Everything not running
    const std::wstring normalizedQuery = ToUpperInvariant(query);
    std::stable_sort(everythingSearchCacheResults_.begin(), everythingSearchCacheResults_.end(),
        [&](const EverythingSearchResult& a, const EverythingSearchResult& b) {
            const std::wstring aName = a.name.empty() ? FileNameFromPath(a.path) : a.name;
            const std::wstring bName = b.name.empty() ? FileNameFromPath(b.path) : b.name;
            const int aRank = NameSearchMatchRank(aName, normalizedQuery);
            const int bRank = NameSearchMatchRank(bName, normalizedQuery);
            if (aRank != bRank)
                return aRank < bRank;

            const std::wstring aNameKey = ToUpperInvariant(aName);
            const std::wstring bNameKey = ToUpperInvariant(bName);
            if (aNameKey != bNameKey)
                return aNameKey < bNameKey;

            const int timeCmp = CompareFileTime(&a.dateModified, &b.dateModified);
            if (timeCmp != 0)
                return timeCmp > 0;

            return ToUpperInvariant(a.path) < ToUpperInvariant(b.path);
        });
    return everythingSearchCacheResults_;
}

std::vector<DesktopApp::QuickNavigationAppEntry>
DesktopApp::BuildQuickNavigationAppIndex(HWND ownerHwnd, HIMAGELIST& systemImageListSmall)
{
    std::vector<QuickNavigationAppEntry> entries;
    systemImageListSmall = nullptr;

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
        entry.absolutePidl.reset(absolute);
        entry.systemIconIndex = imageList ? info.iIcon : -1;
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
    if (quickNavigationAppsIndexed_)
        return;

    bool expected = false;
    if (!quickNavigationAppIndexing_.compare_exchange_strong(expected, true))
        return;

    if (quickNavigationAppIndexThread_.joinable())
        quickNavigationAppIndexThread_.join();

    HWND targetHwnd = hwnd_;
    if (!targetHwnd || !IsWindow(targetHwnd))
    {
        quickNavigationAppIndexing_ = false;
        return;
    }

    const uint64_t serial = ++quickNavigationAppIndexSerial_;
    try
    {
        quickNavigationAppIndexThread_ = std::thread([this, targetHwnd, serial]() {
            auto* result = new QuickNavigationAppIndexResult();
            result->serial = serial;

            HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const bool coInitialized = SUCCEEDED(coHr);
            if (coInitialized)
            {
                result->entries = BuildQuickNavigationAppIndex(targetHwnd, result->systemImageListSmall);
                CoUninitialize();
            }

            if (!PostMessageW(targetHwnd, kQuickNavigationAppsIndexedMessage,
                0, reinterpret_cast<LPARAM>(result)))
            {
                delete result;
                quickNavigationAppIndexing_ = false;
            }
        });
    }
    catch (...)
    {
        quickNavigationAppIndexing_ = false;
    }
}

void DesktopApp::StopQuickNavigationAppIndexing()
{
    ++quickNavigationAppIndexSerial_;
    if (quickNavigationAppIndexThread_.joinable() &&
        quickNavigationAppIndexThread_.get_id() != std::this_thread::get_id())
    {
        quickNavigationAppIndexThread_.join();
    }
    quickNavigationAppIndexing_ = false;

    if (!hwnd_)
        return;

    MSG msg{};
    while (PeekMessageW(&msg, hwnd_, kQuickNavigationAppsIndexedMessage,
        kQuickNavigationAppsIndexedMessage, PM_REMOVE))
    {
        delete reinterpret_cast<QuickNavigationAppIndexResult*>(msg.lParam);
    }
}

void DesktopApp::OnQuickNavigationAppsIndexed(WPARAM /*wParam*/, LPARAM lParam)
{
    std::unique_ptr<QuickNavigationAppIndexResult> result(
        reinterpret_cast<QuickNavigationAppIndexResult*>(lParam));

    if (quickNavigationAppIndexThread_.joinable() &&
        quickNavigationAppIndexThread_.get_id() != std::this_thread::get_id())
    {
        quickNavigationAppIndexThread_.join();
    }
    quickNavigationAppIndexing_ = false;

    if (!result || result->serial != quickNavigationAppIndexSerial_)
        return;

    quickNavigationAppEntries_ = std::move(result->entries);
    if (result->systemImageListSmall)
        quickNavigationSystemImageListSmall_ = result->systemImageListSmall;
    quickNavigationAppsIndexed_ = true;
    quickNavigationAppsExpanded_ = false;
    if (widgetEngine_)
        widgetEngine_->NotifyDesktopChanged("applications");

    if (!GetQuickNavigationEffectiveSearchText().empty())
    {
        RefreshQuickNavigationAppResults();
        quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
            GetQuickNavigationMaxScrollOffset(quickNavigationRect_));
        if (quickNavigationOpen_)
            InvalidateQuickNavigationWindow();
    }
}

void DesktopApp::RefreshQuickNavigationAppResults()
{
    quickNavigationAppResultIndices_.clear();
    const std::wstring query = GetQuickNavigationEffectiveSearchText();
    if (query.empty())
        return;

    StartQuickNavigationAppIndexing();
    if (!quickNavigationAppsIndexed_)
        return;

    for (size_t i = 0; i < quickNavigationAppEntries_.size(); ++i)
    {
        const QuickNavigationAppEntry& entry = quickNavigationAppEntries_[i];
        const std::wstring displayName =
            generalSettings_.demoModeEnabled &&
            demoIdentityAssetsAvailable_
            ? GetDemoIdentityTitle(entry.parsingName)
            : entry.name;
        if (!NameMatchesQuery(displayName, query))
            continue;
        quickNavigationAppResultIndices_.push_back(i);
    }

    std::stable_sort(quickNavigationAppResultIndices_.begin(), quickNavigationAppResultIndices_.end(),
        [&](size_t a, size_t b) {
            const auto displayName = [&](size_t index) {
                const auto& entry = quickNavigationAppEntries_[index];
                return generalSettings_.demoModeEnabled &&
                    demoIdentityAssetsAvailable_
                    ? GetDemoIdentityTitle(entry.parsingName)
                    : entry.name;
            };
            return NameSearchMatchRank(displayName(a), query) <
                NameSearchMatchRank(displayName(b), query);
        });

    if (quickNavigationAppResultIndices_.size() > kQuickNavigationAppResultLimit)
        quickNavigationAppResultIndices_.resize(kQuickNavigationAppResultLimit);
}

const DesktopApp::QuickNavigationAppEntry*
DesktopApp::FindQuickNavigationEverythingAppEntry() const
{
    if (!quickNavigationAppsIndexed_)
        return nullptr;

    const QuickNavigationAppEntry* bestEntry = nullptr;
    int bestRank = kNameSearchNoMatchRank;
    for (const auto& entry : quickNavigationAppEntries_)
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

std::wstring DesktopApp::GetQuickNavigationEverythingNoticeText() const
{
    if (!quickNavigationAppsIndexed_)
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
    auto isTabWidget = [](DesktopWidgetType t) {
        return t == DesktopWidgetType::Collection ||
               t == DesktopWidgetType::FileCategories ||
               t == DesktopWidgetType::FolderMapping;
    };
    std::vector<std::wstring> widgetIds;
    std::vector<bool> eligible;
    widgetIds.reserve(widgets_.size());
    eligible.reserve(widgets_.size());
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        widgetIds.push_back(widgets_[i].id);
        eligible.push_back(
            isTabWidget(widgets_[i].type) &&
            !IsGroupedCollection(widgets_[i]));
    }
    return snowdesktop::quick_navigation_rules::
        OrderIndicesByTabIds(
            navTabOrder_, widgetIds, eligible);
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

    if (quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-1))
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
    if (quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-2))
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

    if (quickNavigationActiveWidgetIndex_ < widgets_.size() &&
        (widgets_[quickNavigationActiveWidgetIndex_].type == DesktopWidgetType::Collection ||
         widgets_[quickNavigationActiveWidgetIndex_].type == DesktopWidgetType::FileCategories))
    {
        const auto& widget = widgets_[quickNavigationActiveWidgetIndex_];
        for (const auto& itemKey : widget.itemKeys)
            appendKey(itemKey);
        return result;
    }
    if (quickNavigationActiveWidgetIndex_ < widgets_.size() &&
        widgets_[quickNavigationActiveWidgetIndex_].type == DesktopWidgetType::FolderMapping)
    {
        for (const auto& fe : widgets_[quickNavigationActiveWidgetIndex_].folderEntries)
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
DesktopApp::BuildQuickNavigationContentModel() const
{
    QuickNavigationContentModel model;
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
        size_t demoCollectionIndex = static_cast<size_t>(-1)) {
        if (itemIndex >= items_.size()) return;
        const DesktopItem& item = items_[itemIndex];
        std::wstring key = ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
        if (key.empty() || seenDesktop.contains(key)) return;
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
            ShouldUseDemoCollectionIdentity(demoCollection)
            ? GetDemoCollectionIdentityTitle(*demoCollection, identity)
            : (ShouldUseDemoIdentity(item)
                ? GetDemoIdentityTitle(identity) : item.name);
        if (!matches(displayName)) return;
        seenDesktop.insert(std::move(key));

        QuickNavigationEntry entry;
        entry.kind = QuickNavigationEntry::Kind::DesktopItem;
        entry.itemIndex = itemIndex;
        entry.demoCollectionIndex = demoCollectionIndex;
        entry.name = displayName;
        entry.path = item.parsingName;
        entry.source = source;
        destination.push_back(std::move(entry));
    };
    auto appendDockDesktopItems =
        [&](std::vector<QuickNavigationEntry>& destination) {
        for (const DockEntry& dockEntry : dockEntries_)
        {
            if (dockEntry.type != DockEntryType::DesktopItem)
                continue;
            appendDesktop(
                destination,
                FindItemIndexByKey(dockEntry.reference),
                _LW("app.nav.source_dock"));
        }
    };
    auto appendSection = [&](
        const std::wstring& label,
        std::vector<QuickNavigationEntry>& entries) {
        if (entries.empty()) return;
        const size_t first = model.entries.size();
        model.entries.insert(
            model.entries.end(),
            std::make_move_iterator(entries.begin()),
            std::make_move_iterator(entries.end()));
        model.sections.push_back(
            {label, first, model.entries.size() - first});
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
                navTabOrder_, widgetIds, eligible);
    };

    if (query.empty())
    {
        if (quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-1))
        {
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

                std::vector<std::wstring>
                    desktopKeys;
                desktopKeys.reserve(items_.size());
                for (const auto& item : items_)
                    desktopKeys.push_back(
                        ToUpperInvariant(
                            item.layoutKey.empty()
                            ? item.parsingName
                            : item.layoutKey));
                std::vector<std::vector<
                    std::wstring>> sourceKeys(
                        sourceWidgets.size() + 1);
                for (const DockEntry& dockEntry :
                    dockEntries_)
                {
                    if (dockEntry.type ==
                        DockEntryType::DesktopItem)
                        sourceKeys[0].push_back(
                            ToUpperInvariant(
                                dockEntry.reference));
                }
                for (size_t sourceIndex = 0;
                    sourceIndex <
                        sourceWidgets.size();
                    ++sourceIndex)
                {
                    const auto& sourceWidget = widgets_[
                        sourceWidgets[sourceIndex]];
                    for (const auto& key : sourceWidget.itemKeys)
                    {
                        sourceKeys[
                            sourceIndex + 1].
                            push_back(
                                ToUpperInvariant(
                                    key));
                    }
                }
                const std::vector<int> owners =
                    snowdesktop::
                        quick_navigation_rules::
                            AssignSourceOwners(
                                desktopKeys,
                                sourceKeys);

                std::vector<QuickNavigationEntry>
                    dockEntries;
                for (const DockEntry& dockEntry :
                    dockEntries_)
                {
                    if (dockEntry.type !=
                        DockEntryType::DesktopItem)
                        continue;
                    const size_t itemIndex =
                        FindItemIndexByKey(
                            dockEntry.reference);
                    if (itemIndex < owners.size() &&
                        owners[itemIndex] == 0)
                        appendDesktop(
                            dockEntries, itemIndex,
                            _LW(
                                "app.nav.source_dock"));
                }
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
                    for (const auto& key : widget.itemKeys)
                    {
                        const size_t itemIndex =
                            FindItemIndexByKey(key);
                        if (itemIndex <
                                owners.size() &&
                            owners[itemIndex] ==
                                static_cast<int>(
                                    sourceIndex + 1))
                            appendDesktop(
                                entries, itemIndex,
                                widgetTitle(widget), widgetIndex);
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
                    if (i < owners.size() &&
                        owners[i] < 0)
                        appendDesktop(
                            looseEntries, i,
                            _LW(
                                "app.interact.free_desktop"));
                }
                appendSection(
                    _LW("app.interact.free_desktop"),
                    looseEntries);
                return model;
            }

            if (navigationSettings_.desktopViewMode ==
                QuickNavigationDesktopViewMode::Initial)
            {
                std::vector<QuickNavigationEntry> all;
                for (size_t i = 0;
                    i < items_.size(); ++i)
                    appendDesktop(
                        all, i,
                        _LW("app.nav.tab_desktop"));
                std::array<std::vector<
                    QuickNavigationEntry>, 27>
                    buckets;
                for (auto& entry : all)
                {
                    const wchar_t bucket =
                        snowdesktop::
                            quick_navigation_rules::
                                InitialBucket(
                                    entry.name);
                    const size_t bucketIndex =
                        bucket >= L'A' &&
                            bucket <= L'Z'
                        ? static_cast<size_t>(
                            bucket - L'A')
                        : 26;
                    buckets[bucketIndex].
                        push_back(
                            std::move(entry));
                }
                for (size_t i = 0;
                    i < buckets.size(); ++i)
                {
                    auto& bucket = buckets[i];
                    std::stable_sort(
                        bucket.begin(), bucket.end(),
                        [](const auto& lhs,
                            const auto& rhs) {
                            return snowdesktop::
                                quick_navigation_rules::
                                    InitialNameLess(
                                        lhs.name,
                                        rhs.name);
                        });
                    const std::wstring label =
                        i < 26
                        ? std::wstring(
                            1, static_cast<wchar_t>(
                                L'A' + i))
                        : L"#";
                    appendSection(
                        label, bucket);
                }
                return model;
            }

            appendDockDesktopItems(model.entries);
            for (const auto& key :
                GetQuickNavigationItemKeys())
                appendDesktop(
                    model.entries,
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
                    model.entries, i,
                    _LW("app.interact.free_desktop"));
            }
            return model;
        }

        if (quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-2))
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
                    entry.name = entryData.name;
                    entry.path = entryData.fullPath;
                    entry.source = source;
                    entries.push_back(
                        std::move(entry));
                }
                appendSection(source, entries);
            }
            return model;
        }

        if (quickNavigationActiveWidgetIndex_ < widgets_.size() &&
            widgets_[quickNavigationActiveWidgetIndex_].type == DesktopWidgetType::FolderMapping)
        {
            const DesktopWidget& widget = widgets_[quickNavigationActiveWidgetIndex_];
        std::wstring source = widget.title.empty() ? _LW("widget.folder_mapping") : widget.title;
            for (size_t ei = 0; ei < widget.folderEntries.size(); ++ei)
            {
                const FolderEntry& entryData = widget.folderEntries[ei];
                QuickNavigationEntry entry;
                entry.kind = QuickNavigationEntry::Kind::FolderEntry;
                entry.widgetIndex = quickNavigationActiveWidgetIndex_;
                entry.folderEntryIndex = ei;
                entry.name = entryData.name;
                entry.path = entryData.fullPath;
                entry.source = source;
                model.entries.push_back(
                    std::move(entry));
            }
            return model;
        }

        for (const auto& key : GetQuickNavigationItemKeys())
            appendDesktop(
                model.entries,
                FindItemIndexByKey(key),
                _LW("widget.collection"),
                quickNavigationActiveWidgetIndex_ < widgets_.size()
                    ? quickNavigationActiveWidgetIndex_
                    : static_cast<size_t>(-1));
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
            entry.name = entryData.name;
            entry.path = entryData.fullPath;
            entry.source = source;
            model.entries.push_back(
                std::move(entry));
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
