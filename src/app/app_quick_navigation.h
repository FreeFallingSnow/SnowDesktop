/**
 * @file app_quick_navigation.h
 * @brief DesktopApp 快捷导航面板的内联实现。
 *
 * 该文件集中维护快捷导航的热键注册、独立窗口、搜索框、结果构建、绘制和交互逻辑。
 */

#pragma once

#include <cwchar>
#include <limits>

#include <dwmapi.h>
#include <imm.h>
#include <array>
#include <iterator>
#include "quick_navigation_rules.h"
#include "search_match.h"

// ── Quick Navigation ───────────────────────────────────────

inline constexpr size_t kQuickNavigationAppResultLimit = 80;
inline constexpr size_t kQuickNavigationAppCollapsedResultCount = 5;

namespace
{
inline std::wstring QuickNavigationReadImeCompositionString(HWND hwnd)
{
    HIMC context = ImmGetContext(hwnd);
    if (!context)
        return {};

    std::wstring result;
    const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
    if (bytes > 0)
    {
        result.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
        ImmGetCompositionStringW(context, GCS_COMPSTR, result.data(), bytes);
        while (!result.empty() && result.back() == L'\0')
            result.pop_back();
    }

    ImmReleaseContext(hwnd, context);
    return result;
}

inline int QuickNavigationRowsHeight(int rows, int cellHeight, int rowGap)
{
    if (rows <= 0)
        return 0;
    return rows * cellHeight + (rows - 1) * rowGap;
}

inline bool QuickNavigationHasFileTime(const FILETIME& value)
{
    return value.dwLowDateTime != 0 || value.dwHighDateTime != 0;
}

inline std::wstring QuickNavigationFormatModifiedTime(const FILETIME& value)
{
    if (!QuickNavigationHasFileTime(value))
        return {};

    FILETIME localTime{};
    SYSTEMTIME systemTime{};
    if (!FileTimeToLocalFileTime(&value, &localTime) ||
        !FileTimeToSystemTime(&localTime, &systemTime))
        return {};

    auto padNumber = [](unsigned value, size_t width) {
        std::wstring text = std::to_wstring(value);
        if (text.size() < width)
            text.insert(text.begin(), width - text.size(), L'0');
        return text;
    };
    return _LFW("app.nav.file_modified",
        padNumber(static_cast<unsigned>(systemTime.wYear), 4),
        padNumber(static_cast<unsigned>(systemTime.wMonth), 2),
        padNumber(static_cast<unsigned>(systemTime.wDay), 2),
        padNumber(static_cast<unsigned>(systemTime.wHour), 2),
        padNumber(static_cast<unsigned>(systemTime.wMinute), 2));
}
}

inline std::vector<EverythingSearchResult> DesktopApp::SearchEverythingCached(
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

inline std::vector<DesktopApp::QuickNavigationAppEntry>
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

inline void DesktopApp::StartQuickNavigationAppIndexing()
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

inline void DesktopApp::StopQuickNavigationAppIndexing()
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

inline void DesktopApp::OnQuickNavigationAppsIndexed(WPARAM /*wParam*/, LPARAM lParam)
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

    if (!GetQuickNavigationEffectiveSearchText().empty())
    {
        RefreshQuickNavigationAppResults();
        quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
            GetQuickNavigationMaxScrollOffset(quickNavigationRect_));
        if (quickNavigationOpen_)
            InvalidateQuickNavigationWindow();
    }
}

inline void DesktopApp::RefreshQuickNavigationAppResults()
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
        if (!NameMatchesQuery(entry.name, query))
            continue;
        quickNavigationAppResultIndices_.push_back(i);
    }

    std::stable_sort(quickNavigationAppResultIndices_.begin(), quickNavigationAppResultIndices_.end(),
        [&](size_t a, size_t b) {
            return NameSearchMatchRank(quickNavigationAppEntries_[a].name, query) <
                NameSearchMatchRank(quickNavigationAppEntries_[b].name, query);
        });

    if (quickNavigationAppResultIndices_.size() > kQuickNavigationAppResultLimit)
        quickNavigationAppResultIndices_.resize(kQuickNavigationAppResultLimit);
}

inline const DesktopApp::QuickNavigationAppEntry*
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

inline std::wstring DesktopApp::GetQuickNavigationEverythingNoticeText() const
{
    if (!quickNavigationAppsIndexed_)
        return _LW("app.interact.everything_not_running");
    return FindQuickNavigationEverythingAppEntry()
        ? _LW("app.interact.everything_click_start")
        : _LW("app.nav.everything_download");
}

inline bool DesktopApp::TryLaunchQuickNavigationEverythingApp()
{
    const QuickNavigationAppEntry* entry = FindQuickNavigationEverythingAppEntry();
    return entry &&
        LaunchQuickNavigationAppEntry(*entry);
}

inline std::vector<size_t> DesktopApp::GetQuickNavigationCollectionIndices() const
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

inline std::vector<std::wstring> DesktopApp::GetQuickNavigationItemKeys() const
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
            for (const auto& key : widgets_[ci].itemKeys)
                appendKey(key);
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
        for (const auto& key : widgets_[quickNavigationActiveWidgetIndex_].itemKeys)
            appendKey(key);
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
            for (const auto& key : widgets_[ci].itemKeys)
                appendKey(key);
        }
    }
    return result;
}

inline DesktopApp::QuickNavigationContentModel
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
        size_t itemIndex, const std::wstring& source) {
        if (itemIndex >= items_.size()) return;
        const DesktopItem& item = items_[itemIndex];
        std::wstring key = ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
        if (key.empty() || seenDesktop.contains(key)) return;
        if (!matches(item.name)) return;
        seenDesktop.insert(std::move(key));

        QuickNavigationEntry entry;
        entry.kind = QuickNavigationEntry::Kind::DesktopItem;
        entry.itemIndex = itemIndex;
        entry.name = item.name;
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
                    for (const auto& key :
                        widgets_[
                            sourceWidgets[
                                sourceIndex]].
                            itemKeys)
                        sourceKeys[
                            sourceIndex + 1].
                            push_back(
                                ToUpperInvariant(
                                    key));
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
                    for (const auto& key :
                        widget.itemKeys)
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
                                widgetTitle(widget));
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
                _LW("widget.collection"));
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
        std::wstring source = widget.title.empty() ? _LW("widget.collection") : widget.title;
        for (const auto& key : widget.itemKeys)
            appendDesktop(
                model.entries,
                FindItemIndexByKey(key), source);
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

inline std::vector<DesktopApp::QuickNavigationEntry>
DesktopApp::GetQuickNavigationEntries() const
{
    return BuildQuickNavigationContentModel().
        entries;
}

inline RECT DesktopApp::GetQuickNavigationRect() const
{
    RECT work{};
    bool foundWorkArea = false;
    POINT anchor = quickNavigationOpenPoint_;
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, anchor) || PtInRect(&page.workArea, anchor))
        {
            work = page.workArea;
            foundWorkArea = true;
            break;
        }
    }

    if (!foundWorkArea)
    {
        POINT screenAnchor{ anchor.x + virtualLeft_, anchor.y + virtualTop_ };
        HMONITOR monitor = MonitorFromPoint(screenAnchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
        {
            work = MakeRect(
                monitorInfo.rcWork.left - virtualLeft_,
                monitorInfo.rcWork.top - virtualTop_,
                monitorInfo.rcWork.right - virtualLeft_,
                monitorInfo.rcWork.bottom - virtualTop_);
            foundWorkArea = true;
        }
    }

    if (!foundWorkArea)
        work = layoutWorkArea_;
    if (IsRectEmptyRect(work))
        work = MakeRect(0, 0, virtualWidth_, virtualHeight_);

    const int workWidth = std::max(1, static_cast<int>(work.right - work.left));
    const int workHeight = std::max(1, static_cast<int>(work.bottom - work.top));
    const int widthLimit = std::max(QuickNavScale(320), workWidth - QuickNavScale(48));
    const int heightLimit = std::max(QuickNavScale(280), workHeight - QuickNavScale(48));
    const int width = std::min(widthLimit, std::max(QuickNavScale(520), std::min(QuickNavScale(860), workWidth - QuickNavScale(120))));
    const int contentTop = QuickNavScale(100);
    const int cellH = QuickNavScale(kQuickNavigationCellHeight);
    const int rowGap = QuickNavScale(kQuickNavigationItemRowGap);
    const int contentPad = QuickNavScale(12);
    const int idealH = contentTop + QuickNavigationRowsHeight(1, cellH, rowGap) + contentPad;
    const int availH = workHeight - QuickNavScale(120);
    int rows = std::clamp(
        (availH - contentTop - contentPad + rowGap) / (cellH + rowGap),
        1, 6);
    const int height = std::min(heightLimit,
        std::max(idealH, contentTop + QuickNavigationRowsHeight(rows, cellH, rowGap) + contentPad));
    const int left = work.left + (workWidth - width) / 2;
    const int top = work.top + (workHeight - height) / 2;
    return MakeRect(left, top, left + width, top + height);
}

inline RECT DesktopApp::GetQuickNavigationSearchRect(const RECT& overlay) const
{
    return MakeRect(overlay.left + QuickNavScale(16), overlay.top + QuickNavScale(18),
        overlay.right - QuickNavScale(16), overlay.top + QuickNavScale(50));
}

inline RECT DesktopApp::GetQuickNavigationTabsRect(const RECT& overlay) const
{
    return MakeRect(overlay.left + QuickNavScale(16), overlay.top + QuickNavScale(58),
        overlay.right - QuickNavScale(16), overlay.top + QuickNavScale(88));
}

inline RECT DesktopApp::GetQuickNavigationViewModeButtonRect(
    const RECT& overlay) const
{
    if (!GetQuickNavigationEffectiveSearchText().empty() ||
        quickNavigationActiveWidgetIndex_ !=
            static_cast<size_t>(-1))
        return MakeRect(0, 0, 0, 0);

    const RECT tabs =
        GetQuickNavigationTabsRect(overlay);
    const int buttonSize = QuickNavScale(28);
    const int top = tabs.top +
        std::max(0,
            static_cast<int>(
                tabs.bottom - tabs.top -
                buttonSize)) / 2;
    return MakeRect(
        tabs.left, top,
        tabs.left + buttonSize,
        top + buttonSize);
}

inline int DesktopApp::GetQuickNavigationTabsStart(
    const RECT& overlay) const
{
    const RECT tabs =
        GetQuickNavigationTabsRect(overlay);
    const RECT button =
        GetQuickNavigationViewModeButtonRect(
            overlay);
    return snowdesktop::
        quick_navigation_rules::
            TabStripLabelStart(
                tabs.left,
                !IsRectEmpty(&button),
                QuickNavScale(28),
                QuickNavScale(8));
}

inline void DesktopApp::SetQuickNavigationDesktopViewMode(
    QuickNavigationDesktopViewMode mode)
{
    if (navigationSettings_.desktopViewMode == mode)
        return;
    navigationSettings_.desktopViewMode = mode;
    SaveNavigationSettings(
        GetNavigationSettingsPath().c_str(),
        navigationSettings_);
    if (settingsWindow_)
        settingsWindow_->SyncNavigationSettings(
            navigationSettings_);
    quickNavigationScrollOffset_ = 0;
    quickNavigationInitialJumpOpen_ = false;
    ResetQuickNavigationKeyboardTarget();
    InvalidateQuickNavigationWindow();
}

inline bool DesktopApp::
TrySetQuickNavigationDesktopViewModeAtPoint(
    POINT point)
{
    if (!GetQuickNavigationEffectiveSearchText().empty() ||
        quickNavigationActiveWidgetIndex_ !=
            static_cast<size_t>(-1))
        return false;

    const RECT button =
        GetQuickNavigationViewModeButtonRect(
            quickNavigationRect_);
    if (PtInRect(&button, point))
    {
        SetQuickNavigationDesktopViewMode(
            snowdesktop::
                quick_navigation_rules::
                    NextQuickNavigationDesktopViewMode(
                        navigationSettings_.
                            desktopViewMode));
        return true;
    }
    return false;
}

inline RECT DesktopApp::GetQuickNavigationContentRect(const RECT& overlay) const
{
    if (!GetQuickNavigationEffectiveSearchText().empty())
        return MakeRect(overlay.left + QuickNavScale(12), overlay.top + QuickNavScale(66),
            overlay.right - QuickNavScale(12), overlay.bottom - QuickNavScale(12));
    return MakeRect(overlay.left + QuickNavScale(12), overlay.top + QuickNavScale(100),
        overlay.right - QuickNavScale(12), overlay.bottom - QuickNavScale(12));
}

inline int DesktopApp::GetQuickNavigationTabStripContentWidth(const RECT& /*overlay*/) const
{
    const size_t n = quickNavTabWidths_.size();
    if (n <= 2) return 0;
    const int gap = QuickNavScale(8);
    int total = 0;
    for (size_t i = 2; i < n; ++i)
        total += quickNavTabWidths_[i] + gap;
    return total - gap; // remove trailing gap
}

inline int DesktopApp::GetQuickNavigationMaxTabScrollOffset(const RECT& overlay) const
{
    RECT tabs = GetQuickNavigationTabsRect(overlay);
    const int tabsStart =
        GetQuickNavigationTabsStart(overlay);
    const auto& tw = quickNavTabWidths_;
    const int gap = QuickNavScale(8);
    const int sepGap = QuickNavScale(6);
    // 可滚动区起始于固定标签（0:桌面, 1:映射）+ 分隔线之后。
    const int fixedWidth = (tw.size() >= 2) ? (tw[0] + gap + tw[1]) : 0;
    const int scrollPad = sepGap + QuickNavScale(1) + gap;
    const int scrollLeft =
        tabsStart + fixedWidth + scrollPad;
    return snowdesktop::
        quick_navigation_rules::
            TabStripMaxScrollOffset(
                GetQuickNavigationTabStripContentWidth(
                    overlay),
                scrollLeft,
                tabs.right);
}

inline int DesktopApp::GetQuickNavigationTabWidth() const
{
    if (!quickNavTabWidths_.empty())
        return quickNavTabWidths_[0];
    return QuickNavScale(72);
}

inline void DesktopApp::EnsureNavTabOrder()
{
    auto isTabWidget = [](DesktopWidgetType t) {
        return t == DesktopWidgetType::Collection ||
               t == DesktopWidgetType::FileCategories ||
               t == DesktopWidgetType::FolderMapping;
    };
    std::unordered_set<std::wstring> orderSet(navTabOrder_.begin(), navTabOrder_.end());
    for (const auto& w : widgets_)
    {
        if (isTabWidget(w.type) &&
            !IsGroupedCollection(w) &&
            !orderSet.count(w.id))
        {
            navTabOrder_.push_back(w.id);
            orderSet.insert(w.id);
        }
    }
    navTabOrder_.erase(
        std::remove_if(navTabOrder_.begin(), navTabOrder_.end(),
            [this, &isTabWidget](const std::wstring& id) {
                for (const auto& w : widgets_)
                    if (isTabWidget(w.type) &&
                        !IsGroupedCollection(w) &&
                        w.id == id)
                        return false;
                return true;
            }),
        navTabOrder_.end());
}

inline RECT DesktopApp::GetQuickNavigationTabRect(const RECT& overlay, size_t tabIndex) const
{
    RECT tabs = GetQuickNavigationTabsRect(overlay);
    const int tabsStart =
        GetQuickNavigationTabsStart(overlay);
    const int gap = QuickNavScale(8);
    const int sepGap = QuickNavScale(6);
    const int scrollPad = sepGap + QuickNavScale(1) + gap; // separator + gap after

    const size_t n = quickNavTabWidths_.size();
    if (n == 0)
        return MakeRect(
            tabsStart, tabs.top,
            tabsStart + QuickNavScale(72),
            tabs.bottom);

    int left = tabsStart;
    int width = QuickNavScale(72);
    if (tabIndex == 0)
    {
        if (n > 0) width = quickNavTabWidths_[0];
    }
    else if (tabIndex == 1)
    {
        if (n > 1)
        {
            left = tabsStart +
                quickNavTabWidths_[0] + gap;
            width = quickNavTabWidths_[1];
        }
    }
    else if (tabIndex >= 2)
    {
        left = tabsStart;
        if (n > 1)
            left = tabsStart +
                quickNavTabWidths_[0] + gap +
                quickNavTabWidths_[1] +
                scrollPad;
        for (size_t i = 2; i < tabIndex && i < n; ++i)
            left += quickNavTabWidths_[i] + gap;
        left -= quickNavigationTabScrollOffset_;
        if (tabIndex < n) width = quickNavTabWidths_[tabIndex];
    }
    return MakeRect(left, tabs.top, left + width, tabs.bottom);
}

inline int DesktopApp::GetQuickNavigationColumnCount(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int cellW = QuickNavScale(kCellWidth);
    const int contentWidth = std::max(1, static_cast<int>(content.right - content.left));
    if (contentWidth < cellW) return 1;
    int columns = contentWidth / cellW;
    if (columns <= 1) return 1;
    int gap = (contentWidth - columns * cellW) / (columns - 1);
    while (columns > 1 && gap < QuickNavScale(8))
    {
        --columns;
        gap = (contentWidth - columns * cellW) / (columns - 1);
    }
    return std::max(1, columns);
}

inline int DesktopApp::GetQuickNavigationGap(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int cellW = QuickNavScale(kCellWidth);
    const int columns = GetQuickNavigationColumnCount(overlay);
    if (columns <= 0) return 0;
    const int contentWidth = std::max(1, static_cast<int>(content.right - content.left));
    int totalGaps = contentWidth - columns * cellW;
    return totalGaps / columns;
}

inline RECT DesktopApp::GetQuickNavigationItemRect(const RECT& overlay, size_t linearIndex) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int cellW = QuickNavScale(kCellWidth);
    const int cellH = QuickNavScale(kQuickNavigationCellHeight);
    const int rowGap = QuickNavScale(kQuickNavigationItemRowGap);
    const int rowPitch = cellH + rowGap;
    const int columns = GetQuickNavigationColumnCount(overlay);
    int col = static_cast<int>(
        linearIndex %
        static_cast<size_t>(columns));
    int row = static_cast<int>(
        linearIndex /
        static_cast<size_t>(columns));
    int itemTop = content.top;
    if (GetQuickNavigationEffectiveSearchText().empty())
    {
        const QuickNavigationContentModel model =
            BuildQuickNavigationContentModel();
        if (model.IsSectioned())
        {
            std::vector<size_t> counts;
            counts.reserve(model.sections.size());
            for (const auto& section : model.sections)
                counts.push_back(section.entryCount);
            const auto layouts =
                snowdesktop::quick_navigation_rules::
                    BuildSectionLayouts(
                        counts, columns, cellH, rowGap,
                        QuickNavScale(28),
                        QuickNavScale(8),
                        QuickNavScale(12));
            snowdesktop::quick_navigation_rules::
                SectionItemCell cell;
            if (
                snowdesktop::quick_navigation_rules::
                    TryGetSectionItemCell(
                        layouts, linearIndex,
                        columns, cellH, rowGap,
                        cell))
            {
                col = cell.column;
                row = cell.row;
                itemTop = content.top +
                    cell.top - row * rowPitch;
            }
        }
    }
    else
    {
        itemTop +=
            QuickNavScale(28) +
            QuickNavScale(8);
    }
    const int gap = GetQuickNavigationGap(overlay);
    int halfPad = gap / 2;
    return MakeRect(
        content.left + halfPad + col * (cellW + gap),
        itemTop + row * rowPitch - quickNavigationScrollOffset_,
        content.left + halfPad + col * (cellW + gap) + cellW,
        itemTop + row * rowPitch + cellH - quickNavigationScrollOffset_);
}

inline RECT DesktopApp::GetQuickNavigationSectionHeaderRect(
    const RECT& overlay, size_t sectionIndex,
    const QuickNavigationContentModel& model) const
{
    const RECT content =
        GetQuickNavigationContentRect(overlay);
    if (sectionIndex >= model.sections.size())
        return MakeRect(0, 0, 0, 0);

    std::vector<size_t> counts;
    counts.reserve(model.sections.size());
    for (const auto& section : model.sections)
        counts.push_back(section.entryCount);
    const auto layouts =
        snowdesktop::quick_navigation_rules::
            BuildSectionLayouts(
                counts,
                GetQuickNavigationColumnCount(overlay),
                QuickNavScale(
                    kQuickNavigationCellHeight),
                QuickNavScale(
                    kQuickNavigationItemRowGap),
                QuickNavScale(28),
                QuickNavScale(8),
                QuickNavScale(12));
    if (sectionIndex >= layouts.size())
        return MakeRect(0, 0, 0, 0);

    const int top = content.top +
        layouts[sectionIndex].headerTop -
        quickNavigationScrollOffset_;
    return MakeRect(
        content.left + QuickNavScale(8), top,
        content.right - QuickNavScale(12),
        top + QuickNavScale(28));
}

inline RECT DesktopApp::
GetQuickNavigationInitialJumpBackRect(
    const RECT& overlay) const
{
    const RECT content =
        GetQuickNavigationContentRect(overlay);
    return MakeRect(
        content.right - QuickNavScale(88),
        content.top,
        content.right - QuickNavScale(8),
        content.top + QuickNavScale(30));
}

inline RECT DesktopApp::
GetQuickNavigationInitialJumpCellRect(
    const RECT& overlay,
    size_t bucketIndex) const
{
    if (bucketIndex >= 27)
        return MakeRect(0, 0, 0, 0);
    const RECT content =
        GetQuickNavigationContentRect(overlay);
    const int gridTop =
        content.top + QuickNavScale(38);
    const int availableHeight =
        std::max(1,
            static_cast<int>(
                content.bottom - gridTop -
                QuickNavScale(8)));
    const int columns =
        availableHeight >= QuickNavScale(252)
        ? 4
        : 7;
    const int rows =
        (27 + columns - 1) / columns;
    const int availableWidth =
        std::max(
            1,
            static_cast<int>(
                content.right - content.left -
                QuickNavScale(32)));
    const int gridWidth = std::min(
        availableWidth,
        QuickNavScale(columns * 68));
    const int cellWidth =
        std::max(1, gridWidth / columns);
    const int cellHeight = std::clamp(
        availableHeight / rows,
        QuickNavScale(28),
        QuickNavScale(44));
    const int gridLeft = content.left +
        (static_cast<int>(
            content.right - content.left) -
            cellWidth * columns) / 2;
    const int column =
        static_cast<int>(
            bucketIndex %
            static_cast<size_t>(columns));
    const int row =
        static_cast<int>(
            bucketIndex /
            static_cast<size_t>(columns));
    return MakeRect(
        gridLeft + column * cellWidth,
        gridTop + row * cellHeight,
        gridLeft + (column + 1) * cellWidth,
        gridTop + (row + 1) * cellHeight);
}

inline bool DesktopApp::
ActivateQuickNavigationInitialJumpBucket(
    size_t bucketIndex)
{
    if (bucketIndex >= 27)
        return false;
    const wchar_t bucket =
        snowdesktop::quick_navigation_rules::
            InitialJumpBucketAt(bucketIndex);
    const std::wstring label(1, bucket);
    const QuickNavigationContentModel model =
        BuildQuickNavigationContentModel();
    for (size_t sectionIndex = 0;
        sectionIndex < model.sections.size();
        ++sectionIndex)
    {
        if (model.sections[sectionIndex].label !=
            label)
            continue;
        const RECT header =
            GetQuickNavigationSectionHeaderRect(
                quickNavigationRect_,
                sectionIndex, model);
        const RECT content =
            GetQuickNavigationContentRect(
                quickNavigationRect_);
        quickNavigationScrollOffset_ =
            std::clamp(
                quickNavigationScrollOffset_ +
                    static_cast<int>(
                        header.top - content.top),
                0,
                GetQuickNavigationMaxScrollOffset(
                    quickNavigationRect_));
        quickNavigationInitialJumpOpen_ = false;
        quickNavigationInitialJumpSelection_ =
            bucketIndex;
        ResetQuickNavigationKeyboardTarget();
        InvalidateQuickNavigationWindow();
        return true;
    }
    return false;
}

inline bool DesktopApp::
HandleQuickNavigationInitialJumpClick(
    POINT point)
{
    if (!quickNavigationInitialJumpOpen_)
        return false;
    const RECT back =
        GetQuickNavigationInitialJumpBackRect(
            quickNavigationRect_);
    if (PtInRect(&back, point))
    {
        quickNavigationInitialJumpOpen_ = false;
        InvalidateQuickNavigationWindow();
        return true;
    }
    for (size_t bucketIndex = 0;
        bucketIndex < 27; ++bucketIndex)
    {
        const RECT cell =
            GetQuickNavigationInitialJumpCellRect(
                quickNavigationRect_,
                bucketIndex);
        if (!PtInRect(&cell, point))
            continue;
        ActivateQuickNavigationInitialJumpBucket(
            bucketIndex);
        return true;
    }
    const RECT content =
        GetQuickNavigationContentRect(
            quickNavigationRect_);
    return PtInRect(&content, point) != FALSE;
}

inline bool DesktopApp::
HandleQuickNavigationInitialJumpKeyboardInput(
    WPARAM key)
{
    if (!quickNavigationInitialJumpOpen_)
        return false;
    if (key == VK_ESCAPE)
    {
        quickNavigationInitialJumpOpen_ = false;
        InvalidateQuickNavigationWindow();
        return true;
    }

    const QuickNavigationContentModel model =
        BuildQuickNavigationContentModel();
    std::array<bool, 27> available{};
    for (const auto& section : model.sections)
    {
        if (section.label.size() != 1)
            continue;
        available[
            snowdesktop::quick_navigation_rules::
                InitialJumpBucketIndex(
                    section.label.front())] = true;
    }
    if (key == VK_RETURN)
        return ActivateQuickNavigationInitialJumpBucket(
            quickNavigationInitialJumpSelection_);
    if (key != VK_LEFT && key != VK_RIGHT &&
        key != VK_UP && key != VK_DOWN)
        return false;

    const RECT currentRect =
        GetQuickNavigationInitialJumpCellRect(
            quickNavigationRect_,
            quickNavigationInitialJumpSelection_);
    const long long currentX =
        (static_cast<long long>(currentRect.left) +
            currentRect.right) / 2;
    const long long currentY =
        (static_cast<long long>(currentRect.top) +
            currentRect.bottom) / 2;
    long long bestScore =
        std::numeric_limits<long long>::max();
    size_t best = static_cast<size_t>(-1);
    for (size_t bucketIndex = 0;
        bucketIndex < available.size();
        ++bucketIndex)
    {
        if (!available[bucketIndex] ||
            bucketIndex ==
                quickNavigationInitialJumpSelection_)
            continue;
        const RECT candidate =
            GetQuickNavigationInitialJumpCellRect(
                quickNavigationRect_,
                bucketIndex);
        const long long x =
            (static_cast<long long>(
                candidate.left) +
                candidate.right) / 2;
        const long long y =
            (static_cast<long long>(
                candidate.top) +
                candidate.bottom) / 2;
        const long long dx = x - currentX;
        const long long dy = y - currentY;
        const bool inDirection =
            (key == VK_LEFT && dx < 0) ||
            (key == VK_RIGHT && dx > 0) ||
            (key == VK_UP && dy < 0) ||
            (key == VK_DOWN && dy > 0);
        if (!inDirection) continue;
        const long long primary =
            key == VK_LEFT || key == VK_RIGHT
            ? std::abs(dx)
            : std::abs(dy);
        const long long secondary =
            key == VK_LEFT || key == VK_RIGHT
            ? std::abs(dy)
            : std::abs(dx);
        const long long score =
            primary * 4 + secondary;
        if (score < bestScore)
        {
            bestScore = score;
            best = bucketIndex;
        }
    }
    if (best < available.size())
    {
        quickNavigationInitialJumpSelection_ =
            best;
        InvalidateQuickNavigationWindow();
    }
    return true;
}

inline bool DesktopApp::TryGetQuickNavigationAppEntryAtPoint(
    POINT point, const QuickNavigationAppEntry*& outEntry) const
{
    outEntry = nullptr;
    if (!quickNavigationOpen_ || GetQuickNavigationEffectiveSearchText().empty())
        return false;
    if (quickNavigationAppResultIndices_.empty())
        return false;

    RECT overlay = quickNavigationRect_;
    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    const int columns = GetQuickNavigationColumnCount(overlay);
    const int desktopCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    const int firstRowTop = content.top + headerH + gap
        + desktopGridH
        + gap + headerH + gap - quickNavigationScrollOffset_;
    const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();

    for (size_t i = 0; i < visibleAppCount; ++i)
    {
        const int rowTop = firstRowTop + static_cast<int>(i) * rowH;
        RECT itemRect = MakeRect(content.left + QuickNavScale(8), rowTop,
            content.right - QuickNavScale(12), rowTop + rowH);
        RECT clipped = itemRect;
        clipped.top = std::max(clipped.top, content.top);
        clipped.bottom = std::min(clipped.bottom, content.bottom);
        if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point))
            continue;
        size_t entryIndex = quickNavigationAppResultIndices_[i];
        if (entryIndex >= quickNavigationAppEntries_.size())
            return false;
        outEntry = &quickNavigationAppEntries_[entryIndex];
        return true;
    }

    return false;
}

inline size_t DesktopApp::FindDockItemIndexForQuickNavigationApp(
    const QuickNavigationAppEntry& entry)
{
    const std::wstring parsingKey =
        ToUpperInvariant(entry.parsingName);
    if (!parsingKey.empty())
    {
        const size_t direct =
            FindItemIndexByKey(parsingKey);
        if (direct < items_.size())
            return direct;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (ToUpperInvariant(items_[i].parsingName) ==
                parsingKey)
                return i;
        }
    }

    std::wstring appUserModelId = parsingKey;
    constexpr std::wstring_view appsFolderMarker =
        L"APPSFOLDER\\";
    const size_t appsFolder =
        appUserModelId.rfind(appsFolderMarker);
    if (appsFolder != std::wstring::npos)
    {
        appUserModelId = appUserModelId.substr(
            appsFolder + appsFolderMarker.size());
    }

    std::vector<size_t> candidates;
    candidates.reserve(
        dockEntries_.size() +
        static_cast<size_t>(
            std::max(
                0,
                dockSettings_.frequentItemCount)));
    for (const DockEntry& dockEntry : dockEntries_)
    {
        if (dockEntry.type !=
            DockEntryType::DesktopItem)
            continue;
        const size_t itemIndex =
            FindItemIndexByKey(
                dockEntry.reference);
        if (itemIndex < items_.size())
            candidates.push_back(itemIndex);
    }
    for (const size_t itemIndex :
        GetFrequentDockItemIndices())
    {
        if (std::find(
                candidates.begin(),
                candidates.end(),
                itemIndex) == candidates.end())
            candidates.push_back(itemIndex);
    }

    size_t nameMatch = static_cast<size_t>(-1);
    bool nameMatchAmbiguous = false;
    for (const size_t i : candidates)
    {
        const DockAppIdentity identity =
            ResolveDockAppIdentity(i);
        if (identity.kind ==
                DockAppIdentityKind::Applications &&
            !appUserModelId.empty() &&
            identity.appUserModelId ==
                appUserModelId)
        {
            return i;
        }
        if (identity.kind != DockAppIdentityKind::None &&
            !entry.name.empty() &&
            _wcsicmp(
                items_[i].name.c_str(),
                entry.name.c_str()) == 0)
        {
            if (nameMatch !=
                static_cast<size_t>(-1))
                nameMatchAmbiguous = true;
            nameMatch = i;
        }
    }
    return nameMatchAmbiguous
        ? static_cast<size_t>(-1)
        : nameMatch;
}

inline bool DesktopApp::LaunchQuickNavigationAppEntry(
    const QuickNavigationAppEntry& entry)
{
    if (!entry.absolutePidl.get())
        return false;

    Pidl launchPidl;
    launchPidl.reset(
        ILClone(entry.absolutePidl.get()));
    if (!launchPidl.get())
        return false;

    const size_t dockItemIndex =
        FindDockItemIndexForQuickNavigationApp(entry);
    const bool wasClosed =
        dockItemIndex < items_.size() &&
        GetDockWindowVisualState(dockItemIndex) ==
            DockWindowVisualState::Closed;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_IDLIST;
    sei.lpIDList = launchPidl.get();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
        return false;

    if (dockItemIndex < items_.size())
    {
        RecordDockItemUsage(dockItemIndex);
        if (wasClosed)
            StartDockLaunchBounce(dockItemIndex);
    }
    return true;
}

inline size_t DesktopApp::GetQuickNavigationVisibleAppResultCount() const
{
    if (quickNavigationAppsExpanded_)
        return quickNavigationAppResultIndices_.size();
    return std::min(quickNavigationAppResultIndices_.size(), kQuickNavigationAppCollapsedResultCount);
}

inline bool DesktopApp::HasQuickNavigationAppExpandButton() const
{
    return !quickNavigationAppsExpanded_ &&
        quickNavigationAppResultIndices_.size() > kQuickNavigationAppCollapsedResultCount;
}

inline bool DesktopApp::TryExpandQuickNavigationAppsAtPoint(POINT point)
{
    if (!quickNavigationOpen_ || GetQuickNavigationEffectiveSearchText().empty() ||
        !HasQuickNavigationAppExpandButton())
        return false;

    RECT overlay = quickNavigationRect_;
    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    const int columns = GetQuickNavigationColumnCount(overlay);
    const int desktopCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    const int buttonTop = content.top + headerH + gap
        + desktopGridH
        + gap + headerH + gap
        + static_cast<int>(GetQuickNavigationVisibleAppResultCount()) * rowH
        - quickNavigationScrollOffset_;
    RECT buttonRect = MakeRect(content.left + QuickNavScale(8), buttonTop,
        content.right - QuickNavScale(12), buttonTop + rowH);
    RECT clipped = buttonRect;
    clipped.top = std::max(clipped.top, content.top);
    clipped.bottom = std::min(clipped.bottom, content.bottom);
    if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point))
        return false;

    quickNavigationAppsExpanded_ = true;
    quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
        GetQuickNavigationMaxScrollOffset(quickNavigationRect_));
    InvalidateQuickNavigationWindow();
    return true;
}

inline bool DesktopApp::HasQuickNavigationEverythingLoadMoreButton() const
{
    return everythingSearchAvailable_ &&
        quickNavigationEverythingHasMore_ &&
        !quickNavigationEverythingResults_.empty() &&
        !GetQuickNavigationEffectiveSearchText().empty();
}

inline bool DesktopApp::TryLoadMoreQuickNavigationEverythingResultsAtPoint(POINT point)
{
    if (!quickNavigationOpen_ || !HasQuickNavigationEverythingLoadMoreButton())
        return false;

    RECT overlay = quickNavigationRect_;
    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    const int columns = GetQuickNavigationColumnCount(overlay);
    const int desktopCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    const int appSectionHeight = quickNavigationAppResultIndices_.empty()
        ? 0
        : headerH + gap + static_cast<int>(visibleAppCount) * rowH +
            (HasQuickNavigationAppExpandButton() ? rowH : 0) + gap;
    const int buttonTop = content.top + headerH + gap
        + desktopGridH
        + gap + appSectionHeight + headerH + gap
        + static_cast<int>(quickNavigationEverythingResults_.size()) * rowH
        - quickNavigationScrollOffset_;
    RECT buttonRect = MakeRect(content.left + QuickNavScale(8), buttonTop,
        content.right - QuickNavScale(12), buttonTop + rowH);
    RECT clipped = buttonRect;
    clipped.top = std::max(clipped.top, content.top);
    clipped.bottom = std::min(clipped.bottom, content.bottom);
    if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point))
        return false;

    const int oldResultCount = static_cast<int>(quickNavigationEverythingResults_.size());
    const int buttonTopBefore = buttonRect.top;
    const DWORD maxLimit = std::numeric_limits<DWORD>::max();
    if (quickNavigationEverythingResultLimit_ > maxLimit - kQuickNavigationEverythingResultBatchSize)
        quickNavigationEverythingResultLimit_ = maxLimit;
    else
        quickNavigationEverythingResultLimit_ += kQuickNavigationEverythingResultBatchSize;

    const bool appsExpanded = quickNavigationAppsExpanded_;
    RefreshQuickNavigationEverythingResults();
    quickNavigationAppsExpanded_ = appsExpanded;
    const int maxScroll = GetQuickNavigationMaxScrollOffset(quickNavigationRect_);
    if (static_cast<int>(quickNavigationEverythingResults_.size()) > oldResultCount)
    {
        const int newDesktopCount = static_cast<int>(GetQuickNavigationEntries().size());
        const int newDesktopRows = newDesktopCount == 0 ? 0 :
            (newDesktopCount + columns - 1) / columns;
        const size_t newVisibleAppCount = GetQuickNavigationVisibleAppResultCount();
        const int newDesktopGridH = QuickNavigationRowsHeight(newDesktopRows,
            QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
        const int newAppSectionHeight = quickNavigationAppResultIndices_.empty()
            ? 0
            : headerH + gap + static_cast<int>(newVisibleAppCount) * rowH +
                (HasQuickNavigationAppExpandButton() ? rowH : 0) + gap;
        const int firstNewRowTop = content.top + headerH + gap
            + newDesktopGridH
            + gap + newAppSectionHeight + headerH + gap
            + oldResultCount * rowH;
        quickNavigationScrollOffset_ = std::clamp(firstNewRowTop - buttonTopBefore, 0, maxScroll);
    }
    else
    {
        quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0, maxScroll);
    }
    InvalidateQuickNavigationWindow();
    return true;
}

inline bool DesktopApp::TryGetQuickNavigationEverythingEntryAtPoint(
    POINT point, QuickNavigationEverythingEntry& outEntry) const
{
    if (!quickNavigationOpen_ || GetQuickNavigationEffectiveSearchText().empty())
        return false;

    RECT overlay = quickNavigationRect_;
    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    const int columns = GetQuickNavigationColumnCount(overlay);
    const int desktopCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    const int appSectionHeight = quickNavigationAppResultIndices_.empty()
        ? 0
        : headerH + gap + static_cast<int>(visibleAppCount) * rowH +
            (HasQuickNavigationAppExpandButton() ? rowH : 0) + gap;
    const int firstRowTop = content.top + headerH + gap
        + desktopGridH
        + gap + appSectionHeight + headerH + gap - quickNavigationScrollOffset_;

    for (size_t i = 0; i < quickNavigationEverythingResults_.size(); ++i)
    {
        const int rowTop = firstRowTop + static_cast<int>(i) * rowH;
        RECT itemRect = MakeRect(content.left + QuickNavScale(8), rowTop,
            content.right - QuickNavScale(12), rowTop + rowH);
        RECT clipped = itemRect;
        clipped.top = std::max(clipped.top, content.top);
        clipped.bottom = std::min(clipped.bottom, content.bottom);
        if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point))
            continue;
        outEntry = quickNavigationEverythingResults_[i];
        return true;
    }

    return false;
}

inline std::vector<DesktopApp::QuickNavigationKeyboardTarget>
DesktopApp::GetQuickNavigationKeyboardTargets() const
{
    std::vector<QuickNavigationKeyboardTarget> targets;
    if (!quickNavigationOpen_) return targets;

    const RECT overlay = quickNavigationRect_;
    const RECT content = GetQuickNavigationContentRect(overlay);
    const std::vector<QuickNavigationEntry> entries = GetQuickNavigationEntries();
    targets.reserve(entries.size() + GetQuickNavigationVisibleAppResultCount() +
        quickNavigationEverythingResults_.size() + 2);
    for (size_t i = 0; i < entries.size(); ++i)
        targets.push_back({ QuickNavigationKeyboardTargetKind::Item,
            i, GetQuickNavigationItemRect(overlay, i) });

    if (GetQuickNavigationEffectiveSearchText().empty())
        return targets;

    const int columns = std::max(1, GetQuickNavigationColumnCount(overlay));
    const int desktopRows = entries.empty() ? 0 :
        (static_cast<int>(entries.size()) + columns - 1) / columns;
    const int headerHeight = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowHeight = QuickNavScale(46);
    const int desktopGridHeight = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight),
        QuickNavScale(kQuickNavigationItemRowGap));
    const int appHeaderTop = content.top + headerHeight + gap +
        desktopGridHeight + gap - quickNavigationScrollOffset_;
    int everythingHeaderTop = appHeaderTop;

    if (!quickNavigationAppResultIndices_.empty())
    {
        const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
        const int appRowsTop = appHeaderTop + headerHeight + gap;
        for (size_t i = 0; i < visibleAppCount; ++i)
        {
            const int top = appRowsTop + static_cast<int>(i) * rowHeight;
            targets.push_back({ QuickNavigationKeyboardTargetKind::App, i,
                MakeRect(content.left + QuickNavScale(8), top,
                    content.right - QuickNavScale(12), top + rowHeight) });
        }

        int appRowsHeight = static_cast<int>(visibleAppCount) * rowHeight;
        if (HasQuickNavigationAppExpandButton())
        {
            const int top = appRowsTop + appRowsHeight;
            targets.push_back({ QuickNavigationKeyboardTargetKind::ExpandApps, 0,
                MakeRect(content.left + QuickNavScale(8), top,
                    content.right - QuickNavScale(12), top + rowHeight) });
            appRowsHeight += rowHeight;
        }
        everythingHeaderTop = appRowsTop + appRowsHeight + gap;
    }

    const int everythingRowsTop = everythingHeaderTop + headerHeight + gap;
    for (size_t i = 0; i < quickNavigationEverythingResults_.size(); ++i)
    {
        const int top = everythingRowsTop + static_cast<int>(i) * rowHeight;
        targets.push_back({ QuickNavigationKeyboardTargetKind::Everything, i,
            MakeRect(content.left + QuickNavScale(8), top,
                content.right - QuickNavScale(12), top + rowHeight) });
    }
    if (HasQuickNavigationEverythingLoadMoreButton())
    {
        const int top = everythingRowsTop +
            static_cast<int>(quickNavigationEverythingResults_.size()) * rowHeight;
        targets.push_back({ QuickNavigationKeyboardTargetKind::LoadMoreEverything, 0,
            MakeRect(content.left + QuickNavScale(8), top,
                content.right - QuickNavScale(12), top + rowHeight) });
    }
    return targets;
}

inline bool DesktopApp::IsQuickNavigationKeyboardTarget(
    QuickNavigationKeyboardTargetKind kind, size_t index) const
{
    return quickNavigationKeyboardTargetKind_ == kind &&
        quickNavigationKeyboardTargetIndex_ == index;
}

inline void DesktopApp::ResetQuickNavigationKeyboardTarget()
{
    quickNavigationKeyboardTargetKind_ = QuickNavigationKeyboardTargetKind::None;
    quickNavigationKeyboardTargetIndex_ = 0;
}

inline void DesktopApp::EnsureQuickNavigationKeyboardTargetVisible(
    const RECT& targetRect)
{
    const RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
    const int margin = QuickNavScale(4);
    if (targetRect.top < content.top + margin)
        quickNavigationScrollOffset_ -= content.top + margin - targetRect.top;
    else if (targetRect.bottom > content.bottom - margin)
        quickNavigationScrollOffset_ += targetRect.bottom - (content.bottom - margin);
    quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
        GetQuickNavigationMaxScrollOffset(quickNavigationRect_));
}

inline bool DesktopApp::HandleQuickNavigationKeyboardInput(WPARAM key)
{
    if (quickNavigationInitialJumpOpen_)
        return
            HandleQuickNavigationInitialJumpKeyboardInput(
                key);
    const bool directionKey = key == VK_LEFT || key == VK_RIGHT ||
        key == VK_UP || key == VK_DOWN;
    if (!quickNavigationOpen_ || (!directionKey && key != VK_RETURN))
        return false;

    std::vector<QuickNavigationKeyboardTarget> targets =
        GetQuickNavigationKeyboardTargets();
    if (targets.empty())
        return true;

    auto findCurrent = [&]() -> size_t {
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (IsQuickNavigationKeyboardTarget(targets[i].kind, targets[i].index))
                return i;
        }
        return static_cast<size_t>(-1);
    };

    size_t current = findCurrent();
    if (key == VK_RETURN)
    {
        if (current >= targets.size()) current = 0;
        quickNavigationKeyboardTargetKind_ = targets[current].kind;
        quickNavigationKeyboardTargetIndex_ = targets[current].index;
        EnsureQuickNavigationKeyboardTargetVisible(targets[current].rect);

        targets = GetQuickNavigationKeyboardTargets();
        current = findCurrent();
        if (current >= targets.size()) return true;
        const RECT rect = targets[current].rect;
        const POINT point{ (rect.left + rect.right) / 2,
            (rect.top + rect.bottom) / 2 };
        ResetQuickNavigationKeyboardTarget();
        return HandleQuickNavigationClick(point);
    }

    size_t next = current;
    if (current >= targets.size())
    {
        next = (key == VK_LEFT || key == VK_UP) ? targets.size() - 1 : 0;
    }
    else
    {
        const RECT currentRect = targets[current].rect;
        const long long currentX =
            (static_cast<long long>(currentRect.left) + currentRect.right) / 2;
        const long long currentY =
            (static_cast<long long>(currentRect.top) + currentRect.bottom) / 2;
        long long bestScore = std::numeric_limits<long long>::max();
        size_t best = static_cast<size_t>(-1);
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (i == current) continue;
            const RECT candidateRect = targets[i].rect;
            const long long candidateX =
                (static_cast<long long>(candidateRect.left) + candidateRect.right) / 2;
            const long long candidateY =
                (static_cast<long long>(candidateRect.top) + candidateRect.bottom) / 2;
            const long long dx = candidateX - currentX;
            const long long dy = candidateY - currentY;
            const bool inDirection =
                (key == VK_LEFT && dx < 0) ||
                (key == VK_RIGHT && dx > 0) ||
                (key == VK_UP && dy < 0) ||
                (key == VK_DOWN && dy > 0);
            if (!inDirection) continue;

            const long long primary = (key == VK_LEFT || key == VK_RIGHT)
                ? std::abs(dx) : std::abs(dy);
            const long long secondary = (key == VK_LEFT || key == VK_RIGHT)
                ? std::abs(dy) : std::abs(dx);
            if (primary * 2 < secondary) continue;
            const long long score = primary * 4 + secondary;
            if (score < bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        if (best < targets.size())
            next = best;
        else if ((key == VK_LEFT || key == VK_UP) && current > 0)
            next = current - 1;
        else if ((key == VK_RIGHT || key == VK_DOWN) && current + 1 < targets.size())
            next = current + 1;
    }

    quickNavigationKeyboardTargetKind_ = targets[next].kind;
    quickNavigationKeyboardTargetIndex_ = targets[next].index;
    EnsureQuickNavigationKeyboardTargetVisible(targets[next].rect);
    InvalidateQuickNavigationWindow();
    return true;
}

inline int DesktopApp::GetQuickNavigationContentHeight(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int columns = GetQuickNavigationColumnCount(overlay);
    const QuickNavigationContentModel model =
        BuildQuickNavigationContentModel();
    const int desktopCount =
        static_cast<int>(model.entries.size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    if (GetQuickNavigationEffectiveSearchText().empty())
    {
        if (model.IsSectioned())
        {
            std::vector<size_t> counts;
            counts.reserve(model.sections.size());
            for (const auto& section :
                model.sections)
                counts.push_back(
                    section.entryCount);
            const auto layouts =
                snowdesktop::quick_navigation_rules::
                    BuildSectionLayouts(
                        counts, columns,
                        QuickNavScale(
                            kQuickNavigationCellHeight),
                        QuickNavScale(
                            kQuickNavigationItemRowGap),
                        QuickNavScale(28),
                        QuickNavScale(8),
                        QuickNavScale(12));
            return snowdesktop::
                quick_navigation_rules::
                    SectionedContentHeight(
                        layouts,
                        QuickNavScale(12));
        }
        const int rows = desktopCount == 0 ? 1 : desktopRows;
        return QuickNavigationRowsHeight(rows, QuickNavScale(kQuickNavigationCellHeight),
            QuickNavScale(kQuickNavigationItemRowGap));
    }

    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    int height = headerH + gap + desktopGridH
        + gap;
    if (!quickNavigationAppResultIndices_.empty())
    {
        height += headerH + gap
            + static_cast<int>(GetQuickNavigationVisibleAppResultCount()) * rowH
            + (HasQuickNavigationAppExpandButton() ? rowH : 0)
            + gap;
    }
    height += headerH + gap
        + static_cast<int>(quickNavigationEverythingResults_.size()) * rowH
        + (HasQuickNavigationEverythingLoadMoreButton() ? rowH : 0)
        + QuickNavScale(8);
    return std::max(height, std::max(1, static_cast<int>(content.bottom - content.top)));
}

inline int DesktopApp::GetQuickNavigationMaxScrollOffset(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int contentHeight = GetQuickNavigationContentHeight(overlay);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    return std::max(0, contentHeight - visibleHeight);
}

inline bool DesktopApp::GetQuickNavigationScrollbarGeometry(
    const RECT& overlay,
    RECT& outTrack, RECT& outThumb, int& outMaxScroll, int& outContentHeight) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    outContentHeight = GetQuickNavigationContentHeight(overlay);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    if (outContentHeight <= visibleHeight)
    {
        ZeroMemory(&outTrack, sizeof(outTrack));
        ZeroMemory(&outThumb, sizeof(outThumb));
        outMaxScroll = 0;
        return false;
    }
    const int trackW = QuickNavScale(5);
    outTrack = MakeRect(content.right - trackW - QuickNavScale(2), content.top + QuickNavScale(4),
        content.right - QuickNavScale(2), content.bottom - QuickNavScale(4));
    const int trackH = std::max<LONG>(1, outTrack.bottom - outTrack.top);
    const int thumbH = std::clamp(visibleHeight * trackH / outContentHeight, QuickNavScale(20), trackH);
    outMaxScroll = std::max(1, outContentHeight - visibleHeight);
    const int thumbTop = outTrack.top + quickNavigationScrollOffset_ * (trackH - thumbH) / outMaxScroll;
    outThumb = MakeRect(outTrack.left, thumbTop, outTrack.right, thumbTop + thumbH);
    return true;
}

/**
 * @brief 注销快捷导航热键
 */
inline void DesktopApp::UnregisterNavigationHotkey()
{
    if (navigationHotkeyRegistered_ && navigationHotkeyHwnd_)
    {
        UnregisterHotKey(navigationHotkeyHwnd_, kQuickNavigationHotkeyId);
        navigationHotkeyRegistered_ = false;
    }
    navigationHotkeyHwnd_ = nullptr;
}

/**
 * @brief 创建快捷导航窗口（若已存在则直接返回）
 * @return 窗口创建是否成功
 */
inline bool DesktopApp::CreateQuickNavigationWindow()
{
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        return true;

    quickNavigationHwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
        kQuickNavigationWindowClassName,
_LW("app.interact.snow_nav_title"),
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!quickNavigationHwnd_)
        return false;

    return true;
}

/**
 * @brief 销毁快捷导航窗口及其子控件
 */
inline void DesktopApp::DestroyQuickNavigationWindow()
{
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        KillTimer(
            quickNavigationHwnd_,
            kQuickNavigationAnimationTimerId);
    quickNavBackdropCompositor_.Reset();
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        RemoveWindowSubclass(quickNavigationSearchEdit_, &DesktopApp::QuickNavigationSearchSubclassProc, 1);
        DestroyWindow(quickNavigationSearchEdit_);
    }
    quickNavigationSearchEdit_ = nullptr;
    if (quickNavigationSearchFont_)
    {
        DeleteObject(quickNavigationSearchFont_);
        quickNavigationSearchFont_ = nullptr;
    }
    ResetQuickNavCompositionResources();
    quickNavDcompEffect_ = nullptr;
    if (quickNavDcompVisual_)
        quickNavDcompVisual_ = nullptr;
    if (quickNavDcompTarget_)
        quickNavDcompTarget_ = nullptr;
    quickNavTabTextFormat_.Reset();
    quickNavItemTextFormat_.Reset();
    quickNavPathTextFormat_.Reset();
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        DestroyWindow(quickNavigationHwnd_);
    quickNavigationHwnd_ = nullptr;
    quickNavigationAnimation_.ResetHidden();
}

/**
 * @brief 创建、同步或移除快捷导航窗口下方的原生毛玻璃层
 */
inline void DesktopApp::UpdateQuickNavigationBackdrop()
{
    if (!quickNavGlassTheme_ || !quickNavigationHwnd_ ||
        !IsWindow(quickNavigationHwnd_))
    {
        quickNavBackdropCompositor_.Reset();
        return;
    }

    if (!quickNavBackdropCompositor_.IsAvailable())
    {
        const bool initiallyVisible =
            quickNavigationAnimation_.GetVisual().visible;
        if (!quickNavBackdropCompositor_.InitializePopup(
                quickNavigationHwnd_, true, initiallyVisible))
        {
            std::wstring message = L"Quick navigation native backdrop unavailable: ";
            message += quickNavBackdropCompositor_.LastError();
            WriteCrashLogEntry(message.c_str());
            return;
        }
        WriteCrashLogEntry(L"Quick navigation native CompositionBackdropBrush initialized");
    }
    else
    {
        quickNavBackdropCompositor_.Reattach(quickNavigationHwnd_);
    }

    RECT clientRect = {
        quickNavigationRect_.left -
            quickNavigationHostRect_.left,
        quickNavigationRect_.top -
            quickNavigationHostRect_.top,
        quickNavigationRect_.right -
            quickNavigationHostRect_.left,
        quickNavigationRect_.bottom -
            quickNavigationHostRect_.top
    };
    if (IsRectEmptyRect(clientRect))
        return;
    const float cornerRadius = static_cast<float>(QuickNavScale(16)) / 2.0f;
    quickNavBackdropCompositor_.BeginFrame(true);
    quickNavBackdropCompositor_.AddPanel(clientRect, cornerRadius,
        quickNavBlurRadius_);
    quickNavBackdropCompositor_.EndFrame();
    const auto visual = quickNavigationAnimation_.GetVisual();
    const float anchorX = static_cast<float>(
        quickNavigationAnimationAnchorPoint_.x -
        quickNavigationHostRect_.left);
    const float anchorY = static_cast<float>(
        quickNavigationAnimationAnchorPoint_.y -
        quickNavigationHostRect_.top);
    quickNavBackdropCompositor_.SetVisualTransform(
        visual.scale, visual.opacity,
        anchorX, anchorY);
    quickNavBackdropCompositor_.SetVisible(visual.visible);
}

/**
 * @brief 确保快捷导航的搜索编辑框已创建
 */
inline void DesktopApp::EnsureQuickNavigationSearchEdit()
{
    if (!quickNavigationHwnd_ || !IsWindow(quickNavigationHwnd_))
        return;
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
        return;

    // 无重定向的 DComp 主窗口不能可靠承载 GDI 子控件，因此搜索框使用
    // 由快捷导航拥有的独立 popup HWND；它仍与主窗口位于同一 UI 线程。
    quickNavigationSearchEdit_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        L"EDIT", L"", WS_POPUP | ES_AUTOHSCROLL,
        0, 0, 1, 1, quickNavigationHwnd_, nullptr,
        instance_, nullptr);
    if (!quickNavigationSearchEdit_)
        return;
    SetWindowLongPtrW(quickNavigationSearchEdit_, GWLP_ID, 1002);

    quickNavigationSearchFont_ = CreateFontW(-QuickNavScale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(quickNavigationSearchEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(quickNavigationSearchFont_ ? quickNavigationSearchFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(quickNavigationSearchEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(QuickNavScale(10), QuickNavScale(10)));
    SendMessageW(quickNavigationSearchEdit_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(_LW("app.nav.search_hint")));
    SetWindowSubclass(quickNavigationSearchEdit_, &DesktopApp::QuickNavigationSearchSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
}

/**
 * @brief 更新快捷导航搜索编辑框的位置和大小
 */
inline void DesktopApp::UpdateQuickNavigationSearchEditRect()
{
    if (!quickNavigationSearchEdit_ || !IsWindow(quickNavigationSearchEdit_))
        return;
    RECT search = GetQuickNavigationSearchRect(quickNavigationRect_);
    SetWindowPos(quickNavigationSearchEdit_, HWND_TOPMOST,
        search.left + virtualLeft_ + QuickNavScale(4),
        search.top + virtualTop_ + QuickNavScale(6),
        std::max<LONG>(1, search.right - search.left - QuickNavScale(8)),
        std::max<LONG>(1, search.bottom - search.top - QuickNavScale(10)),
        SWP_NOACTIVATE);
}

inline std::wstring DesktopApp::GetQuickNavigationEffectiveSearchText() const
{
    if (quickNavigationSearchCompositionText_.empty())
        return quickNavigationSearchText_;

    std::wstring result = quickNavigationSearchText_;
    result += quickNavigationSearchCompositionText_;
    return result;
}

inline void DesktopApp::RefreshQuickNavigationSearchCompositionText(HWND editHwnd, LPARAM compositionFlags)
{
    if ((compositionFlags & GCS_COMPSTR) == 0)
        return;

    const std::wstring previousQuery = GetQuickNavigationEffectiveSearchText();
    quickNavigationSearchCompositionText_ = QuickNavigationReadImeCompositionString(editHwnd);
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
    {
        quickNavigationInitialJumpOpen_ = false;
        ResetQuickNavigationKeyboardTarget();
        quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
        RefreshQuickNavigationEverythingResults();
        quickNavigationScrollOffset_ = 0;
        InvalidateQuickNavigationWindow();
    }
}

inline void DesktopApp::ClearQuickNavigationSearchCompositionText()
{
    if (quickNavigationSearchCompositionText_.empty())
        return;

    const std::wstring previousQuery = GetQuickNavigationEffectiveSearchText();
    quickNavigationSearchCompositionText_.clear();
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
    {
        quickNavigationInitialJumpOpen_ = false;
        ResetQuickNavigationKeyboardTarget();
        quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
        RefreshQuickNavigationEverythingResults();
        quickNavigationScrollOffset_ = 0;
        InvalidateQuickNavigationWindow();
    }
}

/**
 * @brief 刷新快捷导航搜索文本内容（从编辑框读取）
 */
inline void DesktopApp::RefreshQuickNavigationSearchText()
{
    ResetQuickNavigationKeyboardTarget();
    quickNavigationInitialJumpOpen_ = false;
    std::wstring previousQuery = GetQuickNavigationEffectiveSearchText();
    quickNavigationSearchText_.clear();
    if (!quickNavigationSearchEdit_ || !IsWindow(quickNavigationSearchEdit_))
    {
        if (!previousQuery.empty())
            ClearQuickNavigationEverythingResults();
        return;
    }
    int len = GetWindowTextLengthW(quickNavigationSearchEdit_);
    if (len <= 0)
    {
        if (GetQuickNavigationEffectiveSearchText() != previousQuery)
        {
            quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
            RefreshQuickNavigationEverythingResults();
        }
        return;
    }
    std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(quickNavigationSearchEdit_, buffer.data(), len + 1);
    buffer.resize(static_cast<size_t>(len));
    quickNavigationSearchText_ = std::move(buffer);
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
    {
        quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
        RefreshQuickNavigationEverythingResults();
    }
}

inline void DesktopApp::ClearQuickNavigationEverythingResults()
{
    quickNavigationAppResultIndices_.clear();
    quickNavigationAppsExpanded_ = false;
    quickNavigationEverythingResults_.clear();
    quickNavigationEverythingHasMore_ = false;
    quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
}

inline int DesktopApp::GetQuickNavigationEverythingIconIndex(
    const std::wstring& path, bool isDirectory)
{
    if (isDirectory)
    {
        auto cached = quickNavigationEverythingIconCache_.find(L"<DIR>");
        if (cached != quickNavigationEverythingIconCache_.end())
            return cached->second;

        SHFILEINFOW info{};
        DWORD_PTR imageList = SHGetFileInfoW(
            path.empty() ? L"<DIR>" : path.c_str(),
            FILE_ATTRIBUTE_DIRECTORY,
            &info,
            sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        int iconIndex = imageList ? info.iIcon : -1;
        if (imageList)
            quickNavigationSystemImageListSmall_ = reinterpret_cast<HIMAGELIST>(imageList);
        quickNavigationEverythingIconCache_[L"<DIR>"] = iconIndex;
        return iconIndex;
    }

    std::wstring ext = ToUpperInvariant(PathFindExtensionW(path.c_str()));
    if (ext.empty())
        ext = L"<FILE>";

    bool perFileIcon = !path.empty() && (ext == L".EXE" || ext == L".LNK" || ext == L".DLL" ||
        ext == L".ICO" || ext == L".SCR" || ext == L".MSI" || ext == L".CPL");

    std::wstring cacheKey = perFileIcon ? ToUpperInvariant(path) : ext;

    auto cached = quickNavigationEverythingIconCache_.find(cacheKey);
    if (cached != quickNavigationEverythingIconCache_.end())
        return cached->second;

    SHFILEINFOW info{};
    DWORD_PTR imageList = 0;

    if (perFileIcon)
    {
        imageList = SHGetFileInfoW(
            path.c_str(), 0, &info, sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (!imageList)
        {
            imageList = SHGetFileInfoW(
                ext.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        }
    }
    else
    {
        imageList = SHGetFileInfoW(
            ext.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    }

    int iconIndex = imageList ? info.iIcon : -1;
    if (imageList)
        quickNavigationSystemImageListSmall_ = reinterpret_cast<HIMAGELIST>(imageList);

    quickNavigationEverythingIconCache_[cacheKey] = iconIndex;
    return iconIndex;
}

inline void DesktopApp::RefreshQuickNavigationEverythingResults()
{
    const bool preserveLoadedOrder =
        quickNavigationEverythingResultLimit_ > kQuickNavigationEverythingResultBatchSize &&
        !quickNavigationEverythingResults_.empty();
    std::vector<QuickNavigationEverythingEntry> previousResults;
    if (preserveLoadedOrder)
        previousResults = quickNavigationEverythingResults_;

    quickNavigationAppResultIndices_.clear();
    quickNavigationAppsExpanded_ = false;
    quickNavigationEverythingResults_.clear();
    quickNavigationEverythingHasMore_ = false;
    const std::wstring query = GetQuickNavigationEffectiveSearchText();
    if (query.empty())
        return;

    RefreshQuickNavigationAppResults();

    const DWORD requestLimit = std::max<DWORD>(
        quickNavigationEverythingResultLimit_,
        kQuickNavigationEverythingResultBatchSize);
    quickNavigationEverythingResultLimit_ = requestLimit;
    std::vector<EverythingSearchResult> searchResults =
        SearchEverythingCached(query, requestLimit);
    quickNavigationEverythingHasMore_ =
        searchResults.size() >= static_cast<size_t>(requestLimit);

    std::unordered_set<std::wstring> seenPaths;
    auto appendResult = [&](const EverythingSearchResult& result) {
        if (result.path.empty())
            return;
        if (snowdesktop::shell_item_visibility::
                IsAlwaysHidden(
                    result.name.empty()
                        ? result.path
                        : result.name))
            return;
        std::wstring normalizedPath = ToUpperInvariant(result.path);
        if (seenPaths.contains(normalizedPath))
            return;
        seenPaths.insert(std::move(normalizedPath));

        QuickNavigationEverythingEntry entry;
        entry.name = result.name.empty() ? FileNameFromPath(result.path) : result.name;
        entry.path = result.path;
        entry.dateModified = result.dateModified;
        entry.modifiedText = QuickNavigationFormatModifiedTime(result.dateModified);
        entry.isDirectory = result.isDirectory;
        entry.systemIconIndex = GetQuickNavigationEverythingIconIndex(entry.path, entry.isDirectory);
        quickNavigationEverythingResults_.push_back(std::move(entry));
    };

    if (preserveLoadedOrder)
    {
        std::unordered_map<std::wstring, size_t> resultIndicesByPath;
        resultIndicesByPath.reserve(searchResults.size());
        for (size_t i = 0; i < searchResults.size(); ++i)
        {
            if (searchResults[i].path.empty())
                continue;
            resultIndicesByPath.emplace(ToUpperInvariant(searchResults[i].path), i);
        }

        for (const auto& previous : previousResults)
        {
            auto found = resultIndicesByPath.find(ToUpperInvariant(previous.path));
            if (found == resultIndicesByPath.end())
                continue;
            appendResult(searchResults[found->second]);
        }
    }

    for (const auto& result : searchResults)
        appendResult(result);
}

/**
 * @brief 定位并显示快捷导航窗口（含圆角区域设置）
 */
inline void DesktopApp::PositionQuickNavigationWindow()
{
    if (!quickNavigationHwnd_ || !IsWindow(quickNavigationHwnd_))
        return;

    quickNavigationRect_ = GetQuickNavigationRect();
    if (quickNavigationAnimationAnchorMode_ ==
            snowdesktop::
                quick_navigation_animation_rules::
                    AnchorMode::Pointer &&
        virtualWidth_ > 0 && virtualHeight_ > 0)
    {
        quickNavigationHostRect_ =
            MakeRect(
                0, 0,
                virtualWidth_,
                virtualHeight_);
    }
    else
    {
        const RECT anchorRect = MakeRect(
            quickNavigationAnimationAnchorPoint_.x - 1,
            quickNavigationAnimationAnchorPoint_.y - 1,
            quickNavigationAnimationAnchorPoint_.x + 2,
            quickNavigationAnimationAnchorPoint_.y + 2);
        UnionRect(
            &quickNavigationHostRect_,
            &quickNavigationRect_,
            &anchorRect);
        InflateRect(
            &quickNavigationHostRect_, 2, 2);
    }
    const int width = std::max<LONG>(
        1,
        quickNavigationHostRect_.right -
            quickNavigationHostRect_.left);
    const int height = std::max<LONG>(
        1,
        quickNavigationHostRect_.bottom -
            quickNavigationHostRect_.top);

    // Reserve the complete path between the Dock search icon and the panel.
    // ApplyQuickNavigationAnimationFrame narrows the actual HWND region to the
    // current rounded card, so this transparent reserve never captures input.
    SetWindowRgn(
        quickNavigationHwnd_, nullptr, FALSE);
    const DWM_WINDOW_CORNER_PREFERENCE
        cornerPreference =
            DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(
        quickNavigationHwnd_,
        DWMWA_WINDOW_CORNER_PREFERENCE,
        &cornerPreference,
        sizeof(cornerPreference));

    SetWindowPos(quickNavigationHwnd_, HWND_TOPMOST,
        quickNavigationHostRect_.left + virtualLeft_,
        quickNavigationHostRect_.top + virtualTop_,
        width, height,
        SWP_SHOWWINDOW | SWP_NOACTIVATE);
    UpdateQuickNavigationBackdrop();
    EnsureQuickNavigationSearchEdit();
    UpdateQuickNavigationSearchEditRect();
    ApplyQuickNavigationAnimationFrame();
}

/**
 * @brief 使快捷导航窗口失效并触发重绘
 */
inline void DesktopApp::InvalidateQuickNavigationWindow()
{
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        InvalidateRect(quickNavigationHwnd_, nullptr, FALSE);
}

/**
 * @brief 应用并注册导航热键
 */
inline void DesktopApp::ApplyNavigationHotkey()
{
    UnregisterNavigationHotkey();
    if (!navigationSettings_.enabled || navigationSettings_.virtualKey == 0)
        return;

    HWND hotkeyWindow = inputHwnd_ && IsWindow(inputHwnd_)
        ? inputHwnd_
        : (controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_);
    if (!hotkeyWindow)
        return;

    UINT modifiers = navigationSettings_.modifiers | MOD_NOREPEAT;
    navigationHotkeyRegistered_ =
        RegisterHotKey(hotkeyWindow, kQuickNavigationHotkeyId,
            modifiers, navigationSettings_.virtualKey) != FALSE;
    if (navigationHotkeyRegistered_)
        navigationHotkeyHwnd_ = hotkeyWindow;
}

/**
 * @brief 获取指定标签页的显示名称
 */
inline std::wstring DesktopApp::GetQuickNavTabLabel(size_t tab) const
{
    if (tab == 0) return _LW("app.nav.tab_desktop");
    if (tab == 1) return _LW("app.nav.tab_mapping");
    std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
    if (tab - 2 >= ci.size()) return L"";
    const DesktopWidget& widget = widgets_[ci[tab - 2]];
    if (!widget.title.empty())
        return widget.title;
    if (widget.type == DesktopWidgetType::FileCategories)
        return _LW("widget.desktop_files");
    if (widget.type == DesktopWidgetType::FolderMapping)
        return _LW("widget.folder_mapping");
    return _LW("widget.collection") + std::to_wstring(tab - 1);
}

/**
 * @brief 根据文字测量宽度，更新 quickNavTabWidths_
 */
inline void DesktopApp::UpdateQuickNavTabWidths()
{
    std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
    const size_t tabCount = ci.size() + 2;
    quickNavTabWidths_.resize(tabCount, QuickNavScale(72));

    if (!dwriteFactory_ || !quickNavTabTextFormat_)
        return;

    for (size_t i = 0; i < tabCount; ++i)
    {
        std::wstring label = GetQuickNavTabLabel(i);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(label.c_str(),
            static_cast<UINT32>(label.size()), quickNavTabTextFormat_.Get(),
            10000.0f, 10000.0f, &layout)) || !layout)
            continue;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        quickNavTabWidths_[i] = static_cast<int>(std::clamp(
            static_cast<LONG>(static_cast<long>(std::ceil(metrics.widthIncludingTrailingWhitespace)) + QuickNavScale(20)),
            static_cast<LONG>(QuickNavScale(72)), static_cast<LONG>(QuickNavScale(200))));
    }
}

/**
 * @brief 根据拖拽位移计算目标标签索引
 * @param dragTab 被拖拽的标签索引
 * @param deltaX 拖拽水平位移
 * @return 目标标签索引（≥2）
 */
inline int DesktopApp::GetQuickNavTabDragTarget(size_t dragTab, int deltaX) const
{
    const auto& tw = quickNavTabWidths_;
    if (tw.empty() || dragTab >= tw.size()) return 2;
    RECT overlay = quickNavigationRect_;
    int srcCenter = GetQuickNavigationTabRect(overlay, dragTab).left
        + tw[dragTab] / 2 + deltaX;
    int target = 2;
    for (size_t i = 2; i < tw.size(); ++i)
    {
        RECT r = GetQuickNavigationTabRect(overlay, i);
        if (srcCenter < r.left + tw[i] / 2) { target = static_cast<int>(i); break; }
    }
    if (target > static_cast<int>(tw.size()) - 1)
        target = static_cast<int>(tw.size()) - 1;
    return target;
}

/**
 * @brief 打开快捷导航面板
 */
inline void DesktopApp::OpenQuickNavigation(
    bool fromDockSearch)
{
    if (dragSession_.IsActive() || externalDragActive_)
        return;
    CloseCollectionPopup();

    const bool reversingClose =
        quickNavigationAnimation_.IsClosing() &&
        quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_);
    if (reversingClose)
    {
        quickNavigationOpen_ = true;
        if (quickNavigationSearchEdit_ &&
            IsWindow(quickNavigationSearchEdit_))
        {
            EnableWindow(quickNavigationSearchEdit_, TRUE);
            ShowWindow(
                quickNavigationSearchEdit_,
                SW_SHOWNOACTIVATE);
        }
        if (snowdesktop::dock_launch_animation::
                SystemAnimationsEnabled())
        {
            quickNavigationAnimation_.Open(
                GetTickCount64());
            SetTimer(
                quickNavigationHwnd_,
                kQuickNavigationAnimationTimerId,
                snowdesktop::
                    quick_navigation_animation_rules::
                        kFrameIntervalMs,
                nullptr);
        }
        else
        {
            quickNavigationAnimation_.
                ShowImmediately();
            KillTimer(
                quickNavigationHwnd_,
                kQuickNavigationAnimationTimerId);
        }
        ApplyQuickNavigationAnimationFrame();
        if (quickNavigationSearchEdit_ &&
            IsWindow(quickNavigationSearchEdit_))
        {
            SetForegroundWindow(
                quickNavigationSearchEdit_);
            SetFocus(quickNavigationSearchEdit_);
            SendMessageW(
                quickNavigationSearchEdit_,
                EM_SETSEL, 0, -1);
        }
        else
        {
            SetForegroundWindow(
                quickNavigationHwnd_);
            SetFocus(quickNavigationHwnd_);
        }
        return;
    }

    POINT cursor{};
    if (GetCursorPos(&cursor))
    {
        quickNavigationOpenPoint_ = { cursor.x - virtualLeft_, cursor.y - virtualTop_ };
        lastMousePoint_ = quickNavigationOpenPoint_;
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        UINT dpiX = 96, dpiY = 96;
        if (monitor)
            GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        quickNavDpiScale_ = static_cast<float>(dpiX) / 96.0f;
    }
    else
    {
        quickNavigationOpenPoint_ = lastMousePoint_;
    }
    quickNavigationAnimationAnchorPoint_ =
        quickNavigationOpenPoint_;
    quickNavigationAnimationAnchorMode_ =
        snowdesktop::
            quick_navigation_animation_rules::
                AnchorMode::Pointer;
    if (fromDockSearch)
    {
        if (DockContainer* dock =
                GetDockContainerAtPoint(
                    quickNavigationOpenPoint_);
            dock &&
            dock->IsSearchPoint(
                quickNavigationOpenPoint_))
        {
            const RECT searchRect =
                dock->GetSearchRect();
            if (!IsRectEmptyRect(searchRect))
            {
                quickNavigationAnimationAnchorPoint_ = {
                    (searchRect.left +
                        searchRect.right) / 2,
                    (searchRect.top +
                        searchRect.bottom) / 2
                };
                quickNavigationAnimationAnchorMode_ =
                    snowdesktop::
                        quick_navigation_animation_rules::
                            AnchorMode::
                                DockSearch;
            }
        }
    }

    EnsureQuickNavTextFormats();
    UpdateQuickNavTabWidths();
    quickNavigationOpen_ = true;
    EnsureNavTabOrder();
    if (quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-2))
    {
        // keep "映射文件夹全部" tab selection
    }
    else if (quickNavigationActiveWidgetIndex_ >= widgets_.size() ||
        (widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::Collection &&
         widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::FileCategories &&
         widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::FolderMapping))
    {
        quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
    }
    quickNavigationScrollOffset_ = 0;
    quickNavigationTabScrollOffset_ = 0;
    quickNavigationInitialJumpOpen_ = false;
    ResetQuickNavigationKeyboardTarget();
    quickNavigationSearchText_.clear();
    quickNavigationSearchCompositionText_.clear();
    ClearQuickNavigationEverythingResults();
    StartQuickNavigationAppIndexing();
    quickNavigationAnimation_.ResetHidden();
    if (!CreateQuickNavigationWindow())
    {
        quickNavigationOpen_ = false;
        MessageBeep(MB_ICONWARNING);
        return;
    }
    PositionQuickNavigationWindow();
    if (quickNavigationSearchEdit_)
        SetWindowTextW(quickNavigationSearchEdit_, L"");
    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
    {
        EnableWindow(quickNavigationSearchEdit_, TRUE);
        ShowWindow(
            quickNavigationSearchEdit_,
            SW_SHOWNOACTIVATE);
    }
    if (snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled())
    {
        quickNavigationAnimation_.Open(
            GetTickCount64());
        SetTimer(
            quickNavigationHwnd_,
            kQuickNavigationAnimationTimerId,
            snowdesktop::
                quick_navigation_animation_rules::
                    kFrameIntervalMs,
            nullptr);
    }
    else
    {
        quickNavigationAnimation_.
            ShowImmediately();
    }
    ApplyQuickNavigationAnimationFrame();
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        SetForegroundWindow(quickNavigationSearchEdit_);
        SetFocus(quickNavigationSearchEdit_);
        SendMessageW(quickNavigationSearchEdit_, EM_SETSEL, 0, -1);
    }
    else
    {
        SetForegroundWindow(quickNavigationHwnd_);
        SetFocus(quickNavigationHwnd_);
    }
    InvalidateQuickNavigationWindow();
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 关闭快捷导航面板
 */
inline void DesktopApp::CloseQuickNavigation()
{
    if (!quickNavigationOpen_) return;
    if (renamingQuickNavigationItem_ &&
        renameEdit_ && IsWindow(renameEdit_))
        CommitRename(false);
    if (snowdesktop::
            quick_navigation_animation_rules::
                ShouldRefreshCloseAnchor(
                    quickNavigationAnimationAnchorMode_))
    {
        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            const POINT currentPointer = {
                cursor.x - virtualLeft_,
                cursor.y - virtualTop_
            };
            if (currentPointer.x !=
                    quickNavigationAnimationAnchorPoint_.x ||
                currentPointer.y !=
                    quickNavigationAnimationAnchorPoint_.y)
            {
                quickNavigationAnimationAnchorPoint_ =
                    currentPointer;
            }
        }
    }
    quickNavigationOpen_ = false;
    ResetQuickNavigationKeyboardTarget();
    quickNavTabDragIndex_ = static_cast<size_t>(-1);
    quickNavTabDragDeltaX_ = 0;
    quickNavTabDragging_ = false;
    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
        EnableWindow(quickNavigationSearchEdit_, FALSE);
    if (customDesktopVisible_)
        FocusDesktopInputWindow();

    if (!quickNavigationHwnd_ ||
        !IsWindow(quickNavigationHwnd_))
    {
        FinalizeCloseQuickNavigation();
        return;
    }

    if (snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled())
    {
        quickNavigationAnimation_.Close(
            GetTickCount64());
    }
    else
    {
        quickNavigationAnimation_.ResetHidden();
    }
    if (quickNavigationAnimation_.IsHidden())
    {
        FinalizeCloseQuickNavigation();
        return;
    }
    SetTimer(
        quickNavigationHwnd_,
        kQuickNavigationAnimationTimerId,
        snowdesktop::
            quick_navigation_animation_rules::
                kFrameIntervalMs,
        nullptr);
    InvalidateQuickNavigationWindow();
    ApplyQuickNavigationAnimationFrame();
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::ApplyQuickNavigationAnimationFrame()
{
    const auto visual =
        quickNavigationAnimation_.GetVisual();
    const float anchorX = static_cast<float>(
        quickNavigationAnimationAnchorPoint_.x -
        quickNavigationHostRect_.left);
    const float anchorY = static_cast<float>(
        quickNavigationAnimationAnchorPoint_.y -
        quickNavigationHostRect_.top);

    if (quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_))
    {
        const auto scaleCoordinate =
            [scale = visual.scale](
                float value, float anchor) {
                return snowdesktop::
                    quick_navigation_animation_rules::
                        ScaleCoordinate(
                            value, anchor, scale);
            };
        const float panelLeft =
            static_cast<float>(
                quickNavigationRect_.left -
                quickNavigationHostRect_.left);
        const float panelTop =
            static_cast<float>(
                quickNavigationRect_.top -
                quickNavigationHostRect_.top);
        const float panelRight =
            static_cast<float>(
                quickNavigationRect_.right -
                quickNavigationHostRect_.left);
        const float panelBottom =
            static_cast<float>(
                quickNavigationRect_.bottom -
                quickNavigationHostRect_.top);
        const int visibleLeft = static_cast<int>(
            std::floor(
                scaleCoordinate(
                    panelLeft, anchorX)));
        const int visibleTop = static_cast<int>(
            std::floor(
                scaleCoordinate(
                    panelTop, anchorY)));
        const int visibleRight = static_cast<int>(
            std::ceil(
                scaleCoordinate(
                    panelRight, anchorX)));
        const int visibleBottom = static_cast<int>(
            std::ceil(
                scaleCoordinate(
                    panelBottom, anchorY)));
        const int cornerDiameter =
            std::max(
                2,
                static_cast<int>(
                    std::lround(
                        static_cast<float>(
                            QuickNavScale(16)) *
                        visual.scale)));
        if (HRGN visibleRegion =
                CreateRoundRectRgn(
                    visibleLeft,
                    visibleTop,
                    visibleRight + 1,
                    visibleBottom + 1,
                    cornerDiameter,
                    cornerDiameter))
        {
            if (!SetWindowRgn(
                    quickNavigationHwnd_,
                    visibleRegion, FALSE))
            {
                DeleteObject(visibleRegion);
            }
        }
    }

    quickNavBackdropCompositor_.SetVisualTransform(
        visual.scale, visual.opacity,
        anchorX, anchorY);
    quickNavBackdropCompositor_.SetVisible(
        visual.visible);

    if (quickNavDcompVisual_)
    {
        if (quickNavigationHwnd_ &&
            IsWindow(quickNavigationHwnd_))
        {
            const D2D1_MATRIX_3X2_F transform =
                D2D1::Matrix3x2F::Scale(
                    visual.scale,
                    visual.scale,
                    D2D1::Point2F(
                        static_cast<float>(
                            quickNavigationAnimationAnchorPoint_.x -
                            quickNavigationRect_.left),
                        static_cast<float>(
                            quickNavigationAnimationAnchorPoint_.y -
                            quickNavigationRect_.top)));
            quickNavDcompVisual_->SetOffsetX(
                static_cast<float>(
                    quickNavigationRect_.left -
                    quickNavigationHostRect_.left));
            quickNavDcompVisual_->SetOffsetY(
                static_cast<float>(
                    quickNavigationRect_.top -
                    quickNavigationHostRect_.top));
            quickNavDcompVisual_->SetTransform(
                transform);
            if (quickNavDcompEffect_)
            {
                quickNavDcompEffect_->SetOpacity(
                    visual.opacity);
            }
            if (dcompDevice_)
                dcompDevice_->Commit();
        }
    }

    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
    {
        if (!visual.visible)
        {
            ShowWindow(
                quickNavigationSearchEdit_,
                SW_HIDE);
            return;
        }

        const RECT search =
            GetQuickNavigationSearchRect(
                quickNavigationRect_);
        const float screenAnchorX =
            static_cast<float>(
                quickNavigationAnimationAnchorPoint_.x +
                virtualLeft_);
        const float screenAnchorY =
            static_cast<float>(
                quickNavigationAnimationAnchorPoint_.y +
                virtualTop_);
        const float targetLeft =
            static_cast<float>(
                search.left + virtualLeft_ +
                QuickNavScale(4));
        const float targetTop =
            static_cast<float>(
                search.top + virtualTop_ +
                QuickNavScale(6));
        const float targetRight =
            static_cast<float>(
                search.right + virtualLeft_ -
                QuickNavScale(4));
        const float targetBottom =
            static_cast<float>(
                search.bottom + virtualTop_ -
                QuickNavScale(4));
        const int left = static_cast<int>(
            std::lround(
                snowdesktop::
                    quick_navigation_animation_rules::
                        ScaleCoordinate(
                            targetLeft,
                            screenAnchorX,
                            visual.scale)));
        const int top = static_cast<int>(
            std::lround(
                snowdesktop::
                    quick_navigation_animation_rules::
                        ScaleCoordinate(
                            targetTop,
                            screenAnchorY,
                            visual.scale)));
        const int right = static_cast<int>(
            std::lround(
                snowdesktop::
                    quick_navigation_animation_rules::
                        ScaleCoordinate(
                            targetRight,
                            screenAnchorX,
                            visual.scale)));
        const int bottom = static_cast<int>(
            std::lround(
                snowdesktop::
                    quick_navigation_animation_rules::
                        ScaleCoordinate(
                            targetBottom,
                            screenAnchorY,
                            visual.scale)));
        const int editWidth =
            std::max(1, right - left);
        const int editHeight =
            std::max(1, bottom - top);
        const int editCornerDiameter =
            std::max(
                2,
                static_cast<int>(
                    std::lround(
                        static_cast<float>(
                            QuickNavScale(8)) *
                        visual.scale)));
        if (HRGN editRegion =
                CreateRoundRectRgn(
                    0, 0,
                    editWidth + 1,
                    editHeight + 1,
                    editCornerDiameter,
                    editCornerDiameter))
        {
            if (!SetWindowRgn(
                    quickNavigationSearchEdit_,
                    editRegion, FALSE))
            {
                DeleteObject(editRegion);
            }
        }
        SetLayeredWindowAttributes(
            quickNavigationSearchEdit_,
            0,
            static_cast<BYTE>(std::clamp(
                std::lround(
                    visual.opacity * 255.0f),
                0L, 255L)),
            LWA_ALPHA);
        SetWindowPos(
            quickNavigationSearchEdit_,
            HWND_TOPMOST,
            left, top,
            editWidth,
            editHeight,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

inline void DesktopApp::FinalizeCloseQuickNavigation()
{
    if (quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_))
    {
        KillTimer(
            quickNavigationHwnd_,
            kQuickNavigationAnimationTimerId);
    }
    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
    {
        ShowWindow(
            quickNavigationSearchEdit_,
            SW_HIDE);
    }
    if (quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_))
    {
        ShowWindow(
            quickNavigationHwnd_,
            SW_HIDE);
    }

    quickNavigationScrollOffset_ = 0;
    quickNavigationTabScrollOffset_ = 0;
    quickNavigationInitialJumpOpen_ = false;
    quickNavigationSearchText_.clear();
    quickNavigationSearchCompositionText_.clear();
    ClearQuickNavigationEverythingResults();
    quickNavigationRect_ = {};
    quickNavigationHostRect_ = {};
    quickNavigationAnimation_.ResetHidden();
    DestroyQuickNavigationWindow();
    InvalidateDragStaticScene();
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::BeginQuickNavigationItemRename(
    const std::wstring& name, bool isDirectory)
{
    if (renameEdit_ || name.empty() ||
        !quickNavigationOpen_ ||
        !quickNavigationHwnd_ ||
        !IsWindow(quickNavigationHwnd_) ||
        IsRectEmptyRect(
            quickNavigationRenameItemRect_))
        return;

    const RECT itemRect =
        quickNavigationRenameItemRect_;
    const RECT iconRect =
        GetQuickNavItemIconRect(itemRect);
    const int horizontalPad = QuickNavScale(3);
    const int textTop = std::max<LONG>(
        itemRect.top,
        iconRect.bottom +
            std::max(1, QuickNavScale(2)));
    RECT editRect = MakeRect(
        itemRect.left + horizontalPad,
        textTop,
        itemRect.right - horizontalPad,
        std::min<LONG>(
            itemRect.bottom,
            textTop + QuickNavScale(32)));
    if (IsRectEmptyRect(editRect))
        return;

    renameCommitPending_ = false;
    renamingQuickNavigationItem_ = true;
    // The no-redirection DComp host cannot reliably display GDI child
    // controls. Match the search box: use an owned popup positioned over the
    // item's name area so the editor remains visually inside the panel.
    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE |
            WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST,
        L"EDIT", name.c_str(),
        WS_POPUP |
            ES_CENTER | ES_AUTOHSCROLL,
        editRect.left + virtualLeft_,
        editRect.top + virtualTop_,
        editRect.right - editRect.left,
        editRect.bottom - editRect.top,
        quickNavigationHwnd_, nullptr,
        instance_, nullptr);
    if (!renameEdit_)
    {
        renamingQuickNavigationItem_ = false;
        return;
    }

    if (renameFont_)
        DeleteObject(renameFont_);
    renameFont_ = CreateFontW(
        -std::max(1, QuickNavScale(13)),
        0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    SendMessageW(
        renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(
            renameFont_
                ? renameFont_
                : GetStockObject(
                    DEFAULT_GUI_FONT)),
        TRUE);
    SendMessageW(
        renameEdit_, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(
            std::max(1, QuickNavScale(4)),
            std::max(1, QuickNavScale(4))));
    SetWindowSubclass(
        renameEdit_,
        &DesktopApp::RenameEditSubclassProc,
        1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(
        renameEdit_, HWND_TOPMOST,
        editRect.left + virtualLeft_,
        editRect.top + virtualTop_,
        editRect.right - editRect.left,
        editRect.bottom - editRect.top,
        SWP_SHOWWINDOW);

    int selectionEnd = -1;
    if (!isDirectory)
    {
        const size_t dot =
            name.find_last_of(L'.');
        if (dot != std::wstring::npos &&
            dot > 0 && dot + 1 < name.size())
            selectionEnd =
                static_cast<int>(dot);
    }
    SendMessageW(
        renameEdit_, EM_SETSEL,
        0, selectionEnd);
    SetFocus(renameEdit_);
}

inline void DesktopApp::
BeginQuickNavigationDesktopItemRename(
    size_t itemIndex)
{
    if (itemIndex >= items_.size() ||
        !items_[itemIndex].
            desktopIconClsid.empty())
        return;

    wchar_t path[MAX_PATH]{};
    if (!SHGetPathFromIDListW(
            items_[itemIndex].
                absolutePidl.get(),
            path))
        return;
    const DWORD attributes =
        GetFileAttributesW(path);
    const bool isDirectory =
        attributes !=
            INVALID_FILE_ATTRIBUTES &&
        (attributes &
            FILE_ATTRIBUTE_DIRECTORY) != 0;

    renameIndex_ = itemIndex;
    renamingFolderEntry_ = false;
    BeginQuickNavigationItemRename(
        items_[itemIndex].name,
        isDirectory);
    if (!renamingQuickNavigationItem_)
        renameIndex_ =
            static_cast<size_t>(-1);
}

inline void DesktopApp::
BeginQuickNavigationFolderEntryRename(
    size_t widgetIndex, size_t entryIndex)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type !=
            DesktopWidgetType::FolderMapping ||
        entryIndex >=
            widgets_[widgetIndex].
                folderEntries.size())
        return;

    const FolderEntry& entry =
        widgets_[widgetIndex].
            folderEntries[entryIndex];
    renamingFolderEntry_ = true;
    renameFolderWidgetIndex_ = widgetIndex;
    renameFolderEntryIndex_ = entryIndex;
    BeginQuickNavigationItemRename(
        entry.name, entry.isDirectory);
    if (!renamingQuickNavigationItem_)
    {
        renamingFolderEntry_ = false;
        renameFolderWidgetIndex_ =
            static_cast<size_t>(-1);
        renameFolderEntryIndex_ =
            static_cast<size_t>(-1);
    }
}

/**
 * @brief 切换快捷导航面板的打开/关闭状态
 */
inline void DesktopApp::ToggleQuickNavigation()
{
    if (quickNavigationOpen_)
        CloseQuickNavigation();
    else
        OpenQuickNavigation();
}

/**
 * @brief 处理快捷导航面板内的点击事件
 * @param point 点击坐标（客户端坐标）
 * @return 是否已处理
 */
inline bool DesktopApp::HandleQuickNavigationClick(POINT point)
{
    if (!quickNavigationOpen_)
        return false;
    ResetQuickNavigationKeyboardTarget();

    RECT overlay = quickNavigationRect_;
    if (!PtInRect(&overlay, point))
    {
        CloseQuickNavigation();
        return true;
    }

    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    const bool searching = !GetQuickNavigationEffectiveSearchText().empty();
    if (!searching)
    {
        if (TrySetQuickNavigationDesktopViewModeAtPoint(
                point))
            return true;

        RECT tab0Rect = GetQuickNavigationTabRect(overlay, 0);
        if (PtInRect(&tab0Rect, point))
        {
            quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
            quickNavigationScrollOffset_ = 0;
            quickNavigationInitialJumpOpen_ = false;
            InvalidateQuickNavigationWindow();
            return true;
        }
        RECT tab1Rect = GetQuickNavigationTabRect(overlay, 1);
        if (PtInRect(&tab1Rect, point))
        {
            quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-2);
            quickNavigationScrollOffset_ = 0;
            quickNavigationInitialJumpOpen_ = false;
            InvalidateQuickNavigationWindow();
            return true;
        }
    }

    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return true;

    if (HandleQuickNavigationInitialJumpClick(
            point))
        return true;

    if (!searching &&
        quickNavigationActiveWidgetIndex_ ==
            static_cast<size_t>(-1) &&
        navigationSettings_.desktopViewMode ==
            QuickNavigationDesktopViewMode::Initial)
    {
        const QuickNavigationContentModel model =
            BuildQuickNavigationContentModel();
        for (size_t sectionIndex = 0;
            sectionIndex < model.sections.size();
            ++sectionIndex)
        {
            const RECT header =
                GetQuickNavigationSectionHeaderRect(
                    overlay, sectionIndex,
                    model);
            if (!PtInRect(&header, point))
                continue;
            const std::wstring& label =
                model.sections[sectionIndex].label;
            if (label.size() == 1)
            {
                quickNavigationInitialJumpSelection_ =
                    snowdesktop::
                        quick_navigation_rules::
                            InitialJumpBucketIndex(
                                label.front());
                quickNavigationInitialJumpOpen_ =
                    true;
                ResetQuickNavigationKeyboardTarget();
                InvalidateQuickNavigationWindow();
            }
            return true;
        }
    }

    if (!everythingSearchAvailable_ && searching)
    {
        std::vector<QuickNavigationEntry> entries = GetQuickNavigationEntries();
        bool onNotice = false;
        if (entries.empty() && quickNavigationEverythingResults_.empty())
        {
            onNotice = true;
        }
        else
        {
            const int columns = GetQuickNavigationColumnCount(overlay);
            const int desktopRows = entries.empty() ? 0 :
                (static_cast<int>(entries.size()) + columns - 1) / columns;
            const int headerH = QuickNavScale(28);
            const int gap = QuickNavScale(8);
            const int rowH = QuickNavScale(46);
            const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
            const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
                QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
            const int appSectionHeight = quickNavigationAppResultIndices_.empty()
                ? 0
                : headerH + gap + static_cast<int>(visibleAppCount) * rowH +
                    (HasQuickNavigationAppExpandButton() ? rowH : 0) + gap;
            const int listHeaderTop = content.top + headerH
                + desktopGridH
                + gap + appSectionHeight - quickNavigationScrollOffset_;
            RECT noticeHeader = MakeRect(
                content.left + QuickNavScale(8),
                listHeaderTop,
                content.right - QuickNavScale(12),
                listHeaderTop + headerH);
            onNotice = PtInRect(&noticeHeader, point);
        }
        if (onNotice)
        {
            if (!quickNavigationAppsIndexed_)
            {
                StartQuickNavigationAppIndexing();
                InvalidateQuickNavigationWindow();
                return true;
            }

            const bool hasEverythingApp = FindQuickNavigationEverythingAppEntry() != nullptr;
            if (hasEverythingApp)
            {
                CloseQuickNavigation();
                TryLaunchQuickNavigationEverythingApp();
            }
            else
            {
                CloseQuickNavigation();
                ShellExecuteW(nullptr, L"open",
                    L"https://www.voidtools.com/zh-cn/downloads/",
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
            return true;
        }
    }

    if (TryExpandQuickNavigationAppsAtPoint(point))
        return true;

    if (TryLoadMoreQuickNavigationEverythingResultsAtPoint(point))
        return true;

    const QuickNavigationAppEntry* appEntry = nullptr;
    if (TryGetQuickNavigationAppEntryAtPoint(point, appEntry) &&
        appEntry && appEntry->absolutePidl.get())
    {
        CloseQuickNavigation();
        LaunchQuickNavigationAppEntry(*appEntry);
        return true;
    }

    QuickNavigationEverythingEntry everythingEntry;
    if (TryGetQuickNavigationEverythingEntryAtPoint(point, everythingEntry) &&
        !everythingEntry.path.empty())
    {
        CloseQuickNavigation();
        ShellExecuteW(nullptr, L"open", everythingEntry.path.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

    std::vector<QuickNavigationEntry> entries = GetQuickNavigationEntries();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        RECT itemRect = GetQuickNavigationItemRect(overlay, i);
        RECT clipped = itemRect;
        clipped.top = std::max(clipped.top, content.top);
        clipped.bottom = std::min(clipped.bottom, content.bottom);
        if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point)) continue;

        const QuickNavigationEntry entry = std::move(entries[i]);
        CloseQuickNavigation();
        if (entry.kind == QuickNavigationEntry::Kind::DesktopItem &&
            entry.itemIndex != static_cast<size_t>(-1) && entry.itemIndex < items_.size())
        {
            LaunchDesktopItem(entry.itemIndex, true);
        }
        else if (entry.kind == QuickNavigationEntry::Kind::FolderEntry && !entry.path.empty())
        {
            ShellExecuteW(nullptr, L"open", entry.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return true;
    }

    return true;
}

inline bool DesktopApp::HandleQuickNavigationRightClick(POINT point, POINT screenPoint)
{
    if (!quickNavigationOpen_)
        return false;

    const QuickNavigationAppEntry* appEntry = nullptr;
    if (TryGetQuickNavigationAppEntryAtPoint(point, appEntry) && appEntry)
    {
        ShowQuickNavigationAppContextMenu(*appEntry, screenPoint);
        return true;
    }

    QuickNavigationEverythingEntry entry;
    if (TryGetQuickNavigationEverythingEntryAtPoint(point, entry))
    {
        ShowQuickNavigationEverythingContextMenu(entry, screenPoint);
        return true;
    }

    const RECT overlay = quickNavigationRect_;
    const RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    std::vector<QuickNavigationEntry> entries =
        GetQuickNavigationEntries();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        RECT itemRect =
            GetQuickNavigationItemRect(overlay, i);
        RECT clipped = itemRect;
        clipped.top =
            std::max(clipped.top, content.top);
        clipped.bottom =
            std::min(clipped.bottom, content.bottom);
        if (clipped.bottom <= clipped.top ||
            !PtInRect(&clipped, point))
            continue;

        const QuickNavigationEntry selectedEntry =
            std::move(entries[i]);
        quickNavigationRenameItemRect_ =
            itemRect;

        if (selectedEntry.kind ==
                QuickNavigationEntry::Kind::DesktopItem &&
            selectedEntry.itemIndex < items_.size())
        {
            SelectOnly(static_cast<int>(
                selectedEntry.itemIndex));
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (IsProtectedDesktopIcon(
                    items_[selectedEntry.itemIndex]))
                ShowShellContextMenu(
                    screenPoint,
                    static_cast<int>(
                        selectedEntry.itemIndex),
                    true);
            else
                ShowItemContextMenu(
                    screenPoint,
                    static_cast<int>(
                        selectedEntry.itemIndex),
                    false, true);
            return true;
        }

        if (selectedEntry.kind ==
                QuickNavigationEntry::Kind::FolderEntry &&
            selectedEntry.widgetIndex < widgets_.size() &&
            selectedEntry.folderEntryIndex <
                widgets_[selectedEntry.widgetIndex].
                    folderEntries.size())
        {
            ClearSelection();
            widgets_[selectedEntry.widgetIndex].
                folderEntries[
                    selectedEntry.folderEntryIndex].
                selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            ShowFolderEntryContextMenu(
                screenPoint,
                selectedEntry.widgetIndex,
                selectedEntry.folderEntryIndex,
                true);
            return true;
        }
        return true;
    }

    return false;
}

inline bool DesktopApp::CopyTextToClipboard(const std::wstring& text)
{
    if (text.empty())
        return false;

    HWND owner = quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    if (!OpenClipboard(owner))
        return false;

    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle)
    {
        CloseClipboard();
        return false;
    }

    void* data = GlobalLock(handle);
    if (!data)
    {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }

    std::memcpy(data, text.c_str(), bytes);
    GlobalUnlock(handle);

    if (!SetClipboardData(CF_UNICODETEXT, handle))
    {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

inline std::wstring DesktopApp::SanitizeShortcutFileStem(const std::wstring& name)
{
    std::wstring stem = name;
    for (auto& ch : stem)
    {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch))
            ch = L'_';
    }
    while (!stem.empty() && (stem.back() == L'.' || stem.back() == L' '))
        stem.pop_back();
    while (!stem.empty() && stem.front() == L' ')
        stem.erase(stem.begin());
    if (stem.empty())
        stem = _LW("widget.shortcut");
    if (stem.size() > 80)
        stem.resize(80);
    return stem;
}

inline bool DesktopApp::IsApplicationsShellLinkTarget(IShellLinkW* shellLink)
{
    if (!shellLink)
        return false;

    PIDLIST_ABSOLUTE rawPidl = nullptr;
    if (FAILED(shellLink->GetIDList(&rawPidl)) || !rawPidl)
        return false;

    Pidl targetPidl;
    targetPidl.reset(rawPidl);

    bool result = false;
    const std::wstring appsClsid = ToUpperInvariant(kDesktopIconClsidApplications);
    const SIGDN names[] = {
        SIGDN_DESKTOPABSOLUTEPARSING,
        SIGDN_PARENTRELATIVEPARSING,
        SIGDN_NORMALDISPLAY,
    };
    for (SIGDN nameKind : names)
    {
        PWSTR parsingName = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(targetPidl.get(), nameKind, &parsingName)) &&
            parsingName)
        {
            std::wstring normalized = ToUpperInvariant(parsingName);
            result = normalized.find(L"SHELL:APPSFOLDER") != std::wstring::npos ||
                normalized.find(L"APPSFOLDER") != std::wstring::npos ||
                normalized.find(appsClsid) != std::wstring::npos;
        }
        if (parsingName)
            CoTaskMemFree(parsingName);
        if (result)
            return true;
    }

    SHFILEINFOW info{};
    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(targetPidl.get()), 0, &info, sizeof(info),
        SHGFI_PIDL | SHGFI_TYPENAME) && info.szTypeName[0])
    {
        std::wstring typeName = ToUpperInvariant(info.szTypeName);
        result = typeName == L"APPLICATION" || typeName == L"APPLICATIONS" ||
            typeName == _LW("app.nav.app_label") || typeName == _LW("app.interact.app_title");
    }
    return result;
}

inline bool DesktopApp::CreateDesktopShortcutForShellLink(const std::wstring& displayName,
    PIDLIST_ABSOLUTE targetPidl, const std::wstring& targetPath, const std::wstring& workingDirectory)
{
    if (!targetPidl && targetPath.empty())
        return false;

    wchar_t desktopPath[MAX_PATH]{};
    if (!SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE))
        return false;

    std::wstring stem = SanitizeShortcutFileStem(displayName);
    if (stem.empty() && !targetPath.empty())
    {
        wchar_t nameBuf[MAX_PATH]{};
        wcscpy_s(nameBuf, PathFindFileNameW(targetPath.c_str()));
        PathRemoveExtensionW(nameBuf);
        stem = SanitizeShortcutFileStem(nameBuf);
    }

    std::wstring shortcutPath;
    for (int i = 1; i < 1000; ++i)
    {
        std::wstring fileName = i == 1
            ? stem + L".lnk"
            : stem + L" (" + std::to_wstring(i) + L").lnk";
        wchar_t candidate[MAX_PATH]{};
        PathCombineW(candidate, desktopPath, fileName.c_str());
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES)
        {
            shortcutPath = candidate;
            break;
        }
    }
    if (shortcutPath.empty())
    {
        wchar_t fallback[MAX_PATH]{};
        PathCombineW(fallback, desktopPath, (stem + L" (1000).lnk").c_str());
        shortcutPath = fallback;
    }

    ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))) || !shellLink)
        return false;

    HRESULT setTargetHr = targetPidl
        ? shellLink->SetIDList(targetPidl)
        : shellLink->SetPath(targetPath.c_str());
    if (FAILED(setTargetHr))
        return false;

    if (!workingDirectory.empty())
        shellLink->SetWorkingDirectory(workingDirectory.c_str());

    ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink.As(&persistFile)) ||
        FAILED(persistFile->Save(shortcutPath.c_str(), TRUE)))
        return false;

    ReloadItems();
    return true;
}

inline bool DesktopApp::CreateDesktopShortcutForApp(const QuickNavigationAppEntry& entry)
{
    if (!entry.absolutePidl.get())
        return false;
    return CreateDesktopShortcutForShellLink(entry.name, entry.absolutePidl.get(), L"", L"");
}

inline bool DesktopApp::CreateDesktopShortcutForPath(
    const std::wstring& path, bool isDirectory, const std::wstring& displayName)
{
    if (path.empty())
        return false;

    std::wstring workingDirectory;
    if (isDirectory)
    {
        workingDirectory = path;
    }
    else
    {
        wchar_t dir[MAX_PATH]{};
        wcscpy_s(dir, path.c_str());
        if (PathRemoveFileSpecW(dir))
            workingDirectory = dir;
    }

    std::wstring stem = displayName;
    if (stem.empty())
    {
        wchar_t nameBuf[MAX_PATH]{};
        wcscpy_s(nameBuf, PathFindFileNameW(path.c_str()));
        PathRemoveExtensionW(nameBuf);
        stem = nameBuf;
    }
    return CreateDesktopShortcutForShellLink(stem, nullptr, path, workingDirectory);
}

inline void DesktopApp::ShowQuickNavigationAppContextMenu(
    const QuickNavigationAppEntry& entry, POINT screenPoint)
{
    if (!entry.absolutePidl.get())
        return;

    enum : UINT
    {
        kAppOpen = 1,
        kAppCreateShortcut = 2,
        kAppReveal = 3,
    };

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, kAppOpen, _LW("app.nav.open"));
    AppendMenuW(menu,
        snowdesktop::item_location::CanReveal(entry.parsingName)
            ? MF_STRING
            : MF_STRING | MF_GRAYED,
        kAppReveal, _LW("app.menu.open_file_location"));
    AppendMenuW(menu, MF_STRING, kAppCreateShortcut, _LW("app.nav.send_to_desktop"));

    HWND owner = quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    SetForegroundWindow(owner);
    const UINT command = TrackPopupMenuEx(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
        screenPoint.x, screenPoint.y, owner, nullptr);
    DestroyMenu(menu);

    switch (command)
    {
    case kAppOpen:
    {
        CloseQuickNavigation();
        LaunchQuickNavigationAppEntry(entry);
        break;
    }
    case kAppCreateShortcut:
        CreateDesktopShortcutForApp(entry);
        break;
    case kAppReveal:
        snowdesktop::item_location::Reveal(
            hwnd_, entry.parsingName);
        break;
    default:
        break;
    }
}

inline void DesktopApp::ShowQuickNavigationEverythingContextMenu(
    const QuickNavigationEverythingEntry& entry, POINT screenPoint)
{
    if (entry.path.empty())
        return;

    enum : UINT
    {
        kEverythingOpen = 1,
        kEverythingReveal = 2,
        kEverythingCopyPath = 3,
        kEverythingCreateShortcut = 4,
    };

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, kEverythingOpen, _LW("app.nav.open"));
    AppendMenuW(menu,
        snowdesktop::item_location::CanReveal(entry.path)
            ? MF_STRING
            : MF_STRING | MF_GRAYED,
        kEverythingReveal,
        _LW("app.menu.open_file_location"));
    AppendMenuW(menu, MF_STRING, kEverythingCreateShortcut, _LW("app.nav.send_to_desktop"));
    AppendMenuW(menu, MF_STRING, kEverythingCopyPath, _LW("app.nav.copy_path"));

    HWND owner = quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    SetForegroundWindow(owner);
    const UINT command = TrackPopupMenuEx(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
        screenPoint.x, screenPoint.y, owner, nullptr);
    DestroyMenu(menu);

    switch (command)
    {
    case kEverythingOpen:
        ShellExecuteW(nullptr, L"open", entry.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kEverythingReveal:
        snowdesktop::item_location::Reveal(
            hwnd_, entry.path);
        break;
    case kEverythingCopyPath:
        CopyTextToClipboard(entry.path);
        break;
    case kEverythingCreateShortcut:
        CreateDesktopShortcutForPath(entry.path, entry.isDirectory,
            entry.name.empty() ? FileNameFromPath(entry.path) : entry.name);
        break;
    default:
        break;
    }
}

/**
 * @brief 创建/重建快捷导航 DirectWrite 文本格式（DPI 变化时调用）。
 */
inline void DesktopApp::EnsureQuickNavTextFormats()
{
    if (!dwriteFactory_)
        return;

    const float tabSize = static_cast<float>(QuickNavScale(14));
    const float itemSize = static_cast<float>(QuickNavScale(13));
    const float pathSize = static_cast<float>(QuickNavScale(11));
    // 深色用极细字重：Segoe UI Variable 支持连续字重轴，100=Thin 才能真正比 Light(300) 更细。
    const float itemWeightValue = quickNavLightTheme_ ? 550.0f : 100.0f;
    const DWRITE_FONT_WEIGHT itemWeight =
        static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(itemWeightValue));

    auto createOrRecreate = [&](ComPtr<IDWriteTextFormat>& fmt, const wchar_t* family,
        float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT hAlign) {
        const bool stale = !fmt ||
            std::abs(fmt->GetFontSize() - size) > 0.01f ||
            fmt->GetFontWeight() != weight;
        if (stale)
        {
            fmt.Reset();
            dwriteFactory_->CreateTextFormat(family, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
            if (fmt)
            {
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                fmt->SetTextAlignment(hAlign);
            }
        }
    };

    createOrRecreate(quickNavTabTextFormat_, L"Segoe UI", tabSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    createOrRecreate(quickNavItemTextFormat_, L"Segoe UI Variable", itemSize, itemWeight, DWRITE_TEXT_ALIGNMENT_LEADING);
    createOrRecreate(quickNavPathTextFormat_, L"Segoe UI", pathSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!quickNavFaTextFormat_ ||
        std::abs(
            quickNavFaTextFormat_->GetFontSize() -
                tabSize) > 0.01f)
    {
        quickNavFaTextFormat_.Reset();
        quickNavFaTextFormat_ =
            ComPtr<IDWriteTextFormat>(
                CreateFaTextFormat(
                    dwriteFactory_.Get(),
                    tabSize));
        if (quickNavFaTextFormat_)
        {
            quickNavFaTextFormat_->
                SetTextAlignment(
                    DWRITE_TEXT_ALIGNMENT_CENTER);
            quickNavFaTextFormat_->
                SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            quickNavFaTextFormat_->
                SetWordWrapping(
                    DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    ComPtr<IDWriteTextFormat3> itemFormat3;
    if (quickNavItemTextFormat_ && SUCCEEDED(quickNavItemTextFormat_.As(&itemFormat3)) && itemFormat3)
    {
        DWRITE_FONT_AXIS_VALUE axes[] = {
            { DWRITE_FONT_AXIS_TAG_WEIGHT, itemWeightValue }
        };
        itemFormat3->SetFontAxisValues(axes, ARRAYSIZE(axes));
    }
}

/**
 * @brief 重置快捷导航 DComp 表面与渲染缓存（设备丢失或窗口销毁时调用）。
 */
inline void DesktopApp::ResetQuickNavCompositionResources()
{
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    quickNavSysIconCache_.clear();
    if (quickNavDcompVisual_)
        quickNavDcompVisual_->SetContent(nullptr);
    quickNavDcompSurface_.Reset();
    quickNavCompWidth_ = 0;
    quickNavCompHeight_ = 0;
}

/**
 * @brief 快捷导航 DComp 渲染失败后重置表面并安排一次恢复重绘。
 */
inline void DesktopApp::RecoverQuickNavCompositionFailure(const wchar_t* stage, HRESULT hr)
{
    wchar_t buf[192];
    wsprintfW(buf, L"QuickNav %s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render", static_cast<unsigned>(hr));
    WriteCrashLogEntry(buf);

    ResetQuickNavCompositionResources();

    if (!quickNavCompositionRenderRecoveryPending_ && quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
    {
        quickNavCompositionRenderRecoveryPending_ = true;
        InvalidateRect(quickNavigationHwnd_, nullptr, FALSE);
    }
}

/**
 * @brief 创建或调整快捷导航 DComp 表面大小。
 * @return S_OK 成功，否则为 HRESULT 错误码
 */
inline HRESULT DesktopApp::CreateOrResizeQuickNavCompositionSurface()
{
    if (!dcompDevice_ || !quickNavigationHwnd_ || !IsWindow(quickNavigationHwnd_))
        return E_UNEXPECTED;

    const UINT width = static_cast<UINT>(
        std::max<LONG>(
            1,
            quickNavigationRect_.right -
                quickNavigationRect_.left));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(
            1,
            quickNavigationRect_.bottom -
                quickNavigationRect_.top));

    if (!quickNavDcompTarget_)
    {
        HRESULT hr = dcompDevice_->CreateTargetForHwnd(quickNavigationHwnd_, FALSE, &quickNavDcompTarget_);
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(buf, L"QuickNav CreateTargetForHwnd FAILED hr=0x%08X", static_cast<unsigned>(hr));
            WriteCrashLogEntry(buf);
            return hr;
        }
    }
    if (!quickNavDcompVisual_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(&quickNavDcompVisual_);
        if (FAILED(hr) || !quickNavDcompVisual_)
        {
            wchar_t buf[128];
            wsprintfW(buf, L"QuickNav CreateVisual FAILED hr=0x%08X", static_cast<unsigned>(hr));
            WriteCrashLogEntry(buf);
            return hr;
        }
        quickNavDcompTarget_->SetRoot(quickNavDcompVisual_.Get());
    }
    if (!quickNavDcompEffect_)
    {
        HRESULT hr =
            dcompDevice_->CreateEffectGroup(
                &quickNavDcompEffect_);
        if (FAILED(hr) || !quickNavDcompEffect_)
        {
            wchar_t buf[128];
            wsprintfW(
                buf,
                L"QuickNav CreateEffectGroup FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteCrashLogEntry(buf);
            return FAILED(hr) ? hr : E_FAIL;
        }
        hr = quickNavDcompVisual_->SetEffect(
            quickNavDcompEffect_.Get());
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(
                buf,
                L"QuickNav SetEffect FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteCrashLogEntry(buf);
            return hr;
        }
    }

    if (quickNavDcompSurface_ && quickNavCompWidth_ == width && quickNavCompHeight_ == height)
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = dcompDevice_->CreateSurface(width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    if (FAILED(hr))
    {
        wchar_t buf[128];
        wsprintfW(buf, L"QuickNav CreateSurface %ux%u FAILED hr=0x%08X", width, height, static_cast<unsigned>(hr));
        WriteCrashLogEntry(buf);
        return hr;
    }
    hr = quickNavDcompVisual_->SetContent(surface.Get());
    if (FAILED(hr))
    {
        wchar_t buf[128];
        wsprintfW(buf, L"QuickNav SetContent FAILED hr=0x%08X", static_cast<unsigned>(hr));
        WriteCrashLogEntry(buf);
        return hr;
    }
    hr = dcompDevice_->Commit();
    if (FAILED(hr))
    {
        wchar_t buf[128];
        wsprintfW(buf, L"QuickNav CreateSurface Commit FAILED hr=0x%08X", static_cast<unsigned>(hr));
        WriteCrashLogEntry(buf);
        return hr;
    }

    quickNavDcompSurface_ = surface;
    quickNavCompWidth_ = width;
    quickNavCompHeight_ = height;
    return S_OK;
}

/**
 * @brief 绘制快捷导航窗口（含搜索栏、标签页、列表、滚动条）
 * @param hwnd 窗口句柄
 */
inline void DesktopApp::PaintQuickNavigationWindow(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc)
        return;
    // hdc 仅用于验证绘制区域；实际绘制走 DComp surface。
    (void)hdc;

    const QuickNavTheme& t = quickNavLightTheme_ ? kQuickNavLight : kQuickNavDark;

    HRESULT hr = CreateOrResizeQuickNavCompositionSurface();
    if (FAILED(hr))
    {
        RecoverQuickNavCompositionFailure(L"CreateOrResizeQuickNavCompositionSurface", hr);
        EndPaint(hwnd, &ps);
        return;
    }
    ApplyQuickNavigationAnimationFrame();

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    hr = quickNavDcompSurface_->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext), &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        RecoverQuickNavCompositionFailure(L"BeginDraw", hr);
        EndPaint(hwnd, &ps);
        return;
    }

    ComPtr<ID2D1DeviceContext> ctx;
    ctx.Attach(rawContext);
    ctx->SetDpi(96.0f, 96.0f);
    ctx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    // 内容 surface 只按目标面板大小分配；顶层宿主可以覆盖完整动画路径。
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(
            updateOffset.x -
            quickNavigationRect_.left),
        static_cast<float>(
            updateOffset.y -
            quickNavigationRect_.top)));
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // 文本抗锯齿沿用 DComp 默认（与桌面一致），避免在 alpha 表面上强制 ClearType 产生彩色毛边。
    ctx->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    brushCache_.clear();
    brushCacheContext_ = ctx.Get();

    const RECT& overlay = quickNavigationRect_;
    const float windowCornerRadius =
        static_cast<float>(
            QuickNavScale(16)) / 2.0f;
    ComPtr<ID2D1RoundedRectangleGeometry>
        windowClipGeometry;
    bool windowClipPushed = false;
    if (d2dFactory_ &&
        SUCCEEDED(
            d2dFactory_->
                CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(
                        ToD2DRect(overlay),
                        windowCornerRadius,
                        windowCornerRadius),
                    &windowClipGeometry)) &&
        windowClipGeometry)
    {
        ctx->PushLayer(
            D2D1::LayerParameters(
                ToD2DRect(overlay),
                windowClipGeometry.Get()),
            nullptr);
        windowClipPushed = true;
    }
    const float windowAlpha = std::clamp(quickNavAppearance_.widgetAlpha, 0.0f, 1.0f);
    const float borderAlpha =
        quickNavigationAnimation_.IsAnimating()
            ? 0.0f
            : std::clamp(
                quickNavAppearance_.
                    widgetBorderAlpha,
                0.0f, 1.0f);
    DrawD2DRoundedRectangle(
        ctx.Get(), overlay,
        windowCornerRadius,
        D2D1::ColorF(
            quickNavAppearance_.widgetBgR,
            quickNavAppearance_.widgetBgG,
            quickNavAppearance_.widgetBgB,
            windowAlpha),
        D2D1::ColorF(0, 0, 0, 0));
    if (quickNavAppearance_.glassEnabled &&
        quickNavAppearance_.acrylicEnabled)
    {
        POINT screenOrigin{};
        ClientToScreen(quickNavigationHwnd_, &screenOrigin);
        DrawAcrylicNoise(ctx.Get(), overlay,
            static_cast<float>(QuickNavScale(16)) / 2.0f,
            quickNavAppearance_.contentTheme == 1, screenOrigin);
    }
    DrawD2DRoundedRectangle(ctx.Get(),
        MakeRect(overlay.left, overlay.top, overlay.right - 1, overlay.bottom - 1),
        windowCornerRadius,
        D2D1::ColorF(0, 0, 0, 0),
        D2D1::ColorF(quickNavAppearance_.widgetBorderR,
            quickNavAppearance_.widgetBorderG,
            quickNavAppearance_.widgetBorderB, borderAlpha));

    const bool searching = !GetQuickNavigationEffectiveSearchText().empty();
    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    QuickNavigationContentModel contentModel =
        BuildQuickNavigationContentModel();
    const std::vector<QuickNavigationEntry>& entries =
        contentModel.entries;
    quickNavigationTabScrollOffset_ = std::clamp(quickNavigationTabScrollOffset_, 0,
        GetQuickNavigationMaxTabScrollOffset(overlay));
    quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
        GetQuickNavigationMaxScrollOffset(overlay));

    const RECT searchRect = GetQuickNavigationSearchRect(overlay);
    DrawD2DRoundedRectangle(ctx.Get(),
        searchRect,
        static_cast<float>(QuickNavScale(12)) / 2.0f,
        ToD2DColor(t.searchBg), ToD2DColor(t.searchBorder));

    if (!searching)
    {
        RECT tabs = GetQuickNavigationTabsRect(overlay);
        const int tabsStart =
            GetQuickNavigationTabsStart(overlay);
        const int tabClipRight = tabs.right;
        // 左侧外扩一圈避免固定标签圆角 AA 被截断；右侧与搜索框右边界对齐。
        const int clipPad = QuickNavScale(8);
        ctx->PushAxisAlignedClip(
            ToD2DRect(MakeRect(
                tabsStart - clipPad,
                tabs.top, tabClipRight,
                tabs.bottom)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const size_t tabCount = collectionIndices.size() + 2;
        UpdateQuickNavTabWidths();
        const auto& tabWidths = quickNavTabWidths_;
        const int gap = QuickNavScale(8);
        const int sepGap = QuickNavScale(6);
        const int fixedWidth = (tabWidths.size() >= 2 ? tabWidths[0] + gap + tabWidths[1] : 0);
        const int scrollPad = sepGap + QuickNavScale(1) + gap; // separator + gap after
        auto calcTabPosX = [&](size_t tabIdx) -> int {
            if (tabIdx == 0) return tabsStart;
            if (tabIdx == 1) return tabsStart + tabWidths[0] + gap;
            int x = tabsStart + fixedWidth + scrollPad;
            for (size_t i = 2; i < tabIdx && i < tabWidths.size(); ++i)
                x += tabWidths[i] + gap;
            return x - quickNavigationTabScrollOffset_;
        };

        int dragTargetTab = -1;
        if (quickNavTabDragging_ && quickNavTabDragIndex_ != static_cast<size_t>(-1))
            dragTargetTab = GetQuickNavTabDragTarget(
                quickNavTabDragIndex_, quickNavTabDragDeltaX_);

        const int tabInsetY = QuickNavScale(3);
        auto drawTab = [&](size_t tab, int offsetX) {
            if (tab >= tabWidths.size()) return;
            int posX = calcTabPosX(tab) + offsetX;
            int tw = tabWidths[tab];
            // 按钮上下内缩，避免 AA 圆角被 tabs 裁剪边界截断，同时降低视觉高度。
            RECT tabRect = MakeRect(posX, tabs.top + tabInsetY, posX + tw, tabs.bottom - tabInsetY);
            if (tab >= 2)
            {
                int scrollStart =
                    tabsStart + fixedWidth +
                    scrollPad;
                if (tabRect.right <= scrollStart || tabRect.left >= tabClipRight) return;
            }
            else if (tab <= 1)
            {
                if (tabRect.right <= tabsStart ||
                    tabRect.left >=
                        tabsStart + fixedWidth +
                            sepGap)
                    return;
                tabRect.right = std::min<LONG>(
                    tabRect.right,
                    static_cast<LONG>(
                        tabsStart + fixedWidth +
                        sepGap));
            }
            const bool active = (tab == 0 && quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-1))
                || (tab == 1 && quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-2))
                || (tab > 1 && quickNavigationActiveWidgetIndex_ == collectionIndices[tab - 2]);
            bool hovered = false;
            if (!quickNavTabDragging_)
                hovered = PtInRect(&tabRect, lastMousePoint_) != FALSE;

            D2D1_COLOR_F fill, stroke;
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
            {
                fill = ToD2DColor(t.tabDragFill, 0.78f);
                stroke = ToD2DColor(t.tabDragStroke, 0.82f);
            }
            else
            {
                fill = active ? ToD2DColor(t.tabActiveFill, 0.82f)
                    : (hovered ? ToD2DColor(t.tabHoverFill, 0.72f)
                               : ToD2DColor(t.tabDefaultFill, 0.62f));
                stroke = active ? ToD2DColor(t.tabActiveStroke, 0.88f)
                                : ToD2DColor(t.tabDefaultStroke, 0.72f);
            }
            DrawD2DRoundedRectangle(ctx.Get(), tabRect,
                static_cast<float>(QuickNavScale(14)) / 2.0f, fill, stroke);

            std::wstring label = GetQuickNavTabLabel(tab);
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            DrawD2DTextEllipsis(ctx.Get(), label, textRect, quickNavTabTextFormat_.Get(),
                active ? ToD2DColor(RGB(245, 248, 252)) : ToD2DColor(t.tabText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        };

        // 分隔线：固定在 "映射" 标签右侧
        int sepX =
            tabsStart + fixedWidth + sepGap;
        RECT sepRect = MakeRect(sepX, tabs.top + QuickNavScale(8),
            sepX + QuickNavScale(1), tabs.bottom - QuickNavScale(8));
        DrawD2DSeparator(ctx.Get(), sepRect, ToD2DColor(t.tabSeparator));

        // Draw fixed tabs (0, 1) and dragged tab displacement
        for (size_t tab = 0; tab < tabCount && tab < 2; ++tab)
        {
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
                continue;
            int offsetX = 0;
            if (quickNavTabDragging_ && dragTargetTab >= 1)
            {
                size_t src = quickNavTabDragIndex_;
                int dst = dragTargetTab;
                int cur = static_cast<int>(tab);
                int shift = (quickNavTabDragIndex_ < tabWidths.size()
                    ? tabWidths[quickNavTabDragIndex_] + gap : tabWidths[0] + gap);
                if (cur > src && cur <= dst) offsetX = -shift;
                else if (cur < src && cur >= dst) offsetX = shift;
            }
            drawTab(tab, offsetX);
        }

        // Clip to scrollable area for remaining tabs
        int scrollLeft =
            tabsStart + fixedWidth + scrollPad;
        ctx->PopAxisAlignedClip();
        ctx->PushAxisAlignedClip(
            ToD2DRect(MakeRect(scrollLeft - clipPad, tabs.top, tabClipRight, tabs.bottom)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        for (size_t tab = 2; tab < tabCount; ++tab)
        {
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
                continue;
            int offsetX = 0;
            if (quickNavTabDragging_ && dragTargetTab >= 1)
            {
                size_t src = quickNavTabDragIndex_;
                int dst = dragTargetTab;
                int cur = static_cast<int>(tab);
                int shift = (quickNavTabDragIndex_ < tabWidths.size()
                    ? tabWidths[quickNavTabDragIndex_] + gap : tabWidths[0] + gap);
                if (cur > src && cur <= dst) offsetX = -shift;
                else if (cur < src && cur >= dst) offsetX = shift;
            }
            drawTab(tab, offsetX);
        }

        if (quickNavTabDragging_ && quickNavTabDragIndex_ != static_cast<size_t>(-1) &&
            quickNavTabDragIndex_ < tabWidths.size())
        {
            int dragTw = tabWidths[quickNavTabDragIndex_];
            int posX = calcTabPosX(quickNavTabDragIndex_) + quickNavTabDragDeltaX_;
            RECT tabRect = MakeRect(posX, tabs.top + tabInsetY, posX + dragTw, tabs.bottom - tabInsetY);
            tabRect.left = std::max(
                tabRect.left,
                static_cast<LONG>(
                    tabsStart));
            tabRect.right = std::min<LONG>(tabRect.right, static_cast<LONG>(tabClipRight));
            DrawD2DRoundedRectangle(ctx.Get(), tabRect,
                static_cast<float>(QuickNavScale(14)) / 2.0f,
                ToD2DColor(t.tabDragFloatFill, 0.82f),
                ToD2DColor(t.tabDragFloatStroke, 0.88f));

            std::wstring label = GetQuickNavTabLabel(quickNavTabDragIndex_);
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            DrawD2DTextEllipsis(ctx.Get(), label, textRect, quickNavTabTextFormat_.Get(),
                ToD2DColor(t.tabDragFloatText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            if (dragTargetTab >= 2 && static_cast<size_t>(dragTargetTab) <= collectionIndices.size() + 1)
            {
                int insertX = calcTabPosX(static_cast<size_t>(dragTargetTab));
                RECT ind = MakeRect(insertX - gap / 2, tabs.top + QuickNavScale(4),
                    insertX - gap / 2 + QuickNavScale(2), tabs.bottom - QuickNavScale(4));
                DrawD2DSeparator(ctx.Get(), ind, ToD2DColor(t.tabDragIndicator));
            }
        }

        ctx->PopAxisAlignedClip();

        const RECT modeButton =
            GetQuickNavigationViewModeButtonRect(
                overlay);
        if (!IsRectEmpty(&modeButton))
        {
            const bool hovered =
                PtInRect(
                    &modeButton,
                    lastMousePoint_) != FALSE;
            const D2D1_COLOR_F fill =
                hovered
                    ? ToD2DColor(
                        t.tabHoverFill, 0.82f)
                    : ToD2DColor(
                        t.tabActiveFill, 0.72f);
            const D2D1_COLOR_F stroke =
                hovered
                ? ToD2DColor(
                    t.tabActiveStroke, 0.92f)
                : ToD2DColor(
                    t.tabDefaultStroke, 0.76f);
            DrawD2DRoundedRectangle(
                ctx.Get(), modeButton,
                static_cast<float>(
                    QuickNavScale(14)) / 2.0f,
                fill, stroke);
            const std::wstring_view glyph =
                snowdesktop::
                    quick_navigation_rules::
                        QuickNavigationDesktopViewModeGlyph(
                            navigationSettings_.
                                desktopViewMode);
            DrawD2DText(
                ctx.Get(),
                std::wstring(glyph),
                modeButton,
                quickNavFaTextFormat_
                ? quickNavFaTextFormat_.Get()
                : (faTextFormat_
                    ? faTextFormat_.Get()
                    : quickNavTabTextFormat_.Get()),
                ToD2DColor(t.tabText));
        }
    }

    RECT contentApp = GetQuickNavigationContentRect(overlay);
    ctx->PushAxisAlignedClip(ToD2DRect(contentApp), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (quickNavigationInitialJumpOpen_)
    {
        RECT titleRect = MakeRect(
            contentApp.left + QuickNavScale(8),
            contentApp.top,
            contentApp.right - QuickNavScale(96),
            contentApp.top + QuickNavScale(30));
        DrawD2DTextEllipsis(
            ctx.Get(),
            _LW("app.nav.initial_jump_title"),
            titleRect,
            quickNavTabTextFormat_.Get(),
            ToD2DColor(t.headerText),
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const RECT backRect =
            GetQuickNavigationInitialJumpBackRect(
                overlay);
        const bool backHovered =
            PtInRect(&backRect,
                lastMousePoint_) != FALSE;
        if (backHovered)
            DrawD2DRoundedRectangle(
                ctx.Get(), backRect,
                static_cast<float>(
                    QuickNavScale(10)) / 2.0f,
                ToD2DColor(
                    t.tabHoverFill, 0.72f),
                ToD2DColor(
                    t.tabDefaultStroke, 0.72f));
        DrawD2DTextEllipsis(
            ctx.Get(),
            _LW("app.nav.initial_jump_back"),
            backRect,
            quickNavTabTextFormat_.Get(),
            ToD2DColor(t.tabText),
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        std::array<bool, 27> available{};
        for (const auto& section :
            contentModel.sections)
        {
            if (section.label.size() != 1)
                continue;
            available[
                snowdesktop::
                    quick_navigation_rules::
                        InitialJumpBucketIndex(
                            section.label.front())] =
                                true;
        }
        for (size_t bucketIndex = 0;
            bucketIndex < available.size();
            ++bucketIndex)
        {
            const RECT cell =
                GetQuickNavigationInitialJumpCellRect(
                    overlay, bucketIndex);
            const bool hovered =
                PtInRect(
                    &cell,
                    lastMousePoint_) != FALSE;
            const bool selected =
                bucketIndex ==
                    quickNavigationInitialJumpSelection_;
            if (available[bucketIndex] &&
                (hovered || selected))
                DrawD2DRoundedRectangle(
                    ctx.Get(), cell,
                    static_cast<float>(
                        QuickNavScale(10)) / 2.0f,
                    ToD2DColor(
                        selected
                        ? t.tabActiveFill
                        : t.tabHoverFill,
                        0.78f),
                    ToD2DColor(
                        selected
                        ? t.tabActiveStroke
                        : t.tabDefaultStroke,
                        0.76f));
            const std::wstring label(
                1,
                snowdesktop::
                    quick_navigation_rules::
                        InitialJumpBucketAt(
                            bucketIndex));
            DrawD2DTextEllipsis(
                ctx.Get(), label, cell,
                quickNavTabTextFormat_.Get(),
                ToD2DColor(
                    available[bucketIndex]
                    ? t.tabText
                    : t.emptyText),
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    else if (entries.empty() && quickNavigationAppResultIndices_.empty() &&
        (!searching || quickNavigationEverythingResults_.empty()))
    {
        RECT emptyRect = contentApp;
        emptyRect.top += QuickNavScale(28);
        if (searching && !everythingSearchAvailable_)
            DrawD2DTextEllipsis(ctx.Get(), GetQuickNavigationEverythingNoticeText(),
                emptyRect, quickNavItemTextFormat_.Get(), ToD2DColor(t.emptyText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
        else
            DrawD2DTextEllipsis(ctx.Get(),
                !searching
                ? (collectionIndices.empty() ? _LW("app.nav.empty_collection") : _LW("app.nav.empty_category"))
                : _LW("app.nav.no_results"),
                emptyRect, quickNavItemTextFormat_.Get(), ToD2DColor(t.emptyText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    }
    else
    {
        if (searching)
        {
            const int headerH = QuickNavScale(28);
            RECT desktopHeader = MakeRect(contentApp.left + QuickNavScale(8),
                contentApp.top - quickNavigationScrollOffset_,
                contentApp.right - QuickNavScale(12),
                contentApp.top + headerH - quickNavigationScrollOffset_);
            std::wstring desktopLabel = _LFW("app.nav.desktop_results",
                std::to_wstring(entries.size()));
            DrawD2DTextEllipsis(ctx.Get(), desktopLabel, desktopHeader,
                quickNavTabTextFormat_.Get(), ToD2DColor(t.headerText),
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            RECT desktopSep = MakeRect(desktopHeader.left,
                desktopHeader.bottom - QuickNavScale(1),
                desktopHeader.right, desktopHeader.bottom);
            DrawD2DSeparator(ctx.Get(), desktopSep, ToD2DColor(t.headerSeparator));
        }
        else if (contentModel.IsSectioned())
        {
            for (size_t sectionIndex = 0;
                sectionIndex <
                    contentModel.sections.size();
                ++sectionIndex)
            {
                const auto& section =
                    contentModel.sections[
                        sectionIndex];
                const RECT header =
                    GetQuickNavigationSectionHeaderRect(
                        overlay, sectionIndex,
                        contentModel);
                if (header.bottom <= contentApp.top ||
                    header.top >= contentApp.bottom)
                    continue;
                const bool initialJumpHeader =
                    quickNavigationActiveWidgetIndex_ ==
                        static_cast<size_t>(-1) &&
                    navigationSettings_.desktopViewMode ==
                        QuickNavigationDesktopViewMode::
                            Initial;
                if (initialJumpHeader &&
                    PtInRect(
                        &header,
                        lastMousePoint_) != FALSE)
                    DrawD2DRoundedRectangle(
                        ctx.Get(), header,
                        static_cast<float>(
                            QuickNavScale(8)) / 2.0f,
                        ToD2DColor(
                            t.tabHoverFill, 0.58f),
                        ToD2DColor(
                            t.tabDefaultStroke,
                            0.62f));
                const std::wstring label = _LFW(
                    "app.nav.section_header",
                    section.label,
                    std::to_wstring(
                        section.entryCount));
                DrawD2DTextEllipsis(
                    ctx.Get(), label, header,
                    quickNavTabTextFormat_.Get(),
                    ToD2DColor(t.headerText),
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                const RECT separator = MakeRect(
                    header.left,
                    header.bottom -
                        QuickNavScale(1),
                    header.right,
                    header.bottom);
                DrawD2DSeparator(
                    ctx.Get(), separator,
                    ToD2DColor(
                        t.headerSeparator));
            }
        }

        // 桌面项直接走 D2D（ctx 为 ID2D1DeviceContext，与桌面共享 d2dDevice_ 缓存）
        for (size_t i = 0; i < entries.size(); ++i)
        {
            RECT itemRectApp = GetQuickNavigationItemRect(overlay, i);
            if (itemRectApp.bottom <= contentApp.top ||
                itemRectApp.top >= contentApp.bottom)
                continue;

            const QuickNavigationEntry& entry = entries[i];
            const int state = (PtInRect(&itemRectApp, lastMousePoint_) != FALSE ||
                IsQuickNavigationKeyboardTarget(
                    QuickNavigationKeyboardTargetKind::Item, i)) ? 1 : 0;
            // 图标本体复用桌面绘制，标题由快捷导航自绘，避免桌面标题布局和字重互相影响。
            if (entry.kind == QuickNavigationEntry::Kind::DesktopItem &&
                entry.itemIndex < items_.size())
            {
                DesktopIcon icon(&items_[entry.itemIndex], nullptr, this);
                icon.Draw(ctx.Get(), itemRectApp, state, quickNavLightTheme_, false, true);
                DrawQuickNavItemText(ctx.Get(), itemRectApp, items_[entry.itemIndex].name,
                    false, quickNavLightTheme_);
            }
            else if (entry.kind == QuickNavigationEntry::Kind::FolderEntry &&
                entry.widgetIndex < widgets_.size() &&
                entry.folderEntryIndex < widgets_[entry.widgetIndex].folderEntries.size())
            {
                FolderEntry& folderEntry =
                    widgets_[entry.widgetIndex].folderEntries[entry.folderEntryIndex];
                FolderEntryIcon icon(&folderEntry, nullptr, this);
                icon.Draw(ctx.Get(), itemRectApp, state, quickNavLightTheme_, false, true);
                DrawQuickNavItemText(ctx.Get(), itemRectApp, folderEntry.name,
                    false, quickNavLightTheme_);
            }
        }

        if (searching)
        {
            const int columns = GetQuickNavigationColumnCount(overlay);
            const int desktopRows = entries.empty() ? 0 :
                (static_cast<int>(entries.size()) + columns - 1) / columns;
            const int headerH = QuickNavScale(28);
            const int gap = QuickNavScale(8);
            const int rowH = QuickNavScale(46);
            const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
                QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
            const int appHeaderTop = contentApp.top + headerH + gap
                + desktopGridH
                + gap - quickNavigationScrollOffset_;
            int everythingHeaderTop = appHeaderTop;

            if (!quickNavigationAppResultIndices_.empty())
            {
                const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
                RECT appHeader = MakeRect(contentApp.left + QuickNavScale(8),
                    appHeaderTop,
                    contentApp.right - QuickNavScale(12),
                    appHeaderTop + headerH);
                std::wstring appLabel = _LFW("app.nav.app_results",
                    std::to_wstring(quickNavigationAppResultIndices_.size()));
                DrawD2DTextEllipsis(ctx.Get(), appLabel, appHeader,
                    quickNavTabTextFormat_.Get(), ToD2DColor(t.headerText),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                RECT appSep = MakeRect(appHeader.left, appHeader.bottom - QuickNavScale(1),
                    appHeader.right, appHeader.bottom);
                DrawD2DSeparator(ctx.Get(), appSep, ToD2DColor(t.headerSeparator));

                for (size_t i = 0; i < visibleAppCount; ++i)
                {
                    size_t appIndex = quickNavigationAppResultIndices_[i];
                    if (appIndex >= quickNavigationAppEntries_.size())
                        continue;

                    const int rowTop = appHeaderTop + headerH + gap + static_cast<int>(i) * rowH;
                    RECT rowRectApp = MakeRect(contentApp.left + QuickNavScale(8), rowTop,
                        contentApp.right - QuickNavScale(12), rowTop + rowH);
                    if (rowRectApp.bottom <= contentApp.top || rowRectApp.top >= contentApp.bottom)
                        continue;

                    if (PtInRect(&rowRectApp, lastMousePoint_) != FALSE ||
                        IsQuickNavigationKeyboardTarget(
                            QuickNavigationKeyboardTargetKind::App, i))
                        DrawD2DRoundedRectangle(ctx.Get(), rowRectApp,
                            static_cast<float>(QuickNavScale(10)) / 2.0f,
                            ToD2DColor(t.appRowHoverFill), ToD2DColor(t.appRowHoverStroke));

                    const QuickNavigationAppEntry& entry = quickNavigationAppEntries_[appIndex];
                    const int iconSz = QuickNavScale(28);
                    RECT iconRect = MakeRect(rowRectApp.left + QuickNavScale(12),
                        rowRectApp.top + (rowH - iconSz) / 2,
                        rowRectApp.left + QuickNavScale(12) + iconSz,
                        rowRectApp.top + (rowH + iconSz) / 2);
                    DrawQuickNavSysIcon(ctx.Get(), entry.systemIconIndex, iconRect);

                    const int textLeft = iconRect.right + QuickNavScale(10);
                    RECT nameRect = rowRectApp;
                    nameRect.left = textLeft;
                    nameRect.right -= QuickNavScale(12);
                    nameRect.top += QuickNavScale(5);
                    nameRect.bottom = nameRect.top + QuickNavScale(18);

                    RECT typeRect = rowRectApp;
                    typeRect.left = textLeft;
                    typeRect.right -= QuickNavScale(12);
                    typeRect.top += QuickNavScale(24);
                    typeRect.bottom -= QuickNavScale(5);

                    DrawD2DTextEllipsis(ctx.Get(), entry.name, nameRect,
                        quickNavItemTextFormat_.Get(), ToD2DColor(t.appNameText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    DrawD2DTextEllipsis(ctx.Get(), _LW("app.nav.app_label"), typeRect,
                        quickNavPathTextFormat_.Get(), ToD2DColor(t.appTypeText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }

                int appRowsHeight = static_cast<int>(visibleAppCount) * rowH;
                if (HasQuickNavigationAppExpandButton())
                {
                    const int buttonTop = appHeaderTop + headerH + gap + appRowsHeight;
                    RECT buttonRectApp = MakeRect(contentApp.left + QuickNavScale(8), buttonTop,
                        contentApp.right - QuickNavScale(12), buttonTop + rowH);
                    if (buttonRectApp.bottom > contentApp.top && buttonRectApp.top < contentApp.bottom)
                    {
                        const bool hovered = PtInRect(&buttonRectApp, lastMousePoint_) != FALSE ||
                            IsQuickNavigationKeyboardTarget(
                                QuickNavigationKeyboardTargetKind::ExpandApps, 0);
                        std::wstring expandLabel = _LFW("app.interact.expand_apps_fmt",
                            std::to_wstring(quickNavigationAppResultIndices_.size()));
                        DrawD2DTextEllipsis(ctx.Get(), expandLabel, buttonRectApp,
                            quickNavTabTextFormat_.Get(),
                            hovered ? ToD2DColor(t.expandHoverText) : ToD2DColor(t.expandDefaultText),
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                    appRowsHeight += rowH;
                }

                everythingHeaderTop = appHeaderTop + headerH + gap
                    + appRowsHeight
                    + gap;
            }

            if (!everythingSearchAvailable_)
            {
                RECT noticeHeader = MakeRect(contentApp.left + QuickNavScale(8),
                    everythingHeaderTop,
                    contentApp.right - QuickNavScale(12),
                    everythingHeaderTop + headerH);
                DrawD2DTextEllipsis(ctx.Get(), GetQuickNavigationEverythingNoticeText(), noticeHeader,
                    quickNavTabTextFormat_.Get(), ToD2DColor(t.emptyHeaderText),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            else
            {
                RECT everythingHeader = MakeRect(contentApp.left + QuickNavScale(8),
                    everythingHeaderTop,
                    contentApp.right - QuickNavScale(12),
                    everythingHeaderTop + headerH);
                std::wstring everythingLabel = L"Everything  " +
                    std::to_wstring(quickNavigationEverythingResults_.size()) +
                    (quickNavigationEverythingHasMore_
                        ? _LW("app.interact.plus_items")
                        : _LW("app.interact.items_suffix"));
                DrawD2DTextEllipsis(ctx.Get(), everythingLabel, everythingHeader,
                    quickNavTabTextFormat_.Get(), ToD2DColor(t.headerText),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                RECT evSep = MakeRect(everythingHeader.left,
                    everythingHeader.bottom - QuickNavScale(1),
                    everythingHeader.right, everythingHeader.bottom);
                DrawD2DSeparator(ctx.Get(), evSep, ToD2DColor(t.headerSeparator));

                for (size_t i = 0; i < quickNavigationEverythingResults_.size(); ++i)
                {
                    const int rowTop = everythingHeaderTop + headerH + gap
                        + static_cast<int>(i) * rowH;
                    RECT rowRectApp = MakeRect(contentApp.left + QuickNavScale(8), rowTop,
                        contentApp.right - QuickNavScale(12), rowTop + rowH);
                    if (rowRectApp.bottom <= contentApp.top || rowRectApp.top >= contentApp.bottom)
                        continue;

                    if (PtInRect(&rowRectApp, lastMousePoint_) != FALSE ||
                        IsQuickNavigationKeyboardTarget(
                            QuickNavigationKeyboardTargetKind::Everything, i))
                        DrawD2DRoundedRectangle(ctx.Get(), rowRectApp,
                            static_cast<float>(QuickNavScale(10)) / 2.0f,
                            ToD2DColor(t.appRowHoverFill), ToD2DColor(t.appRowHoverStroke));

                    const QuickNavigationEverythingEntry& entry = quickNavigationEverythingResults_[i];
                    const int iconSz = QuickNavScale(28);
                    RECT iconRect = MakeRect(rowRectApp.left + QuickNavScale(12),
                        rowRectApp.top + (rowH - iconSz) / 2,
                        rowRectApp.left + QuickNavScale(12) + iconSz,
                        rowRectApp.top + (rowH + iconSz) / 2);
                    DrawQuickNavSysIcon(ctx.Get(), entry.systemIconIndex, iconRect);

                    const int textLeft = iconRect.right + QuickNavScale(10);
                    RECT nameRect = rowRectApp;
                    nameRect.left = textLeft;
                    nameRect.right -= QuickNavScale(12);
                    nameRect.top += QuickNavScale(5);
                    nameRect.bottom = nameRect.top + QuickNavScale(18);

                    RECT pathRect = rowRectApp;
                    pathRect.left = textLeft;
                    pathRect.right -= QuickNavScale(12);
                    pathRect.top += QuickNavScale(24);
                    pathRect.bottom -= QuickNavScale(5);

                    DrawD2DTextEllipsis(ctx.Get(),
                        entry.name.empty() ? FileNameFromPath(entry.path) : entry.name,
                        nameRect, quickNavItemTextFormat_.Get(), ToD2DColor(t.appNameText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                    const std::wstring modifiedText = entry.modifiedText.empty()
                        ? QuickNavigationFormatModifiedTime(entry.dateModified)
                        : entry.modifiedText;
                    if (!modifiedText.empty())
                    {
                        const int textGap = QuickNavScale(8);
                        const int available = std::max<LONG>(1, pathRect.right - pathRect.left);
                        const int maxDateWidth = std::min(QuickNavScale(156), available);
                        const int minDateWidth = std::min(QuickNavScale(118), maxDateWidth);
                        const int dateWidth = std::clamp(
                            available / 3,
                            minDateWidth,
                            maxDateWidth);
                        RECT modifiedRect = pathRect;
                        modifiedRect.left = std::max<LONG>(modifiedRect.left,
                            modifiedRect.right - dateWidth);
                        pathRect.right = std::max<LONG>(pathRect.left,
                            modifiedRect.left - textGap);

                        DrawD2DTextEllipsis(ctx.Get(), modifiedText, modifiedRect,
                            quickNavPathTextFormat_.Get(), ToD2DColor(t.appTypeText),
                            DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                    DrawD2DTextEllipsis(ctx.Get(), entry.path, pathRect,
                        quickNavPathTextFormat_.Get(), ToD2DColor(t.appTypeText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }

                if (HasQuickNavigationEverythingLoadMoreButton())
                {
                    const int buttonTop = everythingHeaderTop + headerH + gap
                        + static_cast<int>(quickNavigationEverythingResults_.size()) * rowH;
                    RECT buttonRectApp = MakeRect(contentApp.left + QuickNavScale(8), buttonTop,
                        contentApp.right - QuickNavScale(12), buttonTop + rowH);
                    if (buttonRectApp.bottom > contentApp.top && buttonRectApp.top < contentApp.bottom)
                    {
                        const bool hovered = PtInRect(&buttonRectApp, lastMousePoint_) != FALSE ||
                            IsQuickNavigationKeyboardTarget(
                                QuickNavigationKeyboardTargetKind::LoadMoreEverything, 0);
                        DrawD2DTextEllipsis(ctx.Get(), _LW("app.nav.load_more_everything"), buttonRectApp,
                            quickNavTabTextFormat_.Get(),
                            hovered ? ToD2DColor(t.expandHoverText) : ToD2DColor(t.expandDefaultText),
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                }
            }
        }
    }
    ctx->PopAxisAlignedClip();

    RECT track{}, thumb{};
    int maxScroll = 0, contentHeight = 0;
    if (!quickNavigationInitialJumpOpen_ &&
        GetQuickNavigationScrollbarGeometry(overlay,
        track, thumb, maxScroll, contentHeight))
    {
        const int trackW = QuickNavScale(5);
        DrawD2DRoundedRectangle(ctx.Get(), track, static_cast<float>(trackW) / 2.0f,
            ToD2DColor(t.scrollTrack), ToD2DColor(t.scrollTrack));
        const COLORREF thumbColor = (quickNavScrollbarDragging_ || quickNavScrollbarHovered_)
            ? t.scrollThumbHover : t.scrollThumbDefault;
        DrawD2DRoundedRectangle(ctx.Get(), thumb, static_cast<float>(trackW) / 2.0f,
            ToD2DColor(thumbColor), ToD2DColor(thumbColor));
    }

    const RECT modeButton =
        GetQuickNavigationViewModeButtonRect(
            overlay);
    if (!IsRectEmpty(&modeButton) &&
        PtInRect(
            &modeButton,
            lastMousePoint_) != FALSE)
    {
        auto modeLabel = [](
            QuickNavigationDesktopViewMode mode) {
            return mode ==
                QuickNavigationDesktopViewMode::Source
                ? std::wstring(
                    _LW("app.nav.view_source"))
                : (mode ==
                    QuickNavigationDesktopViewMode::Initial
                    ? std::wstring(
                        _LW("app.nav.view_initial"))
                    : std::wstring(
                        _LW("app.nav.view_tile")));
        };
        const auto nextMode =
            snowdesktop::quick_navigation_rules::
                NextQuickNavigationDesktopViewMode(
                    navigationSettings_.
                        desktopViewMode);
        const std::wstring tooltip = _LFW(
            "app.nav.view_mode_tooltip",
            modeLabel(
                navigationSettings_.
                    desktopViewMode),
            modeLabel(nextMode));
        const int tooltipWidth =
            QuickNavScale(240);
        const int tooltipHeight =
            QuickNavScale(28);
        const int tooltipLeft =
            std::clamp(
                static_cast<int>(
                    modeButton.left),
                static_cast<int>(
                    overlay.left +
                    QuickNavScale(8)),
                std::max(
                    static_cast<int>(
                        overlay.left +
                        QuickNavScale(8)),
                    static_cast<int>(
                        overlay.right -
                        QuickNavScale(8) -
                        tooltipWidth)));
        const RECT tooltipRect = MakeRect(
            tooltipLeft,
            modeButton.bottom +
                QuickNavScale(4),
            tooltipLeft + tooltipWidth,
            modeButton.bottom +
                QuickNavScale(4) +
                tooltipHeight);
        DrawD2DRoundedRectangle(
            ctx.Get(), tooltipRect,
            static_cast<float>(
                QuickNavScale(10)) / 2.0f,
            t.popupBg,
            t.popupBorder);
        RECT tooltipTextRect = tooltipRect;
        tooltipTextRect.left +=
            QuickNavScale(8);
        tooltipTextRect.right -=
            QuickNavScale(8);
        DrawD2DTextEllipsis(
            ctx.Get(), tooltip,
            tooltipTextRect,
            quickNavPathTextFormat_.Get(),
            t.popupTitle,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (windowClipPushed)
        ctx->PopLayer();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = quickNavDcompSurface_->EndDraw();
    if (FAILED(hr))
    {
        RecoverQuickNavCompositionFailure(L"EndDraw", hr);
        EndPaint(hwnd, &ps);
        return;
    }
    hr = dcompDevice_->Commit();
    if (FAILED(hr))
    {
        RecoverQuickNavCompositionFailure(L"Paint Commit", hr);
        EndPaint(hwnd, &ps);
        return;
    }
    quickNavCompositionRenderRecoveryPending_ = false;

    EndPaint(hwnd, &ps);
}

/**
 * @brief 快捷导航窗口的消息处理函数
 * @param hwnd 窗口句柄
 * @param msg 消息 ID
 * @param wp wParam
 * @param lp lParam
 * @return 消息处理结果
 */
inline LRESULT DesktopApp::HandleQuickNavigationMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Shell context menus send owner-draw and submenu messages to the
    // TrackPopupMenu owner. Quick Navigation owns menus opened from its
    // entries so it can stay active while the menu is visible.
    if (activeContextMenu3_)
    {
        LRESULT result = 0;
        if (SUCCEEDED(activeContextMenu3_->
                HandleMenuMsg2(msg, wp, lp, &result)))
            return result;
    }
    else if (activeContextMenu2_)
    {
        if (SUCCEEDED(activeContextMenu2_->
                HandleMenuMsg(msg, wp, lp)))
            return 0;
    }

    switch (msg)
    {
    case WM_NCHITTEST:
        if (!quickNavigationOpen_)
            return HTTRANSPARENT;
        break;
    case WM_TIMER:
        if (wp == kQuickNavigationAnimationTimerId)
        {
            quickNavigationAnimation_.Advance(
                GetTickCount64());
            ApplyQuickNavigationAnimationFrame();
            if (!quickNavigationAnimation_.
                    IsAnimating())
            {
                KillTimer(
                    hwnd,
                    kQuickNavigationAnimationTimerId);
                if (quickNavigationAnimation_.
                        IsHidden())
                {
                    FinalizeCloseQuickNavigation();
                }
                else
                {
                    InvalidateQuickNavigationWindow();
                }
            }
            return 0;
        }
        break;
    case WM_PAINT:
        PaintQuickNavigationWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lp) == quickNavigationSearchEdit_)
        {
            HDC hdcEdit = reinterpret_cast<HDC>(wp);
            SetBkMode(hdcEdit, OPAQUE);
            SetTextColor(hdcEdit, RGB(28, 34, 44));
            SetBkColor(hdcEdit, RGB(255, 255, 255));
            SetDCBrushColor(hdcEdit, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        break;
    case WM_LBUTTONDOWN:
    {
        ResetQuickNavigationKeyboardTarget();
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{
            pt.x + quickNavigationHostRect_.left,
            pt.y + quickNavigationHostRect_.top
        };

        {
            RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
            const int trackW = QuickNavScale(5);
            RECT scrollCol = MakeRect(content.right - trackW - QuickNavScale(4), content.top,
                content.right, content.bottom);
            if (!quickNavigationInitialJumpOpen_ &&
                PtInRect(&scrollCol, appPoint))
            {
                RECT track{}, thumb{};
                int maxScroll = 0, contentHeight = 0;
                if (GetQuickNavigationScrollbarGeometry(quickNavigationRect_,
                    track, thumb, maxScroll, contentHeight))
                {
                    if (PtInRect(&thumb, appPoint))
                    {
                        quickNavScrollbarDragging_ = true;
                        quickNavScrollbarDragStartY_ = appPoint.y;
                        quickNavScrollbarDragThumbTop_ = static_cast<int>(thumb.top);
                        quickNavScrollbarDragStartOffset_ = quickNavigationScrollOffset_;
                        SetCapture(hwnd);
                        return 0;
                    }
                    if (PtInRect(&track, appPoint))
                    {
                        int pageSize = std::max(
                            QuickNavScale(kQuickNavigationCellHeight + kQuickNavigationItemRowGap),
                            static_cast<int>(content.bottom - content.top) - QuickNavScale(28));
                        if (appPoint.y < thumb.top)
                            quickNavigationScrollOffset_ = std::max(0,
                                quickNavigationScrollOffset_ - pageSize);
                        else
                            quickNavigationScrollOffset_ = std::min(maxScroll,
                                quickNavigationScrollOffset_ + pageSize);
                        InvalidateQuickNavigationWindow();
                        return 0;
                    }
                }
                return 0;
            }
        }

        if (GetQuickNavigationEffectiveSearchText().empty())
        {
            RECT overlay = quickNavigationRect_;
            std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
            RECT tabs = GetQuickNavigationTabsRect(overlay);
            const int gap = QuickNavScale(8);
            const int sepGap = QuickNavScale(6);
            const int fixedWidth = quickNavTabWidths_.size() >= 2
                ? quickNavTabWidths_[0] + gap + quickNavTabWidths_[1]
                : 0;
            const int scrollPad = sepGap + QuickNavScale(1) + gap;
            const int tabsStart =
                GetQuickNavigationTabsStart(
                    overlay);
            RECT scrollHitBounds = MakeRect(
                tabsStart + fixedWidth +
                    scrollPad,
                tabs.top,
                tabs.right,
                tabs.bottom);
            for (size_t tab = 2; tab < ci.size() + 2; ++tab)
            {
                RECT tabRect = GetQuickNavigationTabRect(overlay, tab);
                RECT visibleTabRect{};
                if (IntersectRect(&visibleTabRect, &tabRect, &scrollHitBounds) &&
                    PtInRect(&visibleTabRect, appPoint))
                {
                    quickNavTabDragIndex_ = tab;
                    quickNavTabDragStartPoint_ = appPoint;
                    quickNavTabDragDeltaX_ = 0;
                    quickNavTabDragging_ = false;
                    SetCapture(hwnd);
                    return 0;
                }
            }
        }

        HandleQuickNavigationClick(appPoint);
        return 0;
    }
    case WM_RBUTTONUP:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{
            pt.x + quickNavigationHostRect_.left,
            pt.y + quickNavigationHostRect_.top
        };
        POINT screenPoint{ pt.x, pt.y };
        ClientToScreen(hwnd, &screenPoint);
        if (HandleQuickNavigationRightClick(appPoint, screenPoint))
            return 0;
        break;
    }
    case WM_CONTEXTMENU:
    {
        if (reinterpret_cast<HWND>(wp) != hwnd)
            break;
        POINT screenPoint{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT clientPoint = screenPoint;
        if (screenPoint.x == -1 && screenPoint.y == -1)
        {
            clientPoint = lastMousePoint_;
            screenPoint = clientPoint;
            screenPoint.x -=
                quickNavigationHostRect_.left;
            screenPoint.y -=
                quickNavigationHostRect_.top;
            ClientToScreen(hwnd, &screenPoint);
        }
        else
        {
            ScreenToClient(hwnd, &clientPoint);
            clientPoint.x +=
                quickNavigationHostRect_.left;
            clientPoint.y +=
                quickNavigationHostRect_.top;
        }
        if (HandleQuickNavigationRightClick(clientPoint, screenPoint))
            return 0;
        break;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{
            pt.x + quickNavigationHostRect_.left,
            pt.y + quickNavigationHostRect_.top
        };
        POINT previousMouse = lastMousePoint_;
        lastMousePoint_ = appPoint;
        if ((previousMouse.x != appPoint.x || previousMouse.y != appPoint.y) &&
            quickNavigationKeyboardTargetKind_ != QuickNavigationKeyboardTargetKind::None)
            ResetQuickNavigationKeyboardTarget();
        TRACKMOUSEEVENT mouseTrack{};
        mouseTrack.cbSize = sizeof(mouseTrack);
        mouseTrack.dwFlags = TME_LEAVE;
        mouseTrack.hwndTrack = hwnd;
        TrackMouseEvent(&mouseTrack);

        if (quickNavScrollbarDragging_)
        {
            RECT track{}, thumb{};
            int maxScroll = 0, contentHeight = 0;
            if (GetQuickNavigationScrollbarGeometry(quickNavigationRect_,
                track, thumb, maxScroll, contentHeight))
            {
                const int trackH = std::max<LONG>(1, track.bottom - track.top);
                const int thumbH = std::max<LONG>(1, thumb.bottom - thumb.top);
                int newThumbTop = appPoint.y - (quickNavScrollbarDragStartY_ -
                    quickNavScrollbarDragThumbTop_);
                newThumbTop = std::clamp(newThumbTop, static_cast<int>(track.top),
                    static_cast<int>(track.bottom - thumbH));
                const int rangeH = std::max(1, trackH - thumbH);
                quickNavigationScrollOffset_ = (newThumbTop - static_cast<int>(track.top))
                    * maxScroll / rangeH;
                quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0, maxScroll);
                InvalidateQuickNavigationWindow();
            }
            return 0;
        }

        if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            int dx = appPoint.x - quickNavTabDragStartPoint_.x;
            if (!quickNavTabDragging_ && std::abs(dx) > 4)
                quickNavTabDragging_ = true;
            if (quickNavTabDragging_)
                quickNavTabDragDeltaX_ = dx;
            InvalidateQuickNavigationWindow();
            return 0;
        }

        bool wasHovered = quickNavScrollbarHovered_;
        quickNavScrollbarHovered_ = false;
        {
            RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
            const int trackW = QuickNavScale(5);
            RECT scrollCol = MakeRect(content.right - trackW - QuickNavScale(4), content.top,
                content.right, content.bottom);
            if (!quickNavigationInitialJumpOpen_ &&
                PtInRect(&scrollCol, appPoint))
            {
                if (GetQuickNavigationContentHeight(quickNavigationRect_) >
                    static_cast<int>(content.bottom - content.top))
                {
                    RECT track{}, thumb{};
                    int ms = 0, ch = 0;
                    if (GetQuickNavigationScrollbarGeometry(quickNavigationRect_,
                        track, thumb, ms, ch) && PtInRect(&thumb, appPoint))
                    {
                        quickNavScrollbarHovered_ = true;
                    }
                }
            }
        }
        if (wasHovered != quickNavScrollbarHovered_)
            InvalidateQuickNavigationWindow();
        else if (previousMouse.x != appPoint.x || previousMouse.y != appPoint.y)
            InvalidateQuickNavigationWindow();
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        lastMousePoint_ = { -1000000, -1000000 };
        quickNavScrollbarHovered_ = false;
        InvalidateQuickNavigationWindow();
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (quickNavScrollbarDragging_)
        {
            ReleaseCapture();
            quickNavScrollbarDragging_ = false;
            InvalidateQuickNavigationWindow();
            return 0;
        }

        if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            ReleaseCapture();
            size_t dragTab = quickNavTabDragIndex_;

            if (quickNavTabDragging_)
            {
                std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
                int targetTab = GetQuickNavTabDragTarget(dragTab, quickNavTabDragDeltaX_);

                if (targetTab != static_cast<int>(dragTab) && targetTab >= 2 &&
                    static_cast<size_t>(targetTab) >= 2 &&
                    static_cast<size_t>(targetTab - 2) < ci.size())
                {
                    size_t srcIdx = dragTab - 2;
                    size_t dstIdx = static_cast<size_t>(targetTab) - 2;
                    EnsureNavTabOrder();

                    if (srcIdx < navTabOrder_.size() && dstIdx < navTabOrder_.size())
                    {
                        std::wstring id = navTabOrder_[srcIdx];
                        navTabOrder_.erase(navTabOrder_.begin() + srcIdx);
                        navTabOrder_.insert(navTabOrder_.begin() + dstIdx, id);
                        quickNavigationActiveWidgetIndex_ = ci[srcIdx];
                        quickNavigationScrollOffset_ = 0;
                        quickNavigationInitialJumpOpen_ = false;
                        SaveLayoutSlots();
                    }
                }
            }
            else
            {
                std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
                if (dragTab >= 2 && dragTab - 2 < ci.size())
                {
                    quickNavigationActiveWidgetIndex_ = ci[dragTab - 2];
                    quickNavigationScrollOffset_ = 0;
                    quickNavigationInitialJumpOpen_ = false;
                }
            }

            quickNavTabDragIndex_ = static_cast<size_t>(-1);
            quickNavTabDragDeltaX_ = 0;
            quickNavTabDragging_ = false;
            InvalidateQuickNavigationWindow();
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL:
        OnMouseWheel(wp, lp);
        return 0;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == quickNavigationSearchEdit_ && HIWORD(wp) == EN_CHANGE)
        {
            RefreshQuickNavigationSearchText();
            quickNavigationScrollOffset_ = 0;
            InvalidateQuickNavigationWindow();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (HandleQuickNavigationKeyboardInput(wp))
            return 0;
        if (wp == VK_ESCAPE)
        {
            if (quickNavScrollbarDragging_)
            {
                ReleaseCapture();
                quickNavScrollbarDragging_ = false;
                quickNavigationScrollOffset_ = quickNavScrollbarDragStartOffset_;
                InvalidateQuickNavigationWindow();
                return 0;
            }
            if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
            {
                ReleaseCapture();
                quickNavTabDragIndex_ = static_cast<size_t>(-1);
                quickNavTabDragDeltaX_ = 0;
                quickNavTabDragging_ = false;
                InvalidateQuickNavigationWindow();
                return 0;
            }
            CloseQuickNavigation();
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
        {
            const HWND activatedWindow = reinterpret_cast<HWND>(lp);
            if (activatedWindow == quickNavigationSearchEdit_ ||
                (renamingQuickNavigationItem_ &&
                    activatedWindow == renameEdit_) ||
                quickNavBackdropCompositor_.IsBackdropWindow(activatedWindow))
                return 0;
            if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
            {
                ReleaseCapture();
                quickNavTabDragIndex_ = static_cast<size_t>(-1);
                quickNavTabDragDeltaX_ = 0;
                quickNavTabDragging_ = false;
            }
            CloseQuickNavigation();
            return 0;
        }
        break;
    case WM_CLOSE:
        CloseQuickNavigation();
        return 0;
    case WM_DESTROY:
        if (quickNavigationHwnd_ == hwnd)
            quickNavigationHwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 快捷导航搜索编辑框的子类化窗口过程
 * @param hwnd 编辑框句柄
 * @param message 消息 ID
 * @param wParam wParam
 * @param lParam lParam
 * @param subclassId 子类化 ID
 * @param refData 引用数据（指向 DesktopApp 实例）
 * @return 消息处理结果
 */
inline LRESULT CALLBACK DesktopApp::QuickNavigationSearchSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* app = reinterpret_cast<DesktopApp*>(refData);
    if (!app) return DefSubclassProc(hwnd, message, wParam, lParam);

    if (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
    {
        const HWND activatedWindow = reinterpret_cast<HWND>(lParam);
        if (activatedWindow != app->quickNavigationHwnd_ &&
            !app->quickNavBackdropCompositor_.IsBackdropWindow(activatedWindow))
        {
            app->CloseQuickNavigation();
            return 0;
        }
    }

    if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        if (app->
            HandleQuickNavigationInitialJumpKeyboardInput(
                wParam))
            return 0;
        app->CloseQuickNavigation();
        return 0;
    }
    if (message == WM_KEYDOWN && app->quickNavigationSearchCompositionText_.empty() &&
        app->HandleQuickNavigationKeyboardInput(wParam))
    {
        return 0;
    }
    if (message == WM_MOUSEWHEEL)
    {
        app->OnMouseWheel(wParam, lParam);
        return 0;
    }
    if (message == WM_IME_STARTCOMPOSITION)
    {
        app->ClearQuickNavigationSearchCompositionText();
    }
    if (message == WM_IME_COMPOSITION)
    {
        app->RefreshQuickNavigationSearchCompositionText(hwnd, lParam);
    }
    if (message == WM_IME_ENDCOMPOSITION)
    {
        app->ClearQuickNavigationSearchCompositionText();
    }

    return DefSubclassProc(hwnd, message, wParam, lParam);
}
