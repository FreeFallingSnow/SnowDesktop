#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "app/dock/persistent_dock_host.h"
#include "settings/settings_controller.h"
#include "settings/settings_snapshot.h"
#include "settings/settings_store.h"
#include "icons/large_icon_assets.h"
#include "common/utils.h"
#include "widget/runtime/widget_engine.h"
#include "widget/data/widget_system_data_provider.h"
#include "ui/preview/component_preview.h"
#include "core/dock.h"
#include "core/slot.h"
#include "app/drag_drop/ole_drag_drop_adapter.h"
#include "widgets/widget_folder_search.h"
#include "ui/input/private_pointer.h"
#include "layout/folder_sort_rules.h"
#include "app/shell/shell_refresh_snapshot.h"
#include "app/popup/popup_window_pair_z_order.h"
#include "desktop/desktop_namespace_registry.h"
#include "settings/animation_settings.h"
#include "behavior_fixture.h"
#include <array>
#include <fstream>
#include <iostream>
#include <exception>
#include <stdexcept>
struct NativeDragIntegrationAccess
{
    static inline HWND completionWindow = nullptr;
    static inline DesktopApp* asyncOwner = nullptr;
    static inline WNDPROC previousWindowProc = nullptr;
    static inline std::exception_ptr asyncException;
    static inline std::uint64_t asyncReadMessages = 0;
    static inline std::uint64_t fileCompletionMessages = 0;
    static inline bool fileDispatch = false;
    static inline LONG_PTR previousWindowData = 0;
    static LRESULT CALLBACK AsyncWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (asyncOwner && window == completionWindow &&
            (message == kBackgroundShellReadyMessage || (fileDispatch && message == kShellFileOperationCompletedMessage) ||
                (message == WM_TIMER && (wp == kDesktopHostWatchTimerId || wp == kShellChangeTimerId ||
                    (fileDispatch && wp == kLayoutScrollSaveTimerId)))))
        {
            try {
                if (message == WM_TIMER && wp == kLayoutScrollSaveTimerId)
                    return DesktopApp::ControlWndProc(window, message, wp, lp);
                if (message == kShellFileOperationCompletedMessage) {
                    ++fileCompletionMessages;
                    return DesktopApp::WndProc(window, message, wp, lp);
                }
                if (message == WM_TIMER && wp == kShellChangeTimerId) {
                    // The production refresh callback schedules this timer to
                    // apply its completed snapshot and any pending disk reload.
                    asyncOwner->OnTimer(wp); return 0;
                }
                if (message == kBackgroundShellReadyMessage) { ++asyncReadMessages; asyncOwner->PollInitialShellRead(); }
                asyncOwner->DrainBackgroundShellWork(); return 0;
            } catch (...) { asyncException = std::current_exception(); return 0; }
        }
        return previousWindowProc ? CallWindowProcW(previousWindowProc, window, message, wp, lp) : DefWindowProcW(window, message, wp, lp);
    }
    static void BindAsync(DesktopApp& app)
    {
        if (asyncOwner || app.hwnd_ != completionWindow || !IsWindow(completionWindow)) throw std::runtime_error("invalid fixture window");
        for (const auto* clsid : {kDesktopIconClsidThisPC, kDesktopIconClsidUserFiles, kDesktopIconClsidNetwork, kDesktopIconClsidControlPanel, kDesktopIconClsidRecycleBin, kDesktopIconClsidApplications}) app.settingsIconVisibility_[ToUpperInvariant(clsid)] = false;
        for (const auto& entry : snowdesktop::LoadDesktopNamespaceRegistrations()) app.settingsIconVisibility_[ToUpperInvariant(entry.clsid)] = false;
        asyncException = {}; asyncReadMessages = 0; fileCompletionMessages = 0;
        previousWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(completionWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AsyncWindowProc)));
        if (!previousWindowProc) throw std::runtime_error("install fixture dispatcher");
        asyncOwner = &app;
        if (!SetTimer(completionWindow, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr)) throw std::runtime_error("start fixture timer");
    }
    static void UnbindAsync(DesktopApp& app)
    {
        if (asyncOwner != &app) return;
        KillTimer(completionWindow, kShellChangeTimerId); KillTimer(completionWindow, kDesktopHostWatchTimerId);
        KillTimer(completionWindow, kExternalOleDragLeaveGraceTimerId);
        KillTimer(completionWindow, kLayoutScrollSaveTimerId);
        if (fileDispatch) SetWindowLongPtrW(completionWindow, GWLP_USERDATA, previousWindowData);
        fileDispatch = false; previousWindowData = 0;
        SetWindowLongPtrW(completionWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousWindowProc));
        previousWindowProc = nullptr; asyncOwner = nullptr;
    }
    static void RequestAsync(DesktopApp& app, bool disk = false) { app.ReloadItems(disk); }
    static bool BackgroundIdle(const DesktopApp& app) { return app.desktopItemsReady_ && !app.shellReloadPending_ && !app.shellRefreshRevision_.Running() && !app.readyShellRefresh_ && !app.layoutReload_.Pending() && app.shellFileOperationInFlight_ == 0 && app.pendingLandingCaches_.empty(); }
    static std::string AsyncState(const DesktopApp& app)
    {
        return "desktopReady=" + std::to_string(app.desktopItemsReady_) +
            " reloadPending=" + std::to_string(app.shellReloadPending_) +
            " refreshRunning=" + std::to_string(app.shellRefreshRevision_.Running()) +
            " readySnapshot=" + std::to_string(static_cast<bool>(app.readyShellRefresh_)) +
            " layoutPending=" + std::to_string(app.layoutReload_.Pending()) +
            " fileOperations=" + std::to_string(app.shellFileOperationInFlight_) +
            " landingCaches=" + std::to_string(app.pendingLandingCaches_.size()) +
            " completionMessages=" + std::to_string(asyncReadMessages);
    }
    static void ThrowAsyncException() { if (asyncException) std::rethrow_exception(asyncException); }
    static DesktopApp& Scene()
    {
        auto& app = *new DesktopApp;
        GridPage page;
        page.id = L"native-page";
        page.monitorId = L"native-monitor";
        page.bounds = RECT{0, 0, 400, 300};
        page.workArea = page.visualWorkArea = page.bounds;
        page.columns = 4; page.rows = 3;
        page.cellWidth = page.cellHeight = 80;
        page.itemPitchWidth = page.itemPitchHeight = 100;
        page.gapX = page.gapY = 20;
        page.marginX = page.marginY = 10;
        app.gridPages_.push_back(page);
        return app;
    }
    static void ConfigureModel(DesktopApp& app, const std::filesystem::path& layout)
    {
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        app.iconWork_.Stop();
        app.desktopItemsReady_ = true;
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
        return item;
    }
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
    static bool Save(DesktopApp& app) { return app.SaveLayoutSlots(false); }
    static void Load(DesktopApp& app) { app.LoadLayoutSlots(); }
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
    static void PerturbBeforeDiskRead(DesktopApp& app)
    {
        for (auto& item : app.items_) item.gridCell = {L"__page:1", 0, 2};
        for (auto& widget : app.widgets_) widget.itemKeys.clear();
    }
    static std::wstring LayoutPath(const DesktopApp& app) { return app.GetLayoutPath(); }
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
    static WidgetContainer* MemberContainer(DesktopApp& app, const std::wstring& id)
    {
        for (const auto& container : app.containers_)
            if (auto* widget = dynamic_cast<WidgetContainer*>(container.get());
                widget && widget->GetWidgetData()->id == id) return widget;
        throw std::runtime_error("native member fixture requires its real container");
    }
    static const auto& Widgets(const DesktopApp& app) { return app.widgets_; }
    static std::unique_ptr<DockContainer> OverflowDock(
        DesktopApp& app, DockPosition position, bool attached, int hover, RECT area)
    {
        app.generalSettings_.dockEnabled = true;
        app.dockSettings_.position = position;
        app.dockSettings_.edgeAttached = attached;
        app.dockSettings_.hoverEffect = hover;
        app.dockSettings_.hoverScale = 2.0f;
        app.dockSettings_.showWindowsButton = true;
        app.dockSettings_.showQuickSearchButton = true;
        app.dockSettings_.showFrequentItems = true;
        app.dockSettings_.frequentItemCount = 2;
        app.items_.clear();
        app.dockUsageStats_.clear();
        app.dockUnpinnedRunningApps_.clear();
        for (int i = 0; i < 2; ++i)
        {
            DesktopItem frequent;
            frequent.name = L"Frequent fixture " + std::to_wstring(i);
            frequent.parsingName = L"missing-frequent-" + std::to_wstring(i) + L".lnk";
            frequent.layoutKey = ToUpperInvariant(frequent.parsingName);
            frequent.isShortcut = true;
            frequent.isApplicationShortcut = true;
            app.dockUsageStats_[frequent.layoutKey] = {2, 1};
            app.items_.push_back(std::move(frequent));
            DockRunningAppInfo running;
            running.executablePath = L"missing-running-" + std::to_wstring(i) + L".exe";
            running.identityKey = running.executablePath;
            running.presence.SetVisible(true, 0, false);
            app.dockUnpinnedRunningApps_.push_back(std::move(running));
        }
        app.RefreshDesktopItemIndexCache();
        app.dockEntries_.clear();
        for (int i = 0; i < 24; ++i)
            app.dockEntries_.push_back({DockEntryType::DesktopItem,
                L"missing-overflow-app-" + std::to_wstring(i) + L".exe", true});
        app.dockEntries_.push_back({DockEntryType::FolderMapping, L"overflow-files-1"});
        app.dockEntries_.push_back({DockEntryType::FolderMapping, L"overflow-files-2"});
        app.dockEntries_.push_back({DockEntryType::DesktopItem, kDesktopIconClsidRecycleBin});
        return std::make_unique<DockContainer>(&app, &app.dockEntries_, area);
    }
    static void DockPointer(DesktopApp& app, POINT pointer) { app.lastMousePoint_ = pointer; }
#include "module_access.inc"
#include "drag_access.inc"
#include "popup_drag_access.inc"
};
namespace behavior
{
using Access = NativeDragIntegrationAccess;
template<class Predicate, class Observation>
void Wait(Predicate done, const std::string& condition, Observation observe)
{
    const auto start = GetTickCount64();
    const auto deadline = start + 12000;
    while (!done())
    {
        if (GetTickCount64() >= deadline)
            Require(false, "waiting for " + condition +
                " after " + std::to_string(GetTickCount64() - start) + "ms; last: " + observe());
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Access::ThrowAsyncException();
    }
    Access::ThrowAsyncException();
}
struct HostProfile : Profile
{
    Apartment apartment;
    WindowOwner window;
    DesktopApp* app = nullptr;
    explicit HostProfile(bool refresh = false)
    {
        try {
            window.handle = CreateWindowExW(0, L"STATIC", L"Behavior completion", 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
            Require(window.handle != nullptr, "create owned message-only window");
            Access::completionWindow = window.handle;
            // No Run(): no desktop window, hooks, Explorer or user's files are involved.
            app = &Access::Scene();
            fs::create_directories(root / L"layout");
            Access::ConfigureModel(*app, root / L"layout");
            // Search/settings use the real host model/controller but do not
            // need Shell refresh dispatch, a timer or an initial layout write.
            if (refresh) { Access::PrepareReload(*app); Access::BindAsync(*app); }
        } catch (...) { Stop(); throw; }
    }
    ~HostProfile() { Stop(); }
    void Stop()
    {
        if (app) Access::StopModel(*app);
        app = nullptr;
        Access::completionWindow = nullptr;
        // The uninitialized host graph has process lifetime; its destructor would
        // shut down global Shell services which this fixture never initialized.
    }
};
}
#include "host_contracts.inc"
#include "drag_contracts.inc"
#include "popup_drag_contracts.inc"
#include "widget_focus_contracts.inc"
namespace behavior
{
// Protect scroll ownership and visible geometry through the real Dock model.
// This does not claim raster/glass acceptance or start a desktop host.
void DockOverflow()
{
    HostProfile profile;
    snowdesktop::animation::SetRuntimePreferences(
        snowdesktop::animation::AlwaysOn, 2, 1, 0, false, false, 1);
    const auto center = [](const RECT& rect) {
        return POINT{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    };
    for (const auto position : {DockPosition::Left, DockPosition::Right,
        DockPosition::Top, DockPosition::Bottom})
        for (const bool attached : {false, true})
            for (const int hover : {1, 2})
            {
                const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
                const RECT area = vertical ? RECT{0, 40, 160, 780} : RECT{40, 0, 780, 160};
                auto dock = Access::OverflowDock(*profile.app, position, attached, hover, area);
                const auto& initial = dock->GetSlots();
                Require(initial.size() == 32, "fixture includes fixed, running and frequent applications plus fixed files/controls");
                const RECT firstApp = initial[0]->GetBounds();
                const RECT running = initial[27]->GetBounds();
                const RECT frequent = initial[29]->GetBounds();
                const std::array<RECT, 4> fixed{initial[24]->GetBounds(), initial[25]->GetBounds(),
                    initial[26]->GetBounds(), dock->GetSearchRect()};
                Require(dock->ScrollByWheelDelta(center(firstApp), -180), "application lane scrolls on overflow");
                const auto& scrolled = dock->GetSlots();
                const RECT movedApp = scrolled[0]->GetBounds();
                Require(!EqualRect(&firstApp, &movedApp), "wheel changes application position");
                const RECT movedRunning = scrolled[27]->GetBounds();
                const RECT movedFrequent = scrolled[29]->GetBounds();
                Require(!EqualRect(&running, &movedRunning) && !EqualRect(&frequent, &movedFrequent),
                    "running and frequent applications share the fixed-application scroll lane");
                const std::array<RECT, 4> after{scrolled[24]->GetBounds(), scrolled[25]->GetBounds(),
                    scrolled[26]->GetBounds(), dock->GetSearchRect()};
                for (size_t i = 0; i < fixed.size(); ++i)
                    Require(EqualRect(&fixed[i], &after[i]), "files and utility controls stay fixed while applications scroll");
                Require(!dock->ScrollByWheelDelta(center(after[0]), -120), "file lane cannot scroll the application lane");
                Require(dock->EntryAtPoint(center(after[0])) == scrolled[24]->GetItem(),
                    "fixed file remains interactive outside the scroll viewport");

                const RECT hidden = scrolled[23]->GetBounds();
                RECT intersection{};
                const RECT bounds = dock->GetBounds();
                Require(!IntersectRect(&intersection, &hidden, &bounds), "fixture has a fully hidden overflow application");
                Access::DockPointer(*profile.app, center(hidden));
                (void)dock->GetElementVisualRect(hidden, center(hidden));
                auto state = dock->CapturePresentationState();
                Require(IsRectEmpty(&state.focusRect), "hidden overflow application cannot acquire hover focus");

                const RECT start = dock->GetWindowsButtonRect();
                POINT pointer = center(start);
                if (vertical) pointer.y = start.bottom - 2;
                else pointer.x = start.right - 2;
                Access::DockPointer(*profile.app, pointer);
                state.focusRect = start;
                state.entry.SetHovered(true, 0, 1.0);
                state.entry.Advance(1000);
                state.single.SetTarget(start, 0, 1.0, vertical ? pointer.y : pointer.x);
                state.single.Advance(1000);
                Require(dock->RestorePresentationState(state), "restore deterministic hover geometry");
                const RECT startVisual = dock->GetElementVisualRect(start, pointer);
                Require(dock->GetElementIconSize(start, startVisual) > dock->GetElementIconSize(start, start),
                    "fixed Start control retains its hover effect over clipped applications");
                const LONG baseEnd = vertical ? start.bottom : start.right;
                const LONG visualEnd = vertical ? startVisual.bottom : startVisual.right;
                if (visualEnd > baseEnd + 2)
                {
                    POINT coveredPointer = center(startVisual);
                    if (vertical) coveredPointer.y = (baseEnd + visualEnd) / 2;
                    else coveredPointer.x = (baseEnd + visualEnd) / 2;
                    Access::DockPointer(*profile.app, coveredPointer);
                    (void)dock->GetElementVisualRect(start, coveredPointer);
                    const auto coveredState = dock->CapturePresentationState();
                    Require(EqualRect(&coveredState.focusRect, &start),
                        "painted Start retains hover above applications clipped behind its expanded bounds");
                    Require(!dock->ScrollByWheelDelta(coveredPointer, -120),
                        "expanded fixed Start cannot scroll applications underneath it");
                    Access::DockPointer(*profile.app, pointer);
                }
                bool coveredApplication = false;
                for (size_t i = 0; i < 24; ++i)
                {
                    const RECT application = scrolled[i]->GetBounds();
                    if (!IntersectRect(&intersection, &application, &start)) continue;
                    coveredApplication = true;
                    const RECT visual = dock->GetElementVisualRect(application, pointer);
                    Require(EqualRect(&application, &visual),
                        "clipped application beneath Start cannot take part in its hover geometry");
                }
                Require(coveredApplication, "fixture reproduces a scrolled application under the fixed Start control");
                const RECT panel = dock->GetVisualPanelBounds(pointer);
                Require(panel.left >= area.left && panel.top >= area.top &&
                    panel.right <= area.right && panel.bottom <= area.bottom,
                    "magnified independent panel stays in its reserved area away from adjacent glass");
                Require(vertical ? panel.bottom == bounds.bottom : panel.right == bounds.right,
                    "hidden trailing applications cannot lengthen the hovered panel");

                const RECT file = after[0];
                const POINT filePointer = center(file);
                Access::DockPointer(*profile.app, filePointer);
                state.focusRect = file;
                state.single.SetTarget(file, 0, 1.0, vertical ? filePointer.y : filePointer.x);
                state.single.Advance(1000);
                Require(dock->RestorePresentationState(state), "restore fixed file hover geometry");
                const RECT fileVisual = dock->GetElementVisualRect(file, filePointer);
                const LONG fileBegin = vertical ? file.top : file.left;
                const LONG fileVisualBegin = vertical ? fileVisual.top : fileVisual.left;
                if (fileVisualBegin < fileBegin - 2)
                {
                    POINT overlap = center(fileVisual);
                    if (vertical) overlap.y = (fileBegin + fileVisualBegin) / 2;
                    else overlap.x = (fileBegin + fileVisualBegin) / 2;
                    Access::DockPointer(*profile.app, overlap);
                    const auto* hit = dock->EntryAtPoint(overlap);
                    if (hit != scrolled[24]->GetItem())
                    {
                        const auto focus = dock->CapturePresentationState().focusRect;
                        const RECT currentVisual = dock->GetElementVisualRect(file, overlap);
                        const auto describe = [](const RECT& rect) {
                            return std::to_string(rect.left) + "," + std::to_string(rect.top) + "," +
                                std::to_string(rect.right) + "," + std::to_string(rect.bottom);
                        };
                        Require(false, "expanded fixed file owns input above clipped applications; position=" +
                            std::to_string(static_cast<int>(position)) + " attached=" + std::to_string(attached) +
                            " hover=" + std::to_string(hover) + " file=" + describe(file) +
                            " painted=" + describe(fileVisual) + " current=" + describe(currentVisual) +
                            " focus=" + describe(focus) + " pointer=" + std::to_string(overlap.x) + "," +
                            std::to_string(overlap.y) + " hit=" + (hit ? std::to_string(hit->GetEntryIndex()) : "none"));
                    }
                    Require(!dock->ScrollByWheelDelta(overlap, -120),
                        "expanded fixed file cannot scroll applications underneath it");
                }
            }
}
}
int main(int argc, char** argv)
{
    try {
        behavior::Require(argc == 2, "one named behavior is required");
        behavior::scene = argv[1];
        if (behavior::scene == "settings-host") behavior::SettingsHostCommit();
        else if (behavior::scene == "group-type-contract") behavior::GroupTypeContract();
        else if (behavior::scene == "search") behavior::Search();
        else if (behavior::scene == "native-dialog") behavior::NativeDialogs();
        else if (behavior::scene == "native-dialog-control") behavior::NativeDialogs(true);
        else if (behavior::scene == "drag-source") behavior::DragSource();
        else if (behavior::scene == "drag-files") behavior::DragFiles();
        else if (behavior::scene == "drag-batches") behavior::DragBatches();
        else if (behavior::scene == "drag-partial") behavior::DragPartial();
        else if (behavior::scene == "drag-release") behavior::DragRelease();
        else if (behavior::scene == "drag-popup") behavior::DragPopup();
        else if (behavior::scene == "widget-focus") behavior::WidgetAuxiliaryFocus();
        else if (behavior::scene == "dock-overflow") behavior::DockOverflow();
        else throw std::runtime_error("unknown behavior");
        std::cout << "PASS " << behavior::scene << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
