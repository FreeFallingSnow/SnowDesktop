#include "app/app.h"
#include "dock_explorer_pin.h"
#include "dock_platform_helpers.h"
#include "dock_running_app_pin_rules.h"
#include "dock_running_drag_state.h"
#include "desktop/desktop_source.h"
#include "layout/logical_slot_picker_rules.h"

namespace drag = snowdesktop::dock_running_drag;
namespace pin = snowdesktop::dock_running_app_pin;

namespace
{
struct RunningDesktopFolderSource
{
    std::wstring key, path, stamp;
    bool operator==(const RunningDesktopFolderSource&) const = default;
};

std::optional<std::wstring> MatchRunningFolderSource(const std::wstring& folder,
    const std::vector<RunningDesktopFolderSource>& sources)
{
    std::wstring matched;
    for (const auto& source : sources)
        if (snowdesktop::dock_explorer_pin::AlreadyPinnedFolder(folder, {{source.path, source.key}}))
        {
            if (!matched.empty()) return std::nullopt; // Do not choose an arbitrary duplicate launcher.
            matched = source.key;
        }
    return matched;
}
}

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
    state->existingDesktopItem = FindDesktopItemForDockRunningApp(*found).has_value();
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
    std::vector<RunningDesktopFolderSource> desktopSources;
    if (state->explorer)
        for (const auto& item : items_)
            desktopSources.push_back({item.layoutKey, item.parsingName, snowdesktop::shell_icon_request::Stamp(item)});
    const bool submitted = shellVisualWork_.Submit(L"dock-running-drag:" + std::to_wstring(state->serial),
        [state, explorerName, desktopSources] {
            auto folder = state->explorer ? ReadDockRunningPinTarget(state->running, true, explorerName) : nullptr;
            const auto key = folder ? MatchRunningFolderSource(folder->folderPath, desktopSources)
                : std::optional<std::wstring>{L""};
            return std::make_pair(std::move(folder), key);
        }, [this, state](const auto& targets) {
            state->folder = targets.second ? targets.first : nullptr;
            state->desktopFolderKey = targets.second.value_or(L"");
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
        const auto desktop = FindDesktopItemForDockRunningApp(*live);
        const bool mainSource = desktop && *desktop < items_.size() &&
            !IsFolderDockEntry({DockEntryType::DesktopItem, items_[*desktop].layoutKey, true});
        const auto folderIndex = FindItemIndexByKey(state.desktopFolderKey);
        const bool folderSource = state.explorer && state.resolved && state.folder &&
            folderIndex < items_.size() &&
            IsFolderDockEntry({DockEntryType::DesktopItem, items_[folderIndex].layoutKey, true});
        state.existingDesktopItem = mainSource || folderSource;
        const bool explorerFolder = state.explorer && state.resolved && state.folder;
        if ((state.target.area == drag::Area::Files && !folderSource && !explorerFolder) ||
            (state.target.area == drag::Area::Fixed && !mainSource && !state.explorer))
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
    if (state.target.area == drag::Area::Fixed || state.target.area == drag::Area::Files)
    {
        if (state.explorer && (state.target.area == drag::Area::Fixed || state.desktopFolderKey.empty()))
            return _LW(state.target.area == drag::Area::Fixed
                ? "app.dock.pin_explorer_to_fixed" : "app.dock.pin_current_folder_to_files");
        return _LW((GetAsyncKeyState(VK_CONTROL) & 0x8000)
            ? "core.dock.release_dock_map_full" : "core.dock.release_move_dock_ctrl");
    }
    return {};
}

void DesktopApp::DrawDockRunningDragPreview(ID2D1DeviceContext* context, DockContainer* dock)
{
    if (!context || !dock || !dockRunningDrag_ || dragSession_.TargetContainer() != dock) return;
    if (dockRunningDrag_->target.area == drag::Area::None) return;
    dock->DrawInsertionPreview(context, 0);
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
    if (!state || exitRequested_ || !generalSettings_.dockEnabled) return;
    const auto live = std::find_if(dockUnpinnedRunningApps_.begin(), dockUnpinnedRunningApps_.end(),
        [&](const auto& app) { return app.presence.Interactive() && app.identityKey == state->running.identityKey; });
    if (live == dockUnpinnedRunningApps_.end()) return;
    const auto commitExisting = [this, state, position, mods](size_t desktop, bool folder) {
        DockContainer* dock = GetDockContainer();
        if (!dock || desktop >= items_.size()) return;
        const size_t index = position.Resolve(dockEntries_, DockMainEntryCount(),
            DockMainEntryCount() + DockFolderEntryCount());
        const std::wstring key = items_[desktop].layoutKey;
        DesktopIcon source(&items_[desktop], nullptr, this);
        CommitDockDrop({&source}, nullptr, dock, index, mods & MK_CONTROL);
        if (!folder && std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const auto& entry) {
                return entry.type == DockEntryType::DesktopItem && entry.reference == ToUpperInvariant(key);
            })) AdoptDockRunningPresentation(state->running.identityKey, desktop);
        RequestLayoutSave(); ApplyPageMapping(); LayoutItems(); InvalidateDockRects();
    };
    // Explorer always uses the explicit default launcher in Fixed, or the
    // exact visible folder in Files. Application matching can name a different
    // Explorer shortcut with directory arguments and must not substitute it.
    if (!state->explorer)
    if (const auto desktop = FindDesktopItemForDockRunningApp(*live); desktop && *desktop < items_.size())
    {
        const bool folder = IsFolderDockEntry({DockEntryType::DesktopItem, items_[*desktop].layoutKey, true});
        if (position.area == (folder ? drag::Area::Files : drag::Area::Fixed)) commitExisting(*desktop, folder);
        return;
    }
    // Ordinary running applications still require an existing desktop entry.
    // Explorer has two well-defined targets and may materialize either one.
    if (!state->explorer || (position.area != drag::Area::Files && position.area != drag::Area::Fixed)) return;
    const auto readSources = [this] {
        std::vector<RunningDesktopFolderSource> sources;
        for (const auto& item : items_)
            sources.push_back({item.layoutKey, item.parsingName, snowdesktop::shell_icon_request::Stamp(item)});
        return sources;
    };
    const bool folder = position.area == drag::Area::Files;
    const auto sources = folder ? readSources() : std::vector<RunningDesktopFolderSource>{};
    // Snapshot every entry used by duplicate detection. Recheck on delivery;
    // another queued drop may have inserted a matching folder in the meantime.
    const std::wstring explorerName = _LW("app.dock.explorer_name");
    const auto ready = folder ? state->folder : state->application;
    const bool submitted = shellVisualWork_.Submit(L"dock-running-pin:" +
        std::to_wstring(state->serial) + L":" + std::to_wstring(retry),
        [state, ready, explorerName, sources, folder] {
            auto resolved = ready ? ready : ReadDockRunningPinTarget(
                state->running, folder, explorerName);
            auto key = resolved && folder ? MatchRunningFolderSource(resolved->folderPath, sources)
                : std::optional<std::wstring>{L""};
            return std::make_pair(std::move(resolved), std::move(key));
        }, [this, state, position, mods, retry, sources, readSources, commitExisting](const auto& result) {
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
            const auto& [target, key] = result;
            if (!target || !key) return; // Unknown folder or ambiguous existing identities.
            if (!key->empty())
            {
                const auto desktop = FindItemIndexByKey(*key);
                if (desktop < items_.size()) commitExisting(desktop, true);
                return;
            }
            const size_t index = position.Resolve(dockEntries_, DockMainEntryCount(),
                DockMainEntryCount() + DockFolderEntryCount());
            if (!PinDockRunningTarget(state->running.identityKey, *target, index))
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
                    snowdesktop::dock_folder_rules::CaptureUserOrder(dockEntries_);
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
        }, [](const std::wstring& path) { DeleteFileW(path.c_str()); }, [this] { RequestLayoutSave(); });
    if (pinned)
    {
        RequestShellRefresh();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return pinned;
}
