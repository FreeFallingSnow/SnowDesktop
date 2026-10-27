#include "dock.h"
#include "app/app.h"

namespace
{
std::wstring RemovalKey(const DockEntry& entry)
{
    return std::to_wstring(static_cast<int>(entry.type)) + L":" +
        (entry.type == DockEntryType::DesktopItem ? ToUpperInvariant(entry.reference) : entry.reference);
}
}

std::vector<std::wstring> DockContainer::EntryRemovalKeys() const
{
    std::vector<std::wstring> keys;
    if (entries_)
    {
        keys.reserve(entries_->size());
        for (const auto& entry : *entries_) keys.push_back(RemovalKey(entry));
    }
    return keys;
}

void DockContainer::PrepareEntryRemoval(size_t entryIndex)
{
    if (!app_ || !entries_ || entryIndex >= entries_->size()) return;
    if (app_->dragSession_.HasContext() || app_->dragDropController_.IsTransportActive() ||
        app_->widgetAction_ == DesktopApp::WidgetAction::Move)
    {
        entryRemovals_->Cancel(RemovalKey((*entries_)[entryIndex]));
        return;
    }
    if (app_->IsRecycleBinDockEntry((*entries_)[entryIndex]) ||
        !snowdesktop::animation::RuntimeAnimationsEnabled() || !app_->d2dContext_) return;
    // Capture final artwork before its item/widget/resources are destroyed.
    // The snapshot shares the renderer's device, never a model or Item pointer.
    ComPtr<ID2D1Device> device;
    app_->d2dContext_->GetDevice(&device);
    ComPtr<ID2D1DeviceContext> capture;
    if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &capture))) return;
    const auto edge = static_cast<UINT32>(std::max(1, static_cast<int>(std::ceil(
        static_cast<float>(ItemIconSize()) * GetMaximumMagnificationScale()))));
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(capture->CreateBitmap(D2D1::SizeU(edge, edge), nullptr, 0, properties, &bitmap))) return;
    capture->SetTarget(bitmap.Get());
    capture->BeginDraw();
    capture->Clear(D2D1::ColorF(0, 0, 0, 0));
    const int pixels = static_cast<int>(edge);
    app_->DrawDockEntry(capture.Get(), (*entries_)[entryIndex], {0, 0, pixels, pixels}, 0, pixels);
    const HRESULT drawn = capture->EndDraw();
    capture->SetTarget(nullptr);
    if (FAILED(drawn)) return;
    entryRemovals_->Prepare(EntryRemovalKeys(), entryIndex,
        app_->IsFolderDockEntry((*entries_)[entryIndex])
            ? snowdesktop::dock_entry_removal::Group::Folder
            : snowdesktop::dock_entry_removal::Group::Fixed, std::move(bitmap));
}

void DockContainer::CancelEntryRemoval(const DockEntry& entry)
{
    const auto key = RemovalKey(entry);
    // A failed overlapping Shell request cannot cancel an already committed
    // exit from another successful request.
    const auto& entries = entryRemovals_->Entries();
    if (std::any_of(entries.begin(), entries.end(), [&](const auto& removal) {
            return removal.key == key && !removal.removed;
        })) entryRemovals_->Cancel(key);
}

void DockContainer::ReconcileEntryRemovals() const
{
    if (entryRemovals_->Entries().empty()) return;
    entryRemovals_->Reconcile(EntryRemovalKeys(),
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(),
        app_ && app_->generalSettings_.dockEnabled && snowdesktop::animation::RuntimeAnimationsEnabled(),
        snowdesktop::animation::RuntimeDurationScale());
}

bool DockContainer::IsEntryRemovalAnimating() const
{
    ReconcileEntryRemovals();
    return entryRemovals_->IsAnimating();
}

bool DockContainer::AdvanceEntryRemovalAnimation(double nowMilliseconds)
{
    ReconcileEntryRemovals();
    return entryRemovals_->Advance(nowMilliseconds,
        app_ && app_->generalSettings_.dockEnabled && snowdesktop::animation::RuntimeAnimationsEnabled());
}

float DockContainer::EntryRemovalUnits(bool folder) const
{
    return entryRemovals_->Units(folder ? snowdesktop::dock_entry_removal::Group::Folder
        : snowdesktop::dock_entry_removal::Group::Fixed);
}

void DockContainer::DrawEntryRemovals(ID2D1DeviceContext* context, bool fixedFolders) const
{
    if (!context || !app_) return;
    const auto& removals = entryRemovals_->Entries();
    for (const auto& [index, cell] : entryRemovalCells_)
    {
        if (index >= removals.size()) continue;
        const auto& removal = removals[index];
        if ((IsEdgeAttached() && removal.group == snowdesktop::dock_entry_removal::Group::Folder) != fixedFolders)
            continue;
        entryRemovals_->Present(index, snowdesktop::UiAnimationScheduler::MonotonicMilliseconds());
        const float amount = removal.presence.Amount();
        if (amount <= 0.0f || !removal.snapshot) continue;
        const int size = std::max(1, static_cast<int>(std::lround(static_cast<float>(ItemIconSize()) * amount)));
        const float left = (cell.left + cell.right - size) * 0.5f;
        const float top = (cell.top + cell.bottom - size) * 0.5f;
        context->DrawBitmap(removal.snapshot.Get(), D2D1::RectF(left, top, left + size, top + size),
            amount, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
}
