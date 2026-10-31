#include "app/app.h"
#include "widgets/widget_folder_search.h"
#include "ui/input/private_pointer.h"
#include "layout/folder_sort_rules.h"
#include "app/shell/shell_refresh_snapshot.h"
#include "desktop/desktop_namespace_registry.h"
#include "diagnostics/debug_profile.h"
#include "ole_file_source.h"
#include <fstream>
#include <iostream>
#include <exception>
#include <stdexcept>
// Supplies an isolated environment and observations; all routing, queues, I/O,
// completion handling and persistence below execute production implementations.
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
    static bool AsyncIdle(const DesktopApp& app)
    {
        return app.desktopItemsReady_ && !app.shellReloadPending_ && !app.shellRefreshRevision_.Running() &&
            !app.readyShellRefresh_ && !app.layoutReload_.Pending();
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
    static Container* Target(const DesktopApp& app) { return app.dragSession_.TargetContainer(); }
    static Slot* TargetSlot(const DesktopApp& app) { return app.dragSession_.TargetSlot(); }
    static HitRegion Region(const DesktopApp& app) { return app.dragSession_.TargetRegion(); }
    static Container* Desktop(const DesktopApp& app) { return app.containers_.front().get(); }
    static DropPreviewList ObserveOlePreview(DesktopApp& app, POINT point, int mods)
    {
        // Observe the target chosen by Enter/Over; do not repair it by resolving again.
        return app.GetCachedDesktopDropPreview(false, app.dragSession_.SourceList(),
            Target(app), TargetSlot(app), Region(app), mods, point);
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
    static void FreshExternalDwell(DesktopApp& app)
    {
        // UI timeout transitions are a separate sequence. Keep real OLE
        // routing/file work, with a fresh hover rather than an elapsed dwell.
        app.CancelCollectionPopupDwell(); app.CancelCollectionGroupTabDwell();
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
    static WidgetContainer* MemberContainer(DesktopApp& app, const std::wstring& id)
    {
        for (const auto& container : app.containers_)
            if (auto* widget = dynamic_cast<WidgetContainer*>(container.get());
                widget && widget->GetWidgetData()->id == id) return widget;
        throw std::runtime_error("native member fixture requires its real container");
    }
    static const auto& Widgets(const DesktopApp& app) { return app.widgets_; }
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
#include "container_access.inc"
#include "module_access.inc"
};

namespace behavior
{
namespace fs = std::filesystem;
using Access = NativeDragIntegrationAccess;
std::string scene;
void Require(bool ok, const std::string& message)
{ if (!ok) throw std::runtime_error(scene + ": " + message); }
void Write(const fs::path& file, std::string_view bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary); stream << bytes;
    Require(static_cast<bool>(stream), "write isolated input");
}
std::string Read(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    Require(static_cast<bool>(stream), "read physical output");
    return {std::istreambuf_iterator<char>(stream), {}};
}
template<class Predicate> void Wait(Predicate done)
{
    const auto deadline = GetTickCount64() + 12000;
    while (!done())
    {
        Require(GetTickCount64() < deadline, "bounded completion deadline");
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Access::ThrowAsyncException();
    }
}
bool Same(const GridCell& a, const GridCell& b)
{ return a.pageId == b.pageId && a.column == b.column && a.row == b.row; }
const DesktopItem& Find(DesktopApp& app, const fs::path& path)
{
    const auto& items = Access::Items(app);
    const auto it = std::find_if(items.begin(), items.end(), [&](const auto& v) {
        std::error_code error;
        return fs::equivalent(fs::path(v.parsingName), path, error) && !error;
    });
    if (it == items.end())
    {
        std::cerr << "Expected path: " << WideToUtf8(path.wstring()) << " model count=" << items.size() << '\n';
        for (const auto& item : items) std::cerr << "Observed: " << WideToUtf8(item.layoutKey) << " path=" << WideToUtf8(item.parsingName) << '\n';
        Require(false, "physical output must be present in refreshed model");
    }
    return *it;
}
struct Profile
{
    fs::path root, desktop;
    DesktopApp* app = nullptr;
    Profile()
    {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "initialize STA");
        GUID guid{}; Require(SUCCEEDED(CoCreateGuid(&guid)), "allocate profile identity");
        wchar_t id[40]{}; StringFromGUID2(guid, id, 40);
        root = fs::weakly_canonical(fs::temp_directory_path()) / (std::wstring(L"SnowBehavior-") + id);
        namespace debug = snowdesktop::debug_profile;
        const auto paths = debug::ResolvePaths(root / L"normal-data");
        debug::Configuration config; config.enabled = true; config.desktop = paths.defaultDesktop;
        std::string error;
        Require(debug::Write(paths, config, error) && debug::Initialize(paths.normalData, {}, error), "isolate profile: " + error);
        desktop = debug::Current().configuration.desktop;
        Access::completionWindow = CreateWindowExW(0, L"STATIC", L"Behavior completion", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(Access::completionWindow != nullptr, "create owned message-only window");
        // No Run(): no desktop window, hooks, Explorer or user's files are involved.
        app = &Access::Scene(false);
        fs::create_directories(root / L"layout");
        Access::ConfigureModel(*app, root / L"layout");
        Write(desktop / L"Sentinel.txt", "untouched");
        Access::PopulateModel(*app, {desktop / L"Sentinel.txt"}, 0);
        Access::PrepareReload(*app); Access::BindAsync(*app);
        Require(Access::Save(*app), "persist starting layout");
    }
    ~Profile()
    {
        if (app) Access::StopModel(*app);
        if (Access::completionWindow) DestroyWindow(Access::completionWindow);
        Access::completionWindow = nullptr;
        // The uninitialized host graph has process lifetime; its destructor would
        // shut down global Shell services which this fixture never initialized.
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root, error);
        if (!error && resolved.parent_path() == fs::weakly_canonical(fs::temp_directory_path()) &&
            resolved.filename().wstring().starts_with(L"SnowBehavior-")) fs::remove_all(resolved, error);
        CoUninitialize();
    }
};
fs::path LinkTarget(const fs::path& path)
{
    ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persist;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link.As(&persist)) && SUCCEEDED(persist->Load(path.c_str(), STGM_READ)), "read persisted shortcut");
    std::array<wchar_t, 32768> target{};
    Require(SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)), "resolve shortcut");
    return target.data();
}
std::vector<fs::path> Outputs(const Profile& p)
{
    std::vector<fs::path> result;
    for (const auto& entry : fs::directory_iterator(p.desktop))
        if (entry.path().filename() != L"Sentinel.txt") result.push_back(entry.path());
    return result;
}
void Reload(Profile& p, const std::vector<std::pair<fs::path, GridCell>>& expected)
{
    Require(Access::Save(*p.app), "save committed state");
    Access::PerturbBeforeDiskRead(*p.app);
    Access::RequestAsync(*p.app, true);
    Wait([&] { return Access::FileDropIdle(*p.app); });
    for (const auto& [path, cell] : expected)
        Require(Same(Find(*p.app, path).gridCell, cell), "disk reload must recover each committed landing");
    Require(Read(p.desktop / L"Sentinel.txt") == "untouched", "unrelated file survives");
}
void Submit(Profile& p, ole_file_source_fixture::Source& source, DWORD keys, DWORD allowed,
    DWORD expected, POINT point, bool cancel = false, DWORD enterKeys = MK_LBUTTON, bool deferredDefault = false)
{
    auto adapter = Access::ExternalAdapter(*p.app);
    auto effect = allowed;
    Require(SUCCEEDED(adapter->DragEnter(&source, enterKeys, Access::ExternalPoint(point), &effect)), "enter OLE boundary");
    Require(source.starts == 0 && (!source.asynchronous || source.fileReads == 0), "hover cannot render delayed files");
    if (cancel)
    {
        Require(SUCCEEDED(adapter->DragLeave()), "cancel through real OLE leave");
        return;
    }
    Access::FreshExternalDwell(*p.app);
    effect = allowed;
    const auto status = adapter->Drop(&source, keys, Access::ExternalPoint(point), &effect);
    Require(SUCCEEDED(status) && (deferredDefault ? (effect == DROPEFFECT_COPY || effect == DROPEFFECT_MOVE) : effect == expected),
        "release must honor latest gesture and source capabilities");
}
// The representative rows are independent product contracts, not a Cartesian
// enumeration: verbs, capability boundary, release-time change, cancel, and
// delayed multi-file extraction each protect a distinct failure.
void Files(std::string_view row)
{
    Require(row == "sync-copy" || row == "copy" || row == "move" || row == "default-move" || row == "link" || row == "batch" || row == "unsupported" || row == "cancel", "unknown file matrix row");
    Profile p;
    const bool delayed = row != "sync-copy";
    const bool rejected = row == "unsupported";
    const bool cancel = row == "cancel";
    const bool link = row == "link";
    const bool move = row == "move" || row == "default-move";
    const bool batch = row == "batch";
    const DWORD keys = row == "default-move" || cancel ? 0 : link ? MK_ALT : move ? MK_SHIFT : MK_CONTROL;
    const DWORD expected = rejected || cancel ? 0 : link ? DROPEFFECT_LINK : move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    const DWORD allowed = rejected ? DROPEFFECT_MOVE : DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    const auto file = p.root / L"source/Payload.txt";
    Write(file, "payload-bytes");
    std::vector<fs::path> files{file};
    if (batch) { files.push_back(p.root / L"source/Folder"); Write(files.back() / L"nested.txt", "nested-bytes"); }
    const auto before = Read(Access::LayoutPath(*p.app));
    auto source = ole_file_source_fixture::Make(files, delayed);
    Submit(p, *source.Get(), keys, allowed, expected, {150,150}, cancel, MK_LBUTTON | MK_SHIFT, row == "default-move");
    if (expected) Wait([&] { return Access::FileDropIdle(*p.app) && (!delayed || source->ends == 1) && Access::Items(*p.app).size() == 1 + files.size(); });
    const auto outputs = Outputs(p);
    Require(outputs.size() == (expected ? files.size() : 0), "exact physical output count");
    Require(!source->wrongApartment && !source->fileReadBeforeStart, "provider apartment and lease ordering");
    if (!expected)
    {
        Require(source->starts == 0 && source->ends == 0 && Read(file) == "payload-bytes" &&
            Read(Access::LayoutPath(*p.app)) == before, "cancel/reject has no I/O or saved-state side effects");
        return;
    }
    if (delayed) Require(source->starts == 1 && source->ends == 1 && SUCCEEDED(source->outcome), "one balanced successful source lease");
    std::vector<std::pair<fs::path, GridCell>> cells;
    for (const auto& original : files)
    {
        Require(fs::exists(original) == !move, "move removes source; copy/link retain it");
        const auto it = std::find_if(outputs.begin(), outputs.end(), [&](const auto& out) {
            return link ? fs::equivalent(LinkTarget(out), original) : out.filename() == original.filename();
        });
        Require(it != outputs.end(), "output identity belongs to its selected source");
        if (!link) Require(Read(original == file ? *it : *it / L"nested.txt") ==
            (original == file ? "payload-bytes" : "nested-bytes"), "output bytes survive actual I/O");
        cells.emplace_back(*it, Find(*p.app, *it).gridCell);
    }
    Require(cells.front().second.pageId == L"__page:1" && cells.front().second.column == 1 && cells.front().second.row == 1,
        "first output occupies the requested empty cell");
    if (batch) Require(!Same(cells[0].second, cells[1].second), "batch outputs cannot overlap");
    Reload(p, cells);
}
struct HeldRead
{
    MSG held{};
    ~HeldRead() { if (held.hwnd) DispatchMessageW(&held); }
    void Capture()
    {
        const auto deadline = GetTickCount64() + 12000;
        while (!held.hwnd)
        {
            Require(GetTickCount64() < deadline, "read completion gate deadline");
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG next{};
            while (PeekMessageW(&next, nullptr, 0, 0, PM_REMOVE))
            {
                if (next.hwnd == Access::completionWindow && next.message == kShellFileOperationCompletedMessage)
                {
                    const auto* result = reinterpret_cast<const snowdesktop::ShellFileOperationUiCompletion*>(next.lParam);
                    Require(result && !result->fileOperation && result->succeeded, "gate the real source read before file submission");
                    held = next; break;
                }
                DispatchMessageW(&next); Access::ThrowAsyncException();
            }
        }
    }
    void Deliver() { const auto next = held; held = {}; DispatchMessageW(&next); Access::ThrowAsyncException(); }
};
void Interleaved(bool failFirst)
{
    Profile p;
    const auto a = p.root / L"a/Same.txt", b = p.root / L"b/Same.txt";
    Write(a, "first"); Write(b, "second");
    auto first = ole_file_source_fixture::Make({a}, true), second = ole_file_source_fixture::Make({b}, true);
    Submit(p, *first.Get(), MK_CONTROL, 7, DROPEFFECT_COPY, {150,150});
    HeldRead gate; gate.Capture();
    Require(Outputs(p).empty() && first->ends == 0, "first request remains pending without committing");
    if (failFirst) Require(fs::remove(a), "remove only owned source to trigger actual commit failure");
    Submit(p, *second.Get(), MK_CONTROL, 7, DROPEFFECT_COPY, {350,250});
    Wait([&] { return second->ends == 1 && Access::FileDropIdle(*p.app) && Access::Items(*p.app).size() == 2; });
    auto outputs = Outputs(p);
    Require(outputs.size() == 1 && Read(outputs[0]) == "second" && first->ends == 0, "second completes independently before first");
    const auto secondPath = outputs[0]; const auto secondCell = Find(*p.app, secondPath).gridCell;
    Require(secondCell.column == 3 && secondCell.row == 2, "second request retains its own landing");
    Access::RequestAsync(*p.app);
    Wait([&] { return Access::FileDropIdle(*p.app); });
    gate.Deliver();
    Wait([&] { return first->ends == 1 && Access::FileDropIdle(*p.app) && Access::Items(*p.app).size() == (failFirst ? 2u : 3u); });
    Require(first->starts == 1 && first->ends == 1 && second->ends == 1 &&
        (failFirst ? FAILED(first->outcome) : SUCCEEDED(first->outcome)), "each request reports its own outcome once");
    outputs = Outputs(p);
    Require(outputs.size() == (failFirst ? 1u : 2u) && Read(secondPath) == "second" &&
        Same(Find(*p.app, secondPath).gridCell, secondCell), "late success/failure cannot overwrite or relocate second");
    std::vector<std::pair<fs::path, GridCell>> cells{{secondPath, secondCell}};
    if (!failFirst)
    {
        const auto it = std::find_if(outputs.begin(), outputs.end(), [&](const auto& out) { return Read(out) == "first"; });
        Require(it != outputs.end() && *it != secondPath, "same-name requests preserve distinct bytes and identities");
        const auto cell = Find(*p.app, *it).gridCell;
        Require(cell.column == 1 && cell.row == 1, "late first completion retains original landing");
        cells.emplace_back(*it, cell);
    }
    Reload(p, cells);
}
}
#include "container_scenarios.h"
#include "module_contracts.h"

int main(int argc, char** argv)
{
    try
    {
        behavior::Require(argc == 2, "one named behavior is required");
        behavior::scene = argv[1];
        if (behavior::scene == "settings") behavior::Settings();
        else if (behavior::scene == "settings-host") behavior::SettingsHostCommit();
        else if (behavior::scene == "manifest") behavior::BuildManifest();
        else if (behavior::scene == "group-type-contract") behavior::GroupTypeContract();
        else if (behavior::scene == "recovery") behavior::Recovery();
        else if (behavior::scene == "search") behavior::Search();
        else if (behavior::scene == "widget") behavior::WidgetContracts();
        else if (behavior::scene == "dock") behavior::DockLifecycle();
        else if (behavior::scene == "interleaved" || behavior::scene == "failed-first")
            behavior::Interleaved(behavior::scene == "failed-first");
        else if (behavior::RunContainerScenario(behavior::scene)) {}
        else behavior::Files(behavior::scene);
        std::cout << "PASS " << behavior::scene << '\n'; return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
