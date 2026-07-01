/**
 * @file app_quick_navigation.h
 * @brief DesktopApp 快捷导航面板的内联实现。
 *
 * 该文件集中维护快捷导航的热键注册、独立窗口、搜索框、结果构建、绘制和交互逻辑。
 */

#pragma once

#include <dwmapi.h>
#include <imm.h>
#include "search_match.h"

// ── Quick Navigation ───────────────────────────────────────

inline constexpr DWORD kQuickNavigationEverythingResultLimit = 200;
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
            return NameSearchMatchRank(aName, normalizedQuery) <
                NameSearchMatchRank(bName, normalizedQuery);
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
        return L"Everything 未运行，正在查找应用...";
    return FindQuickNavigationEverythingAppEntry()
        ? L"Everything 未启动，点击启动"
        : L"未安装 Everything，点击下载";
}

inline bool DesktopApp::TryLaunchQuickNavigationEverythingApp() const
{
    const QuickNavigationAppEntry* entry = FindQuickNavigationEverythingAppEntry();
    if (!entry || !entry->absolutePidl.get())
        return false;

    Pidl launchPidl;
    launchPidl.reset(ILClone(entry->absolutePidl.get()));
    if (!launchPidl.get())
        return false;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_IDLIST;
    sei.lpIDList = launchPidl.get();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

inline std::vector<size_t> DesktopApp::GetQuickNavigationCollectionIndices() const
{
    auto isTabWidget = [](DesktopWidgetType t) {
        return t == DesktopWidgetType::Collection ||
               t == DesktopWidgetType::FileCategories ||
               t == DesktopWidgetType::FolderMapping;
    };
    std::vector<size_t> result;
    std::unordered_set<size_t> seen;

    for (const auto& id : navTabOrder_)
    {
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (isTabWidget(widgets_[i].type) &&
                widgets_[i].id == id && !seen.count(i))
            {
                result.push_back(i);
                seen.insert(i);
                break;
            }
        }
    }
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (isTabWidget(widgets_[i].type) && !seen.count(i))
        {
            result.push_back(i);
            seen.insert(i);
        }
    }
    return result;
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

inline std::vector<DesktopApp::QuickNavigationEntry> DesktopApp::GetQuickNavigationEntries() const
{
    std::vector<QuickNavigationEntry> result;
    std::unordered_set<std::wstring> seenDesktop;
    const std::wstring query = GetQuickNavigationEffectiveSearchText();
    auto matches = [&](const std::wstring& name) {
        return query.empty() || NameMatchesQuery(name, query);
    };
    auto appendDesktop = [&](size_t itemIndex, const std::wstring& source) {
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
        result.push_back(std::move(entry));
    };

    if (query.empty())
    {
        if (quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-1))
        {
            for (const auto& key : GetQuickNavigationItemKeys())
                appendDesktop(FindItemIndexByKey(key), L"集合");

            std::unordered_set<std::wstring> desktopKeys;
            for (const auto& widget : widgets_)
            {
                if (widget.type != DesktopWidgetType::Collection &&
                    widget.type != DesktopWidgetType::FileCategories) continue;
                for (const auto& key : widget.itemKeys)
                    desktopKeys.insert(ToUpperInvariant(key));
            }

            auto isLnkOrUrl = [](const std::wstring& path) -> bool {
                if (path.size() < 4) return false;
                std::wstring ext = path.substr(path.size() - 4);
                for (auto& c : ext) c = static_cast<wchar_t>(towupper(c));
                return ext == L".LNK" || ext == L".URL";
            };

            for (size_t i = 0; i < items_.size(); ++i)
            {
                const DesktopItem& item = items_[i];
                if (desktopKeys.contains(ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey))) continue;
                if (!isLnkOrUrl(item.parsingName)) continue;
                appendDesktop(i, L"自由桌面");
            }
            return result;
        }

        if (quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-2))
        {
            for (size_t wi = 0; wi < widgets_.size(); ++wi)
            {
                if (widgets_[wi].type != DesktopWidgetType::FolderMapping) continue;
                std::wstring source = widgets_[wi].title.empty() ? L"文件夹映射" : widgets_[wi].title;
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
                    result.push_back(std::move(entry));
                }
            }
            return result;
        }

        if (quickNavigationActiveWidgetIndex_ < widgets_.size() &&
            widgets_[quickNavigationActiveWidgetIndex_].type == DesktopWidgetType::FolderMapping)
        {
            const DesktopWidget& widget = widgets_[quickNavigationActiveWidgetIndex_];
            std::wstring source = widget.title.empty() ? L"文件夹映射" : widget.title;
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
                result.push_back(std::move(entry));
            }
            return result;
        }

        for (const auto& key : GetQuickNavigationItemKeys())
            appendDesktop(FindItemIndexByKey(key), L"集合");

        bool isDesktopAll = quickNavigationActiveWidgetIndex_ >= widgets_.size() ||
            (widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::Collection &&
             widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::FileCategories &&
             widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::FolderMapping);
        if (isDesktopAll)
        {
            std::unordered_set<std::wstring> collectionKeys;
            for (const auto& widget : widgets_)
            {
                if (widget.type != DesktopWidgetType::Collection &&
                    widget.type != DesktopWidgetType::FileCategories &&
                    widget.type != DesktopWidgetType::FolderMapping) continue;
                if (widget.type == DesktopWidgetType::FolderMapping)
                {
                    for (const auto& fe : widget.folderEntries)
                        collectionKeys.insert(ToUpperInvariant(fe.fullPath));
                }
                else
                {
                    for (const auto& key : widget.itemKeys)
                        collectionKeys.insert(ToUpperInvariant(key));
                }
            }

            auto isLnkOrUrl = [](const std::wstring& path) -> bool {
                if (path.size() < 4) return false;
                std::wstring ext = path.substr(path.size() - 4);
                for (auto& c : ext) c = static_cast<wchar_t>(towupper(c));
                return ext == L".LNK" || ext == L".URL";
            };

            for (size_t i = 0; i < items_.size(); ++i)
            {
                const DesktopItem& item = items_[i];
                std::wstring key = ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
                if (collectionKeys.contains(key)) continue;
                if (!isLnkOrUrl(item.parsingName)) continue;
                appendDesktop(i, L"自由桌面");
            }
        }

        return result;
    }

    for (size_t i = 0; i < items_.size(); ++i)
        appendDesktop(i, L"桌面");

    for (size_t ci : GetQuickNavigationCollectionIndices())
    {
        const DesktopWidget& widget = widgets_[ci];
        if (widget.type == DesktopWidgetType::FolderMapping) continue;
        std::wstring source = widget.title.empty() ? L"集合" : widget.title;
        for (const auto& key : widget.itemKeys)
            appendDesktop(FindItemIndexByKey(key), source);
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        const DesktopWidget& widget = widgets_[wi];
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;
        std::wstring source = widget.title.empty() ? L"文件夹映射" : widget.title;
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
            result.push_back(std::move(entry));
        }
    }

    std::stable_sort(result.begin(), result.end(),
        [&](const QuickNavigationEntry& a, const QuickNavigationEntry& b) {
            return NameSearchMatchRank(a.name, query) <
                NameSearchMatchRank(b.name, query);
        });
    return result;
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
    const auto& tw = quickNavTabWidths_;
    const int gap = QuickNavScale(8);
    const int sepGap = QuickNavScale(6);
    // 可滚动区起始于固定标签（0:桌面, 1:映射）+ 分隔线之后。
    const int fixedWidth = (tw.size() >= 2) ? (tw[0] + gap + tw[1]) : 0;
    const int scrollPad = sepGap + QuickNavScale(1) + gap;
    const int scrollLeft = tabs.left + fixedWidth + scrollPad;
    const int available = std::max(1, static_cast<int>(tabs.right - scrollLeft));
    return std::max(0, GetQuickNavigationTabStripContentWidth(overlay) - available);
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
        if (isTabWidget(w.type) && !orderSet.count(w.id))
        {
            navTabOrder_.push_back(w.id);
            orderSet.insert(w.id);
        }
    }
    navTabOrder_.erase(
        std::remove_if(navTabOrder_.begin(), navTabOrder_.end(),
            [this, &isTabWidget](const std::wstring& id) {
                for (const auto& w : widgets_)
                    if (isTabWidget(w.type) && w.id == id) return false;
                return true;
            }),
        navTabOrder_.end());
}

inline RECT DesktopApp::GetQuickNavigationTabRect(const RECT& overlay, size_t tabIndex) const
{
    RECT tabs = GetQuickNavigationTabsRect(overlay);
    const int gap = QuickNavScale(8);
    const int sepGap = QuickNavScale(6);
    const int scrollPad = sepGap + QuickNavScale(1) + gap; // separator + gap after

    const size_t n = quickNavTabWidths_.size();
    if (n == 0)
        return MakeRect(tabs.left, tabs.top, tabs.left + QuickNavScale(72), tabs.bottom);

    int left = tabs.left;
    int width = QuickNavScale(72);
    if (tabIndex == 0)
    {
        if (n > 0) width = quickNavTabWidths_[0];
    }
    else if (tabIndex == 1)
    {
        if (n > 1)
        {
            left = tabs.left + quickNavTabWidths_[0] + gap;
            width = quickNavTabWidths_[1];
        }
    }
    else if (tabIndex >= 2)
    {
        left = tabs.left;
        if (n > 1)
            left = tabs.left + quickNavTabWidths_[0] + gap + quickNavTabWidths_[1] + scrollPad;
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
    const int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
    const int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
    const int gap = GetQuickNavigationGap(overlay);
    int halfPad = gap / 2;
    const int top = content.top + (GetQuickNavigationEffectiveSearchText().empty()
        ? 0
        : QuickNavScale(28) + QuickNavScale(8));
    return MakeRect(
        content.left + halfPad + col * (cellW + gap),
        top + row * rowPitch - quickNavigationScrollOffset_,
        content.left + halfPad + col * (cellW + gap) + cellW,
        top + row * rowPitch + cellH - quickNavigationScrollOffset_);
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

inline int DesktopApp::GetQuickNavigationContentHeight(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int columns = GetQuickNavigationColumnCount(overlay);
    const int desktopCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    if (GetQuickNavigationEffectiveSearchText().empty())
    {
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
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kQuickNavigationWindowClassName,
L"SnowDesktop 快捷导航",
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

    quickNavigationSearchEdit_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL,
        0, 0, 1, 1, quickNavigationHwnd_, reinterpret_cast<HMENU>(1002),
        instance_, nullptr);
    if (!quickNavigationSearchEdit_)
        return;

    quickNavigationSearchFont_ = CreateFontW(-QuickNavScale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(quickNavigationSearchEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(quickNavigationSearchFont_ ? quickNavigationSearchFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(quickNavigationSearchEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(QuickNavScale(10), QuickNavScale(10)));
    SendMessageW(quickNavigationSearchEdit_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"在桌面、应用、Everything中搜索"));
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
    OffsetRect(&search, -quickNavigationRect_.left, -quickNavigationRect_.top);
    SetWindowPos(quickNavigationSearchEdit_, HWND_TOP,
        search.left + QuickNavScale(4), search.top + QuickNavScale(6),
        std::max<LONG>(1, search.right - search.left - QuickNavScale(8)),
        std::max<LONG>(1, search.bottom - search.top - QuickNavScale(10)),
        SWP_NOZORDER | SWP_NOACTIVATE);
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
            RefreshQuickNavigationEverythingResults();
        return;
    }
    std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(quickNavigationSearchEdit_, buffer.data(), len + 1);
    buffer.resize(static_cast<size_t>(len));
    quickNavigationSearchText_ = std::move(buffer);
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
        RefreshQuickNavigationEverythingResults();
}

inline void DesktopApp::ClearQuickNavigationEverythingResults()
{
    quickNavigationAppResultIndices_.clear();
    quickNavigationAppsExpanded_ = false;
    quickNavigationEverythingResults_.clear();
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
    ClearQuickNavigationEverythingResults();
    const std::wstring query = GetQuickNavigationEffectiveSearchText();
    if (query.empty())
        return;

    RefreshQuickNavigationAppResults();

    std::unordered_set<std::wstring> seenPaths;
    for (const auto& result : SearchEverythingCached(
        query, kQuickNavigationEverythingResultLimit))
    {
        if (result.path.empty())
            continue;
        std::wstring normalizedPath = ToUpperInvariant(result.path);
        if (seenPaths.contains(normalizedPath))
            continue;
        seenPaths.insert(std::move(normalizedPath));

        QuickNavigationEverythingEntry entry;
        entry.name = result.name.empty() ? FileNameFromPath(result.path) : result.name;
        entry.path = result.path;
        entry.isDirectory = result.isDirectory;
        entry.systemIconIndex = GetQuickNavigationEverythingIconIndex(entry.path, entry.isDirectory);
        quickNavigationEverythingResults_.push_back(std::move(entry));
    }
}

/**
 * @brief 定位并显示快捷导航窗口（含圆角区域设置）
 */
inline void DesktopApp::PositionQuickNavigationWindow()
{
    if (!quickNavigationHwnd_ || !IsWindow(quickNavigationHwnd_))
        return;

    quickNavigationRect_ = GetQuickNavigationRect();
    const int width = std::max<LONG>(1, quickNavigationRect_.right - quickNavigationRect_.left);
    const int height = std::max<LONG>(1, quickNavigationRect_.bottom - quickNavigationRect_.top);

    // Prefer compositor-provided corners. Unlike a window region, DWM corners
    // retain partial-alpha edge pixels and therefore do not look stair-stepped.
    SetWindowRgn(quickNavigationHwnd_, nullptr, FALSE);
    const DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
    const HRESULT cornerResult = DwmSetWindowAttribute(quickNavigationHwnd_,
        DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
    if (FAILED(cornerResult))
    {
        // Older Windows versions do not expose DWM corner preferences.
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
            QuickNavScale(16), QuickNavScale(16));
        if (region)
            SetWindowRgn(quickNavigationHwnd_, region, TRUE);
    }

    SetWindowPos(quickNavigationHwnd_, HWND_TOPMOST,
        quickNavigationRect_.left + virtualLeft_,
        quickNavigationRect_.top + virtualTop_,
        width, height,
        SWP_SHOWWINDOW);
    EnsureQuickNavigationSearchEdit();
    UpdateQuickNavigationSearchEditRect();
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
    if (tab == 0) return L"桌面";
    if (tab == 1) return L"映射";
    std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
    if (tab - 2 >= ci.size()) return L"";
    const DesktopWidget& widget = widgets_[ci[tab - 2]];
    if (!widget.title.empty())
        return widget.title;
    if (widget.type == DesktopWidgetType::FileCategories)
        return L"桌面文件";
    if (widget.type == DesktopWidgetType::FolderMapping)
        return L"文件夹映射";
    return L"集合" + std::to_wstring(tab - 1);
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
inline void DesktopApp::OpenQuickNavigation()
{
    if (dragSession_.IsActive() || externalDragActive_)
        return;
    CloseCollectionPopup();
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
    quickNavigationSearchText_.clear();
    quickNavigationSearchCompositionText_.clear();
    ClearQuickNavigationEverythingResults();
    StartQuickNavigationAppIndexing();
    if (!CreateQuickNavigationWindow())
    {
        quickNavigationOpen_ = false;
        MessageBeep(MB_ICONWARNING);
        return;
    }
    PositionQuickNavigationWindow();
    if (quickNavigationSearchEdit_)
        SetWindowTextW(quickNavigationSearchEdit_, L"");
    ShowWindow(quickNavigationHwnd_, SW_SHOWNA);
    AnimateWindow(quickNavigationHwnd_, 160, AW_BLEND);
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
        ShowWindow(quickNavigationSearchEdit_, SW_SHOW);
    SetForegroundWindow(quickNavigationHwnd_);
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        SetFocus(quickNavigationSearchEdit_);
        SendMessageW(quickNavigationSearchEdit_, EM_SETSEL, 0, -1);
    }
    else
    {
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
    quickNavigationOpen_ = false;
    quickNavigationScrollOffset_ = 0;
    quickNavigationTabScrollOffset_ = 0;
    quickNavigationSearchText_.clear();
    quickNavigationSearchCompositionText_.clear();
    ClearQuickNavigationEverythingResults();
    quickNavigationRect_ = {};
    quickNavTabDragIndex_ = static_cast<size_t>(-1);
    quickNavTabDragDeltaX_ = 0;
    quickNavTabDragging_ = false;
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
    {
        if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
            ShowWindow(quickNavigationSearchEdit_, SW_HIDE);
        AnimateWindow(quickNavigationHwnd_, 120, AW_BLEND | AW_HIDE);
    }
    DestroyQuickNavigationWindow();
    if (customDesktopVisible_)
        FocusDesktopInputWindow();
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
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
        RECT tab0Rect = GetQuickNavigationTabRect(overlay, 0);
        if (PtInRect(&tab0Rect, point))
        {
            quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
            quickNavigationScrollOffset_ = 0;
            InvalidateQuickNavigationWindow();
            return true;
        }
        RECT tab1Rect = GetQuickNavigationTabRect(overlay, 1);
        if (PtInRect(&tab1Rect, point))
        {
            quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-2);
            quickNavigationScrollOffset_ = 0;
            InvalidateQuickNavigationWindow();
            return true;
        }
    }

    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return true;

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
                TryLaunchQuickNavigationEverythingApp();
                CloseQuickNavigation();
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

    const QuickNavigationAppEntry* appEntry = nullptr;
    if (TryGetQuickNavigationAppEntryAtPoint(point, appEntry) &&
        appEntry && appEntry->absolutePidl.get())
    {
        Pidl launchPidl;
        launchPidl.reset(ILClone(appEntry->absolutePidl.get()));
        CloseQuickNavigation();
        if (launchPidl.get())
        {
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_IDLIST;
            sei.lpIDList = launchPidl.get();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        }
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
            ShellExecuteW(nullptr, L"open", items_[entry.itemIndex].parsingName.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
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
    if (!TryGetQuickNavigationEverythingEntryAtPoint(point, entry))
        return false;

    ShowQuickNavigationEverythingContextMenu(entry, screenPoint);
    return true;
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
        stem = L"快捷方式";
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
            typeName == L"应用" || typeName == L"应用程序";
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
    };

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, kAppOpen, L"打开");
    AppendMenuW(menu, MF_STRING, kAppCreateShortcut, L"发送快捷方式到桌面");

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
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_IDLIST;
        sei.lpIDList = entry.absolutePidl.get();
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
        break;
    }
    case kAppCreateShortcut:
        CreateDesktopShortcutForApp(entry);
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

    AppendMenuW(menu, MF_STRING, kEverythingOpen, L"打开");
    AppendMenuW(menu, MF_STRING, kEverythingReveal, L"在资源管理器中显示");
    AppendMenuW(menu, MF_STRING, kEverythingCreateShortcut, L"发送快捷方式到桌面");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kEverythingCopyPath, L"复制路径");

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
    {
        std::wstring params = L"/select,\"" + entry.path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
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

    RECT client{};
    GetClientRect(quickNavigationHwnd_, &client);
    const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));

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
    // 将 app 坐标（quickNavigationRect_ 基准）映射到 surface 局部坐标。
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(updateOffset.x - quickNavigationRect_.left),
        static_cast<float>(updateOffset.y - quickNavigationRect_.top)));
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // 文本抗锯齿沿用 DComp 默认（与桌面一致），避免在 alpha 表面上强制 ClearType 产生彩色毛边。
    ctx->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    brushCache_.clear();
    brushCacheContext_ = ctx.Get();

    const RECT& overlay = quickNavigationRect_;
    // 窗口背景（不透明满铺）+ 圆角边框。圆角透明由 DWM 角偏好/窗口区域裁剪。
    DrawD2DFilledRectangle(ctx.Get(), overlay, ToD2DColor(t.windowBg), D2D1::ColorF(0, 0, 0, 0));
    DrawD2DRoundedRectangle(ctx.Get(),
        MakeRect(overlay.left, overlay.top, overlay.right - 1, overlay.bottom - 1),
        static_cast<float>(QuickNavScale(16)) / 2.0f,
        D2D1::ColorF(0, 0, 0, 0), ToD2DColor(t.windowBorder));

    const bool searching = !GetQuickNavigationEffectiveSearchText().empty();
    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    std::vector<QuickNavigationEntry> entries = GetQuickNavigationEntries();
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
        const int tabClipRight = searchRect.right;
        // 左侧外扩一圈避免固定标签圆角 AA 被截断；右侧与搜索框右边界对齐。
        const int clipPad = QuickNavScale(8);
        ctx->PushAxisAlignedClip(
            ToD2DRect(MakeRect(tabs.left - clipPad, tabs.top, tabClipRight, tabs.bottom)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const size_t tabCount = collectionIndices.size() + 2;
        UpdateQuickNavTabWidths();
        const auto& tabWidths = quickNavTabWidths_;
        const int gap = QuickNavScale(8);
        const int sepGap = QuickNavScale(6);
        const int fixedWidth = (tabWidths.size() >= 2 ? tabWidths[0] + gap + tabWidths[1] : 0);
        const int scrollPad = sepGap + QuickNavScale(1) + gap; // separator + gap after
        auto calcTabPosX = [&](size_t tabIdx) -> int {
            if (tabIdx == 0) return tabs.left;
            if (tabIdx == 1) return tabs.left + tabWidths[0] + gap;
            int x = tabs.left + fixedWidth + scrollPad;
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
                int scrollStart = tabs.left + fixedWidth + scrollPad;
                if (tabRect.right <= scrollStart || tabRect.left >= tabClipRight) return;
            }
            else if (tab <= 1)
            {
                if (tabRect.right <= tabs.left || tabRect.left >= tabs.left + fixedWidth + sepGap) return;
                tabRect.right = std::min(tabRect.right, tabs.left + fixedWidth + sepGap);
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
                fill = ToD2DColor(t.tabDragFill);
                stroke = ToD2DColor(t.tabDragStroke);
            }
            else
            {
                fill = active ? ToD2DColor(t.tabActiveFill)
                    : (hovered ? ToD2DColor(t.tabHoverFill) : ToD2DColor(t.tabDefaultFill));
                stroke = active ? ToD2DColor(t.tabActiveStroke) : ToD2DColor(t.tabDefaultStroke);
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
        int sepX = tabs.left + fixedWidth + sepGap;
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
        int scrollLeft = tabs.left + fixedWidth + scrollPad;
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
            tabRect.left = std::max(tabRect.left, tabs.left);
            tabRect.right = std::min<LONG>(tabRect.right, static_cast<LONG>(tabClipRight));
            DrawD2DRoundedRectangle(ctx.Get(), tabRect,
                static_cast<float>(QuickNavScale(14)) / 2.0f,
                ToD2DColor(t.tabDragFloatFill), ToD2DColor(t.tabDragFloatStroke));

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
    }

    RECT contentApp = GetQuickNavigationContentRect(overlay);
    ctx->PushAxisAlignedClip(ToD2DRect(contentApp), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (entries.empty() && quickNavigationAppResultIndices_.empty() &&
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
                    ? (collectionIndices.empty() ? L"暂无集合组件" : L"当前分类暂无项目")
                    : L"没有匹配结果",
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
            std::wstring desktopLabel = L"桌面结果  " + std::to_wstring(entries.size()) + L" 项";
            DrawD2DTextEllipsis(ctx.Get(), desktopLabel, desktopHeader,
                quickNavTabTextFormat_.Get(), ToD2DColor(t.headerText),
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            RECT desktopSep = MakeRect(desktopHeader.left,
                desktopHeader.bottom - QuickNavScale(1),
                desktopHeader.right, desktopHeader.bottom);
            DrawD2DSeparator(ctx.Get(), desktopSep, ToD2DColor(t.headerSeparator));
        }

        // 桌面项直接走 D2D（ctx 为 ID2D1DeviceContext，与桌面共享 d2dDevice_ 缓存）
        for (size_t i = 0; i < entries.size(); ++i)
        {
            RECT itemRectApp = GetQuickNavigationItemRect(overlay, i);
            if (itemRectApp.bottom <= contentApp.top ||
                itemRectApp.top >= contentApp.bottom)
                continue;

            const QuickNavigationEntry& entry = entries[i];
            const int state = PtInRect(&itemRectApp, lastMousePoint_) != FALSE ? 1 : 0;
            // 图标本体复用桌面绘制，标题由快捷导航自绘，避免桌面标题布局和字重互相影响。
            if (entry.kind == QuickNavigationEntry::Kind::DesktopItem &&
                entry.itemIndex < items_.size())
            {
                DesktopIcon icon(&items_[entry.itemIndex], nullptr, this);
                icon.Draw(ctx.Get(), itemRectApp, state, quickNavLightTheme_, false);
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
                icon.Draw(ctx.Get(), itemRectApp, state, quickNavLightTheme_, false);
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
                std::wstring appLabel = L"应用  " +
                    std::to_wstring(quickNavigationAppResultIndices_.size()) + L" 项";
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

                    if (PtInRect(&rowRectApp, lastMousePoint_) != FALSE)
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
                    DrawD2DTextEllipsis(ctx.Get(), L"应用", typeRect,
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
                        const bool hovered = PtInRect(&buttonRectApp, lastMousePoint_) != FALSE;
                        std::wstring expandLabel = L"展开全部应用结果（" +
                            std::to_wstring(quickNavigationAppResultIndices_.size()) + L" 项）";
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
                    std::to_wstring(quickNavigationEverythingResults_.size()) + L" 项";
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

                    if (PtInRect(&rowRectApp, lastMousePoint_) != FALSE)
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
                    DrawD2DTextEllipsis(ctx.Get(), entry.path, pathRect,
                        quickNavPathTextFormat_.Get(), ToD2DColor(t.appTypeText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
            }
        }
    }
    ctx->PopAxisAlignedClip();

    RECT track{}, thumb{};
    int maxScroll = 0, contentHeight = 0;
    if (GetQuickNavigationScrollbarGeometry(overlay,
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
    switch (msg)
    {
    case WM_PAINT:
        PaintQuickNavigationWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lp) == quickNavigationSearchEdit_)
        {
            HDC hdcEdit = reinterpret_cast<HDC>(wp);
            SetBkColor(hdcEdit, RGB(255, 255, 255));
            SetDCBrushColor(hdcEdit, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        break;
    case WM_LBUTTONDOWN:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{ pt.x + quickNavigationRect_.left, pt.y + quickNavigationRect_.top };

        {
            RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
            const int trackW = QuickNavScale(5);
            RECT scrollCol = MakeRect(content.right - trackW - QuickNavScale(4), content.top,
                content.right, content.bottom);
            if (PtInRect(&scrollCol, appPoint))
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
            for (size_t tab = 2; tab < ci.size() + 2; ++tab)
            {
                RECT tabRect = GetQuickNavigationTabRect(overlay, tab);
                if (PtInRect(&tabRect, appPoint))
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
        POINT appPoint{ pt.x + quickNavigationRect_.left, pt.y + quickNavigationRect_.top };
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
            screenPoint.x -= quickNavigationRect_.left;
            screenPoint.y -= quickNavigationRect_.top;
            ClientToScreen(hwnd, &screenPoint);
        }
        else
        {
            ScreenToClient(hwnd, &clientPoint);
            clientPoint.x += quickNavigationRect_.left;
            clientPoint.y += quickNavigationRect_.top;
        }
        if (HandleQuickNavigationRightClick(clientPoint, screenPoint))
            return 0;
        break;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{ pt.x + quickNavigationRect_.left, pt.y + quickNavigationRect_.top };
        POINT previousMouse = lastMousePoint_;
        lastMousePoint_ = appPoint;
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
            if (PtInRect(&scrollCol, appPoint))
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

    if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        app->CloseQuickNavigation();
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
