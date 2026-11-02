// Included after the real host fixture and its access adapter.
#include "navigation/search_match.h"
#include "navigation/quick_navigation_search_async.h"
#include "shell/shell_extension_service.h"
#include "diagnostics/diagnostic_log_modules.h"
#include <wrl/implements.h>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace behavior
{
// Real native modal loops, with only their user decision automated. No desktop
// host, Explorer, files, device backend or credentials are involved.
struct NativeDialogProgress
{
    static inline thread_local NativeDialogProgress* current = nullptr;
    static constexpr wchar_t title[] = L"SnowDesktop isolated dialog regression";
    snowdesktop::UiAnimationScheduler scheduler;
    DWORD thread = GetCurrentThreadId();
    ULONGLONG started = GetTickCount64();
    HWND window = nullptr;
    UINT_PTR watchdog = 0;
    int ticks = 0, frames = 0, commits = 0;
    bool wrongThread = false, timedOut = false;

    NativeDialogProgress()
    {
        Require(scheduler.Initialize(), "initialize real dialog scheduler");
        Require(scheduler.ScheduleInterval(10, [this](auto) {
            wrongThread |= GetCurrentThreadId() != thread;
            ++ticks;
        }) != 0, "schedule component deadline");
        Require(scheduler.StartAnimation(snowdesktop::UiAnimationSurface::Desktop,
            [this](double) {
                wrongThread |= GetCurrentThreadId() != thread;
                ++frames;
                return true;
            }) != 0, "schedule animation frames");
        watchdog = SetTimer(nullptr, 0, 10, +[](HWND, UINT, UINT_PTR, DWORD) {
            auto* progress = current;
            if (!progress) return;
            // MessageBox supplies no creation callback. Match only this
            // thread's active test dialog, never an external application.
            if (!progress->window)
            {
                const HWND active = GetActiveWindow();
                wchar_t caption[96]{};
                if (active && GetWindowTextW(active, caption, 96) &&
                    std::wstring_view(caption) == title)
                    progress->window = active;
            }
            const bool advanced = progress->ticks >= 2 &&
                progress->frames >= 2 && progress->commits >= 2;
            progress->timedOut = GetTickCount64() - progress->started >= 1500;
            if (progress->window && (advanced || progress->timedOut))
                PostMessageW(progress->window, WM_CLOSE, 0, 0);
        });
        Require(watchdog != 0, "start bounded dialog watchdog");
        current = this;
    }
    ~NativeDialogProgress()
    {
        KillTimer(nullptr, watchdog);
        current = nullptr;
    }
    TASKDIALOGCONFIG TaskDialog()
    {
        TASKDIALOGCONFIG dialog{sizeof(dialog)};
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        dialog.pszWindowTitle = title;
        dialog.pszContent = L"Checking component deadlines and animation while a dialog is open.";
        dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        dialog.nDefaultButton = IDCANCEL;
        dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(this);
        dialog.pfCallback = +[](HWND window, UINT notification, WPARAM, LPARAM, LONG_PTR data) -> HRESULT {
            if (notification == TDN_CREATED)
                reinterpret_cast<NativeDialogProgress*>(data)->window = window;
            return S_OK;
        };
        return dialog;
    }
};

void NativeDialogs(bool rawControl = false)
{
    Profile profile;
    Apartment apartment;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    Require(InitCommonControlsEx(&controls) != FALSE, "initialize native dialogs");
    for (const auto* kind : {"task", "message", "form", "modal-window"})
    {
        NativeDialogProgress progress;
        snowdesktop::operation_feedback::Session session(&progress.scheduler, [&] {
            progress.wrongThread |= GetCurrentThreadId() != progress.thread;
            ++progress.commits;
        });
        int selected = 0;
        if (std::string_view(kind) == "task")
        {
            auto dialog = progress.TaskDialog();
            const auto result = rawControl
                ? TaskDialogIndirect(&dialog, &selected, nullptr, nullptr)
                : snowdesktop::operation_feedback::ShowTaskDialog(&dialog, &selected, nullptr, nullptr);
            Require(SUCCEEDED(result), "show actual native task dialog");
        }
        else if (std::string_view(kind) == "message")
            selected = snowdesktop::operation_feedback::ShowMessageBox(nullptr,
                L"Checking a native warning dialog.", NativeDialogProgress::title, MB_OKCANCEL);
        else if (std::string_view(kind) == "form")
        {
            struct Template { DLGTEMPLATE dialog; WORD menu, windowClass, title; } form{};
            form.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
            form.dialog.cx = 240; form.dialog.cy = 60;
            const auto procedure = +[](HWND window, UINT message, WPARAM, LPARAM parameter) -> INT_PTR {
                if (message == WM_INITDIALOG)
                {
                    reinterpret_cast<NativeDialogProgress*>(parameter)->window = window;
                    return TRUE;
                }
                if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
                return FALSE;
            };
            selected = static_cast<int>(snowdesktop::operation_feedback::ShowDialog(
                GetModuleHandleW(nullptr), &form.dialog, nullptr, procedure,
                reinterpret_cast<LPARAM>(&progress)));
        }
        else
        {
            // Substitute only the Shell picker boundary. Its real Show call
            // still runs a native modal loop on the calling STA.
            struct Modal final : Microsoft::WRL::RuntimeClass<
                Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IModalWindow>
            {
                TASKDIALOGCONFIG dialog{};
                int selected = 0;
                HRESULT STDMETHODCALLTYPE Show(HWND owner) override
                {
                    dialog.hwndParent = owner;
                    return TaskDialogIndirect(&dialog, &selected, nullptr, nullptr);
                }
            };
            auto modal = Microsoft::WRL::Make<Modal>();
            modal->dialog = progress.TaskDialog();
            Require(SUCCEEDED(snowdesktop::operation_feedback::ShowModalWindow(modal.Get(), nullptr)),
                "show modal COM window on its STA");
            selected = modal->selected;
        }
        std::cout << "dialog=" << kind << " ticks=" << progress.ticks <<
            " frames=" << progress.frames << " commits=" << progress.commits << '\n';
        Require(selected == IDCANCEL, std::string(kind) + ": cancellation result is preserved");
        Require(!progress.timedOut && progress.ticks >= 2 && progress.frames >= 2 &&
            progress.commits >= 2 && !progress.wrongThread,
            std::string(kind) + ": component deadlines, animation and presentation must advance on the UI thread before the dialog closes");
        const auto ticks = progress.ticks, frames = progress.frames, commits = progress.commits;
        // Drain queued messages without dispatching the outer scheduler. A
        // retired dialog must no longer advance deadlines or presentation.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Require(progress.ticks == ticks && progress.frames == frames && progress.commits == commits,
            "closing the dialog retires its presentation bridge");
    }
}

void Search()
{
    HostProfile profile;
    std::vector<fs::path> files;
    for (const auto* name : {L"z-editor", L"editor notes", L"editor", L"unrelated"})
    {
        files.push_back(profile.desktop / name);
        Write(files.back(), "search-entry");
    }
    Require(Access::QuickNavigationNames(*profile.app, files, L"editor") ==
        std::vector<std::wstring>{L"editor", L"editor notes", L"z-editor"},
        "production search model filters misses and orders exact, prefix and interior matches");
    Require(NameMatchesQuery(L"文件管理器", L"wjglq") && !NameMatchesQuery(L"editor", L"unrelated"),
        "name-matching contract accepts pinyin initials and rejects an unrelated query");
    struct Gate { std::mutex mutex; std::condition_variable ready; bool entered = false, release = false; };
    auto gate = std::make_shared<Gate>();
    snowdesktop::QuickNavigationEverythingSearchAsync search([gate](const std::wstring& query, DWORD) {
        if (query == L"old")
        {
            std::unique_lock lock(gate->mutex); gate->entered = true; gate->ready.notify_all();
            if (!gate->ready.wait_for(lock, std::chrono::seconds(5), [&] { return gate->release; }))
                return snowdesktop::QuickNavigationEverythingSearchResponse{{},ERROR_TIMEOUT};
        }
        EverythingSearchResult entry; entry.name = query; entry.path = L"C:\\isolated\\" + query;
        return snowdesktop::QuickNavigationEverythingSearchResponse{{entry},ERROR_SUCCESS};
    });
    Require(search.Submit(nullptr, 0, {1,L"old",20}), "submit first query");
    { std::unique_lock lock(gate->mutex); Require(gate->ready.wait_for(lock,std::chrono::seconds(3),[&] { return gate->entered; }), "provider entered"); }
    Require(search.Submit(nullptr,0,{2,L"discarded",20}) && search.Submit(nullptr,0,{3,L"latest",20}), "typing remains responsive during blocked query");
    { std::lock_guard lock(gate->mutex); gate->release = true; gate->ready.notify_all(); }
    std::optional<snowdesktop::QuickNavigationEverythingSearchResult> result;
    Wait([&] { auto next=search.TakeCompleted(); if(next) result=std::move(next); return result && result->generation == 3; },
        "latest search result generation=3", [&] {
            return result ? "published generation=" + std::to_string(result->generation) : "no published result";
        });
    Require(result->query == L"latest" && result->results.size() == 1 && result->results[0].name == L"latest", "only newest pending query reaches published results");
    search.Stop(); Require(!search.Submit(nullptr,0,{4,L"after-stop",20}), "disposed search rejects further work");
}
// Diagnostic drafts immediately select the diagnostic sink for inspection.
// The real host adapter mirror and JSON persistence change on commit.
// No Windows setting or hotkey changes.
void SettingsHostCommit()
{
    HostProfile profile;
    // This fixture does not run DesktopApp's destructor. Match its explicit
    // pre-CRT shutdown while COM and the main thread are still alive.
    struct MenuServiceLifetime
    {
        ~MenuServiceLifetime() { snowdesktop::shell_extensions::SharedMenuService().Shutdown(); }
    } menuServiceLifetime;
    using namespace snowdesktop;
    auto& controller = Access::HostSettings(*profile.app);
    const auto before = controller.Snapshot()->values.general;
    auto draft = before;
    draft.diagnosticLogModules ^= diagnostics::ModuleBit(DiagnosticLogModule::Settings);
    controller.UpdateGeneral(draft,SettingsUpdateMode::Draft);
    Require(controller.FlushPending().Succeeded() &&
        Access::AppliedLogModules(*profile.app) == before.diagnosticLogModules &&
        diagnostics::IsLogEnabled(DiagnosticLogModule::Settings) ==
            ((draft.diagnosticLogModules & diagnostics::ModuleBit(DiagnosticLogModule::Settings)) != 0),
        "diagnostic draft selects its sink immediately without committing the host mirror");
    controller.UpdateGeneral(draft,SettingsUpdateMode::Preview);
    Require(controller.FlushPending().Succeeded() &&
        Access::AppliedLogModules(*profile.app) == before.diagnosticLogModules,
        "appearance preview cannot commit the logging preference");
    controller.RequestCommit(SettingsDomain::General);
    Require(controller.FlushPending().Succeeded() &&
        Access::AppliedLogModules(*profile.app) == draft.diagnosticLogModules &&
        diagnostics::IsLogEnabled(DiagnosticLogModule::Settings) ==
            ((draft.diagnosticLogModules & diagnostics::ModuleBit(DiagnosticLogModule::Settings)) != 0),
        "real host commit reconciles its logging mirror with the selected diagnostic sink");
    SettingsValues disk;
    Require(CreateNativeSettingsStore()->Load(disk).Succeeded() &&
        disk.general.diagnosticLogModules == draft.diagnosticLogModules,
        "real host commit persists the applied preference");

    // The real creation path must retain an unexecuted permission placeholder,
    // so first-time Add can proceed to consent. No dialog or network is needed.
    struct WidgetRuntimeLifetime
    {
        DesktopApp& app;
        ~WidgetRuntimeLifetime() { Access::ReleaseWidgetRuntime(app); }
    } widgetRuntimeLifetime{*profile.app};
    auto& engine = Access::InitializeWidgetRuntime(*profile.app);
    const auto package = profile.root / L"permission-package";
    Write(package / L"widget.json", R"({"schemaVersion":2,"apiVersion":2,
        "dataVersion":1,"id":"12345678-1234-5678-1234-567812345678",
        "slug":"creation-consent","version":"1.0.0","entry":"main.lua",
        "name":"Creation consent","permissions":["network.internet"]})");
    Write(package / L"main.lua", R"(return widget.define({
        setup=function() storage.set("started", "1") end,
        view=function() return {type="text",key="body",text="Ready"} end})");
    const std::wstring packageId = L"12345678-1234-5678-1234-567812345678";
    std::wstring installError;
    Require(WidgetEngine::InstallWidgetPackage((package / L"widget.json").wstring(), installError),
        "install private package requiring network consent");
    const auto installed = WidgetEngine::GetWidgetPackage(packageId);
    Require(installed && installed->permissionState == widget::PermissionDecisionState::Pending,
        "newly installed sensitive package starts with pending consent");
    const auto created = Access::CreateLuaWidget(*profile.app, packageId);
    Require(!created.empty(),
        "first Add must create a permission placeholder before requesting consent");
    Require(std::any_of(Access::Widgets(*profile.app).begin(), Access::Widgets(*profile.app).end(),
        [&](const auto& value) { return value.id == created && value.packageId == packageId; }),
        "permission placeholder is committed to the real desktop model");
    using widget_runtime::WidgetHostStateKind;
    Require(engine.GetWidgetHostState(created, packageId).kind == WidgetHostStateKind::PermissionPending &&
        engine.GetWidgets().empty() && engine.RuntimeGetStorageValue(created, "started").empty(),
        "pending consent cannot execute Lua or write widget storage");
    std::string permissionError;
    Require(engine.ApplyWidgetPermissionDecision(packageId, widget::PermissionDecisionState::Denied,
        {}, {}, permissionError), "record explicit denial in the private profile");
    Require(!engine.EnsureWidgetLoaded(created, packageId) &&
        engine.GetWidgetHostState(created, packageId).kind == WidgetHostStateKind::PermissionDenied &&
        engine.RuntimeGetStorageValue(created, "started").empty(),
        "denial preserves an unexecuted permission placeholder");
    Require(engine.ApplyWidgetPermissionDecision(packageId, widget::PermissionDecisionState::Granted,
        {"network.internet"}, {}, permissionError),
        "save explicit consent in the private profile: " + permissionError);
    const bool retried = engine.RetryWidget(created, packageId);
    Require(retried, "explicit consent loads the same previously blocked instance: " +
        engine.RuntimeGetStorageValue(created, "lastError"));
    Require(engine.GetWidgetHostState(created, packageId).kind == WidgetHostStateKind::Ready &&
        engine.RuntimeGetStorageValue(created, "started") == "1",
        "Lua setup executes after consent on the original desktop instance");
    const auto authorized = Access::CreateLuaWidget(*profile.app, packageId);
    Require(!authorized.empty() && authorized != created &&
        engine.GetWidgetHostState(authorized, packageId).kind == WidgetHostStateKind::Ready,
        "already authorized packages continue to create ready instances");

    const auto broken = profile.root / L"broken-package";
    Write(broken / L"widget.json", R"({"schemaVersion":2,"apiVersion":2,
        "dataVersion":1,"id":"64b52de4-173c-4c19-a878-9e2c0c68eb36",
        "slug":"creation-error","version":"1.0.0","entry":"main.lua",
        "name":"Creation error","permissions":[]})");
    Write(broken / L"main.lua", "error('controlled component load failure')");
    Require(WidgetEngine::InstallWidgetPackage((broken / L"widget.json").wstring(), installError),
        "install isolated runtime-error fixture");
    const std::wstring brokenId = L"64b52de4-173c-4c19-a878-9e2c0c68eb36";
    const auto failed = Access::CreateLuaWidget(*profile.app, brokenId);
    Require(!failed.empty() &&
        engine.GetWidgetHostState(failed, brokenId).kind == WidgetHostStateKind::LoadFailed,
        "ordinary Add retains the recoverable error placeholder for a failed script");
}
void GroupTypeContract()
{
    HostProfile profile(true);
    const auto desktopFile = profile.desktop / L"A.needle.txt";
    const auto desktopFolder = profile.desktop / L"Z.needle";
    const auto mapped = profile.root / L"mapped";
    const auto mappedFile = mapped / L"A.needle.txt";
    const auto mappedFolder = mapped / L"Z.needle";
    Write(desktopFile,"desktop-source-bytes"); Write(mappedFile,"mapped-source-bytes");
    fs::create_directories(desktopFolder); fs::create_directories(mappedFolder);
    // Each source starts file-before-folder, so both must actually reorder.
    Access::SeedGroupTypeSources(*profile.app,{desktopFile,desktopFolder},mapped);
    const std::vector<fs::path> expected{desktopFolder,desktopFile,mappedFolder,mappedFile};
    const auto check = [&](const char* phase) {
        const auto ordered = Access::GroupTypeOrder(*profile.app);
        const bool correctOrder = ordered.size() == expected.size() &&
            std::equal(ordered.begin(),ordered.end(),expected.begin(),expected.end(),
                [](const auto& a,const auto& b) { return fs::equivalent(a,b); });
        const auto& widgets = Access::Widgets(*profile.app);
        const auto group = std::find_if(widgets.begin(),widgets.end(),[](const auto& value) { return value.id==L"type-group"; });
        if (!correctOrder)
        {
            const auto print = [&](const char* label, const auto& paths) {
                std::cerr << label;
                for (const auto& path : paths)
                    std::cerr << ' ' << path.lexically_relative(profile.root).generic_string();
                std::cerr << '\n';
            };
            std::cerr << "group ordering phase=" << phase << '\n';
            print("expected:", expected); print("observed:", ordered);
            if (group != widgets.end())
                std::cerr << "sort=" << static_cast<int>(group->contentSortColumn) <<
                    " ascending=" << group->contentSortAscending <<
                    " active=" << WideToUtf8(group->activeCategoryId) << '\n';
            std::cerr << Access::AsyncState(*profile.app) << '\n';
        }
        Require(correctOrder, std::string(phase) +
            ": user contract: retain source sections and place folders before files inside each section");
        Require(group!=widgets.end() && group->activeCategoryId==L"type-desktop" &&
            group->childWidgetIds==std::vector<std::wstring>{L"type-desktop",L"type-mapped"},
            "sorting preserves original source order and active source");
        Require(Read(desktopFile)=="desktop-source-bytes" && Read(mappedFile)=="mapped-source-bytes" &&
            fs::is_empty(desktopFolder) && fs::is_empty(mappedFolder),
            "sorting and reload cannot change either source's files");
    };
    (void)Access::GroupTypeOrder(*profile.app,true); check("after sort");
    Require(Access::Save(*profile.app),"persist accepted grouped Type sort");
    const auto refresh = [&](bool disk) {
        const auto delivered = Access::asyncReadMessages;
        Access::RequestAsync(*profile.app,disk);
        Wait([&] { return Access::BackgroundIdle(*profile.app) && Access::asyncReadMessages>delivered; },
            disk ? "disk reload applied and background idle" : "refresh applied and background idle",
            [&] { return Access::AsyncState(*profile.app); });
    };
    refresh(false); check("after refresh");
    Access::DiscardGroupTypeRuntimeState(*profile.app);
    refresh(true); check("after disk reload");
}
}
