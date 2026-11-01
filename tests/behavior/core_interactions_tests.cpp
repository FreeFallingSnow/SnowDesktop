#include "app/app.h"
#include "widgets/widget_folder_search.h"
#include "ui/input/private_pointer.h"
#include "layout/folder_sort_rules.h"
#include "app/shell/shell_refresh_snapshot.h"
#include "desktop/desktop_namespace_registry.h"
#include "behavior_fixture.h"
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
    static LRESULT CALLBACK AsyncWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (asyncOwner && window == completionWindow &&
            (message == kBackgroundShellReadyMessage ||
                (message == WM_TIMER && (wp == kDesktopHostWatchTimerId || wp == kShellChangeTimerId))))
        {
            try {
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
        app.iconWork_.Stop();
        for (const auto* clsid : {kDesktopIconClsidThisPC, kDesktopIconClsidUserFiles, kDesktopIconClsidNetwork, kDesktopIconClsidControlPanel, kDesktopIconClsidRecycleBin, kDesktopIconClsidApplications}) app.settingsIconVisibility_[ToUpperInvariant(clsid)] = false;
        for (const auto& entry : snowdesktop::LoadDesktopNamespaceRegistrations()) app.settingsIconVisibility_[ToUpperInvariant(entry.clsid)] = false;
        asyncException = {}; asyncReadMessages = 0;
        previousWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(completionWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AsyncWindowProc)));
        if (!previousWindowProc) throw std::runtime_error("install fixture dispatcher");
        asyncOwner = &app;
        if (!SetTimer(completionWindow, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr)) throw std::runtime_error("start fixture timer");
    }
    static void UnbindAsync(DesktopApp& app)
    {
        if (asyncOwner != &app) return;
        KillTimer(completionWindow, kShellChangeTimerId); KillTimer(completionWindow, kDesktopHostWatchTimerId);
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
#include "module_access.inc"
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
    HostProfile()
    {
        try {
            window.handle = CreateWindowExW(0, L"STATIC", L"Behavior completion", 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
            Require(window.handle != nullptr, "create owned message-only window");
            Access::completionWindow = window.handle;
            // No Run(): no desktop window, hooks, Explorer or user's files are involved.
            app = &Access::Scene(false);
            fs::create_directories(root / L"layout");
            Access::ConfigureModel(*app, root / L"layout");
            Write(desktop / L"Sentinel.txt", "untouched");
            Access::PopulateModel(*app, {desktop / L"Sentinel.txt"}, 0);
            Access::PrepareReload(*app); Access::BindAsync(*app);
            Require(Access::Save(*app), "persist starting layout");
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
int main(int argc, char** argv)
{
    try {
        behavior::Require(argc == 2, "one named behavior is required");
        behavior::scene = argv[1];
        if (behavior::scene == "settings-host") behavior::SettingsHostCommit();
        else if (behavior::scene == "group-type-contract") behavior::GroupTypeContract();
        else if (behavior::scene == "search") behavior::Search();
        else throw std::runtime_error("unknown behavior");
        std::cout << "PASS " << behavior::scene << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
