#include "widget.h"
#include "slot.h"
#include "types.h"
#include "app.h"
#include "drop_model.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>
#include <unordered_set>

static std::vector<std::wstring> FileCategoryOrder()
{
    return {
        L"all", L"folders", L"videos", L"images", L"documents",
        L"archives", L"audio", L"programs", L"others",
    };
}

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

static std::wstring DesktopItemExtensionUpper(const DesktopItem& item)
{
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
        return ToUpperInvariant(PathFindExtensionW(path));
    return ToUpperInvariant(PathFindExtensionW(item.name.c_str()));
}

static bool IsShortcutItem(const DesktopItem& item)
{
    return DesktopItemExtensionUpper(item) == L".LNK";
}

static bool IsFilesystemFolder(const DesktopItem& item)
{
    wchar_t path[MAX_PATH]{};
    if (!SHGetPathFromIDListW(item.absolutePidl.get(), path)) return false;
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

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

static size_t FindItemIndexByKeyPublic(DesktopApp* app, const std::wstring& key)
{
    if (!app) return static_cast<size_t>(-1);
    std::wstring normalized = ToUpperInvariant(key);
    const auto& items = app->GetDesktopItems();
    for (size_t i = 0; i < items.size(); ++i)
        if (ToUpperInvariant(items[i].layoutKey) == normalized)
            return i;
    return static_cast<size_t>(-1);
}

static std::vector<std::wstring> CategoryKeys(DesktopApp* app, const DesktopWidget* data,
    const std::wstring& categoryId)
{
    std::vector<std::wstring> keys;
    std::unordered_set<std::wstring> seen;
    if (!app || !data) return keys;

    auto appendKey = [&](const std::wstring& rawKey) {
        size_t itemIdx = FindItemIndexByKeyPublic(app, rawKey);
        if (itemIdx == static_cast<size_t>(-1)) return;
        const DesktopItem& item = app->GetDesktopItems()[itemIdx];
        if (!IsCollectable(app, item)) return;
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (seen.insert(key).second) keys.push_back(key);
    };

    if (!categoryId.empty() && categoryId != L"all")
    {
        for (const auto& rawKey : data->itemKeys)
        {
            size_t itemIdx = FindItemIndexByKeyPublic(app, rawKey);
            if (itemIdx == static_cast<size_t>(-1)) continue;
            if (FileCategoryIdForItem(app->GetDesktopItems()[itemIdx]) == categoryId)
                appendKey(rawKey);
        }
        return keys;
    }

    if (categoryId == L"all")
    {
        for (const auto& rawKey : data->itemKeys)
            appendKey(rawKey);
        return keys;
    }

    for (const auto& id : FileCategoryOrder())
    {
        if (id == L"all") continue;
        for (const auto& rawKey : data->itemKeys)
        {
            size_t itemIdx = FindItemIndexByKeyPublic(app, rawKey);
            if (itemIdx == static_cast<size_t>(-1)) continue;
            if (FileCategoryIdForItem(app->GetDesktopItems()[itemIdx]) == id)
                appendKey(rawKey);
        }
    }
    return keys;
}

static std::vector<std::wstring> VisibleCategoryIds(DesktopApp* app, const DesktopWidget* data)
{
    std::vector<std::wstring> ids;
    for (const auto& id : FileCategoryOrder())
    {
        if (!CategoryKeys(app, data, id).empty())
            ids.push_back(id);
    }
    return ids;
}

static std::wstring ActiveCategoryId(DesktopApp* app, const DesktopWidget* data);

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
        if (data_->activeCategoryId.empty())
            data_->activeCategoryId = ActiveCategoryId(app_, data_);
        InvalidateSlots();
    }
    return changed;
}

static std::wstring ActiveCategoryId(DesktopApp* app, const DesktopWidget* data)
{
    if (!data) return L"";
    auto visible = VisibleCategoryIds(app, data);
    if (!data->activeCategoryId.empty() &&
        std::find(visible.begin(), visible.end(), data->activeCategoryId) != visible.end())
        return data->activeCategoryId;
    return visible.empty() ? L"" : visible.front();
}

static RECT FileCategoryTabsRect(FileCategories* widget)
{
    if (!widget) return {};
    RECT body = widget->GetBodyRect();
    InflateRect(&body, -10, -8);
    if (IsRectEmptyRect(body)) return {};
    return MakeRect(body.left, body.top, body.right,
        std::min<LONG>(body.bottom, body.top + 30));
}

static RECT FileCategoryContentRect(FileCategories* widget)
{
    if (!widget) return {};
    RECT body = widget->GetBodyRect();
    InflateRect(&body, -4, -8);
    if (IsRectEmptyRect(body)) return {};
    RECT tabs = FileCategoryTabsRect(widget);
    body.top = std::min<LONG>(body.bottom, tabs.bottom + 8);
    return body;
}

static RECT FileCategoryTabRect(FileCategories* widget, size_t index)
{
    if (!widget) return {};
    DesktopWidget* data = widget->GetWidgetData();
    auto tabs = VisibleCategoryIds(widget->GetApp(), data);
    if (index >= tabs.size()) return {};

    RECT tabsRect = FileCategoryTabsRect(widget);
    if (IsRectEmptyRect(tabsRect)) return {};
    constexpr int minTabWidth = 64;
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
    InflateRect(&rect, -2, -2);
    return rect;
}

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

static int FileCategoryContentHeight(FileCategories* widget, size_t itemCount)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    if (data->listMode)
        return static_cast<int>(itemCount) * 38;
    int columns = std::max(1, data->gridSpan.columns);
    int rows = static_cast<int>((itemCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
    return rows * FileCategoryCellHeight(widget);
}

static int FileCategoryMaxScrollOffset(FileCategories* widget)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    auto keys = CategoryKeys(widget->GetApp(), data, ActiveCategoryId(widget->GetApp(), data));
    RECT content = FileCategoryContentRect(widget);
    int contentHeight = std::max<int>(1, content.bottom - content.top);
    return std::max(0, FileCategoryContentHeight(widget, keys.size()) - contentHeight + kMinCellHeight / 2);
}

static RECT FileCategoryItemRect(FileCategories* widget, size_t linearIndex)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return {};
    RECT content = FileCategoryContentRect(widget);
    int scroll = std::clamp(data->scrollOffset, 0, FileCategoryMaxScrollOffset(widget));
    if (data->listMode)
    {
        RECT rect = MakeRect(content.left,
            content.top + static_cast<LONG>(linearIndex * 38) - scroll,
            content.right,
            content.top + static_cast<LONG>((linearIndex + 1) * 38) - scroll);
        InflateRect(&rect, -4, -2);
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

static RECT FileCategoryToggleRect(FileCategories* widget)
{
    if (!widget) return {};
    RECT handle = widget->GetMoveHandleRect();
    constexpr int btnSize = 14;
    constexpr int gap = 4;
    constexpr int resizeReserve = 20;
    return MakeRect(handle.right - resizeReserve - gap - btnSize, handle.top + 5,
        handle.right - resizeReserve - gap, handle.bottom - 3);
}

static size_t InsertIndexForVisibleSlot(DesktopApp* app, DesktopWidget* data, size_t visibleIndex)
{
    if (!app || !data) return 0;
    auto visibleKeys = CategoryKeys(app, data, ActiveCategoryId(app, data));
    if (visibleIndex < visibleKeys.size())
    {
        std::wstring anchor = ToUpperInvariant(visibleKeys[visibleIndex]);
        for (size_t i = 0; i < data->itemKeys.size(); ++i)
            if (ToUpperInvariant(data->itemKeys[i]) == anchor) return i;
    }
    return data->itemKeys.size();
}

std::vector<std::unique_ptr<Slot>> FileCategories::BuildSlots()
{
    slotItemCache_.clear();

    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || !app_) return slots;

    auto keys = CategoryKeys(app_, data_, ActiveCategoryId(app_, data_));
    for (size_t idx = 0; idx < keys.size(); ++idx)
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

Item* FileCategories::GetSlotItem(size_t idx) const
{
    if (!data_ || !app_) return nullptr;
    auto keys = CategoryKeys(app_, data_, ActiveCategoryId(app_, data_));
    if (idx >= keys.size()) return nullptr;
    size_t itemIdx = app_->FindItemIndexByKey(keys[idx]);
    if (itemIdx == static_cast<size_t>(-1)) return nullptr;
    auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[itemIdx],
        const_cast<FileCategories*>(this), app_);
    Item* result = icon.get();
    slotItemCache_.push_back(std::move(icon));
    return result;
}

Item* FileCategories::GetMemberItem(size_t idx) const
{
    if (!data_ || !app_) return nullptr;
    auto keys = CategoryKeys(app_, data_, ActiveCategoryId(app_, data_));
    if (idx >= keys.size()) return nullptr;
    size_t itemIdx = app_->FindItemIndexByKey(keys[idx]);
    if (itemIdx == static_cast<size_t>(-1)) return nullptr;
    auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[itemIdx],
        const_cast<FileCategories*>(this), app_);
    Item* result = icon.get();
    dragSourceCache_.push_back(std::move(icon));
    return result;
}

std::vector<size_t> FileCategories::GetSelectedMemberIndices() const
{
    std::vector<size_t> result;
    if (!data_ || !app_) return result;
    auto keys = CategoryKeys(app_, data_, ActiveCategoryId(app_, data_));
    for (size_t i = 0; i < keys.size(); ++i)
    {
        size_t itemIdx = app_->FindItemIndexByKey(keys[i]);
        if (itemIdx == static_cast<size_t>(-1)) continue;
        if (app_->GetDesktopItems()[itemIdx].selected)
            result.push_back(i);
    }
    return result;
}

void FileCategories::ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore)
{
    if (!data_ || !app_) return;
    (void)indices;

    auto activeKeys = CategoryKeys(app_, data_, ActiveCategoryId(app_, data_));
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
            insertAt = InsertIndexForVisibleSlot(app_, data_, anchorIdx);
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
    InvalidateSlots();
}

size_t FileCategories::GetSlotCount() const
{
    if (!data_) return 0;
    return CategoryKeys(app_, data_, ActiveCategoryId(app_, data_)).size();
}

int FileCategories::GetItemHeight() const
{
    if (!data_) return 38;
    return data_->listMode ? 38 : FileCategoryCellHeight(const_cast<FileCategories*>(this));
}

int FileCategories::GetItemWidth() const
{
    if (!data_ || data_->listMode) return WidgetContainer::GetItemWidth();
    RECT content = FileCategoryContentRect(const_cast<FileCategories*>(this));
    return std::max<int>(1, (content.right - content.left) / std::max(1, data_->gridSpan.columns));
}

int FileCategories::GetMaxScrollOffset() const
{
    return FileCategoryMaxScrollOffset(const_cast<FileCategories*>(this));
}

int FileCategories::GetTotalContentHeight() const
{
    auto keys = CategoryKeys(app_, data_, ActiveCategoryId(app_, data_));
    return FileCategoryContentHeight(const_cast<FileCategories*>(this), keys.size());
}

int FileCategories::GetVisibleContentHeight() const
{
    RECT content = FileCategoryContentRect(const_cast<FileCategories*>(this));
    return std::max(1, (int)(content.bottom - content.top));
}

void FileCategories::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || !data_) return;
    DragSourceList sourceList = app_->BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = app_->BuildDropPreviewList(sourceList, this, targetSlot, region, mods,
        app_->dragSession_.CurrentPoint());
    app_->ExecuteDropPipeline(sourceList, preview);
}

size_t FileCategories::GetDropInsertIndex(Slot* targetSlot, HitRegion region) const
{
    size_t visibleInsert = targetSlot ? targetSlot->GetIndex() : GetSlotCount();
    if (targetSlot && region == HitRegion::SortAfter)
        ++visibleInsert;
    return InsertIndexForVisibleSlot(app_, data_, visibleInsert);
}

bool FileCategories::AllowsDesktopKey(const std::wstring& key) const
{
    size_t itemIdx = FindItemIndexByKeyPublic(app_, key);
    return itemIdx != static_cast<size_t>(-1) &&
        IsCollectable(app_, app_->GetDesktopItems()[itemIdx]);
}

void FileCategories::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    (void)body;

    std::vector<std::wstring> categoryIds = VisibleCategoryIds(app_, data_);
    if (categoryIds.empty())
    {
        RECT empty = GetBodyRect();
        InflateRect(&empty, -12, -12);
        ComPtr<IDWriteTextFormat> centered;
        if (app_->dwriteFactory_)
        {
            app_->dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &centered);
            if (centered)
            {
                centered->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                centered->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                centered->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        app_->DrawD2DText(context, L"暂无散文件", empty,
            centered ? centered.Get() : app_->listItemTextFormat_.Get(),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));
        return;
    }

    std::wstring activeCategory = ActiveCategoryId(app_, data_);
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
            app_->DrawD2DRoundedRectangle(context, tab, 7.0f,
                active ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f)
                       : (hovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f)
                                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f)),
                active ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.78f)
                       : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));

            std::wstring label = FileCategoryLabel(categoryIds[i]) + L" " +
                std::to_wstring(CategoryKeys(app_, data_, categoryIds[i]).size());
            RECT textRect = MakeRect(tab.left + 4, tab.top, tab.right - 4, tab.bottom);

            ComPtr<IDWriteTextFormat> tabFmt;
            if (app_->dwriteFactory_)
            {
                app_->dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr,
                    DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &tabFmt);
                if (tabFmt)
                {
                    tabFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    tabFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    tabFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                }
            }
            app_->DrawD2DText(context, label, textRect,
                tabFmt ? tabFmt.Get() : app_->listItemTextFormat_.Get(),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, active ? 0.98f : 0.78f));
        }
        context->PopAxisAlignedClip();

        RECT line = MakeRect(tabsRect.left, tabsRect.bottom + 2, tabsRect.right, tabsRect.bottom + 3);
        app_->DrawD2DFilledRectangle(context, line,
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
    }

    auto keys = CategoryKeys(app_, data_, activeCategory);
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

        DrawListItem(context, itemRect, di.iconBitmap, di.name, di.selected);
    }
    context->PopAxisAlignedClip();
}

void FileCategories::DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered)
{
    if (!data_ || !app_) return;
    RECT toggle = FileCategoryToggleRect(this);
    bool hot = PtInRect(&toggle, app_->lastMousePoint_) != FALSE;
    app_->DrawD2DRoundedRectangle(context, toggle, 4.0f,
        hot ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
    app_->DrawD2DText(context, data_->listMode ? L"" : L"", toggle,
        app_->faTextFormat_ ? app_->faTextFormat_.Get() : app_->listItemTextFormat_.Get(),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
    (void)handleRect;
    (void)hovered;
}

WidgetHit FileCategories::HitTestWidget(POINT pt) const
{
    WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base == WidgetHit::None || !data_) return base;

    if (!CategoryIdAtPoint(pt).empty())
        return WidgetHit::CategoryTab;

    if (base == WidgetHit::MoveHandle)
    {
        RECT toggle = FileCategoryToggleRect(const_cast<FileCategories*>(this));
        if (PtInRect(&toggle, pt))
            return WidgetHit::ListToggleBtn;
    }
    return base;
}

std::wstring FileCategories::CategoryIdAtPoint(POINT pt) const
{
    auto categories = VisibleCategoryIds(app_, data_);
    for (size_t i = 0; i < categories.size(); ++i)
    {
        RECT tab = FileCategoryTabRect(const_cast<FileCategories*>(this), i);
        if (PtInRect(&tab, pt))
            return categories[i];
    }
    return L"";
}

bool FileCategories::IsPointInTabsRect(POINT pt) const
{
    RECT tabs = FileCategoryTabsRect(const_cast<FileCategories*>(this));
    return !IsRectEmptyRect(tabs) && PtInRect(&tabs, pt) != FALSE;
}

bool FileCategories::TryScrollTabs(POINT pt, int delta)
{
    if (!data_ || !app_) return false;
    RECT tabs = FileCategoryTabsRect(this);
    if (IsRectEmptyRect(tabs) || !PtInRect(&tabs, pt)) return false;

    auto categories = VisibleCategoryIds(app_, data_);
    int tabCount = static_cast<int>(categories.size());
    if (tabCount <= 0) return false;

    constexpr int minTabWidth = 64;
    int tabsWidth = tabs.right - tabs.left;
    int equalWidth = std::max(1, tabsWidth / tabCount);
    int tabWidth = std::max(minTabWidth, equalWidth);
    int totalWidth = tabWidth * tabCount;
    int maxScroll = std::max(0, totalWidth - tabsWidth);
    if (maxScroll <= 0) return false;

    data_->tabScrollOffset = std::clamp(data_->tabScrollOffset - delta / 2, 0, maxScroll);
    return true;
}

std::vector<Item*> FileCategories::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!data_ || !app_) return result;

    for (const auto& slot : const_cast<FileCategories*>(this)->GetSlots())
    {
        if (!slot) continue;
        Item* slotItem = slot->GetItem();
        auto* slotIcon = dynamic_cast<DesktopIcon*>(slotItem);
        DesktopItem* di = slotIcon ? slotIcon->GetDesktopItem() : nullptr;
        if (!di || !di->selected) continue;

        size_t idx = app_->FindItemIndexByKey(di->layoutKey);
        if (idx == static_cast<size_t>(-1)) continue;
        auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[idx], const_cast<FileCategories*>(this), app_);
        icon->SetBounds(slot->GetBounds());
        result.push_back(icon.get());
        dragSourceCache_.push_back(std::move(icon));
    }
    return result;
}
