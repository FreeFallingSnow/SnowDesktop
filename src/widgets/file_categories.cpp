/**
 * @file file_categories.cpp
 * @brief 实现 FileCategories 组件——一个带分类标签页的可滚动文件分类面板。
 *
 * 该组件将桌面项目按类型（文件夹、视频、图片、文档、压缩包、音频、程序等）
 * 分类展示，支持标签页切换、列表/网格模式切换、拖拽排序、滚动等功能。
 * 所有桌面级散文件会自动收集到"全部"分类下，并按扩展名归入对应类别。
 */

#include "widget.h"
#include "slot.h"
#include "types.h"
#include "app.h"
#include "drop_model.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>
#include <unordered_set>

static RECT FileCategoryItemRect(FileCategories* widget, size_t linearIndex);

/**
 * @brief 获取文件分类的有序列表。
 * @return 包含所有分类 ID 的字符串向量，顺序决定了标签页的排列顺序。
 */
static std::vector<std::wstring> FileCategoryOrder()
{
    return {
        L"all", L"folders", L"videos", L"images", L"documents",
        L"archives", L"audio", L"programs", L"others",
    };
}

/**
 * @brief 获取分类 ID 对应的中文显示标签。
 * @param id 分类 ID，如 "all"、"folders"、"videos" 等。
 * @return 对应的中文标签字符串，如"全部"、"文件夹"、"视频"。未知 ID 返回"其他"。
 */
static std::wstring FileCategoryLabel(const std::wstring& id)
{
    if (id == L"all") return L"全部";
    if (id == L"folders") return L"文件夹";
    if (id == L"videos") return L"视频";
    if (id == L"images") return L"图片";
    if (id == L"documents") return L"文档";
    if (id == L"archives") return L"压缩包";
    if (id == L"audio") return L"音频";
    if (id == L"programs") return L"程序";
    return L"其他";
}

/**
 * @brief 获取桌面项目文件扩展名的大写形式。
 * @param item 桌面项目，包含 PIDL 路径和文件名信息。
 * @return 全大写扩展名字符串（含点号），如 ".PNG"、".DOCX"。
 */
static std::wstring DesktopItemExtensionUpper(const DesktopItem& item)
{
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
        return ToUpperInvariant(PathFindExtensionW(path));
    return ToUpperInvariant(PathFindExtensionW(item.name.c_str()));
}

/**
 * @brief 判断桌面项目是否为快捷方式文件（.lnk 或 .url）。
 * @param item 桌面项目。
 * @return true 如果扩展名为 .LNK 或 .URL；否则返回 false。
 */
static bool IsShortcutItem(const DesktopItem& item)
{
    const std::wstring ext = DesktopItemExtensionUpper(item);
    return ext == L".LNK" || ext == L".URL";
}

/**
 * @brief 判断桌面项目是否为文件系统上的真实文件夹。
 * @param item 桌面项目。
 * @return true 如果 PIDL 可解析为路径且文件属性包含 FILE_ATTRIBUTE_DIRECTORY。
 */
static bool IsFilesystemFolder(const DesktopItem& item)
{
    wchar_t path[MAX_PATH]{};
    if (!SHGetPathFromIDListW(item.absolutePidl.get(), path)) return false;
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/**
 * @brief 根据扩展名或属性确定桌面项目所属的分类 ID。
 * @param item 桌面项目。
 * @return 分类 ID 字符串，可能为 "folders"、"videos"、"images"、"documents"、
 *         "archives"、"audio"、"programs" 或 "others"。
 */
static std::wstring FileCategoryIdForItem(const DesktopItem& item)
{
    const std::wstring ext = DesktopItemExtensionUpper(item);
    if (ext == L".ZIP" || ext == L".RAR" || ext == L".7Z" || ext == L".TAR" ||
        ext == L".GZ" || ext == L".BZ2" || ext == L".XZ")
        return L"archives";
    if (IsFilesystemFolder(item))
        return L"folders";
    if (ext == L".MP4" || ext == L".MOV" || ext == L".AVI" || ext == L".MKV" ||
        ext == L".WMV" || ext == L".WEBM" || ext == L".M4V")
        return L"videos";
    if (ext == L".PNG" || ext == L".JPG" || ext == L".JPEG" || ext == L".GIF" ||
        ext == L".BMP" || ext == L".WEBP" || ext == L".HEIC" || ext == L".SVG")
        return L"images";
    if (ext == L".TXT" || ext == L".MD" || ext == L".DOC" || ext == L".DOCX" ||
        ext == L".PDF" || ext == L".XLS" || ext == L".XLSX" || ext == L".PPT" ||
        ext == L".PPTX" || ext == L".CSV")
        return L"documents";
    if (ext == L".MP3" || ext == L".WAV" || ext == L".FLAC" || ext == L".AAC" ||
        ext == L".M4A" || ext == L".OGG")
        return L"audio";
    if (ext == L".EXE" || ext == L".MSI" || ext == L".BAT" || ext == L".CMD" ||
        ext == L".LNK")
        return L"programs";
    return L"others";
}

/**
 * @brief 判断桌面项目是否应收录到分类面板中。
 *        排除系统图标（此电脑、用户文件、网络、控制面板、回收站）和快捷方式文件。
 * @param app DesktopApp 实例指针。
 * @param item 待判断的桌面项目。
 * @return true 如果项目应被收录；false 如果受保护或为快捷方式。
 */
static bool IsCollectable(DesktopApp* app, const DesktopItem& item)
{
    (void)app;
    std::wstring clsid = !item.desktopIconClsid.empty()
        ? item.desktopIconClsid
        : ExtractClsidText(item.parsingName);
    bool protectedIcon = clsid == kDesktopIconClsidThisPC ||
        clsid == kDesktopIconClsidUserFiles ||
        clsid == kDesktopIconClsidNetwork ||
        clsid == kDesktopIconClsidControlPanel ||
        clsid == kDesktopIconClsidRecycleBin;
    return !protectedIcon && !IsShortcutItem(item) && !item.layoutKey.empty();
}

void FileCategories::EnsureCategorySnapshot() const
{
    if (!data_ || !app_) return;
    if (categorySnapshot_.valid &&
        categorySnapshot_.desktopItemCount == app_->GetDesktopItems().size() &&
        categorySnapshot_.sourceKeys == data_->itemKeys)
        return;

    categorySnapshot_ = {};
    categorySnapshot_.desktopItemCount = app_->GetDesktopItems().size();
    categorySnapshot_.sourceKeys = data_->itemKeys;

    std::unordered_set<std::wstring> seen;
    auto& allKeys = categorySnapshot_.keysByCategory[L"all"];
    for (const auto& rawKey : data_->itemKeys)
    {
        size_t itemIdx = app_->FindItemIndexByKey(rawKey);
        if (itemIdx == static_cast<size_t>(-1)) continue;
        const DesktopItem& item = app_->GetDesktopItems()[itemIdx];
        if (!IsCollectable(app_, item)) continue;
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!seen.insert(key).second) continue;
        allKeys.push_back(key);
        categorySnapshot_.keysByCategory[FileCategoryIdForItem(item)].push_back(key);
    }

    for (const auto& id : FileCategoryOrder())
    {
        auto it = categorySnapshot_.keysByCategory.find(id);
        if (it != categorySnapshot_.keysByCategory.end() && !it->second.empty())
            categorySnapshot_.visibleCategoryIds.push_back(id);
    }
    categorySnapshot_.valid = true;
}

void FileCategories::InvalidateCategorySnapshot() const
{
    categorySnapshot_.valid = false;
}

const std::vector<std::wstring>& FileCategories::CachedCategoryKeys(
    const std::wstring& categoryId) const
{
    static const std::vector<std::wstring> empty;
    EnsureCategorySnapshot();
    auto it = categorySnapshot_.keysByCategory.find(
        categoryId.empty() ? L"all" : categoryId);
    return it != categorySnapshot_.keysByCategory.end() ? it->second : empty;
}

const std::vector<std::wstring>& FileCategories::CachedVisibleCategoryIds() const
{
    EnsureCategorySnapshot();
    return categorySnapshot_.visibleCategoryIds;
}

std::wstring FileCategories::CachedActiveCategoryId() const
{
    if (!data_) return L"";
    const auto& visible = CachedVisibleCategoryIds();
    if (!data_->activeCategoryId.empty() &&
        std::find(visible.begin(), visible.end(), data_->activeCategoryId) != visible.end())
        return data_->activeCategoryId;
    return visible.empty() ? L"" : visible.front();
}

const std::vector<std::wstring>& FileCategories::GetSearchResultKeys() const
{
    if (searchText_.empty())
        return CachedCategoryKeys(CachedActiveCategoryId());
    EnsureCategorySnapshot();
    const auto& allKeys = CachedCategoryKeys(L"all");
    std::wstring query = ToUpperInvariant(searchText_);
    searchResultCache_.clear();
    for (const auto& key : allKeys)
    {
        size_t itemIdx = app_->FindItemIndexByKey(key);
        if (itemIdx == static_cast<size_t>(-1)) continue;
        if (ToUpperInvariant(app_->GetDesktopItems()[itemIdx].name).find(query) != std::wstring::npos)
            searchResultCache_.push_back(key);
    }
    return searchResultCache_;
}

/**
 * @brief 收集顶级桌面项目中可收录的项到 widget 的 itemKeys 中。
 *        跳过已在其他组件中的项目以及已存在的 key。
 * @return true 如果至少新增了一个项目；false 如果没有变化或参数无效。
 *
 * 收集完成后会重置滚动偏移并更新激活分类。
 */
bool FileCategories::CollectTopLevelDesktopItems()
{
    if (!data_ || !app_) return false;

    std::unordered_set<std::wstring> existing;
    for (const auto& key : data_->itemKeys)
        existing.insert(ToUpperInvariant(key));

    bool changed = false;
    for (const auto& item : app_->GetDesktopItems())
    {
        if (!IsCollectable(app_, item) || app_->IsItemInAnyWidget(item))
            continue;

        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (key.empty() || existing.contains(key))
            continue;

        data_->itemKeys.push_back(key);
        existing.insert(key);
        changed = true;
    }

    if (changed)
    {
        data_->scrollOffset = 0;
        InvalidateCategorySnapshot();
        if (data_->activeCategoryId.empty())
            data_->activeCategoryId = CachedActiveCategoryId();
        InvalidateSlots();
    }
    return changed;
}

bool FileCategories::PruneUncollectableItems()
{
    if (!data_ || !app_) return false;
    const size_t oldSize = data_->itemKeys.size();
    data_->itemKeys.erase(
        std::remove_if(data_->itemKeys.begin(), data_->itemKeys.end(),
            [&](const std::wstring& key) {
                size_t itemIdx = app_->FindItemIndexByKey(key);
                return itemIdx != static_cast<size_t>(-1) &&
                    !IsCollectable(app_, app_->GetDesktopItems()[itemIdx]);
            }),
        data_->itemKeys.end());
    if (data_->itemKeys.size() == oldSize) return false;

    data_->scrollOffset = 0;
    InvalidateCategorySnapshot();
    data_->activeCategoryId = CachedActiveCategoryId();
    InvalidateSlots();
    return true;
}

/**
 * @brief 计算搜索框的矩形范围（位于组件最顶部）。
 * @return 搜索框矩形，如果组件无效或区域过小则返回空矩形。
 */
RECT FileCategories::GetSearchBoxRect() const
{
    if (!data_ || !app_) return {};
    RECT body = GetBodyRect();
    InflateRect(&body, -Cu(10.0f), -Cu(8.0f));
    if (IsRectEmptyRect(body)) return {};
    InflateRect(&body, -Cu(2.0f), 0);
    if (IsRectEmptyRect(body)) return {};
    LONG top = body.top;
    LONG bottom = std::min<LONG>(body.bottom, top + Cu(26.0f));
    if (bottom <= top) return {};
    return MakeRect(body.left, top, body.right, bottom);
}

/**
 * @brief 计算分类标签页区域的矩形范围（位于搜索框下方）。
 * @param widget FileCategories 组件指针。
 * @return 标签页区域矩形，如果组件无效或区域过小则返回空矩形。
 */
static RECT FileCategoryTabsRect(FileCategories* widget)
{
    if (!widget) return {};
    RECT body = widget->GetBodyRect();
    InflateRect(&body, -widget->Cu(10.0f), -widget->Cu(8.0f));
    if (IsRectEmptyRect(body)) return {};
    RECT search = widget->GetSearchBoxRect();
    LONG top = IsRectEmptyRect(search) ? body.top : search.bottom + widget->Cu(4.0f);
    LONG bottom = std::min<LONG>(body.bottom, top + widget->Cu(30.0f));
    if (bottom <= top) return {};
    return MakeRect(body.left, top, body.right, bottom);
}

/**
 * @brief 计算文件分类内容区域的矩形范围（标签页下方）。
 * @param widget FileCategories 组件指针。
 * @return 内容区域矩形，如果组件无效或区域过小则返回空矩形。
 */
static RECT FileCategoryContentRect(FileCategories* widget)
{
    if (!widget) return {};
    RECT body = widget->GetBodyRect();
    InflateRect(&body, -widget->Cu(4.0f), -widget->Cu(8.0f));
    if (IsRectEmptyRect(body)) return {};
    if (widget->IsSearchActive())
    {
        RECT search = widget->GetSearchBoxRect();
        if (!IsRectEmptyRect(search))
            body.top = std::min<LONG>(body.bottom, search.bottom + widget->Cu(4.0f));
        return body;
    }
    RECT tabs = FileCategoryTabsRect(widget);
    if (!IsRectEmptyRect(tabs))
        body.top = std::min<LONG>(body.bottom, tabs.bottom + widget->Cu(8.0f));
    return body;
}

RECT FileCategories::GetContentViewportRect() const
{
    return FileCategoryContentRect(const_cast<FileCategories*>(this));
}

void FileCategories::ApplyMarqueeSelection(const RECT& contentRect)
{
    if (!data_ || !app_)
        return;

    for (const auto& key : data_->itemKeys)
    {
        size_t itemIndex = app_->FindItemIndexByKey(key);
        if (itemIndex != static_cast<size_t>(-1))
            app_->GetDesktopItems()[itemIndex].selected = false;
    }

    const auto& keys = GetSearchResultKeys();
    const int scroll = GetScrollOffset();
    for (size_t i = 0; i < keys.size(); ++i)
    {
        size_t itemIndex = app_->FindItemIndexByKey(keys[i]);
        if (itemIndex == static_cast<size_t>(-1))
            continue;
        RECT itemRect = FileCategoryItemRect(this, i);
        OffsetRect(&itemRect, 0, scroll);
        app_->GetDesktopItems()[itemIndex].selected =
            RectsIntersect(itemRect, contentRect);
    }
}

/**
 * @brief 计算指定索引的单个分类标签页的矩形范围。
 *        支持标签页横向滚动（通过 tabScrollOffset）。
 * @param widget FileCategories 组件指针。
 * @param index 标签页索引（从 0 开始）。
 * @return 标签页矩形，如果索引超出范围或组件无效则返回空矩形。
 */
static RECT FileCategoryTabRect(FileCategories* widget, size_t index)
{
    if (!widget) return {};
    DesktopWidget* data = widget->GetWidgetData();
    const auto& tabs = widget->CachedVisibleCategoryIds();
    if (index >= tabs.size()) return {};

    RECT tabsRect = FileCategoryTabsRect(widget);
    if (IsRectEmptyRect(tabsRect)) return {};
    const int minTabWidth = widget->Cu(64.0f);
    int tabCount = static_cast<int>(tabs.size());
    int equalWidth = std::max<int>(1, (tabsRect.right - tabsRect.left) / std::max(1, tabCount));
    int tabWidth = std::max(minTabWidth, equalWidth);
    int totalWidth = tabWidth * tabCount;
    int maxScroll = std::max(0, totalWidth - static_cast<int>(tabsRect.right - tabsRect.left));
    int scroll = std::clamp(data ? data->tabScrollOffset : 0, 0, maxScroll);
    int startX = tabsRect.left - scroll;
    RECT rect = MakeRect(
        startX + static_cast<LONG>(index * tabWidth),
        tabsRect.top,
        index + 1 == tabs.size() ? startX + totalWidth : startX + static_cast<LONG>((index + 1) * tabWidth),
        tabsRect.bottom);
    InflateRect(&rect, -widget->Cu(2.0f), -widget->Cu(2.0f));
    return rect;
}

/**
 * @brief 获取网格模式下每个单元格的高度。
 * @param widget FileCategories 组件指针。
 * @return 单元格高度（像素），默认 kMinCellHeight。
 */
static int FileCategoryCellHeight(FileCategories* widget)
{
    if (!widget || !widget->GetApp() || !widget->GetApp()->GetDesktopGrid())
        return kMinCellHeight;
    DesktopWidget* data = widget->GetWidgetData();
    for (const auto& page : widget->GetApp()->GetDesktopGrid()->GetPages())
        if (data && page.id == data->gridCell.pageId)
            return page.cellHeight;
    return kMinCellHeight;
}

/**
 * @brief 计算指定数量项目所需的内容总高度。
 * @param widget FileCategories 组件指针。
 * @param itemCount 项目数量。
 * @return 内容总高度（像素），列表模式下每项 38px，网格模式下按行列数计算。
 */
static int FileCategoryContentHeight(FileCategories* widget, size_t itemCount)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    if (data->listMode)
        return static_cast<int>(itemCount) * widget->Cu(38.0f);
    int columns = std::max(1, data->gridSpan.columns);
    int rows = static_cast<int>((itemCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
    return rows * FileCategoryCellHeight(widget);
}

/**
 * @brief 计算内容区域的最大滚动偏移量。
 * @param widget FileCategories 组件指针。
 * @return 最大滚动偏移（像素，非负值），当内容高度小于可视区域时返回 0。
 */
static int FileCategoryMaxScrollOffset(FileCategories* widget)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    const auto& keys = widget->GetSearchResultKeys();
    RECT content = FileCategoryContentRect(widget);
    int contentHeight = std::max<int>(1, content.bottom - content.top);
    return std::max(0, FileCategoryContentHeight(widget, keys.size()) -
        contentHeight + widget->Cu(kMinCellHeight / 2.0f));
}

/**
 * @brief 计算指定线性索引的项目在内容区域中的矩形位置。
 *        支持滚动偏移和列表/网格两种布局模式。
 * @param widget FileCategories 组件指针。
 * @param linearIndex 项目在分类中的线性索引（从 0 开始）。
 * @return 项目矩形，如果组件无效则返回空矩形。
 */
static RECT FileCategoryItemRect(FileCategories* widget, size_t linearIndex)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return {};
    RECT content = FileCategoryContentRect(widget);
    int scroll = std::clamp(data->scrollOffset, 0, FileCategoryMaxScrollOffset(widget));
    if (data->listMode)
    {
        const int itemHeight = widget->Cu(38.0f);
        RECT rect = MakeRect(content.left,
            content.top + static_cast<LONG>(linearIndex * itemHeight) - scroll,
            content.right,
            content.top + static_cast<LONG>((linearIndex + 1) * itemHeight) - scroll);
        InflateRect(&rect, -widget->Cu(4.0f), -widget->Cu(2.0f));
        return rect;
    }

    int columns = std::max(1, data->gridSpan.columns);
    int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
    int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
    int itemW = std::max<int>(1, (content.right - content.left) / columns);
    int cellH = FileCategoryCellHeight(widget);
    return MakeRect(
        content.left + col * itemW,
        content.top + row * cellH - scroll,
        col + 1 == columns ? content.right : content.left + (col + 1) * itemW,
        content.top + (row + 1) * cellH - scroll);
}

/**
 * @brief 计算列表/网格模式切换按钮的矩形范围（位于拖拽手柄右侧）。
 * @param widget FileCategories 组件指针。
 * @return 切换按钮矩形。
 */
static RECT FileCategoryToggleRect(FileCategories* widget)
{
    if (!widget) return {};
    RECT handle = widget->GetMoveHandleRect();
    const int btnSize = widget->Cu(14.0f);
    const int gap = widget->Cu(4.0f);
    const int resizeReserve = widget->Cu(20.0f);
    return MakeRect(handle.right - resizeReserve - gap - btnSize,
        handle.top + widget->Cu(5.0f),
        handle.right - resizeReserve - gap, handle.bottom - widget->Cu(3.0f));
}

/**
 * @brief 根据可见槽位索引计算在 data->itemKeys 中的实际插入位置。
 * @param app DesktopApp 实例指针。
 * @param data 桌面组件数据。
 * @param visibleIndex 在激活分类可见列表中的索引。
 * @return data->itemKeys 中对应的插入位置索引。
 */
static size_t InsertIndexForVisibleSlot(FileCategories* widget, size_t visibleIndex)
{
    DesktopApp* app = widget ? widget->GetApp() : nullptr;
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!app || !data) return 0;
    const auto& visibleKeys = widget->GetSearchResultKeys();
    if (visibleIndex < visibleKeys.size())
    {
        std::wstring anchor = ToUpperInvariant(visibleKeys[visibleIndex]);
        for (size_t i = 0; i < data->itemKeys.size(); ++i)
            if (ToUpperInvariant(data->itemKeys[i]) == anchor) return i;
    }
    return data->itemKeys.size();
}

/**
 * @brief 构建当前激活分类下所有可见项目的 Slot 列表。
 *        每个 Slot 对应一个可视区域内的桌面项目。
 * @return 唯一指针向量，包含所有可见项目对应的 Slot。
 */
std::vector<std::unique_ptr<Slot>> FileCategories::BuildSlots()
{
    slotItemCache_.clear();

    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || !app_) return slots;

    const auto& keys = GetSearchResultKeys();
    if (keys.empty()) return slots;

    RECT content = FileCategoryContentRect(this);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    const int scroll = std::clamp(data_->scrollOffset, 0, FileCategoryMaxScrollOffset(this));
    size_t firstIndex = 0;
    size_t lastIndex = keys.size();

    if (data_->listMode)
    {
        const int itemHeight = Cu(38.0f);
        const int firstRow = std::max(0, scroll / itemHeight - 1);
        const int lastRow = (scroll + visibleHeight + itemHeight - 1) / itemHeight + 1;
        firstIndex = static_cast<size_t>(firstRow);
        lastIndex = std::min(keys.size(), static_cast<size_t>(std::max(firstRow, lastRow)));
    }
    else
    {
        const int columns = std::max(1, data_->gridSpan.columns);
        const int cellHeight = std::max(1, FileCategoryCellHeight(this));
        const int firstRow = std::max(0, scroll / cellHeight - 1);
        const int lastRow = (scroll + visibleHeight + cellHeight - 1) / cellHeight + 1;
        firstIndex = std::min(keys.size(), static_cast<size_t>(firstRow * columns));
        lastIndex = std::min(keys.size(), static_cast<size_t>(std::max(firstRow, lastRow) * columns));
    }

    slots.reserve(lastIndex - firstIndex);
    for (size_t idx = firstIndex; idx < lastIndex; ++idx)
    {
        RECT cell = FileCategoryItemRect(this, idx);
        if (IsRectEmptyRect(cell)) continue;
        auto slot = std::make_unique<Slot>(this, cell, idx);
        Item* item = GetSlotItem(idx);
        if (item) item->SetBounds(cell);
        slot->SetItem(item);
        slots.push_back(std::move(slot));
    }
    return slots;
}

/**
 * @brief 获取指定索引的 Slot 对应的 Item（桌面图标）。
 *        结果缓存在 slotItemCache_ 中。
 * @param idx 在激活分类可见列表中的索引。
 * @return 指向 Item 的指针，如果索引无效或组件无效则返回 nullptr。
 */
Item* FileCategories::GetSlotItem(size_t idx) const
{
    if (!data_ || !app_) return nullptr;
    const auto& keys = GetSearchResultKeys();
    if (idx >= keys.size()) return nullptr;
    size_t itemIdx = app_->FindItemIndexByKey(keys[idx]);
    if (itemIdx == static_cast<size_t>(-1)) return nullptr;
    auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[itemIdx],
        const_cast<FileCategories*>(this), app_);
    Item* result = icon.get();
    slotItemCache_.push_back(std::move(icon));
    return result;
}

/**
 * @brief 获取指定索引的成员 Item，用于拖拽操作。
 *        结果缓存在 dragSourceCache_ 中。
 * @param idx 在激活分类可见列表中的索引。
 * @return 指向 Item 的指针，如果索引无效或组件无效则返回 nullptr。
 */
Item* FileCategories::GetMemberItem(size_t idx) const
{
    if (!data_ || !app_) return nullptr;
    const auto& keys = GetSearchResultKeys();
    if (idx >= keys.size()) return nullptr;
    size_t itemIdx = app_->FindItemIndexByKey(keys[idx]);
    if (itemIdx == static_cast<size_t>(-1)) return nullptr;
    auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[itemIdx],
        const_cast<FileCategories*>(this), app_);
    Item* result = icon.get();
    dragSourceCache_.push_back(std::move(icon));
    return result;
}

/**
 * @brief 获取在激活分类中处于选中状态的项目索引列表。
 * @return 选中项目的索引向量，如果无选中项或组件无效则返回空向量。
 */
std::vector<size_t> FileCategories::GetSelectedMemberIndices() const
{
    std::vector<size_t> result;
    if (!data_ || !app_) return result;
    const auto& keys = GetSearchResultKeys();
    for (size_t i = 0; i < keys.size(); ++i)
    {
        size_t itemIdx = app_->FindItemIndexByKey(keys[i]);
        if (itemIdx == static_cast<size_t>(-1)) continue;
        if (app_->GetDesktopItems()[itemIdx].selected)
            result.push_back(i);
    }
    return result;
}

/**
 * @brief 重新排序成员项目：将选中的项目移动到指定可见索引之前。
 * @param indices 未使用的参数，保留以匹配接口。实际使用选中状态来确定移动项目。
 * @param insertBefore 目标插入位置的可见索引。
 */
void FileCategories::ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore)
{
    if (!data_ || !app_) return;
    (void)indices;

    const auto& activeKeys = GetSearchResultKeys();
    std::vector<std::wstring> selectedKeys;
    for (const auto& key : activeKeys)
    {
        size_t itemIdx = app_->FindItemIndexByKey(key);
        if (itemIdx != static_cast<size_t>(-1) && app_->GetDesktopItems()[itemIdx].selected)
            selectedKeys.push_back(ToUpperInvariant(key));
    }
    if (selectedKeys.empty()) return;

    std::unordered_set<std::wstring> selectedSet(selectedKeys.begin(), selectedKeys.end());
    size_t insertAt = data_->itemKeys.size();
    if (insertBefore < activeKeys.size())
    {
        size_t anchorIdx = insertBefore;
        while (anchorIdx < activeKeys.size() &&
            selectedSet.contains(ToUpperInvariant(activeKeys[anchorIdx])))
            ++anchorIdx;
        if (anchorIdx < activeKeys.size())
            insertAt = InsertIndexForVisibleSlot(this, anchorIdx);
    }

    size_t before = 0;
    for (size_t i = 0; i < std::min(insertAt, data_->itemKeys.size()); ++i)
        if (selectedSet.contains(ToUpperInvariant(data_->itemKeys[i]))) ++before;
    insertAt -= std::min(insertAt, before);

    data_->itemKeys.erase(
        std::remove_if(data_->itemKeys.begin(), data_->itemKeys.end(),
            [&](const std::wstring& key) { return selectedSet.contains(ToUpperInvariant(key)); }),
        data_->itemKeys.end());

    insertAt = std::min(insertAt, data_->itemKeys.size());
    for (const auto& key : selectedKeys)
        data_->itemKeys.insert(data_->itemKeys.begin() + static_cast<std::ptrdiff_t>(insertAt++), key);
    InvalidateCategorySnapshot();
    InvalidateSlots();
}

/**
 * @brief 获取当前激活分类下的项目总数。
 * @return Slot 数量，如果组件数据无效则返回 0。
 */
size_t FileCategories::GetSlotCount() const
{
    if (!data_) return 0;
    return GetSearchResultKeys().size();
}

/**
 * @brief 获取每个项目的高度。
 * @return 列表模式下返回 38px，网格模式下返回单元格高度。
 */
int FileCategories::GetItemHeight() const
{
    if (!data_) return Cu(38.0f);
    return data_->listMode
        ? Cu(38.0f)
        : FileCategoryCellHeight(const_cast<FileCategories*>(this));
}

/**
 * @brief 获取每个项目的宽度。
 * @return 列表模式下返回 WidgetContainer 的默认宽度，网格模式下按列均分。
 */
int FileCategories::GetItemWidth() const
{
    if (!data_ || data_->listMode) return WidgetContainer::GetItemWidth();
    RECT content = FileCategoryContentRect(const_cast<FileCategories*>(this));
    return std::max<int>(1, (content.right - content.left) / std::max(1, data_->gridSpan.columns));
}

/**
 * @brief 获取内容区域的最大允许滚动偏移量。
 * @return 最大滚动偏移值（像素）。
 */
int FileCategories::GetMaxScrollOffset() const
{
    return FileCategoryMaxScrollOffset(const_cast<FileCategories*>(this));
}

/**
 * @brief 获取当前激活分类下所有项目的总内容高度。
 * @return 总高度（像素）。
 */
int FileCategories::GetTotalContentHeight() const
{
    const auto& keys = GetSearchResultKeys();
    return FileCategoryContentHeight(const_cast<FileCategories*>(this), keys.size());
}

/**
 * @brief 获取内容区域的可视高度（减去标签页区域后的剩余高度）。
 * @return 可视内容高度（像素，至少为 1）。
 */
int FileCategories::GetVisibleContentHeight() const
{
    RECT content = FileCategoryContentRect(const_cast<FileCategories*>(this));
    return std::max(1, (int)(content.bottom - content.top));
}

/**
 * @brief 处理项目拖放事件，执行拖放管道操作。
 * @param sourceItems 被拖拽的源项目列表。
 * @param origin 来源容器。
 * @param targetSlot 目标槽位（可为空）。
 * @param region 拖放命中区域。
 * @param mods 按键修饰符。
 */
void FileCategories::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || !data_) return;
    DragSourceList sourceList = app_->BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = app_->BuildDropPreviewList(sourceList, this, targetSlot, region, mods,
        app_->dragSession_.CurrentPoint());
    app_->ExecuteDropPipeline(sourceList, preview);
}

/**
 * @brief 获取拖放操作的插入索引。
 * @param targetSlot 目标槽位，为空时追加到末尾。
 * @param region 命中区域，SortAfter 表示插入到目标之后。
 * @return 在 data->itemKeys 中的插入位置。
 */
size_t FileCategories::GetDropInsertIndex(Slot* targetSlot, HitRegion region) const
{
    size_t visibleInsert = targetSlot ? targetSlot->GetIndex() : GetSlotCount();
    if (targetSlot && region == HitRegion::SortAfter)
        ++visibleInsert;
    return InsertIndexForVisibleSlot(const_cast<FileCategories*>(this), visibleInsert);
}

/**
 * @brief 判断指定的 layoutKey 是否允许添加到本组件中。
 * @param key 要检查的 layoutKey。
 * @return true 如果该项目存在且可收录；否则返回 false。
 */
bool FileCategories::AllowsDesktopKey(const std::wstring& key) const
{
    size_t itemIdx = app_ ? app_->FindItemIndexByKey(key) : static_cast<size_t>(-1);
    return itemIdx != static_cast<size_t>(-1) &&
        IsCollectable(app_, app_->GetDesktopItems()[itemIdx]);
}

/**
 * @brief 绘制组件的内容区域，包括分类标签页、分隔线和项目列表/网格。
 * @param context D2D 设备上下文。
 * @param body 组件的 body 矩形。
 */
void FileCategories::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    (void)body;

    const auto& categoryIds = CachedVisibleCategoryIds();
    IDWriteTextFormat* normalFormat = GetCuTextFormat(13.0f, false, true);
    IDWriteTextFormat* boldFormat = GetCuTextFormat(13.0f, true, true);
    bool searching = !searchText_.empty();

    RECT searchRect = GetSearchBoxRect();
    if (!IsRectEmptyRect(searchRect))
    {
        bool searchHovered = PtInRect(&searchRect, app_->lastMousePoint_) != FALSE;
        app_->DrawD2DRoundedRectangle(context, searchRect, static_cast<float>(Cu(7.0f)),
            searchFocused_
                ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f)
                : (searchHovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f)
                                 : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f)),
            searchFocused_
                ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.70f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));

        IDWriteTextFormat* searchFormat = GetCuTextFormat(13.0f, false, false);
        RECT searchTextRect = MakeRect(searchRect.left + Cu(8.0f), searchRect.top,
            searchRect.right - Cu(8.0f), searchRect.bottom);

        if (searching || searchFocused_)
        {
            std::wstring displayText = searchText_;
            if (searchFocused_)
            {
                static auto frameCount = 0;
                frameCount++;
                if ((frameCount / 30) % 2 == 0)
                    displayText += L"|";
            }
            app_->DrawD2DText(context, displayText, searchTextRect,
                searchFormat ? searchFormat :
                    (app_->fileCategoryTabTextFormat_
                        ? app_->fileCategoryTabTextFormat_.Get()
                        : app_->listItemTextFormat_.Get()),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f));
        }
        else
        {
            app_->DrawD2DText(context, L"  搜索文件...", searchTextRect,
                searchFormat ? searchFormat :
                    (app_->fileCategoryTabTextFormat_
                        ? app_->fileCategoryTabTextFormat_.Get()
                        : app_->listItemTextFormat_.Get()),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f));
        }
    }

    if (searching)
    {
        const auto& keys = GetSearchResultKeys();
        RECT content = GetContentViewportRect();
        if (IsRectEmptyRect(content)) return;

        const auto& slots = GetSlots();
        const auto& items = app_->GetDesktopItems();

        if (keys.empty())
        {
            RECT empty = GetBodyRect();
            InflateRect(&empty, -Cu(12.0f), -Cu(12.0f));
            app_->DrawD2DText(context, L"没有匹配结果", empty,
                normalFormat ? normalFormat :
                    (app_->navTabTextFormat_ ? app_->navTabTextFormat_.Get() : app_->listItemTextFormat_.Get()),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));
            return;
        }

        context->PushAxisAlignedClip(app_->ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t i = 0; i < slots.size(); ++i)
        {
            size_t idx = slots[i]->GetIndex();
            if (idx >= keys.size()) continue;
            RECT itemRect = slots[i]->GetBounds();
            if (itemRect.bottom <= content.top || itemRect.top >= content.bottom) continue;

            size_t itemIdx = app_->FindItemIndexByKey(keys[idx]);
            if (itemIdx == static_cast<size_t>(-1)) continue;
            const DesktopItem& di = items[itemIdx];

            if (!data_->listMode)
            {
                RECT bodyRect = GetBodyRect();
                bool hovered = !di.selected && PtInRect(&itemRect, app_->lastMousePoint_) && PtInRect(&bodyRect, app_->lastMousePoint_);
                DesktopIcon icon(const_cast<DesktopItem*>(&di), this, app_);
                icon.Draw(context, itemRect, di.selected ? 2 : (hovered ? 1 : 0));
                continue;
            }
            DrawListItem(context, itemRect, di.iconBitmap, di.sysIconIndex,
                di.name, di.selected);
        }
        context->PopAxisAlignedClip();
        return;
    }

    if (categoryIds.empty())
    {
        RECT empty = GetBodyRect();
        InflateRect(&empty, -Cu(12.0f), -Cu(12.0f));
        app_->DrawD2DText(context, L"暂无散文件", empty,
            normalFormat ? normalFormat :
                (app_->navTabTextFormat_ ? app_->navTabTextFormat_.Get() : app_->listItemTextFormat_.Get()),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));
        return;
    }

    std::wstring activeCategory = CachedActiveCategoryId();
    RECT tabsRect = FileCategoryTabsRect(this);
    if (!IsRectEmptyRect(tabsRect))
    {
        context->PushAxisAlignedClip(app_->ToD2DRect(tabsRect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t i = 0; i < categoryIds.size(); ++i)
        {
            RECT tab = FileCategoryTabRect(this, i);
            if (IsRectEmptyRect(tab)) continue;

            bool active = categoryIds[i] == activeCategory;
            bool hovered = PtInRect(&tab, app_->lastMousePoint_) != FALSE;
            app_->DrawD2DRoundedRectangle(context, tab, static_cast<float>(Cu(7.0f)),
                active ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f)
                       : (hovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f)
                                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f)),
                active ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.78f)
                       : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));

            std::wstring label = FileCategoryLabel(categoryIds[i]) + L" " +
                std::to_wstring(CachedCategoryKeys(categoryIds[i]).size());
            RECT textRect = MakeRect(tab.left + Cu(4.0f), tab.top,
                tab.right - Cu(4.0f), tab.bottom);

            app_->DrawD2DText(context, label, textRect,
                boldFormat ? boldFormat :
                    (app_->fileCategoryTabTextFormat_
                        ? app_->fileCategoryTabTextFormat_.Get()
                        : app_->listItemTextFormat_.Get()),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, active ? 0.98f : 0.78f));
        }
        context->PopAxisAlignedClip();

        RECT line = MakeRect(tabsRect.left, tabsRect.bottom + Cu(2.0f),
            tabsRect.right, tabsRect.bottom + Cu(3.0f));
        app_->DrawD2DFilledRectangle(context, line,
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
    }

    const auto& keys = CachedCategoryKeys(activeCategory);
    RECT content = FileCategoryContentRect(this);
    if (IsRectEmptyRect(content)) return;

    const auto& slots = GetSlots();
    const auto& items = app_->GetDesktopItems();

    context->PushAxisAlignedClip(app_->ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < slots.size(); ++i)
    {
        size_t idx = slots[i]->GetIndex();
        if (idx >= keys.size()) continue;
        RECT itemRect = slots[i]->GetBounds();
        if (itemRect.bottom <= content.top || itemRect.top >= content.bottom) continue;

        size_t itemIdx = app_->FindItemIndexByKey(keys[idx]);
        if (itemIdx == static_cast<size_t>(-1)) continue;
        const DesktopItem& di = items[itemIdx];

        if (!data_->listMode)
        {
            RECT bodyRect = GetBodyRect();
            bool hovered = !di.selected && PtInRect(&itemRect, app_->lastMousePoint_) && PtInRect(&bodyRect, app_->lastMousePoint_);
            DesktopIcon icon(const_cast<DesktopItem*>(&di), this, app_);
            icon.Draw(context, itemRect, di.selected ? 2 : (hovered ? 1 : 0));
            continue;
        }

        DrawListItem(context, itemRect, di.iconBitmap, di.sysIconIndex,
            di.name, di.selected);
    }
    context->PopAxisAlignedClip();
}

/**
 * @brief 绘制组件右上角的列表/网格模式切换按钮。
 * @param context D2D 设备上下文。
 * @param handleRect 拖拽手柄矩形。
 * @param hovered 手柄是否悬停。
 */
void FileCategories::DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered)
{
    if (!data_ || !app_) return;
    RECT toggle = FileCategoryToggleRect(this);
    bool hot = PtInRect(&toggle, app_->lastMousePoint_) != FALSE;
    app_->DrawD2DRoundedRectangle(context, toggle, static_cast<float>(Cu(4.0f)),
        hot ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
    IDWriteTextFormat* faFormat = GetCuFaTextFormat(14.0f);
    app_->DrawD2DText(context, data_->listMode ? L"" : L"", toggle,
        faFormat ? faFormat :
            (app_->faTextFormat_ ? app_->faTextFormat_.Get() : app_->listItemTextFormat_.Get()),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
    (void)handleRect;
    (void)hovered;
}

/**
 * @brief 对指定点进行命中测试，判断该点落在组件的哪个区域。
 * @param pt 测试点的屏幕坐标。
 * @return 命中结果，可能为 None、MoveHandle、CategoryTab、ListToggleBtn 等。
 */
WidgetHit FileCategories::HitTestWidget(POINT pt) const
{
    WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base == WidgetHit::None || !data_) return base;

    if (searchText_.empty() && !CategoryIdAtPoint(pt).empty())
        return WidgetHit::CategoryTab;

    RECT searchRect = GetSearchBoxRect();
    if (!IsRectEmptyRect(searchRect) && PtInRect(&searchRect, pt))
        return WidgetHit::SearchBox;

    if (base == WidgetHit::MoveHandle)
    {
        RECT toggle = FileCategoryToggleRect(const_cast<FileCategories*>(this));
        if (PtInRect(&toggle, pt))
            return WidgetHit::ListToggleBtn;
    }
    return base;
}

/**
 * @brief 获取指定坐标点所在的分类标签 ID。
 * @param pt 测试点的屏幕坐标。
 * @return 分类 ID 字符串，如果未落在任何标签上则返回空字符串。
 */
std::wstring FileCategories::CategoryIdAtPoint(POINT pt) const
{
    const auto& categories = CachedVisibleCategoryIds();
    for (size_t i = 0; i < categories.size(); ++i)
    {
        RECT tab = FileCategoryTabRect(const_cast<FileCategories*>(this), i);
        if (PtInRect(&tab, pt))
            return categories[i];
    }
    return L"";
}

/**
 * @brief 判断指定点是否落在标签页矩形区域内。
 * @param pt 测试点的屏幕坐标。
 * @return true 如果点在标签页区域内；否则返回 false。
 */
bool FileCategories::IsPointInTabsRect(POINT pt) const
{
    RECT tabs = FileCategoryTabsRect(const_cast<FileCategories*>(this));
    return !IsRectEmptyRect(tabs) && PtInRect(&tabs, pt) != FALSE;
}

/**
 * @brief 尝试在标签页区域进行横向滚动。
 * @param pt 鼠标坐标，用于判断是否在标签页区域内。
 * @param delta 鼠标滚轮滚动量。
 * @return true 如果成功处理滚动（点在标签页内且可滚动）；否则返回 false。
 */
bool FileCategories::TryScrollTabs(POINT pt, int delta)
{
    if (!data_ || !app_) return false;
    RECT tabs = FileCategoryTabsRect(this);
    if (IsRectEmptyRect(tabs) || !PtInRect(&tabs, pt)) return false;

    const auto& categories = CachedVisibleCategoryIds();
    int tabCount = static_cast<int>(categories.size());
    if (tabCount <= 0) return false;

    const int minTabWidth = Cu(64.0f);
    int tabsWidth = tabs.right - tabs.left;
    int equalWidth = std::max(1, tabsWidth / tabCount);
    int tabWidth = std::max(minTabWidth, equalWidth);
    int totalWidth = tabWidth * tabCount;
    int maxScroll = std::max(0, totalWidth - tabsWidth);
    if (maxScroll <= 0) return false;

    data_->tabScrollOffset = std::clamp(data_->tabScrollOffset - delta / 2, 0, maxScroll);
    return true;
}

/**
 * @brief 获取当前处于选中状态的 Item 列表，用于拖拽操作。
 *        结果缓存在 dragSourceCache_ 中。
 * @return 选中项的 Item 指针向量。
 */
std::vector<Item*> FileCategories::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!data_ || !app_) return result;

    const auto& keys = GetSearchResultKeys();
    for (size_t i = 0; i < keys.size(); ++i)
    {
        size_t idx = app_->FindItemIndexByKey(keys[i]);
        if (idx == static_cast<size_t>(-1)) continue;
        DesktopItem* di = &app_->GetDesktopItems()[idx];
        if (!di->selected) continue;

        auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[idx], const_cast<FileCategories*>(this), app_);
        icon->SetBounds(FileCategoryItemRect(const_cast<FileCategories*>(this), i));
        result.push_back(icon.get());
        dragSourceCache_.push_back(std::move(icon));
    }
    return result;
}
