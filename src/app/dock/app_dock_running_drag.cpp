#include "app/app.h"
#include "dock_explorer_pin.h"
#include "dock_platform_helpers.h"
#include "dock_running_app_pin_rules.h"
#include "desktop/desktop_source.h"
#include "layout/logical_slot_picker_rules.h"

namespace drag = snowdesktop::dock_running_drag;
namespace pin = snowdesktop::dock_running_app_pin;

struct DesktopApp::DockRunningDragState
{
    DockRunningAppInfo running;
    std::unique_ptr<DockRunningItem> visual;
    std::shared_ptr<DockRunningPinTarget> application;
    std::shared_ptr<DockRunningPinTarget> folder;
    bool explorer = false;
    bool existingDesktopItem = false;
    bool resolved = false;
    std::uint64_t serial = 0;
    drag::Target target;
};

std::shared_ptr<DesktopApp::DockRunningPinTarget> DesktopApp::ReadDockRunningPinTarget(
    const DockRunningAppInfo& running, bool folder, const std::wstring& explorerName)
{
    auto target = std::make_shared<DockRunningPinTarget>();
    if (snowdesktop::shortcut_application_rules::IsExplorerExecutable(running.executablePath) &&
        snowdesktop::dock_explorer_pin::IsFolderWindow(running.window))
    {
        std::wstring path;
        if (folder)
        {
            const auto current = snowdesktop::dock_explorer_pin::ReadCurrentFolder(running.window);
            if (!current) return {};
            path = target->folderPath = current->path;
            target->entry.name = current->name;
        }
        else
        {
            path = snowdesktop::dock_explorer_pin::ExecutablePath();
            target->entry.name = explorerName;
            target->identity.executablePath = NormalizeDockExecutablePath(path);
        }
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (path.empty() || FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
            return {};
        target->entry.absolutePidl.reset(pidl);
        target->entry.parsingName = path;
        return target;
    }
    if (folder) return {};
    HIMAGELIST imageList = nullptr;
    auto applications = BuildQuickNavigationAppIndex(nullptr, imageList, 32);
    std::vector<pin::ApplicationIdentity> identities;
    identities.reserve(applications.size());
    for (const auto& entry : applications)
    {
        auto identity = pin::ReadApplicationIdentity(entry.absolutePidl.get());
        identity.executablePath = NormalizeDockExecutablePath(identity.executablePath);
        identity.appUserModelId = ToUpperInvariant(identity.appUserModelId);
        if (identity.appUserModelId.empty())
        {
            const auto launch = snowdesktop::logical_slot_picker_rules::NormalizeApplicationLaunchTarget(entry.parsingName);
            constexpr std::wstring_view marker = L"APPSFOLDER\\";
            const auto at = ToUpperInvariant(launch).find(marker);
            if (at != std::wstring::npos)
                identity.appUserModelId = ToUpperInvariant(launch.substr(at + marker.size()));
        }
        identities.push_back(std::move(identity));
    }
    const auto match = pin::FindApplication(identities, running.executablePath,
        running.appUserModelId, running.ancestorExecutablePaths);
    if (!match) return {};
    target->entry = std::move(applications[*match]);
    target->identity.executablePath = std::move(identities[*match].executablePath);
    target->identity.appUserModelId = std::move(identities[*match].appUserModelId);
    return target;
}

bool DesktopApp::BeginDockRunningDrag(POINT point)
{
    const auto found = std::find_if(dockUnpinnedRunningApps_.begin(), dockUnpinnedRunningApps_.end(),
        [&](const auto& app) { return app.presence.Interactive() && app.identityKey == dockPressedRunningAppKey_; });
    if (found == dockUnpinnedRunningApps_.end() || !mouseDownHit_) return false;
    auto state = std::make_shared<DockRunningDragState>();
    state->running = *found;
    // The snapshot only identifies the application. The live model remains
    // the sole owner of its bitmap and tracked-window presentation.
    state->running.iconBitmap = nullptr;
    state->serial = ++dockRunningDragSerial_;
    state->explorer = snowdesktop::shortcut_application_rules::IsExplorerExecutable(found->executablePath) &&
        snowdesktop::dock_explorer_pin::IsFolderWindow(found->window);
    state->existingDesktopItem = !state->explorer && FindDesktopItemForDockRunningApp(*found).has_value();
    state->visual = std::make_unique<DockRunningItem>(this, nullptr,
        static_cast<size_t>(found - dockUnpinnedRunningApps_.begin()));
    const RECT bounds = mouseDownHit_->GetBounds();
    state->visual->SetBounds(bounds);
    dockRunningDrag_ = state;
    PrepareDockBackdropForDragTransition();
    // This is a Dock application gesture, not a filesystem/OLE payload.
    // Its owned wrapper survives slot and monitor-host rebuilds.
    dragSession_.Begin(nullptr, {state->visual.get()}, {}, mouseDownPoint_, point);
    dragSession_.SetVisualItemBounds({bounds}, 0);
    dragGroupOriginX_ = bounds.left;
    dragGroupOriginY_ = bounds.top;
    mouseDownHit_ = nullptr;
    ClearDockPressedState();
    InvalidateDockContainers();
    UpdateFloatingDockWindowBounds(false);
    DismissDockWindowPreviewUntilLeave();
    if (hwnd_) SetTimer(hwnd_, kNativeDragHoverRecoveryTimerId, kNativeDragHoverRecoveryIntervalMs, nullptr);
    const std::wstring explorerName = _LW("app.dock.explorer_name");
    const bool submitted = shellVisualWork_.Submit(L"dock-running-drag:" + std::to_wstring(state->serial),
        [state, explorerName] {
            auto application = state->existingDesktopItem ? std::shared_ptr<DockRunningPinTarget>{} :
                ReadDockRunningPinTarget(state->running, false, explorerName);
            auto folder = state->explorer ? ReadDockRunningPinTarget(state->running, true, explorerName) : nullptr;
            return std::make_pair(std::move(application), std::move(folder));
        }, [this, state](const auto& targets) {
            state->application = targets.first;
            state->folder = targets.second;
            state->resolved = true;
            // Presentation-only delivery is allowed while holding the pointer.
            if (dockRunningDrag_ == state && dragSession_.IsActive())
                UpdateDockRunningDrag(dragSession_.CurrentPoint());
        }, hwnd_, kBackgroundShellReadyMessage);
    if (!submitted) state->resolved = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void DesktopApp::ResetDockRunningDrag()
{
    const bool hadDrag = dockRunningDrag_ != nullptr;
    dockRunningDrag_.reset();
    if (hadDrag)
    {
        InvalidateDockContainers();
        UpdateFloatingDockWindowBounds(false);
    }
}

bool DesktopApp::IsDockRunningDragExplorer() const
{
    return dockRunningDrag_ && dockRunningDrag_->explorer;
}

void DesktopApp::UpdateDockRunningDrag(POINT point)
{
    if (!dockRunningDrag_ || !dragSession_.IsActive()) return;
    auto& state = *dockRunningDrag_;
    dragSession_.UpdatePoint(point);
    state.target = {};
    DockContainer* dock = nullptr;
    const auto live = std::find_if(dockUnpinnedRunningApps_.begin(), dockUnpinnedRunningApps_.end(),
        [&](const auto& app) { return app.presence.Interactive() && app.identityKey == state.running.identityKey; });
    if (live != dockUnpinnedRunningApps_.end() && !IsExternalDropWindowAt(point) &&
        !IsPointOccludedByOpenPopup(point))
    {
        dock = GetDockContainerAtPoint(point);
        if (dock) state.target = dock->GetRunningDragTarget(point);
        if ((state.target.area == drag::Area::Files &&
                (!state.explorer || (state.resolved && !state.folder))) ||
            (state.target.area == drag::Area::Fixed && state.resolved &&
                !state.existingDesktopItem && !state.application))
            state.target = {};
    }
    dragSession_.UpdateTarget(state.target.area == drag::Area::None ? nullptr : dock,
        nullptr, state.target.area == drag::Area::None ? HitRegion::None : HitRegion::Empty);
    ShowDragHintWindow(point, GetDockRunningDragHint());
    InvalidateFloatingDockWindow(true);
}

std::wstring DesktopApp::GetDockRunningDragHint() const
{
    if (!dockRunningDrag_) return {};
    const auto& state = *dockRunningDrag_;
    if (state.target.area == drag::Area::Running) return _LW("core.drag.release_adjust_order");
    if (state.target.area == drag::Area::Files)
        return _LW(state.resolved ? "app.dock.pin_current_folder_to_files" : "widget.folder_mapping.loading");
    if (state.target.area == drag::Area::Fixed)
    {
        if (!state.resolved && !state.existingDesktopItem) return _LW("quickNav.apps.loading");
        if (state.explorer) return _LW("app.dock.pin_explorer_to_fixed");
        if (!state.existingDesktopItem) return _LW("app.dock.pin_move_to_dock");
        return _LW((GetAsyncKeyState(VK_CONTROL) & 0x8000)
            ? "core.dock.release_dock_map_full" : "core.dock.release_move_dock_ctrl");
    }
    return {};
}

void DesktopApp::DrawDockRunningDragPreview(ID2D1DeviceContext* context, DockContainer* dock)
{
    if (!context || !dock || !dockRunningDrag_ || dragSession_.TargetContainer() != dock) return;
    const auto& target = dockRunningDrag_->target;
    if (target.area == drag::Area::None) return;
    const RECT bounds = dock->GetBounds();
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(context->CreateSolidColorBrush(D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.95f), &brush))) return;
    const float axis = static_cast<float>(target.axis);
    const bool vertical = dock->GetInsertionStyle() == BarStyle::HBar;
    const D2D1_RECT_F line = vertical
        ? D2D1::RectF(static_cast<float>(bounds.left + 10), axis - 2,
            static_cast<float>(bounds.right - 10), axis + 2)
        : D2D1::RectF(axis - 2, static_cast<float>(bounds.top + 10),
            axis + 2, static_cast<float>(bounds.bottom - 10));
    context->FillRoundedRectangle(D2D1::RoundedRect(line, 2, 2), brush.Get());
}

void DesktopApp::CommitDockRunningDrag(POINT point, int mods)
{
    UpdateDockRunningDrag(point);
    const auto state = dockRunningDrag_;
    if (!state || state->target.area == drag::Area::None) return;
    const auto target = state->target;
    drag::PinPosition position{target.area, target.index};
    if (target.area != drag::Area::Running)
    {
        const size_t begin = target.area == drag::Area::Files ? DockMainEntryCount() : 0;
        const size_t end = target.area == drag::Area::Files
            ? DockMainEntryCount() + DockFolderEntryCount() : DockMainEntryCount();
        if (target.index < end) position.before = dockEntries_[target.index];
        if (target.index > begin) position.after = dockEntries_[target.index - 1];
    }
    // Release the gesture before any path mutates vectors or rebuilds hosts.
    EndDragSession();
    if (target.area == drag::Area::Running)
    {
        if (drag::Reorder(dockUnpinnedRunningApps_, state->running.identityKey, target.index))
        {
            InvalidateDockContainers();
            InvalidateDragStaticScene();
            InvalidateDockRects();
        }
        return;
    }
    QueueDockRunningPin(state, position, mods, 0);
}

void DesktopApp::QueueDockRunningPin(std::shared_ptr<DockRunningDragState> state,
    drag::PinPosition position, int mods, unsigned retry)
{
    const auto readSources = [this] {
        std::vector<snowdesktop::dock_explorer_pin::FolderPinSource> sources;
        for (const auto& entry : dockEntries_)
        {
            if (entry.type == DockEntryType::FolderMapping)
            {
                const auto index = FindWidgetIndexById(entry.reference);
                if (index < widgets_.size()) sources.push_back({widgets_[index].sourceFolderPath, entry.reference});
            }
            else if (entry.type == DockEntryType::DesktopItem && !IsRecycleBinDockEntry(entry))
            {
                const auto index = FindItemIndexByKey(entry.reference);
                sources.push_back({index < items_.size() ? items_[index].parsingName : entry.reference,
                    entry.reference + (index < items_.size() ? snowdesktop::shell_icon_request::Stamp(items_[index]) : L"")});
            }
        }
        return sources;
    };
    const auto sources = readSources();
    // Snapshot every entry used by duplicate detection. Recheck on delivery;
    // another queued drop may have inserted a matching folder in the meantime.
    const std::wstring explorerName = _LW("app.dock.explorer_name");
    const auto ready = position.area == drag::Area::Files ? state->folder : state->application;
    const bool submitted = shellVisualWork_.Submit(L"dock-running-pin:" +
        std::to_wstring(state->serial) + L":" + std::to_wstring(retry),
        [state, position, ready, explorerName, sources] {
            auto resolved = ready ? ready : ReadDockRunningPinTarget(
                state->running, position.area == drag::Area::Files, explorerName);
            const bool duplicate = resolved && !resolved->folderPath.empty() &&
                snowdesktop::dock_explorer_pin::AlreadyPinnedFolder(resolved->folderPath, sources);
            return std::make_pair(std::move(resolved), duplicate);
        }, [this, state, position, mods, retry, sources, readSources](const auto& result) {
            if (exitRequested_ || !generalSettings_.dockEnabled) return;
            const auto live = std::find_if(dockUnpinnedRunningApps_.begin(), dockUnpinnedRunningApps_.end(),
                [&](const auto& app) { return app.presence.Interactive() && app.identityKey == state->running.identityKey; });
            if (live == dockUnpinnedRunningApps_.end()) return;
            if (position.area == drag::Area::Files && sources != readSources())
            {
                if (retry < 3) QueueDockRunningPin(state, position, mods, retry + 1);
                else MessageBeep(MB_ICONWARNING);
                return;
            }
            const size_t index = position.Resolve(dockEntries_, DockMainEntryCount(),
                DockMainEntryCount() + DockFolderEntryCount());
            if (position.area == drag::Area::Fixed && !state->explorer)
                if (const auto desktop = FindDesktopItemForDockRunningApp(*live); desktop && *desktop < items_.size())
                {
                    DockContainer* dock = GetDockContainer();
                    if (!dock) return;
                    const std::wstring key = items_[*desktop].layoutKey;
                    DesktopIcon source(&items_[*desktop], nullptr, this);
                    CommitDockDrop({&source}, nullptr, dock, index, mods & MK_CONTROL);
                    if (std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const auto& entry) {
                        return entry.type == DockEntryType::DesktopItem && entry.reference == ToUpperInvariant(key);
                    })) AdoptDockRunningPresentation(state->running.identityKey, *desktop);
                    SaveLayoutSlots();
                    ApplyPageMapping();
                    LayoutItems();
                    InvalidateDockRects();
                    return;
                }
            if (!result.first || result.second || !PinDockRunningTarget(state->running.identityKey, *result.first, index))
                MessageBeep(MB_ICONWARNING);
        }, hwnd_, kBackgroundShellReadyMessage);
    if (!submitted) MessageBeep(MB_ICONWARNING);
}

void DesktopApp::AdoptDockRunningPresentation(const std::wstring& runningKey, size_t itemIndex)
{
    auto& item = items_[itemIndex];
    const auto key = DockItemWindowKey(item);
    pin::AdoptRunningPresentation(item, dockRunningWindows_[key], dockUnpinnedRunningApps_,
        runningKey, [this](HBITMAP bitmap) {
            EraseD2DIconCacheForBitmap(bitmap);
            DeleteObject(bitmap);
        });
    InvalidateDockContainers();
    InvalidateDragStaticScene();
}

bool DesktopApp::PinDockRunningTarget(const std::wstring& runningKey,
    const DockRunningPinTarget& target, size_t insertIndex)
{
    if (!hwnd_ || !IsWindow(hwnd_) || exitRequested_ || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive()) return false;
    DockContainer* dock = GetDockContainer();
    if (!dock) return false;
    const bool pinned = pin::CreateAndPin([dock] { return dock->HasCapacity(1); },
        [&] { return pin::CreateShortcut(snowdesktop::desktop_source::Directory(),
            SanitizeShortcutFileStem(target.entry.name), target.entry.absolutePidl.get()); },
        [&](const std::wstring& path) {
            auto item = pin::ReadShortcutItem(path, target.entry.name, target.folderPath.empty());
            if (!item) return false;
            item->layoutKey = ToUpperInvariant(path);
            item->gridCell = {kDockPageId, 0, 0};
            if (!AddMaterializedItemsToDock({path}, insertIndex, false)) return false;
            items_.push_back(std::move(*item));
            RefreshDesktopItemIndexCache();
            const size_t itemIndex = items_.size() - 1;
            const auto key = DockItemWindowKey(items_[itemIndex]);
            const auto stamp = snowdesktop::shell_icon_request::Stamp(items_[itemIndex]);
            if (!target.folderPath.empty())
            {
                const auto folderKey = L"I:" + ToUpperInvariant(
                    snowdesktop::dock_refresh_cache::SourceKey(items_[itemIndex].layoutKey, path));
                const auto cached = dockFolderTargetCache_.Read(folderKey, stamp);
                dockFolderTargetCache_.Publish(folderKey, cached.ticket,
                    snowdesktop::item_location::FolderTarget{target.folderPath,
                        snowdesktop::item_location::FolderTargetKind::Shortcut, true});
                NormalizeDockRecycleBinPosition();
                // The new path was not in items_ during initial insertion, so
                // its folder classification could not yet be known. Restore
                // the requested file-area boundary after publishing it.
                const auto added = std::find_if(dockEntries_.begin(), dockEntries_.end(), [&](const auto& entry) {
                    return entry.type == DockEntryType::DesktopItem && entry.reference == ToUpperInvariant(path);
                });
                if (added != dockEntries_.end())
                {
                    DockEntry entry = std::move(*added);
                    dockEntries_.erase(added);
                    const size_t begin = DockMainEntryCount();
                    const size_t end = begin + DockFolderEntryCount();
                    const size_t index = std::clamp(insertIndex, begin, end);
                    dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(index), std::move(entry));
                    InvalidateDockContainers();
                }
            }
            else
            {
                const auto cacheKey = snowdesktop::dock_refresh_cache::SourceKey(key, path);
                const auto cached = dockAppIdentityCache_.Read(cacheKey, stamp);
                DockAppIdentity identity = target.identity;
                identity.sourceParsingName = path;
                identity.kind = !identity.executablePath.empty()
                    ? DockAppIdentityKind::Executable : DockAppIdentityKind::Applications;
                dockAppIdentityCache_.Publish(cacheKey, cached.ticket, std::move(identity));
                AdoptDockRunningPresentation(runningKey, itemIndex);
            }
            RebuildContainersAndItems();
            ApplyPageMapping();
            LayoutItems();
            return true;
        }, [](const std::wstring& path) { DeleteFileW(path.c_str()); }, [this] { SaveLayoutSlots(); });
    if (pinned)
    {
        RequestShellRefresh();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return pinned;
}
