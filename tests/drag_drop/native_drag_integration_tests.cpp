#include "app/app.h"
#include "app/shell/shell_refresh_snapshot.h"
#include "desktop/desktop_namespace_registry.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

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

    static LRESULT CALLBACK AsyncWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (asyncOwner && window == completionWindow &&
            (message == kBackgroundShellReadyMessage || message == kShellFileOperationCompletedMessage ||
                (message == WM_TIMER && (wp == kShellChangeTimerId || wp == kExternalOleDragLeaveGraceTimerId))))
        {
            try
            {
                if (message == kBackgroundShellReadyMessage) ++asyncReadMessages;
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
        previousWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(completionWindow,
            GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AsyncWindowProc)));
        if (!previousWindowProc) throw std::runtime_error("install the isolated native completion dispatcher");
        asyncOwner = &app;
    }
    static void UnbindAsync(DesktopApp& app)
    {
        if (asyncOwner != &app) return;
        KillTimer(completionWindow, kShellChangeTimerId);
        KillTimer(completionWindow, kExternalOleDragLeaveGraceTimerId);
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
    static bool FileDropIdle(const DesktopApp& app)
    {
        return AsyncIdle(app) && app.shellFileOperationInFlight_ == 0 && app.pendingLandingCaches_.empty();
    }
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

    static bool Save(DesktopApp& app) { return app.SaveLayoutSlots(false); }
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
                if (item.gridCell.column < 0 || item.gridCell.column >= page.columns ||
                    item.gridCell.row < 0 || item.gridCell.row >= page.rows) return false;
                const auto index = offset + static_cast<std::size_t>(item.gridCell.column + item.gridCell.row * page.columns);
                const auto& slots = app.containers_.front()->GetSlots();
                if (index >= slots.size()) return false;
                const auto frame = slots[index]->GetBounds();
                return EqualRect(&frame, &item.bounds) && frame.right > frame.left && frame.bottom > frame.top;
            }
            offset += static_cast<std::size_t>(page.columns * page.rows);
        }
        return false;
    }
    static const auto& Widgets(const DesktopApp& app) { return app.widgets_; }
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
        app.StopShellFileOperationWorker();
        UnbindAsync(app);
        app.folderNotifications_.Clear();
        app.StopIconLoader();
        app.appIndexWork_.Stop();
        app.dataCleanupWorker_.Stop();
        if (app.largeIconAssets_) app.largeIconAssets_->Stop();
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear(); app.widgets_.clear();
        app.hwnd_ = nullptr;
    }
};

#include "native_drag_commit_cases.h"
#include "native_drag_reload_cases.h"
#include "native_drag_async_reload_cases.h"
#include "native_ole_file_cases.h"
#include "native_ole_leave_cases.h"

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
        if (argc == 1) RunTargets();
        else if (argc == 2 && std::string_view(argv[1]) == "--commit-storage") native_drag_commit_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--reload-storage") native_drag_reload_cases::Run();
        else if (argc == 2 && std::string_view(argv[1]) == "--async-reload-storage") native_drag_async_reload_cases::Run();
        else if (argc == 3 && std::string_view(argv[1]) == "--ole-files") native_ole_file_cases::Run(argv[2]);
        else if (argc == 3 && std::string_view(argv[1]) == "--ole-gesture-files") native_ole_file_cases::Run(argv[2], true);
        else if (argc == 2 && std::string_view(argv[1]) == "--ole-leave-lifecycle") native_ole_leave_cases::Run();
        else throw std::runtime_error("unknown native drag integration mode");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
