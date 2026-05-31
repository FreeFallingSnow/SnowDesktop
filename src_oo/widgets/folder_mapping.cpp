#include "widget.h"
#include "slot.h"
#include "item.h"
#include "types.h"
#include "app.h"
#include "drop_model.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>

static RECT FolderMappingContentRect(FolderMapping* widget)
{
    if (!widget) return {};
    RECT body = widget->GetBodyRect();
    InflateRect(&body, -4, -8);
    return body;
}

static int FolderMappingCellHeight(FolderMapping* widget)
{
    if (!widget || !widget->GetApp() || !widget->GetApp()->GetDesktopGrid())
        return kMinCellHeight;
    DesktopWidget* data = widget->GetWidgetData();
    for (const auto& page : widget->GetApp()->GetDesktopGrid()->GetPages())
        if (data && page.id == data->gridCell.pageId)
            return page.cellHeight;
    return kMinCellHeight;
}

static int FolderMappingContentHeight(FolderMapping* widget, size_t itemCount)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    if (data->listMode)
        return static_cast<int>(itemCount) * 38;
    int columns = std::max(1, data->gridSpan.columns);
    int rows = static_cast<int>((itemCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
    return rows * FolderMappingCellHeight(widget);
}

static int FolderMappingMaxScrollOffset(FolderMapping* widget)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    RECT content = FolderMappingContentRect(widget);
    int contentHeight = std::max<int>(1, content.bottom - content.top);
    return std::max(0, FolderMappingContentHeight(widget, data->folderEntries.size()) - contentHeight + kMinCellHeight / 2);
}

static RECT FolderMappingItemRect(FolderMapping* widget, size_t linearIndex)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return {};
    RECT content = FolderMappingContentRect(widget);
    int scroll = std::clamp(data->scrollOffset, 0, FolderMappingMaxScrollOffset(widget));
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
    int cellH = FolderMappingCellHeight(widget);
    return MakeRect(
        content.left + col * itemW,
        content.top + row * cellH - scroll,
        col + 1 == columns ? content.right : content.left + (col + 1) * itemW,
        content.top + (row + 1) * cellH - scroll);
}

static void EraseKeyFromWidget(DesktopWidget& widget, const std::wstring& key)
{
    std::wstring normalized = ToUpperInvariant(key);
    widget.itemKeys.erase(
        std::remove_if(widget.itemKeys.begin(), widget.itemKeys.end(),
            [&](const std::wstring& existing) { return ToUpperInvariant(existing) == normalized; }),
        widget.itemKeys.end());
}

Item* FolderMapping::GetSlotItem(size_t idx) const
{
    if (!data_ || idx >= data_->folderEntries.size()) return nullptr;
    auto icon = std::make_unique<FolderEntryIcon>(&data_->folderEntries[idx],
        const_cast<FolderMapping*>(this), app_);
    Item* result = icon.get();
    slotItemCache_.push_back(std::move(icon));
    return result;
}

std::vector<std::unique_ptr<Slot>> FolderMapping::BuildSlots()
{
    slotItemCache_.clear();

    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || !app_) return slots;

    size_t total = IncludeTrailingEmptySlot()
        ? data_->folderEntries.size() + 1
        : data_->folderEntries.size();
    for (size_t idx = 0; idx < total; ++idx)
    {
        RECT cell = FolderMappingItemRect(this, idx);
        if (IsRectEmptyRect(cell)) continue;
        auto slot = std::make_unique<Slot>(this, cell, idx);
        Item* item = GetSlotItem(idx);
        if (item) item->SetBounds(cell);
        slot->SetItem(item);
        slots.push_back(std::move(slot));
    }
    return slots;
}

Item* FolderMapping::GetMemberItem(size_t idx) const
{
    if (!data_ || idx >= data_->folderEntries.size()) return nullptr;
    auto icon = std::make_unique<FolderEntryIcon>(&data_->folderEntries[idx],
        const_cast<FolderMapping*>(this), app_);
    Item* result = icon.get();
    dragSourceCache_.push_back(std::move(icon));
    return result;
}

std::vector<size_t> FolderMapping::GetSelectedMemberIndices() const
{
    std::vector<size_t> result;
    if (!data_) return result;
    for (size_t i = 0; i < data_->folderEntries.size(); ++i)
        if (data_->folderEntries[i].selected) result.push_back(i);
    return result;
}

void FolderMapping::ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore)
{
    if (!data_) return;
    std::vector<FolderEntry> moving;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        if (*it >= data_->folderEntries.size()) continue;
        moving.push_back(std::move(data_->folderEntries[*it]));
        data_->folderEntries.erase(data_->folderEntries.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    size_t adjusted = insertBefore;
    for (auto idx : indices)
        if (idx < insertBefore) --adjusted;
    if (adjusted > data_->folderEntries.size()) adjusted = data_->folderEntries.size();
    for (auto it = moving.rbegin(); it != moving.rend(); ++it)
        data_->folderEntries.insert(data_->folderEntries.begin() + static_cast<std::ptrdiff_t>(adjusted++), std::move(*it));
    InvalidateSlots();
}

size_t FolderMapping::GetSlotCount() const
{
    return data_ ? data_->folderEntries.size() : 0;
}

int FolderMapping::GetItemHeight() const
{
    return (data_ && data_->listMode) ? 38 : FolderMappingCellHeight(const_cast<FolderMapping*>(this));
}

int FolderMapping::GetItemWidth() const
{
    if (!data_ || data_->listMode) return WidgetContainer::GetItemWidth();
    RECT content = FolderMappingContentRect(const_cast<FolderMapping*>(this));
    return std::max<int>(1, (content.right - content.left) / std::max(1, data_->gridSpan.columns));
}

int FolderMapping::GetMaxScrollOffset() const
{
    return FolderMappingMaxScrollOffset(const_cast<FolderMapping*>(this));
}

int FolderMapping::GetTotalContentHeight() const
{
    return FolderMappingContentHeight(const_cast<FolderMapping*>(this), data_ ? data_->folderEntries.size() : 0);
}

int FolderMapping::GetVisibleContentHeight() const
{
    RECT content = FolderMappingContentRect(const_cast<FolderMapping*>(this));
    return std::max(1, (int)(content.bottom - content.top));
}

void FolderMapping::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || !data_) return;

    if (origin == this)
    {
        // Same-source reorder via ReorderMembers
        auto indices = GetSelectedMemberIndices();
        size_t insertAt = targetSlot ? targetSlot->GetIndex() : data_->folderEntries.size();
        ReorderMembers(indices, insertAt);
        return;
    }

    // Cross-source: copy/move/link physical files to mapped folder
    if (data_->sourceFolderPath.empty()) return;
    DropPayload payload = DropPayload::From(sourceItems);
    if (payload.filePaths.empty()) return;
    DropAction action = DropActionFromMods(mods, payload.hasExternalFiles ? DropAction::Copy : DropAction::Move);

    std::wstring destFolder = data_->sourceFolderPath;
    if (!destFolder.empty() && destFolder.back() != L'\\') destFolder += L'\\';

    if (action == DropAction::Link)
    {
        for (const auto& path : payload.filePaths)
        {
            std::wstring name = PathFindFileNameW(path.c_str());
            std::wstring stem = name;
            if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".lnk") == 0)
                stem = stem.substr(0, stem.size() - 4);
            std::wstring linkPath;
            for (int i = 1; i < 1000; ++i)
            {
                linkPath = destFolder + stem + (i == 1 ? L".lnk" : L" (" + std::to_wstring(i) + L").lnk");
                if (GetFileAttributesW(linkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                    break;
            }
            ComPtr<IShellLinkW> shellLink;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
            {
                shellLink->SetPath(path.c_str());
                shellLink->SetWorkingDirectory(destFolder.c_str());
                ComPtr<IPersistFile> persistFile;
                if (SUCCEEDED(shellLink.As(&persistFile)))
                    persistFile->Save(linkPath.c_str(), TRUE);
            }
        }
    }
    else
    {
        std::wstring from;
        for (const auto& path : payload.filePaths)
        {
            from += path;
            from += L'\0';
        }
        from += L'\0';

        std::wstring to = destFolder + L'\0';
        SHFILEOPSTRUCTW op{};
        op.hwnd = app_->hwnd_;
        op.wFunc = (action == DropAction::Copy) ? FO_COPY : FO_MOVE;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_RENAMEONCOLLISION;
        SHFileOperationW(&op);
    }

    if (action == DropAction::Move)
    {
        for (const auto& key : payload.desktopKeys)
        {
            for (auto& widget : app_->GetWidgets())
                EraseKeyFromWidget(widget, key);
        }
    }

    if (auto* sourceWidget = dynamic_cast<WidgetContainer*>(origin))
    {
        DesktopWidget* sourceData = sourceWidget->GetWidgetData();
        if (action == DropAction::Move && sourceData && sourceData->type == DesktopWidgetType::FolderMapping)
        {
            auto& widgets = app_->GetWidgets();
            for (size_t i = 0; i < widgets.size(); ++i)
                if (&widgets[i] == sourceData) { app_->RefreshFolderMappingWidget(i); break; }
        }
    }

    size_t widgetIndex = static_cast<size_t>(-1);
    auto& widgets = app_->GetWidgets();
    for (size_t i = 0; i < widgets.size(); ++i)
        if (&widgets[i] == data_) { widgetIndex = i; break; }
    if (widgetIndex != static_cast<size_t>(-1))
        app_->RefreshFolderMappingWidget(widgetIndex);
    InvalidateSlots();
    (void)targetSlot; (void)region;
}

void FolderMapping::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    (void)body;

    if (data_->folderEntries.empty())
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
        app_->DrawD2DText(context, L"空文件夹", empty,
            centered ? centered.Get() : app_->listItemTextFormat_.Get(),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));
        return;
    }

    auto& slots = GetSlots();
    RECT content = FolderMappingContentRect(this);
    bool listMode = data_->listMode;

    context->PushAxisAlignedClip(app_->ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < slots.size() && i < data_->folderEntries.size(); ++i)
    {
        const FolderEntry& entry = data_->folderEntries[i];
        RECT cell = slots[i]->GetBounds();
        if (cell.bottom <= content.top || cell.top >= content.bottom) continue;

        if (!listMode)
        {
            FolderEntryIcon icon(const_cast<FolderEntry*>(&entry), this, app_);
            icon.Draw(context, cell, entry.selected ? 2 : 0);
            continue;
        }

        DrawListItem(context, cell, entry.iconBitmap, entry.name, entry.selected);
    }
    context->PopAxisAlignedClip();
}

void FolderMapping::DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered)
{
    if (!data_ || !app_) return;

    constexpr int btnSize = 14;
    constexpr int gap = 4;
    constexpr int gapBetween = 7;
    constexpr int resizeReserve = 20;
    RECT toggleBtn = {
        handleRect.right - resizeReserve - gap - btnSize - gapBetween - btnSize,
        handleRect.top + 5,
        handleRect.right - resizeReserve - gap - btnSize - gapBetween,
        handleRect.bottom - 3
    };
    RECT openBtn = {
        handleRect.right - resizeReserve - gap - btnSize,
        handleRect.top + 5,
        handleRect.right - resizeReserve - gap,
        handleRect.bottom - 3
    };

    auto drawFaButton = [&](RECT rect, const std::wstring& glyph) {
        bool hot = PtInRect(&rect, app_->lastMousePoint_) != FALSE;
        app_->DrawD2DRoundedRectangle(context, rect, 4.0f,
            hot ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
        app_->DrawD2DText(context, glyph, rect,
            app_->faTextFormat_ ? app_->faTextFormat_.Get() : app_->listItemTextFormat_.Get(),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
    };

    drawFaButton(toggleBtn, data_->listMode ? L"" : L"");
    drawFaButton(openBtn, L"");
    (void)hovered;
}

WidgetHit FolderMapping::HitTestWidget(POINT pt) const
{
    WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base != WidgetHit::MoveHandle || !data_) return base;

    RECT handle = GetMoveHandleRect();
    constexpr int btnSize = 14;
    constexpr int gap = 4;
    constexpr int gapBetween = 7;
    constexpr int resizeReserve = 20;
    RECT toggleBtn = {
        handle.right - resizeReserve - gap - btnSize - gapBetween - btnSize,
        handle.top + 5,
        handle.right - resizeReserve - gap - btnSize - gapBetween,
        handle.bottom - 3
    };
    RECT openBtn = {
        handle.right - resizeReserve - gap - btnSize,
        handle.top + 5,
        handle.right - resizeReserve - gap,
        handle.bottom - 3
    };
    if (PtInRect(&toggleBtn, pt)) return WidgetHit::ListToggleBtn;
    if (PtInRect(&openBtn, pt)) return WidgetHit::OpenFolderBtn;
    return base;
}

std::vector<Item*> FolderMapping::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!data_) return result;

    for (const auto& slot : const_cast<FolderMapping*>(this)->GetSlots())
    {
        if (!slot) continue;
        Item* slotItem = slot->GetItem();
        auto* slotIcon = dynamic_cast<FolderEntryIcon*>(slotItem);
        FolderEntry* entry = slotIcon ? slotIcon->GetFolderEntry() : nullptr;
        if (!entry || !entry->selected) continue;

        auto icon = std::make_unique<FolderEntryIcon>(entry, const_cast<FolderMapping*>(this), app_);
        icon->SetBounds(slot->GetBounds());
        result.push_back(icon.get());
        dragSourceCache_.push_back(std::move(icon));
    }
    return result;
}
