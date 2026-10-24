#include "app/app.h"
#include "app/dock/dock_running_drag_state.h"
#include "app/input/native_item_drag_source.h"
#include "app/shell/shell_refresh_snapshot.h"
#include "desktop/desktop_namespace_registry.h"
#include "app/widgets/widget_pair_drag_source.h"
#include "diagnostics/input_diagnostics.h"
#include "system/status_bar/status_bar_interaction.h"
#include "../test_temporary_directory.h"
#include <fstream>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <condition_variable>

// This fixture supplies models and the initial gesture only. Target selection
// executes DesktopApp -> controller -> actual container virtual hit methods.
// Run() is never called. Process-lifetime owners deliberately avoid the host
// destructor, whose global Shell shutdown is outside this windowless test.
struct NativeDragIntegrationAccess
{
    static inline HWND completionWindow = nullptr;
    static inline DesktopApp* asyncOwner = nullptr;
    static inline WNDPROC previousWindowProc = nullptr;
    static inline std::exception_ptr asyncException;
    static inline std::uint64_t asyncReadMessages = 0;
    static inline std::uint64_t asyncShellTimers = 0;
    static inline std::uint64_t fileCompletionMessages = 0;
    static inline std::uint64_t graceTimerMessages = 0;
    static inline std::uint64_t maintenanceTimerMessages = 0;

    static LRESULT CALLBACK AsyncWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (asyncOwner && window == completionWindow &&
            (message == kBackgroundShellReadyMessage || message == kShellFileOperationCompletedMessage ||
                message == kUrlDropDownloadCompletedMessage ||
                (message == WM_TIMER && (wp == kShellChangeTimerId || wp == kExternalOleDragLeaveGraceTimerId ||
                    wp == kDesktopHostWatchTimerId))))
        {
            try
            {
                if (message == WM_TIMER && wp == kDesktopHostWatchTimerId)
                {
                    ++maintenanceTimerMessages;
                    // Production OnTimer invokes this same guarded service.
                    // The remaining desktop recovery/watch services require
                    // an initialized host and stay outside this fixture.
                    asyncOwner->DrainBackgroundShellWork();
                    return 0;
                }
                if (message == kBackgroundShellReadyMessage)
                {
                    ++asyncReadMessages;
                    // Keep the production read/delivery services while the
                    // isolated fixture supplies its own running-window input.
                    // The full dispatcher also starts global window capture,
                    // which this message-only owner does not initialize.
                    asyncOwner->PollInitialShellRead();
                    asyncOwner->DrainBackgroundShellWork();
                    return 0;
                }
                else if (message == WM_TIMER && wp == kExternalOleDragLeaveGraceTimerId) ++graceTimerMessages;
                else if (message == WM_TIMER) ++asyncShellTimers;
                else ++fileCompletionMessages;
                return asyncOwner->HandleMessage(window, message, wp, lp);
            }
            catch (...) { asyncException = std::current_exception(); return 0; }
        }
        return previousWindowProc ? CallWindowProcW(previousWindowProc, window, message, wp, lp)
            : DefWindowProcW(window, message, wp, lp);
    }

    static void BindAsync(DesktopApp& app)
    {
        if (asyncOwner || app.hwnd_ != completionWindow || !IsWindow(completionWindow))
            throw std::runtime_error("async native fixture must exclusively own its message-only window");
        // Keep real model reads, timers, delivery and reload. Pixel providers
        // and global clipboard/cache services are outside this contract.
        app.iconWork_.Stop();
        for (const auto* clsid : {kDesktopIconClsidThisPC, kDesktopIconClsidUserFiles, kDesktopIconClsidNetwork,
                kDesktopIconClsidControlPanel, kDesktopIconClsidRecycleBin, kDesktopIconClsidApplications})
            app.settingsIconVisibility_[ToUpperInvariant(clsid)] = false;
        for (const auto& entry : snowdesktop::LoadDesktopNamespaceRegistrations())
            app.settingsIconVisibility_[ToUpperInvariant(entry.clsid)] = false;
        asyncException = {};
        asyncReadMessages = 0;
        asyncShellTimers = 0;
        fileCompletionMessages = 0;
        graceTimerMessages = 0;
        maintenanceTimerMessages = 0;
        previousWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(completionWindow,
            GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AsyncWindowProc)));
        if (!previousWindowProc) throw std::runtime_error("install the isolated native completion dispatcher");
        asyncOwner = &app;
        if (!SetTimer(completionWindow, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr))
            throw std::runtime_error("start the native fixture's production-period background delivery timer");
    }
    static void UnbindAsync(DesktopApp& app)
    {
        if (asyncOwner != &app) return;
        KillTimer(completionWindow, kShellChangeTimerId);
        KillTimer(completionWindow, kExternalOleDragLeaveGraceTimerId);
        KillTimer(completionWindow, kDesktopHostWatchTimerId);
        SetWindowLongPtrW(completionWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousWindowProc));
        previousWindowProc = nullptr;
        asyncOwner = nullptr;
    }
    static void RequestAsync(DesktopApp& app, bool disk = false) { app.ReloadItems(disk); }
    static bool AsyncRunning(const DesktopApp& app) { return app.shellRefreshRevision_.Running(); }
    static bool AsyncIdle(const DesktopApp& app)
    {
        return app.desktopItemsReady_ && !app.shellReloadPending_ && !app.shellRefreshRevision_.Running() &&
            !app.readyShellRefresh_ && !app.layoutReload_.Pending();
    }
    static void EndActualGesture(DesktopApp& app) { app.EndDragSession(); }
    static void RequestMappedRead(DesktopApp& app, const std::wstring& path) { app.RequestFolderRefresh({path}); }
    static bool FolderReaderIdle(const DesktopApp& app) { return app.folderReadsPending_.empty(); }
    static void EndExternalModel(DesktopApp& app)
    {
        app.dragDropController_.EndExternalDrag();
        app.EndDragSession();
    }
    static void BeginExternal(DesktopApp& app, std::size_t count, POINT point)
    {
        app.dragDropController_.BeginExternalDrag({static_cast<int>(count), false, false});
        app.dragSession_.Begin(nullptr, {}, {}, point, point);
    }
    static HRESULT DropExternal(DesktopApp& app, IDataObject* source, DWORD keys, POINT point, DWORD& effect)
    {
        POINT screen = point;
        if (!ClientToScreen(completionWindow, &screen)) throw std::runtime_error("convert fixture client point through its own message window");
        ComPtr<OleDragDropAdapter> adapter;
        adapter.Attach(new OleDragDropAdapter(&app));
        return adapter->Drop(source, keys | MK_LBUTTON, {screen.x, screen.y}, &effect);
    }
    static ComPtr<OleDragDropAdapter> ExternalAdapter(DesktopApp& app)
    {
        ComPtr<OleDragDropAdapter> adapter;
        adapter.Attach(new OleDragDropAdapter(&app));
        return adapter;
    }
    // Forward the real OLE boundary to the real host, supplying only the
    // synchronous menu UI service. No effect, source read or commit is faked.
    struct RightMenuBridge final : OleDragDropHandler
    {
        DesktopApp& app;
        DesktopApp::RightDragMenuPresenter menu;
        DWORD dropKeys = 0;
        RightMenuBridge(DesktopApp& host, DesktopApp::RightDragMenuPresenter presenter)
            : app(host), menu(std::move(presenter)) {}
        HRESULT HandleOleDragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override
        { return app.HandleOleDragEnter(data, keys, point, effect); }
        HRESULT HandleOleDragOver(DWORD keys, POINTL point, DWORD* effect) override
        { return app.HandleOleDragOver(keys, point, effect); }
        HRESULT HandleOleDragLeave() override { return app.HandleOleDragLeave(); }
        HRESULT HandleOleDrop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override
        {
            dropKeys = keys;
            return app.HandleOleDropWithMenu(data, keys, point, effect, menu);
        }
        HRESULT HandleOleQueryContinueDrag(BOOL escape, DWORD keys) override
        { return app.HandleOleQueryContinueDrag(escape, keys); }
        HRESULT HandleOleGiveFeedback(DWORD effect) override { return app.HandleOleGiveFeedback(effect); }
    };
    static bool RightMenuRetained(const DesktopApp& app)
    {
        return app.dragDropController_.IsExternalDragActive() && !app.dragSession_.IsActive() &&
            app.dragSession_.HasContext() && app.dragSession_.Items().empty();
    }
    static void RebuildDuringMenu(DesktopApp& app) { app.RebuildContainersAndItems(); }
    static POINTL ExternalPoint(POINT point)
    {
        if (!ClientToScreen(completionWindow, &point))
            throw std::runtime_error("convert native OLE input through the owned message-only window");
        return {point.x, point.y};
    }
    static const ExternalDragSummary& ExternalSummary(const DesktopApp& app)
    { return app.dragDropController_.ExternalSummary(); }
    static bool ExternalSession(const DesktopApp& app)
    {
        return app.dragDropController_.IsExternalDragActive() && app.dragSession_.IsActive() &&
            app.dragSession_.Items().empty();
    }
    static DropAction CurrentAction(const DesktopApp& app) { return app.dragSession_.Action(); }
    static bool NoPresentationWindows(const DesktopApp& app)
    {
        return app.hwnd_ == completionWindow && !app.inputHwnd_ && !app.controlHwnd_ &&
            !app.dragPreviewHwnd_ && !app.hintHwnd_ && !app.dcompDevice_ && !app.dwriteFactory_;
    }
    static DropPreviewList ObserveOlePreview(DesktopApp& app, POINT point, int mods)
    {
        // Observe the target chosen by Enter/Over; do not repair it by resolving again.
        return app.GetCachedDesktopDropPreview(false, app.dragSession_.SourceList(),
            Target(app), TargetSlot(app), Region(app), mods, point);
    }
    static HRESULT ExternalLeave(DesktopApp& app, bool pointerOnSurface)
    { return app.HandleExternalOleDragLeave(pointerOnSurface); }
    static bool LeavePending(const DesktopApp& app) { return app.externalOleDragLeavePending_; }
    static bool LeaveCleared(const DesktopApp& app)
    {
        return !app.externalOleDragLeavePending_ && !app.dragDropController_.IsExternalDragActive() &&
            app.dragDropController_.ExternalSummary().fileCount == 0 && !app.dragSession_.IsActive() &&
            !app.dragSession_.HasContext() && !app.dragSession_.TargetContainer() && app.cachedDropPreview_.landings.empty();
    }
    static void BeginSelfModel(DesktopApp& app)
    {
        BeginModel(app, 0, 1);
        app.dragDropController_.BeginSelfDrag();
    }
    static bool InternalSelfSession(const DesktopApp& app)
    {
        return app.dragDropController_.IsSelfDragActive() && app.dragSession_.IsActive() &&
            app.dragSession_.Items().size() == 1 && app.dragSession_.Source() == Desktop(app);
    }
    static void EndSelfModel(DesktopApp& app)
    {
        app.dragDropController_.EndSelfDrag();
        app.EndDragSession();
    }
    static bool FileDropIdle(const DesktopApp& app)
    {
        return AsyncIdle(app) && app.shellFileOperationInFlight_ == 0 && app.pendingLandingCaches_.empty();
    }
    static bool NoPendingFileDrop(const DesktopApp& app)
    { return app.shellFileOperationInFlight_ == 0 && app.pendingLandingCaches_.empty(); }
    static bool FolderDropReadIdle(const DesktopApp& app)
    {
        // Directory ingestion is judged by the delivered members and their
        // order below, not by the implementation's placement-cache lifetime.
        return AsyncIdle(app) && app.shellFileOperationInFlight_ == 0 && app.folderReadsPending_.empty();
    }
    static void SeedDock(DesktopApp& app)
    {
        if (app.d2dDevice_ || app.dcompDevice_) throw std::runtime_error("Dock model requires uninitialized graphics to prevent any persistent host allocation");
        app.generalSettings_.dockEnabled = true;
        app.dockSettings_.showWindowsButton = false;
        app.dockSettings_.showQuickSearchButton = false;
        app.dockSettings_.showFrequentItems = false;
        app.dockSettings_.showRunningApps = false;
        app.dockSettings_.hoverEffect = 0;
        app.dockSettings_.showOnlyWhenSummoned = false;
        app.dockSettings_.position = DockPosition::Bottom;
        app.dockSettings_.edgeAttached = false;
        app.dockEntries_.clear();
        app.dockAreas_ = {RECT{0, 200, 400, 300}};
        app.desktopItemsReady_ = true;
        app.LayoutItems();
    }
    static DockContainer* Dock(DesktopApp& app)
    {
        auto* dock = app.GetDockContainer();
        if (!dock) throw std::runtime_error("actual native Dock container must exist");
        return dock;
    }
    static POINT DockPoint(DesktopApp& app)
    {
        const auto bounds = Dock(app)->GetBounds();
        if (IsRectEmpty(&bounds)) throw std::runtime_error("empty enabled Dock still needs a real hit frame");
        return {(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
    }
    static const auto& DockEntries(const DesktopApp& app) { return app.dockEntries_; }
    static void SeedDockFiles(DesktopApp& app, const std::vector<std::filesystem::path>& dockFiles,
        const std::filesystem::path& sentinel)
    {
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        for (const auto& file : dockFiles) app.items_.push_back(FileItem(file));
        auto desktop = FileItem(sentinel); desktop.gridCell = {L"__page:1", 3, 2};
        app.items_.push_back(std::move(desktop));
        app.RefreshDesktopItemIndexCache(); SeedDock(app);
        for (const auto& file : dockFiles)
        {
            const auto index = app.FindItemIndexByKey(ToUpperInvariant(file.wstring()));
            if (index >= app.items_.size()) throw std::runtime_error("native Dock seed requires the stable original file identity");
            app.items_[index].gridCell = {kDockPageId, 0, 0};
            app.dockEntries_.push_back({DockEntryType::DesktopItem, app.items_[index].layoutKey, false});
        }
        app.RefreshCollectedKeysCache(); app.LayoutItems();
    }
    static DragSourceList BeginDockFiles(DesktopApp& app, const std::vector<std::wstring>& selected,
        POINT destination, int modifiers)
    {
        auto* origin = Dock(app);
        for (const auto& slot : origin->GetSlots())
            if (auto* entry = dynamic_cast<DockEntryItem*>(slot->GetItem()))
                entry->SetSelected(std::find(selected.begin(), selected.end(), entry->GetReference()) != selected.end());
        auto items = origin->GetSelectedItems();
        if (items.size() != selected.size()) throw std::runtime_error("native Dock must capture the complete selected file entries");
        RECT frame = items.front()->GetBounds();
        const POINT start{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        for (auto* item : items) { const auto bounds = item->GetBounds(); UnionRect(&frame, &frame, &bounds); }
        app.dragGroupOriginX_ = frame.left; app.dragGroupOriginY_ = frame.top; app.mouseDownPoint_ = start;
        auto captured = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), captured, start, destination);
        app.dragSession_.UpdateActionFromMods(modifiers);
        return captured;
    }
    static bool DockSourceBindingsCurrent(DesktopApp& app)
    {
        auto* dock = Dock(app);
        const auto& slots = dock->GetSlots();
        if (app.dragSession_.Source() != dock || app.dragSession_.Items().size() != app.dragSession_.SourceList().entries.size()) return false;
        for (const auto& entry : app.dragSession_.SourceList().entries)
        {
            const auto found = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
                const auto* item = dynamic_cast<DockEntryItem*>(slot->GetItem());
                return item && item->GetReference() == entry.dockReference;
            });
            if (found == slots.end() || entry.item != (*found)->GetItem() ||
                entry.sourceIndex >= app.dragSession_.Items().size() || app.dragSession_.Items()[entry.sourceIndex] != (*found)->GetItem()) return false;
        }
        return true;
    }
    static bool ChooseDockFileAction(DesktopApp& app, POINT point, DWORD& keys,
        const DesktopApp::RightDragMenuPresenter& menu)
    { return app.ChooseRightDragDropAction(point, keys, 7, false, menu); }
    static void SeedMenuFileSource(DesktopApp& app, const std::vector<std::filesystem::path>& files, int source)
    {
        if (!source) PopulateModel(app, files, 0);
        else SeedMemberModel(app, files, source == 1 ? DesktopWidgetType::Collection
            : source == 2 ? DesktopWidgetType::FileCategories : DesktopWidgetType::FolderMapping, false);
        for (auto& widget : app.widgets_) { widget.customTitle = widget.title; widget.userRenamed = true; }
        auto& page = app.gridPages_.front(); page.columns = 12; page.rows = 8;
        page.bounds = page.workArea = page.visualWorkArea = RECT{0, 0, 1200, 800};
        app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        app.LayoutItems();
    }
    static DragSourceList BeginMenuFileSource(DesktopApp& app, int source, POINT destination)
    {
        for (auto& item : app.items_) item.selected = std::filesystem::path(item.parsingName).filename() == L"A.txt";
        for (auto& widget : app.widgets_)
            for (auto& entry : widget.folderEntries) entry.selected = entry.name == L"A.txt";
        Container* origin = source ? MemberContainer(app, L"ordered") : static_cast<Container*>(Desktop(app));
        auto items = origin->GetSelectedItems();
        if (items.size() != 1) throw std::runtime_error("native menu source must initially capture exactly A");
        const auto frame = items.front()->GetBounds();
        const POINT start{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        app.dragGroupOriginX_ = frame.left; app.dragGroupOriginY_ = frame.top; app.mouseDownPoint_ = start;
        auto captured = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), captured, start, destination);
        return captured;
    }
    static void ChangeMenuFileSelection(DesktopApp& app, bool keepOriginal)
    {
        const auto selected = [&](const auto& name) { return name == L"B.txt" || (keepOriginal && name == L"A.txt"); };
        for (auto& item : app.items_) item.selected = selected(std::filesystem::path(item.parsingName).filename().wstring());
        for (auto& widget : app.widgets_)
            for (auto& entry : widget.folderEntries) entry.selected = selected(entry.name);
    }
    static const auto& CapturedSources(const DesktopApp& app) { return app.dragSession_.SourceList(); }
    static bool MenuFileBindingsCurrent(DesktopApp& app, int source)
    {
        Container* current = source ? MemberContainer(app, L"ordered") : static_cast<Container*>(Desktop(app));
        if (app.dragSession_.Source() != current || app.dragSession_.Items().size() != app.dragSession_.SourceList().entries.size()) return false;
        std::vector<Item*> live;
        for (const auto& object : app.items_oo_) if (object->GetContainer() == current) live.push_back(object.get());
        if (const auto* widget = dynamic_cast<WidgetContainer*>(current))
        {
            // Drag wrappers and display-slot wrappers have distinct owners.
            // Inspect ownership without rebuilding either cache under test.
            for (const auto& item : widget->dragSourceCache_) live.push_back(item.get());
            for (const auto& item : widget->slotItemCache_) live.push_back(item.get());
        }
        else for (const auto& slot : current->GetSlots()) if (auto* item = slot->GetItem()) live.push_back(item);
        for (const auto& entry : app.dragSession_.SourceList().entries)
            if (entry.sourceIndex >= app.dragSession_.Items().size() ||
                std::none_of(live.begin(), live.end(), [&](auto* item) {
                    return item == entry.item && item == app.dragSession_.Items()[entry.sourceIndex] && PathsEqualInsensitive(item->GetPath(), entry.filePath);
                })) return false;
        return true;
    }
    static bool InternalMenuRetained(const DesktopApp& app)
    { return app.dragSession_.HasContext() && !app.dragSession_.IsActive() && !app.dragSession_.SourceList().Empty(); }
    static void DropDockFiles(DesktopApp& app, int modifiers)
    {
        auto* target = Target(app); auto* slot = TargetSlot(app);
        auto* origin = app.dragSession_.Source(); const auto region = Region(app);
        const auto items = app.dragSession_.Items();
        if (target != Desktop(app) || region != HitRegion::Empty) throw std::runtime_error("Dock fixture requires an actual empty desktop target");
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, slot, region, modifiers);
        app.EndDragSession();
    }
    static bool NoDockWindows(const DesktopApp& app)
    { return NoPresentationWindows(app) && !app.d2dDevice_ && app.persistentDockHosts_.empty(); }
    static void ThrowAsyncException() { if (asyncException) std::rethrow_exception(asyncException); }
    static DesktopApp& Scene(bool secondary)
    {
        auto& app = *new DesktopApp;
        GridPage page;
        page.id = L"native-page";
        page.monitorId = L"native-monitor";
        page.bounds = secondary ? RECT{-800, 0, -400, 300} : RECT{0, 0, 400, 300};
        page.workArea = page.visualWorkArea = page.bounds;
        page.columns = 4; page.rows = 3;
        page.cellWidth = page.cellHeight = 80;
        page.itemPitchWidth = page.itemPitchHeight = 100;
        page.gapX = page.gapY = 20;
        page.marginX = page.marginY = 10;
        app.gridPages_.push_back(page);
        for (int i = 0; i < 2; ++i)
        {
            DesktopItem item;
            item.name = i ? L"B" : L"A";
            item.layoutKey = item.parsingName = item.name;
            item.gridCell = {page.id, i * 2, 0};
            item.gridSpan = {1, 1};
            item.selected = true;
            item.bounds = {page.bounds.left + 10 + i * 200, 10,
                page.bounds.left + 90 + i * 200, 90};
            app.items_.push_back(std::move(item));
        }
        app.containers_.push_back(std::make_unique<DesktopGrid>(&app.gridPages_, &app.items_, &app));
        for (auto& item : app.items_)
            app.items_oo_.push_back(std::make_unique<DesktopIcon>(&item, app.containers_.front().get(), &app));
        return app;
    }

    static void Begin(DesktopApp& app, bool external, std::size_t count)
    {
        std::vector<Item*> items;
        DragSourceList sources;
        Container* origin = nullptr;
        if (external)
            app.dragDropController_.BeginExternalDrag({static_cast<int>(count), false, false});
        else
        {
            origin = app.containers_.front().get();
            for (std::size_t i = 0; i < count; ++i) items.push_back(app.items_oo_[i].get());
            sources = app.BuildDragSourceList(items, origin);
        }
        const POINT point{app.gridPages_.front().bounds.left + 50, 50};
        app.dragSession_.Begin(origin, std::move(items), std::move(sources), point, point);
    }

    static Container* AddStorage(DesktopApp& app, DesktopWidgetType type, bool icon)
    {
        DesktopWidget widget;
        widget.id = L"native-storage";
        widget.type = type;
        widget.title = L"Native target";
        widget.gridCell = {L"native-page", 1, 1};
        widget.gridSpan = icon ? GridSpan{1, 1} : GridSpan{3, 2};
        widget.singleCellIconMode = icon;
        const int left = app.gridPages_.front().bounds.left;
        widget.bounds = icon ? RECT{left + 110, 110, left + 190, 190}
            : RECT{left + 110, 110, left + 390, 290};
        app.widgets_.push_back(std::move(widget));
        auto object = CreateWidget(&app.widgets_.back(), &app);
        auto* container = dynamic_cast<WidgetContainer*>(object.get());
        if (!container) throw std::runtime_error("native storage factory must construct a real container");
        object.release();
        app.containers_.emplace_back(container);
        return container;
    }

    static void Resolve(DesktopApp& app, POINT point)
    {
        if ((app.hwnd_ && app.hwnd_ != completionWindow) || app.inputHwnd_ || app.controlHwnd_)
            throw std::runtime_error("windowless native test must never initialize a host window");
        app.ResolveCurrentDragTargetAt(point);
    }

    static Container* Target(const DesktopApp& app) { return app.dragSession_.TargetContainer(); }
    static Slot* TargetSlot(const DesktopApp& app) { return app.dragSession_.TargetSlot(); }
    static HitRegion Region(const DesktopApp& app) { return app.dragSession_.TargetRegion(); }
    static Container* Desktop(const DesktopApp& app) { return app.containers_.front().get(); }
    static void Hide(DesktopApp& app) { app.desktopIconsHidden_ = true; }

    static void ConfigureModel(DesktopApp& app, const std::filesystem::path& layout)
    {
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        app.hwnd_ = completionWindow;
        app.initializationExperimentDirectory_ = layout;
        app.generalSettings_.dockEnabled = false;
        app.generalSettings_.statusBar.enabled = false;
        app.categorySettings_.collectProgramsEnabled = true;
        app.gridPages_.front().id = L"__page:1";
        app.savedPageIds_ = {L"__page:1"};
        app.savedPageColumns_[L"__page:1"] = 4;
        app.savedPageRows_[L"__page:1"] = 3;
        app.savedPageNames_[L"__page:1"] = L"Model page";
        app.lastMonitorPageId_.clear();
    }

    static DesktopItem FileItem(const std::filesystem::path& path)
    {
        DesktopItem item;
        item.name = path.filename().wstring();
        item.parsingName = path.wstring();
        item.layoutKey = ToUpperInvariant(path.wstring());
        item.gridSpan = {1, 1};
        item.iconBitmap = CreateBitmap(512, 512, 1, 32, nullptr);
        if (!item.iconBitmap) throw std::runtime_error("native fixture bitmap allocation failed");
        item.iconBitmapSize = {512, 512};
        item.iconState = IconState::FullQuality;
        return item;
    }

    // source: 0 = free desktop, 1 = collection, 2 = manual desktop-files widget.
    static void PopulateModel(DesktopApp& app, const std::vector<std::filesystem::path>& files, int source)
    {
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            auto item = FileItem(files[i]);
            item.gridCell = {L"__page:1", static_cast<int>(i) * 2, 0};
            item.selected = true;
            app.items_.push_back(std::move(item));
        }
        if (source)
        {
            DesktopWidget widget;
            widget.id = L"native-origin";
            widget.type = source == 1 ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
            widget.title = L"Origin";
            widget.gridCell = {L"__page:1", 0, 0};
            widget.gridSpan = {1, 1};
            widget.autoCollect = false;
            for (const auto& item : app.items_) widget.itemKeys.push_back(item.layoutKey);
            app.widgets_.push_back(std::move(widget));
        }
        app.desktopItemsReady_ = true;
        app.RefreshDesktopItemIndexCache();
        app.LayoutItems();
    }

    static void BeginModel(DesktopApp& app, int source, std::size_t count)
    {
        for (std::size_t i = 0; i < app.items_.size(); ++i) app.items_[i].selected = i < count;
        Container* origin = app.containers_.front().get();
        if (source)
            for (const auto& container : app.containers_)
                if (auto* widget = dynamic_cast<WidgetContainer*>(container.get());
                    widget && widget->GetWidgetData()->id == L"native-origin") origin = widget;
        auto items = origin->GetSelectedItems();
        if (items.size() != count) throw std::runtime_error("native source must expose the selected real members");
        auto captured = app.BuildDragSourceList(items, origin);
        const LONG left = app.gridPages_.front().bounds.left;
        app.dragGroupOriginX_ = left + 10;
        app.dragGroupOriginY_ = 10;
        app.mouseDownPoint_ = {left + 50, 50};
        app.dragSession_.Begin(origin, std::move(items), std::move(captured), {left + 50, 50}, {left + 50, 50});
    }

    static DropPreviewList ObserveModelPreview(DesktopApp& app, POINT point, int mods = 0)
    {
        Resolve(app, point);
        const bool hasItemDrag = app.dragSession_.IsActive() && !app.dragSession_.Items().empty();
        return app.GetCachedDesktopDropPreview(hasItemDrag, app.dragSession_.SourceList(),
            Target(app), TargetSlot(app), Region(app), mods, point);
    }

    static void DropModel(DesktopApp& app)
    {
        auto* target = Target(app);
        auto* slot = TargetSlot(app);
        auto* origin = app.dragSession_.Source();
        const auto region = Region(app);
        const auto items = app.dragSession_.Items();
        if (!dynamic_cast<DesktopGrid*>(target) || region == HitRegion::Handoff)
            throw std::runtime_error("model fixture must submit through the actual non-handoff desktop target");
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, slot, region, 0);
        app.dragSession_.End();
    }

    static void RequestSave(DesktopApp& app) { app.RequestLayoutSave(); }
    static void UseSaveWindow(DesktopApp& app) { app.controlHwnd_ = completionWindow; }
    static void SetSaveColumn(DesktopApp& app, int column) { app.items_[0].gridCell.column = column; }
    static void SetSaveGesture(DesktopApp& app, bool active) {
        if (active)
            app.largeIconGesture_ = DesktopApp::LargeIconGesture{app.items_[0].layoutKey,
                {}, app.items_[0].gridCell, false, true, true};
        else app.largeIconGesture_.reset();
    }
    static bool PendingSave(const DesktopApp& app) { return app.layoutSavePending_; }
    static bool ScrollSavePending(const DesktopApp& app) { return app.layoutScrollSave_.Pending(); }
    static void DispatchSaveTimer(DesktopApp& app) { app.OnTimer(kLayoutScrollSaveTimerId); }
    static void DueSaveTimer(DesktopApp& app) {
        app.layoutScrollSave_.Request(snowdesktop::LayoutScrollSave::Clock::now() - std::chrono::seconds(2));
    }
    static void FlushSave(DesktopApp& app) { app.FlushQueuedLayoutSave(); }
    static void DrainSave(DesktopApp& app) { app.layoutSaveQueue_.Drain(); }
    static std::uint64_t LayoutRevision(const DesktopApp& app) { return app.layoutPassRevision_; }
    static bool Save(DesktopApp& app) { return app.SaveLayoutSlots(false); }
    static WidgetContainer* MemberContainer(DesktopApp& app, const std::wstring& id)
    {
        for (const auto& container : app.containers_)
            if (auto* widget = dynamic_cast<WidgetContainer*>(container.get());
                widget && widget->GetWidgetData()->id == id) return widget;
        throw std::runtime_error("native member fixture requires its real container");
    }
    static void SeedExternalStorage(DesktopApp& app, DesktopWidgetType rootType,
        DesktopWidgetType memberType, bool icon, const std::filesystem::path& member)
    {
        const bool grouped = IsGroupWidgetType(rootType);
        DesktopWidget target;
        target.id = grouped ? L"ingress-active" : L"ingress-root";
        target.type = memberType; target.title = target.customTitle = L"Existing destination";
        target.gridCell = {L"__page:1", 0, 0}; target.gridSpan = {4, 3};
        target.autoCollect = false; target.activeCategoryId = L"all";
        if (memberType == DesktopWidgetType::FolderMapping)
        {
            target.sourceFolderPath = member.parent_path().wstring();
            auto snapshot = snowdesktop::shell_refresh::ReadFolder(target.sourceFolderPath, false);
            if (!snapshot.complete) throw std::runtime_error("read real mapped ingress baseline");
            target.folderEntries = std::move(snapshot.entries);
        }
        else
        {
            auto item = FileItem(member); target.itemKeys = {item.layoutKey};
            app.items_.push_back(std::move(item));
        }
        if (!grouped) { target.gridSpan = icon ? GridSpan{1, 1} : GridSpan{4, 3}; target.singleCellIconMode = icon; }
        app.widgets_.push_back(std::move(target));
        if (grouped)
        {
            DesktopWidget inactive;
            inactive.id = L"ingress-inactive"; inactive.type = DesktopWidgetType::Collection;
            inactive.title = inactive.customTitle = L"Unchanged inactive source";
            inactive.gridCell = {L"__page:1", 0, 0}; inactive.gridSpan = {2, 2};
            inactive.autoCollect = false; inactive.privacyMode = true;
            if (rootType == DesktopWidgetType::FileGroup) inactive.type = DesktopWidgetType::FileCategories;
            app.widgets_.push_back(std::move(inactive));
            DesktopWidget root;
            root.id = L"ingress-root"; root.type = rootType;
            root.title = root.customTitle = L"Ingress group";
            root.childWidgetIds = {L"ingress-active", L"ingress-inactive"}; root.activeCategoryId = L"ingress-active";
            root.gridCell = {L"__page:1", 0, 0}; root.gridSpan = icon ? GridSpan{1, 1} : GridSpan{4, 3};
            root.singleCellIconMode = icon; root.autoCollect = false;
            app.widgets_.push_back(std::move(root));
        }
        app.desktopItemsReady_ = true; app.RefreshDesktopItemIndexCache(); app.LayoutItems();
    }
    static void FreshExternalDwell(DesktopApp& app)
    {
        // UI timeout transitions are a separate sequence. Keep real OLE
        // routing/file work, with a fresh hover rather than an elapsed dwell.
        app.CancelCollectionPopupDwell(); app.CancelCollectionGroupTabDwell();
    }
    static bool NoIngressPopup(const DesktopApp& app)
    { return NoPresentationWindows(app) && app.popupWidgetIndex_ == static_cast<std::size_t>(-1); }
    static void SeedMemberModel(DesktopApp& app, const std::vector<std::filesystem::path>& files,
        DesktopWidgetType type, bool list)
    {
        DesktopWidget widget;
        widget.id = L"ordered"; widget.type = type; widget.title = L"Ordered members";
        widget.gridCell = {L"__page:1", 0, 0}; widget.gridSpan = {4, 3};
        widget.listMode = list; widget.autoCollect = false;
        widget.activeCategoryId = L"all";
        if (type == DesktopWidgetType::FolderMapping)
        {
            widget.sourceFolderPath = files.front().parent_path().wstring();
            auto snapshot = snowdesktop::shell_refresh::ReadFolder(widget.sourceFolderPath, false);
            if (!snapshot.complete) throw std::runtime_error("read actual mapped member fixture");
            widget.folderEntries = std::move(snapshot.entries);
        }
        else for (const auto& path : files)
        {
            auto item = FileItem(path);
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
                throw std::runtime_error("read actual native member metadata");
            item.fileSize = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
            item.modifiedTime = attributes.ftLastWriteTime;
            widget.itemKeys.push_back(item.layoutKey);
            app.items_.push_back(std::move(item));
        }
        app.widgets_.push_back(std::move(widget));
        app.desktopItemsReady_ = true;
        app.RefreshDesktopItemIndexCache();
        app.LayoutItems();
    }
    static void SortMembers(DesktopApp& app, int mode, bool ascending)
    { app.SortWidgetContents(0, mode, ascending); }
    static void SeedGroupedMembers(DesktopApp& app, const std::vector<std::filesystem::path>& files, bool list,
        DesktopWidgetType childType = DesktopWidgetType::Collection)
    {
        SeedMemberModel(app, files, childType, list);
        // Release views before growing their model vector; all following views
        // are rebuilt by the real layout after group membership exists.
        app.items_oo_.clear(); app.containers_.clear();
        DesktopWidget other;
        other.id = L"other"; other.type = childType == DesktopWidgetType::Collection
            ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
        other.title = other.customTitle = L"Unchanged child";
        other.gridCell = {L"__page:1", 0, 0}; other.gridSpan = {2, 2};
        other.listMode = !list; other.contentSortColumn = snowdesktop::list_detail_rules::Column::Size;
        other.contentSortAscending = false;
        app.widgets_.push_back(std::move(other));
        DesktopWidget group;
        group.id = L"ordered-group"; group.type = childType == DesktopWidgetType::Collection
            ? DesktopWidgetType::CollectionGroup : DesktopWidgetType::FileGroup;
        group.title = group.customTitle = L"Ordered group";
        group.childWidgetIds = {L"ordered", L"other"}; group.activeCategoryId = L"ordered";
        group.gridCell = {L"__page:1", 0, 0}; group.gridSpan = {4, 3}; group.listMode = list;
        app.widgets_.push_back(std::move(group));
        app.LayoutItems();
    }
    static void SortGroupedMembers(DesktopApp& app, int mode, bool ascending)
    { app.SortWidgetContents(2, mode, ascending); }
    static DragSourceList BeginGroupedMembers(DesktopApp& app, const std::vector<std::wstring>& names)
    {
        for (auto& item : app.items_) item.selected = std::find(names.begin(), names.end(), item.name) != names.end();
        for (auto& widget : app.widgets_)
        {
            widget.selected = false;
            for (auto& entry : widget.folderEntries)
                entry.selected = std::find(names.begin(), names.end(), entry.name) != names.end();
        }
        auto* origin = MemberContainer(app, L"ordered-group");
        auto items = origin->GetSelectedItems();
        if (items.size() != names.size()) throw std::runtime_error("native active-group capture requires every selected member");
        auto captured = app.BuildDragSourceList(items, origin);
        const auto& slots = origin->GetSlots();
        if (slots.empty()) throw std::runtime_error("native active-group content must expose real member slots");
        const auto frame = slots.front()->GetBounds();
        const POINT point{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        app.dragSession_.Begin(origin, std::move(items), captured, point, point);
        return captured;
    }
    static DropPreviewList GroupedMemberPreview(DesktopApp& app, bool atEnd, std::size_t before = 0)
    {
        auto* target = MemberContainer(app, L"ordered-group");
        const auto& slots = target->GetSlots();
        auto* boundary = atEnd ? slots.back().get() : slots.at(before).get();
        const auto frame = boundary->GetBounds();
        app.dragSession_.UpdatePoint({atEnd ? frame.right - 1 : frame.left + 1,
            atEnd ? frame.bottom - 1 : frame.top + 1});
        return app.BuildDropPreviewList(app.dragSession_.SourceList(), target,
            atEnd ? nullptr : boundary, atEnd ? HitRegion::SortAfter : HitRegion::SortBefore,
            0, app.dragSession_.CurrentPoint());
    }
    static void DropGroupedMembers(DesktopApp& app, bool atEnd, std::size_t before = 0)
    {
        auto* target = MemberContainer(app, L"ordered-group");
        if (target->NeedsShellReloadAfterDrop())
            throw std::runtime_error("group release fixture requires the actual caller's layout-without-shell-reload branch");
        const auto& slots = target->GetSlots();
        auto* slot = atEnd ? nullptr : slots.at(before).get();
        const auto items = app.dragSession_.Items();
        auto* origin = app.dragSession_.Source();
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, slot, atEnd ? HitRegion::SortAfter : HitRegion::SortBefore, 0);
        // Match the actual pointer caller's post-callback boundary. A native
        // container callback can update data before its group slots rebuild.
        app.SaveLayoutSlots();
        app.ClearSelection();
        app.EndDragSession();
        app.ApplyPageMapping();
        app.LayoutItems();
    }
    static void SeedMixedModel(DesktopApp& app, const std::vector<std::filesystem::path>& files,
        const std::vector<std::filesystem::path>& inside, DesktopWidgetType type, DesktopWidgetType targetType)
    {
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            auto item = FileItem(files[i]);
            item.gridCell = {L"__page:1", static_cast<int>(i) * 2, 0};
            app.items_.push_back(std::move(item));
        }
        if (type != DesktopWidgetType::FolderMapping)
            for (const auto& file : inside) app.items_.push_back(FileItem(file));
        DesktopWidget entrance;
        entrance.id = L"mixed-component"; entrance.title = L"Original component"; entrance.type = type;
        entrance.customTitle = entrance.title;
        entrance.gridCell = {L"__page:1", 0, 2}; entrance.gridSpan = {1, 1}; entrance.singleCellIconMode = true;
        entrance.listMode = true; entrance.privacyMode = true; entrance.autoCollect = false;
        if (type == DesktopWidgetType::FolderMapping)
            entrance.sourceFolderPath = inside.front().parent_path().wstring();
        else if (IsGroupWidgetType(type))
        {
            entrance.childWidgetIds = {L"mixed-child-0", L"mixed-child-1"};
            entrance.activeCategoryId = L"mixed-child-0";
        }
        else for (const auto& file : inside) entrance.itemKeys.push_back(ToUpperInvariant(file.wstring()));
        app.widgets_.push_back(std::move(entrance));
        if (IsGroupWidgetType(type))
            for (std::size_t i = 0; i < inside.size(); ++i)
            {
                DesktopWidget child;
                child.id = L"mixed-child-" + std::to_wstring(i); child.title = L"Original child";
                child.customTitle = child.title;
                child.type = type == DesktopWidgetType::CollectionGroup ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
                child.gridCell = {L"__page:1", 0, 2}; child.gridSpan = {1, 1}; child.autoCollect = false;
                child.itemKeys = {ToUpperInvariant(inside[i].wstring())};
                app.widgets_.push_back(std::move(child));
            }
        DesktopWidget target;
        target.id = L"mixed-target"; target.title = L"Destination"; target.type = targetType;
        target.gridCell = {L"__page:1", 1, 1}; target.gridSpan = {3, 2}; target.autoCollect = false;
        target.activeCategoryId = L"all";
        app.widgets_.push_back(std::move(target));
        app.desktopItemsReady_ = true;
        app.RefreshDesktopItemIndexCache();
        app.LayoutItems();
    }
    static DragSourceList BeginMixed(DesktopApp& app, const std::vector<std::filesystem::path>& files,
        std::size_t count, bool reverse, int mods = 0)
    {
        for (auto& item : app.items_)
            item.selected = std::any_of(files.begin(), files.begin() + static_cast<std::ptrdiff_t>(count),
                [&](const auto& file) { return item.layoutKey == ToUpperInvariant(file.wstring()); });
        for (auto& widget : app.widgets_) widget.selected = widget.id == L"mixed-component";
        auto* origin = Desktop(app);
        auto items = origin->GetSelectedItems();
        if (items.size() != count + 1) throw std::runtime_error("native mixed capture must contain every file and the component");
        if (reverse) std::reverse(items.begin(), items.end());
        auto captured = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), captured, {50, 50}, {250, 250});
        app.dragSession_.UpdateActionFromMods(mods);
        return captured;
    }
    static DropPreviewList MixedPreview(DesktopApp& app, int mods = 0)
    {
        return app.BuildDropPreviewList(app.dragSession_.SourceList(), MemberContainer(app, L"mixed-target"),
            nullptr, HitRegion::SortAfter, mods, {250, 250});
    }
    static void DropMixed(DesktopApp& app, int mods = 0)
    {
        auto* target = MemberContainer(app, L"mixed-target");
        const auto items = app.dragSession_.Items();
        auto* origin = app.dragSession_.Source();
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, nullptr, HitRegion::SortAfter, mods);
        // Execute the real post-release persistence/layout services explicitly;
        // the pointer-message caller itself is outside this fixture boundary.
        app.SaveLayoutSlots();
        app.ClearSelection();
        app.EndDragSession();
        app.ApplyPageMapping();
        app.LayoutItems();
    }
    static DragSourceList BeginMembers(DesktopApp& app, const std::vector<std::wstring>& names)
    {
        const auto selected = [&](const auto& name) { return std::find(names.begin(), names.end(), name) != names.end(); };
        for (auto& item : app.items_) item.selected = selected(item.name);
        for (auto& entry : app.widgets_.front().folderEntries) entry.selected = selected(entry.name);
        auto* origin = MemberContainer(app, L"ordered");
        auto items = origin->GetSelectedItems();
        if (items.size() != names.size()) throw std::runtime_error("native member capture must retain the complete selected set");
        auto source = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), source, {50, 50}, {50, 50});
        return source;
    }
    static DropPreviewList MemberPreview(DesktopApp& app, bool atEnd)
    {
        auto* target = MemberContainer(app, L"ordered");
        const auto& slots = target->GetSlots();
        if (slots.empty()) throw std::runtime_error("native member preview requires real content slots");
        return app.BuildDropPreviewList(app.dragSession_.SourceList(), target,
            atEnd ? nullptr : slots.front().get(), atEnd ? HitRegion::SortAfter : HitRegion::SortBefore, 0, {50, 50});
    }
    static void DropMembers(DesktopApp& app, bool atEnd)
    {
        auto* target = MemberContainer(app, L"ordered");
        const auto& slots = target->GetSlots();
        auto* slot = atEnd ? nullptr : slots.front().get();
        const auto items = app.dragSession_.Items();
        auto* origin = app.dragSession_.Source();
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, slot, atEnd ? HitRegion::SortAfter : HitRegion::SortBefore, 0);
        app.EndDragSession();
    }
    static void DateHeaders(DesktopApp& app, bool enabled)
    {
        app.widgets_.front().dateHeaders = enabled;
        auto* container = MemberContainer(app, L"ordered");
        if (auto* categories = dynamic_cast<FileCategories*>(container)) categories->InvalidateCategoryCache();
        else if (auto* mapping = dynamic_cast<FolderMapping*>(container)) mapping->InvalidateFilterCache();
        else container->InvalidateSlots();
    }
    static bool GuardSave(DesktopApp& app, int guard)
    {
        const bool ready = app.desktopItemsReady_;
        std::vector<GridPage> pages;
        if (guard == 0) app.desktopItemsReady_ = false;
        if (guard == 1) pages.swap(app.gridPages_);
        if (guard == 2) app.layoutReload_.Request();
        const bool result = Save(app);
        app.desktopItemsReady_ = ready;
        if (guard == 1) pages.swap(app.gridPages_);
        if (guard == 2) app.layoutReload_ = {};
        return result;
    }
    static void Load(DesktopApp& app) { app.LoadLayoutSlots(); }
    static void ReadSnapshot(DesktopApp& app, const std::vector<std::filesystem::path>& files, bool complete)
    {
        snowdesktop::shell_refresh::Snapshot snapshot;
        snapshot.desktopComplete = complete;
        for (const auto& file : files) snapshot.desktopItems.push_back(FileItem(file));
        app.LoadDesktopItems(&snapshot, true);
        if (complete) app.LayoutItems();
    }
    static const auto& Items(const DesktopApp& app) { return app.items_; }
    static void PrepareReload(DesktopApp& app)
    {
        // These providers are outside the layout contract. Do not read the
        // user's clipboard or collect caches while exercising ReloadItems.
        app.clipboardReadWork_.Stop();
        app.dataCleanupWorker_.Stop();
        app.settingsController_ = std::make_unique<snowdesktop::SettingsController>(
            snowdesktop::CreateNativeSettingsStore());
        if (!app.settingsController_->Initialize().Succeeded())
            throw std::runtime_error("initialize actual native settings store in the isolated profile");
        auto general = app.generalSettings_;
        general.dockEnabled = true;
        snowdesktop::DesktopDisplaySettings desktop;
        desktop.dockEnabled = true;
        if (!app.settingsController_->SynchronizeGeneral(general) ||
            !app.settingsController_->SynchronizeDesktop(desktop))
            throw std::runtime_error("seed distinct controller mirrors before actual disk reload");
    }
    static void Reload(DesktopApp& app, snowdesktop::shell_refresh::Snapshot& snapshot, bool disk)
    {
        app.ReloadItems(disk, &snapshot);
    }
    static bool ReloadPending(const DesktopApp& app) { return app.layoutReload_.Pending(); }
    static bool ShellReloadPending(const DesktopApp& app) { return app.shellReloadPending_; }
    static void BeginDesktopGesture(DesktopApp& app, const std::wstring& key)
    {
        for (auto& item : app.items_) item.selected = item.layoutKey == key;
        auto* origin = app.containers_.front().get();
        auto items = origin->GetSelectedItems();
        if (items.size() != 1) throw std::runtime_error("reload gesture must select the captured visible desktop identity");
        auto captured = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), std::move(captured), {50, 50}, {50, 50});
    }
    static void EndGesture(DesktopApp& app) { app.dragSession_.End(); }
    static bool ReloadSettingsAgree(const DesktopApp& app)
    {
        const auto snapshot = app.settingsController_->Snapshot();
        return snapshot && snapshot->initialized && !snapshot->values.general.dockEnabled &&
            !snapshot->values.desktop.dockEnabled && !app.generalSettings_.dockEnabled;
    }
    static void PerturbBeforeDiskRead(DesktopApp& app)
    {
        for (auto& item : app.items_) item.gridCell = {L"__page:1", 0, 2};
        for (auto& widget : app.widgets_) widget.itemKeys.clear();
    }
    static bool WriteReplacementPositions(DesktopApp& app,
        const std::vector<std::wstring>& keys, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto found = std::find_if(app.items_.begin(), app.items_.end(),
                [&](const auto& item) { return item.layoutKey == keys[i]; });
            if (found == app.items_.end()) throw std::runtime_error("replacement writer requires each stable file identity");
            found->gridCell = {L"__page:1", static_cast<int>(i) * 2 + 1, 2};
        }
        app.LayoutItems();
        return Save(app);
    }
    static bool HasMatchingRuntimeSlot(DesktopApp& app, const DesktopItem& item)
    {
        std::size_t offset = 0;
        for (const auto& page : app.gridPages_)
        {
            if (page.id == item.gridCell.pageId)
            {
                if (item.gridSpan.columns < 1 || item.gridSpan.rows < 1 ||
                    item.gridCell.column < 0 || item.gridCell.column > page.columns - item.gridSpan.columns ||
                    item.gridCell.row < 0 || item.gridCell.row > page.rows - item.gridSpan.rows) return false;
                const auto& slots = app.containers_.front()->GetSlots();
                RECT frame{};
                for (int row = 0; row < item.gridSpan.rows; ++row)
                    for (int column = 0; column < item.gridSpan.columns; ++column)
                    {
                        const auto index = offset + static_cast<std::size_t>(item.gridCell.column + column +
                            (item.gridCell.row + row) * page.columns);
                        if (index >= slots.size()) return false;
                        const auto cell = slots[index]->GetBounds();
                        if (IsRectEmpty(&cell)) return false;
                        UnionRect(&frame, &frame, &cell);
                    }
                return EqualRect(&frame, &item.bounds) && frame.right > frame.left && frame.bottom > frame.top;
            }
            offset += static_cast<std::size_t>(page.columns * page.rows);
        }
        return false;
    }
    static const auto& Widgets(const DesktopApp& app) { return app.widgets_; }
    static void SeedWidgetTransactions(DesktopApp& app, std::vector<DesktopWidget> widgets,
        const std::vector<std::filesystem::path>& files, const std::vector<std::wstring>& dockIds = {})
    {
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        app.widgets_ = std::move(widgets);
        auto& page = app.gridPages_.front();
        page.columns = 12; page.rows = 8;
        page.bounds = page.workArea = page.visualWorkArea = RECT{0, 0, 1200, 800};
        app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        for (const auto& file : files) app.items_.push_back(FileItem(file));
        if (!dockIds.empty())
        {
            SeedDock(app);
            for (const auto& id : dockIds)
            {
                const auto index = app.FindWidgetIndexById(id);
                if (index >= app.widgets_.size()) throw std::runtime_error("native Dock seed requires the actual component identity");
                app.widgets_[index].gridCell = {kDockPageId, 0, 0};
                app.dockEntries_.push_back({DockEntryTypeForWidget(app.widgets_[index].type), id, false});
            }
        }
        app.desktopItemsReady_ = true;
        app.RefreshDesktopItemIndexCache(); app.LayoutItems();
    }
    static DragSourceList BeginWidgetEntrance(DesktopApp& app, const std::wstring& id,
        const std::wstring& groupId = {}, bool dock = false)
    {
        for (auto& item : app.items_) item.selected = false;
        for (auto& widget : app.widgets_) widget.selected = widget.id == id;
        Container* origin = !groupId.empty() ? MemberContainer(app, groupId)
            : dock ? static_cast<Container*>(Dock(app)) : static_cast<Container*>(Desktop(app));
        if (dock)
            for (const auto& slot : origin->GetSlots())
                if (auto* entry = dynamic_cast<DockEntryItem*>(slot->GetItem())) entry->SetSelected(entry->GetReference() == id);
        auto items = origin->GetSelectedItems();
        if (items.size() != 1) throw std::runtime_error("native entrance capture must expose exactly the selected component");
        auto captured = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), captured, {50, 50}, {50, 50});
        const auto decoded = app.GetWidgetPairDragSource();
        if (decoded.widgetIndex >= app.widgets_.size() || app.widgets_[decoded.widgetIndex].id != id || decoded.groupId != groupId)
            throw std::runtime_error("actual pair source decoder must recover the captured entrance identity and owner");
        return captured;
    }
    static bool CommitWidgetPair(DesktopApp& app, const std::wstring& sourceId,
        const std::wstring& targetId, bool merge, bool captured)
    {
        const auto source = captured ? app.GetWidgetPairDragSource()
            : snowdesktop::widget_pair_drag::Source{app.FindWidgetIndexById(sourceId), {}};
        const auto target = app.FindWidgetIndexById(targetId);
        if (source.widgetIndex >= app.widgets_.size() || target >= app.widgets_.size()) return false;
        const auto options = snowdesktop::widget_pair_drop::GetOptions(app.widgets_[source.widgetIndex].type, app.widgets_[target].type);
        const auto action = snowdesktop::widget_pair_drop::ResolveAction(options, !merge, merge, false);
        // The real release ends the captured session before vector mutation.
        // The target identity and body source are explicit commit-boundary inputs;
        // native pointer hit testing would consult the user's desktop windows.
        app.EndDragSession();
        return app.CommitWidgetPairDrop(source.widgetIndex, target, action, source.groupId);
    }
    static DragSourceList BeginGroupLabels(DesktopApp& app, const std::wstring& groupId,
        const std::vector<std::wstring>& selected)
    {
        for (auto& item : app.items_) item.selected = false;
        for (auto& widget : app.widgets_)
            widget.selected = std::find(selected.begin(), selected.end(), widget.id) != selected.end();
        auto* origin = MemberContainer(app, groupId);
        auto items = origin->GetSelectedItems();
        if (items.size() != selected.size()) throw std::runtime_error("native group must expose every selected label");
        auto captured = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), captured, {50, 50}, {-100, -100});
        return captured;
    }
    static void DropGroupLabels(DesktopApp& app, const std::wstring& targetId)
    {
        auto* target = MemberContainer(app, targetId);
        auto* origin = app.dragSession_.Source();
        const auto items = app.dragSession_.Items();
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, nullptr, HitRegion::SortAfter, 0);
    }
    static void FinishGroups(DesktopApp& app) { app.FinishWidgetGroupTransitions(); }
    static const auto& Pages(const DesktopApp& app) { return app.gridPages_; }
    static std::wstring LayoutPath(const DesktopApp& app) { return app.GetLayoutPath(); }
    static bool Ready(const DesktopApp& app) { return app.desktopItemsReady_; }
    static RECT SlotFrame(DesktopApp& app, int column, int row)
    {
        const auto& slots = app.containers_.front()->GetSlots();
        const auto index = static_cast<std::size_t>(column + row * 4);
        if (index >= slots.size()) throw std::runtime_error("native committed cell must retain a real runtime slot");
        return slots[index]->GetBounds();
    }
    static bool HasVisibleIcon(const DesktopApp& app, const DesktopItem& item)
    {
        return std::count_if(app.items_oo_.begin(), app.items_oo_.end(), [&](const auto& object) {
            const auto* icon = dynamic_cast<const DesktopIcon*>(object.get());
            return icon && icon->GetDesktopItem() == &item && icon->GetContainer() == app.containers_.front().get();
        }) == 1;
    }
    static void StopModel(DesktopApp& app)
    {
        app.FlushQueuedLayoutSave();
        app.CancelDeferredLayoutSave();
        app.StopShellFileOperationWorker();
        app.StopUrlDropDownloadWorker();
        UnbindAsync(app);
        app.folderNotifications_.Clear();
        app.StopIconLoader();
        app.appIndexWork_.Stop();
        app.dataCleanupWorker_.Stop();
        if (app.largeIconAssets_) app.largeIconAssets_->Stop();
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear(); app.widgets_.clear();
        app.hwnd_ = nullptr;
        app.controlHwnd_ = nullptr;
    }
#include "native_group_surface_access.inc"
#include "native_ole_sequence_access.inc"
#include "native_content_dock_access.inc"
#include "native_group_filter_access.inc"
#include "native_popup_request_access.inc"
#include "native_ole_pending_access.inc"
#include "native_dock_partition_access.inc"
#include "native_dock_running_access.inc"
#include "native_desktop_compat_access.inc"
#include "native_namespace_rejection_access.inc"
#include "../icons/native_icon_customization_access.inc"
#include "../dock/native_preview_order_access.inc"
#include "../layout/native_list_hover_access.inc"
#include "native_input_safety_access.inc"
#include "../render/native_foreground_culling_access.inc"
#include "../dock/native_dock_hover_access.inc"
#include "../widget/native_widget_surface_handoff_access.inc"
#include "../system/native_status_selection_access.inc"
};

#include "native_drag_commit_cases.h"
#include "native_async_layout_save_cases.h"
#include "native_drag_reload_cases.h"
#include "native_drag_async_reload_cases.h"
#include "native_ole_file_cases.h"
#include "native_ole_leave_cases.h"
#include "native_member_order_cases.h"
#include "native_mixed_selection_cases.h"
#include "native_group_member_order_cases.h"
#include "native_right_drag_cases.h"
#include "native_ole_widget_cases.h"
#include "native_folder_delivery_cases.h"
#include "native_default_file_cases.h"
#include "native_ole_dock_cases.h"
#include "native_widget_pair_cases.h"
#include "native_dock_return_cases.h"
#include "native_right_capture_cases.h"
#include "native_group_surface_cases.h"
#include "native_ole_sequence_cases.h"
#include "native_content_dock_cases.h"
#include "native_owned_content_cases.h"
#include "native_owned_content_rejection_cases.h"
#include "native_group_filter_cases.h"
#include "native_popup_request_cases.h"
#include "native_ole_pending_cases.h"
#include "native_dock_partition_cases.h"
#include "native_dock_running_cases.h"
#include "native_desktop_compat_cases.h"
#include "native_namespace_rejection_cases.h"
#include "native_external_namespace_cases.h"
#include "../icons/native_icon_customization_cases.h"
#include "../dock/native_preview_order_cases.h"
#include "../dock/native_folder_icon_completion_cases.h"
#include "../layout/native_list_hover_cases.h"
#include "../layout/native_popup_display_menu_cases.h"
#include "native_input_safety_cases.h"
#include "../render/native_foreground_culling_cases.h"
#include "../dock/native_dock_hover_cases.h"
#include "../widget/native_widget_surface_handoff_cases.h"
#include "../system/native_status_selection_cases.h"

namespace
{
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void RunTargets()
{
    using Access = NativeDragIntegrationAccess;
    std::size_t scenes = 0;
    for (const bool secondary : {false, true})
        for (const bool external : {false, true})
            for (const std::size_t count : {1u, 2u})
            {
                auto& app = Access::Scene(secondary);
                const LONG x = secondary ? -650 : 150;
                Access::Begin(app, external, count);
                Access::Resolve(app, {x, 150});
                Require(Access::Target(app) == Access::Desktop(app) &&
                    Access::Region(app) == HitRegion::Empty && Access::TargetSlot(app) &&
                    Access::TargetSlot(app)->GetIndex() == 5,
                    "actual host desktop hit must select the physical free row and column");
                Access::Resolve(app, {-4000, -4000});
                Require(!Access::Target(app) && Access::Region(app) == HitRegion::None,
                    "actual host target resolution must clear a target outside all native frames");
                ++scenes;
                for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories,
                        DesktopWidgetType::FolderMapping, DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
                    for (const bool icon : {false, true})
                    {
                        auto& storageScene = Access::Scene(secondary);
                        Container* expected = Access::AddStorage(storageScene, type, icon);
                        Access::Begin(storageScene, external, count);
                        Access::Resolve(storageScene, {x, 150});
                        Require(Access::Target(storageScene) == expected &&
                            Access::Region(storageScene) != HitRegion::None &&
                            Access::Region(storageScene) != HitRegion::Blocked,
                            "actual host must select the foreground native storage body or icon rather than the desktop behind it");
                        Access::Hide(storageScene);
                        Access::Resolve(storageScene, {x, 150});
                        Require(!Access::Target(storageScene),
                            "actual host visibility filtering must prevent hidden unretained storage and desktop targets");
                        ++scenes;
                    }
            }
    std::cout << scenes << " actual host and native container target scenes passed\n";
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 1) {
            native_foreground_culling_cases::Run();
            native_dock_hover_cases::Run();
            native_widget_surface_handoff_cases::Run();
            RunTargets();
            native_preview_order_cases::Run();
        }
        else if (argc == 2 && std::string_view(argv[1]) == "--foreground-culling")
            native_foreground_culling_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--dock-hover")
            native_dock_hover_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--dock-hover-legacy")
            native_dock_hover_cases::Run(true);
        else if (argc == 2 && std::string_view(argv[1]) == "--widget-surface-handoff")
            native_widget_surface_handoff_cases::Run();
        else if (argc == 4 && std::string_view(argv[1]) == "--dock-preview-fixture")
            return native_preview_order_cases::RunFixture(argv[2], argv[3]);
        else if (argc == 2 && std::string_view(argv[1]) == "--dock-preview-order") native_preview_order_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--dock-preview-order-proxy") native_preview_order_cases::Run(1);
        else if (argc == 2 && std::string_view(argv[1]) == "--commit-storage") native_drag_commit_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--async-layout-save") native_async_layout_save_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--reload-storage") native_drag_reload_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--async-reload-storage") native_drag_async_reload_cases::Run();
        else if (argc == 3 && std::string_view(argv[1]) == "--ole-files") native_ole_file_cases::Run(argv[2]);
        else if (argc == 3 && std::string_view(argv[1]) == "--ole-gesture-files") native_ole_file_cases::Run(argv[2], true);
        else if (argc == 2 && std::string_view(argv[1]) == "--ole-leave-lifecycle") native_ole_leave_cases::Run();
        else if (argc == 3 && std::string_view(argv[1]) == "--member-order") native_member_order_cases::Run(argv[2]);
        else if (argc == 3 && std::string_view(argv[1]) == "--mixed-selection") native_mixed_selection_cases::Run(argv[2]);
        else if (argc == 4 && std::string_view(argv[1]) == "--mixed-file-action") native_mixed_selection_cases::Run(argv[2], argv[3]);
        else if (argc == 2 && std::string_view(argv[1]) == "--group-member-order") native_group_member_order_cases::Run();
        else if (argc == 3 && std::string_view(argv[1]) == "--group-member-order") native_group_member_order_cases::Run(argv[2]);
        else if (argc == 3 && std::string_view(argv[1]) == "--right-drag-files") native_right_drag_cases::Run(argv[2]);
        else if (argc == 4 && std::string_view(argv[1]) == "--ole-widget-files") native_ole_widget_cases::Run(argv[2], argv[3]);
        else if (argc == 2 && std::string_view(argv[1]) == "--folder-delivery-after-drag") native_folder_delivery_cases::Run();
        else if (argc == 4 && std::string_view(argv[1]) == "--ole-default-files") return native_default_file_cases::Run(argv[2], argv[3]);
        else if (argc == 3 && std::string_view(argv[1]) == "--ole-dock-folders") native_ole_dock_cases::Run(argv[2]);
        else if (argc == 2 && std::string_view(argv[1]) == "--widget-pair-storage") native_widget_pair_cases::RunPairs();
        else if (argc == 2 && std::string_view(argv[1]) == "--group-lifecycle-storage") native_widget_pair_cases::RunLifecycle();
        else if (argc == 2 && std::string_view(argv[1]) == "--dock-file-returns") native_dock_return_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--right-menu-capture") native_right_capture_cases::Run();
        else if (argc == 3 && std::string_view(argv[1]) == "--group-surfaces") native_group_surface_cases::Run(argv[2]);
        else if (argc == 2 && std::string_view(argv[1]) == "--ole-return") native_ole_sequence_cases::RunReturn();
        else if (argc == 2 && std::string_view(argv[1]) == "--ole-sequences") native_ole_sequence_cases::RunAsync();
        else if (argc == 2 && std::string_view(argv[1]) == "--dock-generated-relations") native_content_dock_cases::RunDockRelations();
        else if (argc == 3 && std::string_view(argv[1]) == "--owned-content") {
            native_owned_content_cases::RunShellFiles(argv[2]);
            native_owned_content_cases::Run(argv[2]); native_owned_content_rejection_cases::Run(argv[2]);
            native_owned_content_cases::Run(argv[2], false, true);
        }
        else if (argc == 3 && std::string_view(argv[1]) == "--owned-content-cancel") native_owned_content_cases::Run(argv[2], true);
        else if (argc == 3 && std::string_view(argv[1]) == "--group-filter") native_group_filter_cases::RunFiltered(argv[2]);
        else if (argc == 3 && std::string_view(argv[1]) == "--group-type") native_group_filter_cases::RunType(argv[2]);
        else if (argc == 2 && std::string_view(argv[1]) == "--group-type-search") native_group_filter_cases::RunTypeSearch();
        else if (argc == 2 && std::string_view(argv[1]) == "--group-dwell") native_group_filter_cases::RunDwell();
        else if (argc == 2 && std::string_view(argv[1]) == "--group-dwell-category") native_group_filter_cases::RunCategoryDwell();
        else if (argc == 2 && std::string_view(argv[1]) == "--popup-request") native_popup_request_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--external-dwell") native_popup_request_cases::RunExternalReplacement();
        else if (argc == 2 && std::string_view(argv[1]) == "--ole-target-changes") native_ole_pending_cases::RunMutations();
        else if (argc == 2 && std::string_view(argv[1]) == "--ole-partial-batch") native_ole_pending_cases::RunPartial();
        else if (argc == 2 && std::string_view(argv[1]) == "--dock-partitions") native_dock_partition_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--dock-running-pin-storage") native_dock_running_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--shortcut-targets") native_desktop_compat_cases::RunShortcuts();
        else if (argc == 2 && std::string_view(argv[1]) == "--folder-shortcut-handoff") native_desktop_compat_cases::RunFolderHandoff();
        else if (argc == 2 && std::string_view(argv[1]) == "--large-icon-storage-return") native_desktop_compat_cases::RunAppearance();
        else if (argc == 2 && std::string_view(argv[1]) == "--list-title-hover") native_list_hover_cases::RunHover();
        else if (argc == 2 && std::string_view(argv[1]) == "--list-title-fonts") native_list_hover_cases::RunFonts();
        else if (argc == 2 && std::string_view(argv[1]) == "--popup-display-menu") native_popup_display_menu_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--input-safety") {
            native_input_safety_cases::Run();
            native_status_selection_cases::Run();
        }
        else if (argc == 2 && std::string_view(argv[1]) == "--status-selection") native_status_selection_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--dock-folder-icon-completion")
            native_folder_icon_completion_cases::RunStandalone();
        else if (argc == 2 && std::string_view(argv[1]) == "--icon-customization") {
            native_icon_customization_cases::Run();
            native_folder_icon_completion_cases::RunStandalone();
        }
        else if (argc == 2 && std::string_view(argv[1]) == "--namespace-rejection") {
            native_namespace_rejection_cases::Run(); native_external_namespace_cases::Run();
        }
        else throw std::runtime_error("unknown native drag integration mode");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << " [" << native_drag_commit_cases::context << "]\n";
        return 1;
    }
}
