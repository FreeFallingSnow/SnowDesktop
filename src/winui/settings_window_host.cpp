#include "pch.h"

#include "settings_window_host.h"
#include "../settings_search_catalog.h"
#include "../data_paths.h"
#include "../app_font.h"
#include "../pending_window_message.h"
#include "../performance_trace.h"
#include "../diagnostic_log.h"
#include "../shell_launch_worker.h"
#include "../status_bar_shell_shortcut.h"
#include "../theme_library_settings.h"
#include "../theme_workshop.h"
#include "../theme_preview.h"

#include "SettingsShell.xaml.h"
#include "winui_runtime.h"
#include "authoring_toolchain.h"
#include "../steam_app_identity.h"
#include "../widget_engine.h"
#include "../widget_settings_service.h"
#include "../widget_package_file_export.h"
#include "../utils.h"

#include <shobjidl.h>
#include <dwmapi.h>
#include <psapi.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <thread>
#include <stdexcept>
#include <vector>

namespace snowdesktop::winui
{
namespace mud = winrt::Microsoft::UI::Dispatching;
namespace muw = winrt::Microsoft::UI::Windowing;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace wf = winrt::Windows::Foundation;
namespace wui = winrt::Windows::UI;
namespace shell_impl = winrt::SnowDesktop::implementation;

namespace
{
constexpr wchar_t kSettingsWindowClassName[] =
    L"SnowDesktop.WinUI3.SettingsWindow";
constexpr int kDefaultClientWidth = 1100;
constexpr int kDefaultClientHeight = 760;
constexpr int kMinimumClientWidth = 840;
constexpr int kMinimumClientHeight = 520;
constexpr UINT kDispatchOwnerTaskMessage = WM_APP + 0x347;
constexpr UINT kApplyXamlBackdropMessage = WM_APP + 0x348;
constexpr UINT kUpdateIntegratedTitleBarInsetsMessage = WM_APP + 0x349;
constexpr UINT kRefreshExternalStateMessage = WM_APP + 0x34a;
constexpr UINT_PTR kWorkingSetTrimTimerIdSeed = 0x53440000;
constexpr UINT kWorkingSetTrimDelayMs = 2000;
constexpr ULONGLONG kWorkingSetTrimCooldownMs = 30000;
constexpr SIZE_T kWorkingSetTrimMinimumGrowth =
    static_cast<SIZE_T>(64) * 1024 * 1024;

bool HasSignificantWorkingSetGrowth(
    SIZE_T current, SIZE_T baseline) noexcept
{
    return current >= baseline &&
        current - baseline >= kWorkingSetTrimMinimumGrowth;
}

std::optional<SIZE_T> QueryCurrentProcessWorkingSet() noexcept
{
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(
            GetCurrentProcess(), &counters, sizeof(counters)))
    {
        return std::nullopt;
    }
    return counters.WorkingSetSize;
}

bool TrimProcessWorkingSet() noexcept
{
    // This releases pageable physical memory, not committed virtual memory.
    // The caller delays and rate-limits the process-wide operation so routine
    // settings visits do not evict active desktop, Dock, or widget pages.
    return SetProcessWorkingSetSize(GetCurrentProcess(),
               static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1)) != FALSE;
}

bool ActivateSettingsWindow(HWND window) noexcept
{
    if (!window || !IsWindow(window))
        return false;

    const auto requestActivation = [window]() {
        (void)SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(window);
        SetActiveWindow(window);
    };
    requestActivation();
    if (GetForegroundWindow() == window)
        return true;

    // The tray and single-instance messages can be dispatched after Windows
    // has transferred foreground ownership to another process. Match the
    // existing Dock activation policy: briefly share input queues only for a
    // retry against SnowDesktop's own responsive window, then always detach.
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground
        ? GetWindowThreadProcessId(foreground, nullptr)
        : 0;
    const DWORD currentThread = GetCurrentThreadId();
    const bool attached = foregroundThread != 0 &&
        foregroundThread != currentThread &&
        AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    if (attached)
    {
        requestActivation();
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
    return GetForegroundWindow() == window;
}

bool QueryHighContrastEnabled(bool& enabled) noexcept
{
    HIGHCONTRASTW state{};
    state.cbSize = sizeof(state);
    if (!SystemParametersInfoW(
            SPI_GETHIGHCONTRAST, sizeof(state), &state, 0))
    {
        enabled = false;
        return false;
    }
    enabled = (state.dwFlags & HCF_HIGHCONTRASTON) != 0;
    return true;
}

wf::IReference<wui::Color> BoxTitleBarColor(
    std::uint8_t alpha,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue)
{
    return winrt::box_value(wui::Color{alpha, red, green, blue})
        .as<wf::IReference<wui::Color>>();
}

DWORD WindowsBuildNumber() noexcept
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtlGetVersion)
        return 0;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return rtlGetVersion(&version) == 0 && version.dwMajorVersion >= 10
        ? version.dwBuildNumber
        : 0;
}

bool SupportsMicaBackdrop() noexcept
{
    return WindowsBuildNumber() >= 22000;
}

bool SupportsDwmSystemBackdrop() noexcept
{
    // DWMWA_SYSTEMBACKDROP_TYPE is formally supported starting with 22H2.
    return WindowsBuildNumber() >= 22621;
}

void ApplySettingsWindowChrome(HWND window, bool darkTheme) noexcept
{
    if (!window || !IsWindow(window))
        return;

    const BOOL dark = darkTheme ? TRUE : FALSE;
    (void)DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
        &dark, sizeof(dark));

    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    (void)DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));

    bool highContrast = false;
    const bool allowMaterial = QueryHighContrastEnabled(highContrast) &&
        !highContrast;
    const bool useMica = SupportsDwmSystemBackdrop() && allowMaterial;
    const DWM_SYSTEMBACKDROP_TYPE backdrop = useMica
        ? DWMSBT_MAINWINDOW
        : DWMSBT_NONE;
    (void)DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE,
        &backdrop, sizeof(backdrop));

    // Windows 11 21H2 can host Island Mica but lacks the public top-level
    // system-backdrop attribute. Match its base color so the native caption
    // and Windows-owned buttons still meet the panel without a bright seam.
    COLORREF captionColor = DWMWA_COLOR_DEFAULT;
    if (allowMaterial && SupportsMicaBackdrop() &&
        !SupportsDwmSystemBackdrop())
    {
        captionColor = darkTheme ? RGB(32, 32, 32) : RGB(243, 243, 243);
    }
    (void)DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR,
        &captionColor, sizeof(captionColor));
}



std::wstring FormatWin32Error(const wchar_t* operation, DWORD error)
{
    std::wstring result = operation ? operation : L"Win32 operation";
    result += L" (";
    result += std::to_wstring(error);
    result += L")";
    return result;
}

bool IsUsableControllerSnapshot(
    const ISettingsController::SnapshotPtr& snapshot) noexcept
{
    return snapshot && snapshot->initialized;
}

bool IsWidgetsBackendPage(SettingsPage page) noexcept
{
    return page == SettingsPage::Widgets ||
        page == SettingsPage::DeveloperTools;
}

std::optional<std::filesystem::path> DialogResultPath(
    IFileDialog* dialog)
{
    if (!dialog)
        return std::nullopt;
    winrt::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put())) || !item)
        return std::nullopt;
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) ||
        !rawPath)
    {
        return std::nullopt;
    }
    std::filesystem::path result(rawPath);
    CoTaskMemFree(rawPath);
    return result;
}

std::optional<std::filesystem::path> ShowOpenPathDialog(
    HWND owner,
    std::wstring_view title,
    const std::vector<std::pair<std::wstring, std::wstring>>& filters,
    bool chooseFolder)
{
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))) || !dialog)
    {
        return std::nullopt;
    }

    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)))
        return std::nullopt;
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    options |= chooseFolder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    if (FAILED(dialog->SetOptions(options)))
        return std::nullopt;
    if (!title.empty())
        (void)dialog->SetTitle(std::wstring(title).c_str());

    std::vector<COMDLG_FILTERSPEC> specifications;
    specifications.reserve(filters.size());
    for (const auto& [name, pattern] : filters)
        specifications.push_back({name.c_str(), pattern.c_str()});
    if (!specifications.empty() && FAILED(dialog->SetFileTypes(
            static_cast<UINT>(specifications.size()),
            specifications.data())))
    {
        return std::nullopt;
    }
    if (FAILED(dialog->Show(owner)))
        return std::nullopt;
    return DialogResultPath(dialog.get());
}

std::optional<std::filesystem::path> ShowSavePathDialog(
    HWND owner,
    std::wstring_view title,
    std::wstring_view suggestedFileName,
    const std::vector<std::pair<std::wstring, std::wstring>>& filters,
    std::wstring_view defaultExtension)
{
    winrt::com_ptr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))) || !dialog)
    {
        return std::nullopt;
    }
    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)))
        return std::nullopt;
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
        FOS_OVERWRITEPROMPT;
    if (FAILED(dialog->SetOptions(options)))
        return std::nullopt;
    if (!title.empty())
        (void)dialog->SetTitle(std::wstring(title).c_str());
    if (!suggestedFileName.empty())
        (void)dialog->SetFileName(std::wstring(suggestedFileName).c_str());
    if (!defaultExtension.empty())
    {
        (void)dialog->SetDefaultExtension(
            std::wstring(defaultExtension).c_str());
    }

    std::vector<COMDLG_FILTERSPEC> specifications;
    specifications.reserve(filters.size());
    for (const auto& [name, pattern] : filters)
        specifications.push_back({name.c_str(), pattern.c_str()});
    if (!specifications.empty() && FAILED(dialog->SetFileTypes(
            static_cast<UINT>(specifications.size()),
            specifications.data())))
    {
        return std::nullopt;
    }
    if (FAILED(dialog->Show(owner)))
        return std::nullopt;
    return DialogResultPath(dialog.get());
}
} // namespace

struct SettingsWindowHost::Impl
{
    struct CallbackState
    {
        std::atomic<bool> alive{true};
        std::atomic<bool> snapshotQueued{false};
        std::atomic<bool> flushQueued{false};
        std::mutex snapshotMutex;
        ISettingsController::SnapshotPtr latestSnapshot;
        mud::DispatcherQueue dispatcher{nullptr};
        Impl* owner = nullptr;
    };

    DWORD ownerThreadId = 0;
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    ISettingsController* controller = nullptr;
    widget_runtime::IWidgetSettingsService* widgetSettingsService = nullptr;
    WidgetEngine* widgetEngine = nullptr;
    SettingsWindowHostOptions options;
    WinUiRuntime runtime;
    winrt::com_ptr<shell_impl::SettingsShell> shell;
    muw::AppWindow appWindow{nullptr};
    muw::AppWindowTitleBar appWindowTitleBar{nullptr};
    SettingsSearchIndex searchIndex;
    std::shared_ptr<CallbackState> callbacks;
    std::unique_ptr<IWidgetsPageBackend> widgetsPageBackend;
    std::unique_ptr<IBackupDataPageBackend> backupDataPageBackend;
    std::uint64_t viewEpoch = 0;
    bool widgetsPageActive = false;
    SettingsPage widgetsBackendPage = SettingsPage::Home;
    bool backupDataPageActive = false;
    bool initialized = false;
    bool shuttingDown = false;
    bool applyingSnapshot = false;
    bool interactionSuspended = true;
    bool darkTheme = false;
    bool viewReleaseQueued = false;
    bool workingSetTrimQueued = false;
    std::uint64_t workingSetTrimEpoch = 0;
    UINT_PTR nextWorkingSetTrimTimerId = kWorkingSetTrimTimerIdSeed;
    UINT_PTR activeWorkingSetTrimTimerId = 0;
    std::optional<SIZE_T> settingsSessionWorkingSetBaseline;
    ULONGLONG lastWorkingSetTrimTick = 0;
    /** Legacy five-click About unlock; retained for this host lifetime. */
    DebugPageSession debugSession;
    bool systemBackdropUpdateQueued = false;
    bool integratedTitleBarInsetsUpdateQueued = false;
    bool externalStateRefreshQueued = false;
    std::wstring lastError;

    struct ThemeTask
    {
        std::atomic_bool cancel{false};
        std::uint64_t generation = 0;
        ThemeLibraryRequest request;
        unsigned previewScope = themes::All;
        themes::Package snapshot;
        steam_bridge::ThemePublishPlan plan;
        std::filesystem::path directory, cover, customCover;
        std::function<void(ThemeLibraryResult)> completed;
        ~ThemeTask() { if (!directory.empty()) { std::error_code ec; std::filesystem::remove_all(directory, ec); } }
    };
    std::shared_ptr<ThemeTask> themeTask;
    std::map<std::string, std::filesystem::path> themeCovers;

    std::filesystem::path ThemeBridge() const { return std::filesystem::path(GetExecutableDirectoryPath()) / L"SnowDesktopSteamBridge.exe"; }
    bool ThemeSharingAvailable() const { return themes::workshop::Available(ThemeBridge(), SNOWDESKTOP_VERSION); }
    void CompleteThemeTask(const std::shared_ptr<ThemeTask>& task, bool success, std::string error)
    {
        if (themeTask != task) return;
        ThemeLibraryResult result;
        result.succeeded = success; result.sharingAvailable = ThemeSharingAvailable();
        std::string ignored;
        if (!themes::Load(themes::LibraryPath(), result.library, ignored)) result.succeeded = false;
        result.message = L(success ? "themeLibrary.success" :
            error == "cancelled" ? "themeLibrary.cancelled" :
            error == "agreementRequired" ? "themeLibrary.agreementRequired" :
            error == "authorMismatch" ? "themeLibrary.authorMismatch" :
            error == "stalePreparation" ? "themeLibrary.stalePreview" :
            error == "creationUncertain" ? "themeLibrary.creationUncertain" : "themeLibrary.operationFailed");
        if (shell) shell->HideProgress(task->generation);
        auto completed = std::move(task->completed);
        themeTask.reset();
        if (completed) completed(std::move(result));
    }
    bool CurrentThemeTask(const std::shared_ptr<ThemeTask>& task) const
    {
        if (themeTask != task || task->cancel.load() || !shell || !controller ||
            !controller->IsGenerationCurrent(task->generation)) return false;
        const auto snapshot = controller->Snapshot();
        return snapshot && snapshot->sessionActive && !snapshot->externalReplacementPending;
    }
    void PublishThemeTask(const std::shared_ptr<ThemeTask>& task)
    {
        if (!CurrentThemeTask(task)) { CompleteThemeTask(task, false, "cancelled"); return; }
        std::string error; themes::Library library; themes::Package latest;
        if (!ThemeSharingAvailable() || !themes::Load(themes::LibraryPath(), library, error) ||
            !themes::Export(library, task->request.id, latest, error) ||
            themes::EncodePackage(latest, error) != themes::EncodePackage(task->snapshot, error))
        { CompleteThemeTask(task, false, "stalePreparation"); return; }
        (void)shell->ShowProgress({task->generation, L("themeLibrary.share"), L("themeLibrary.preparing"), true, 0, true});
        const auto weak = std::weak_ptr<CallbackState>(callbacks);
        const auto bridge = ThemeBridge(), data = std::filesystem::path(GetDataDirectoryPath());
        std::thread([weak, task, bridge, data] {
            std::string output, detail; bool success = false;
            try { success = themes::workshop::Publish(bridge, task->plan, data, output, detail, &task->cancel); }
            catch (...) { detail = "publishFailed"; }
            if (const auto state = weak.lock(); state && state->alive.load())
                (void)state->dispatcher.TryEnqueue([weak, task, success, detail] {
                    if (const auto current = weak.lock(); current && current->alive.load() && current->owner)
                        current->owner->CompleteThemeTask(task, success, detail);
                });
        }).detach();
    }
    void ShowThemeTask(const std::shared_ptr<ThemeTask>& task)
    {
        if (!CurrentThemeTask(task)) { CompleteThemeTask(task, false, "cancelled"); return; }
        shell->HideProgress(task->generation);
        const auto theme = themes::Resolve(task->snapshot, task->request.id);
        if (!theme) { CompleteThemeTask(task, false, "stalePreparation"); return; }
        shell_impl::SettingsShellDialogRequest dialog;
        dialog.generation = task->generation; dialog.title = winrt::to_hstring(theme->name).c_str();
        dialog.closeButtonText = L("settings.dialog.cancel"); dialog.previewImagePath = task->cover.wstring();
        const bool share = task->request.command == ThemeLibraryCommand::Share;
        dialog.defaultClose = share;
        dialog.message = L("themeLibrary.fixedPreview");
        if (theme->kind == themes::Kind::Global)
        {
            for (const auto& [bit, key] : std::vector<std::pair<unsigned, const char*>>{
                {themes::Components,"themeLibrary.components"}, {themes::Dock,"themeLibrary.dock"},
                {themes::StatusBar,"themeLibrary.statusBar"}, {themes::Taskbar,"themeLibrary.taskbar"}})
                if (task->previewScope & bit) dialog.message += L"\n" + L(key);
            for (const auto& [id, key] : std::vector<std::pair<std::string, const char*>>{
                {theme->quickPanel,"themeLibrary.quickPanel"}, {theme->popup,"themeLibrary.popup"}})
                if (const auto child = themes::Resolve(task->snapshot, id); child && task->previewScope == theme->scopes)
                    dialog.message += L"\n" + L(key) + L": " + std::wstring(winrt::to_hstring(child->name));
        }
        if (share)
        {
            dialog.message += L"\n\n" + L("themeLibrary.confirmShare");
            dialog.primaryButtonText = L(task->plan.publishedFileId ? "themeLibrary.publishUpdate" : "themeLibrary.publish");
        }
        const auto weak = std::weak_ptr<CallbackState>(callbacks);
        shell->ShowConfirmation(std::move(dialog), [weak, task, share](bool confirmed) {
            if (const auto state = weak.lock(); state && state->alive.load() && state->owner)
            {
                if (share && confirmed) state->owner->PublishThemeTask(task);
                else state->owner->CompleteThemeTask(task, !share, share ? "cancelled" : "");
            }
        });
    }
    void BeginThemeTask(std::uint64_t generation, ThemeLibraryRequest request,
        std::function<void(ThemeLibraryResult)> completed)
    {
        if (themeTask || !controller || !controller->IsGenerationCurrent(generation) || !shell)
        { if (completed) completed({}); return; }
        auto task = std::make_shared<ThemeTask>(); task->generation = generation;
        task->request = std::move(request); task->completed = std::move(completed); themeTask = task;
        if (!CurrentThemeTask(task)) { CompleteThemeTask(task, false, "cancelled"); return; }
        const bool sync = task->request.command == ThemeLibraryCommand::SyncSubscriptions;
        const bool share = task->request.command == ThemeLibraryCommand::Share;
        if ((share || sync) && !ThemeSharingAvailable()) { CompleteThemeTask(task, false, "missingCapability"); return; }
        std::string error; themes::Library library;
        if (!sync)
        {
            if (task->request.id.empty() && task->request.command == ThemeLibraryCommand::Preview &&
                (task->request.target == "dock" || task->request.target == "taskbar"))
            {
                auto theme = themes::CaptureTarget(task->request.target, controller->Snapshot()->values);
                theme.id = themes::CreateId();
                theme.name = winrt::to_string(L(task->request.target == "dock" ? "themeLibrary.dock" : "themeLibrary.taskbar"));
                theme.scopes = themes::All; theme.quickPanel = "builtin/quickpanel/dark"; theme.popup = "builtin/popup/dark";
                task->request.id = theme.id; task->snapshot.emplace(theme.id, std::move(theme));
            }
            else if (!themes::Load(themes::LibraryPath(), library, error) ||
                !themes::Export(library, task->request.id, task->snapshot, error))
            { CompleteThemeTask(task, false, error); return; }
        }
        if (task->request.command == ThemeLibraryCommand::ChooseCover)
        {
            const auto selected = ShowOpenPathDialog(window, L("themeLibrary.chooseCover"),
                {{L("themeLibrary.chooseCover"), L"*.png;*.jpg;*.jpeg;*.bmp"}}, false);
            if (!selected) { CompleteThemeTask(task, false, "cancelled"); return; }
            themeCovers[task->request.id] = *selected;
            while (themeCovers.size() > 16) themeCovers.erase(themeCovers.begin());
        }
        if (task->request.command == ThemeLibraryCommand::Regenerate) themeCovers.erase(task->request.id);
        if (const auto selected = themeCovers.find(task->request.id); selected != themeCovers.end()) task->customCover = selected->second;
        wchar_t temporary[MAX_PATH + 1]{};
        if (!GetTempPathW(MAX_PATH, temporary)) { CompleteThemeTask(task, false, "writeFailed"); return; }
        task->directory = std::filesystem::path(temporary) / (L"SnowDesktop-theme-" + std::wstring(winrt::to_hstring(themes::CreateId())));
        (void)shell->ShowProgress({generation, L(sync ? "themeLibrary.sync" : "themeLibrary.preview"), L("themeLibrary.preparing"), true, 0, true});
        const auto weak = std::weak_ptr<CallbackState>(callbacks);
        const auto host = std::filesystem::path(GetExecutableDirectoryPath()) / L"SnowDesktop.exe", bridge = ThemeBridge(), data = std::filesystem::path(GetDataDirectoryPath());
        const auto libraryPath = themes::LibraryPath();
        std::thread([weak, task, sync, share, host, bridge, data, libraryPath] {
            const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool success = false; std::string detail;
            try
            {
                if (sync) { themes::Library result; success = themes::workshop::Sync(bridge, libraryPath, result, detail, &task->cancel); }
                else
                {
                    const auto theme = themes::Resolve(task->snapshot, task->request.id);
                    const auto scope = theme && theme->kind == themes::Kind::Global ?
                        (share || task->request.target == "global" ? theme->scopes : themes::TargetScope(task->request.target)) : themes::All;
                    task->previewScope = scope;
                    if (share)
                    {
                        success = themes::workshop::Prepare(task->snapshot, task->request.id, scope, task->directory, data,
                            task->customCover, [host](const auto& package, auto root, auto applicable, const auto& directory,
                                auto& cover, auto& error, auto* cancel) {
                                return themes::preview::Render(host, package, root, applicable, directory, cover, error, cancel);
                            }, task->plan, detail, &task->cancel);
                        task->cover = task->plan.preview;
                    }
                    else
                    {
                        success = themes::preview::Render(host, task->snapshot, task->request.id, scope, task->directory, task->cover, detail, &task->cancel);
                        if (success && !task->customCover.empty())
                        {
                            success = themes::preview::NormalizeCover(task->customCover, task->cover, detail);
                        }
                    }
                }
            }
            catch (...) { detail = "previewFailed"; }
            if (SUCCEEDED(initialized)) CoUninitialize();
            if (const auto state = weak.lock(); state && state->alive.load())
                (void)state->dispatcher.TryEnqueue([weak, task, success, detail, sync] {
                    if (const auto current = weak.lock(); current && current->alive.load() && current->owner)
                    {
                        if (!success || sync) current->owner->CompleteThemeTask(task, success, detail);
                        else current->owner->ShowThemeTask(task);
                    }
                });
        }).detach();
    }

    [[nodiscard]] bool OnOwnerThread() const noexcept
    {
        return ownerThreadId != 0 &&
            ownerThreadId == GetCurrentThreadId();
    }

    [[nodiscard]] bool Visible() const noexcept
    {
        return window && IsWindow(window) &&
            IsWindowVisible(window) != FALSE;
    }

    [[nodiscard]] bool DebugPageVisible()
    {
        // Reopening settings during an experiment also unlocks this settings
        // session. Turning the experiment off must not remove its own page.
        return debugSession.Visible(options.debugVisible && options.debugVisible());
    }

    std::wstring L(std::string_view key) const
    {
        if (options.localize)
        {
            std::wstring value = options.localize(key);
            if (!value.empty())
                return value;
        }
        return {};
    }

    void SetError(std::wstring message)
    {
        lastError = std::move(message);
        WriteDiagnosticLogEntry((L"SettingsUI pid=" + std::to_wstring(GetCurrentProcessId()) +
            L" error: " + lastError).c_str(), DiagnosticLogLevel::Error);
    }

    void QueueSystemBackdropUpdate() noexcept
    {
        if (systemBackdropUpdateQueued || shuttingDown || !window ||
            !IsWindow(window) || !runtime.IsAttached())
        {
            return;
        }

        // MicaBackdrop construction can synchronously wait for the current
        // DispatcherQueue. Post back to the HWND so Attach has returned and
        // SnowDesktop's normal message pump has advanced at least once.
        if (PostMessageW(window, kApplyXamlBackdropMessage, 0, 0))
        {
            systemBackdropUpdateQueued = true;
            return;
        }

        // A failed post must leave a usable opaque surface, particularly if a
        // high-contrast transition is trying to remove an existing backdrop.
        (void)runtime.SetSystemBackdropEnabled(false);
        if (shell)
            shell->SetSystemBackdropActive(false);
    }

    void ApplyDeferredSystemBackdrop() noexcept
    {
        systemBackdropUpdateQueued = false;
        if (shuttingDown || !runtime.IsAttached() || !shell)
            return;

        bool highContrast = false;
        const bool shouldEnable = SupportsMicaBackdrop() &&
            QueryHighContrastEnabled(highContrast) && !highContrast;
        bool active = false;
        if (shouldEnable)
            active = runtime.SetSystemBackdropEnabled(true);
        else
            (void)runtime.SetSystemBackdropEnabled(false);

        // The root becomes transparent only after the Island accepted Mica.
        // Windows 10, high contrast, and platform failures retain a current
        // theme-resource solid brush instead.
        shell->SetSystemBackdropActive(active);
    }

    void RefreshExternalStateNow() noexcept
    {
        if (shuttingDown || !OnOwnerThread() || !controller || !shell)
            return;
        try
        {
            if (options.refreshExternalState)
                options.refreshExternalState();
            ApplySnapshotNow(controller->Snapshot());
            shell->RefreshRuntimeState();
        }
        catch (...)
        {
        }
    }

    void QueueExternalStateRefresh() noexcept
    {
        if (externalStateRefreshQueued || shuttingDown || !Visible())
            return;
        if (PostMessageW(window, kRefreshExternalStateMessage, 0, 0))
            externalStateRefreshQueued = true;
    }

    void ApplyDeferredExternalStateRefresh() noexcept
    {
        externalStateRefreshQueued = false;
        if (Visible())
            RefreshExternalStateNow();
    }

    void ApplyIntegratedTitleBarButtonColors() noexcept
    {
        if (!appWindowTitleBar)
            return;

        try
        {
            bool highContrast = false;
            if (!QueryHighContrastEnabled(highContrast) || highContrast)
            {
                const wf::IReference<wui::Color> systemDefault{nullptr};
                appWindowTitleBar.ButtonBackgroundColor(systemDefault);
                appWindowTitleBar.ButtonInactiveBackgroundColor(
                    systemDefault);
                appWindowTitleBar.ButtonHoverBackgroundColor(systemDefault);
                appWindowTitleBar.ButtonPressedBackgroundColor(systemDefault);
                return;
            }

            // With content extended into the caption, transparent resting
            // backgrounds let the same XAML/Mica surface continue underneath
            // all three Windows-owned caption buttons. Theme-aware overlays
            // preserve pointer feedback without reintroducing a solid strip.
            const auto transparent = BoxTitleBarColor(0, 0, 0, 0);
            const auto hover = darkTheme
                ? BoxTitleBarColor(24, 255, 255, 255)
                : BoxTitleBarColor(15, 0, 0, 0);
            const auto pressed = darkTheme
                ? BoxTitleBarColor(15, 255, 255, 255)
                : BoxTitleBarColor(25, 0, 0, 0);
            appWindowTitleBar.ButtonBackgroundColor(transparent);
            appWindowTitleBar.ButtonInactiveBackgroundColor(transparent);
            appWindowTitleBar.ButtonHoverBackgroundColor(hover);
            appWindowTitleBar.ButtonPressedBackgroundColor(pressed);
        }
        catch (...)
        {
            // Keep Windows defaults if color customization is unavailable.
        }
    }

    [[nodiscard]] bool ConfigureIntegratedTitleBar()
    {
        if (!window || !IsWindow(window) ||
            !muw::AppWindowTitleBar::IsCustomizationSupported())
        {
            SetError(L"Integrated settings title bar is not supported");
            return false;
        }

        try
        {
            const auto windowId =
                winrt::Microsoft::UI::GetWindowIdFromWindow(window);
            appWindow = muw::AppWindow::GetFromWindowId(windowId);
            if (!appWindow)
            {
                SetError(L"Get AppWindow for settings window failed");
                return false;
            }
            appWindowTitleBar = appWindow.TitleBar();
            if (!appWindowTitleBar)
            {
                SetError(L"Get settings AppWindowTitleBar failed");
                return false;
            }
            // Use the WinAppSDK-owned full-customization path: XAML supplies
            // the title-bar content while Windows keeps caption buttons,
            // the default drag region, maximize/Snap behavior and their
            // accessibility providers. The title bar has no interactive
            // content, so the platform-owned default drag region must not be
            // replaced with custom caption rectangles.
            appWindowTitleBar.ExtendsContentIntoTitleBar(true);
            appWindowTitleBar.PreferredHeightOption(
                muw::TitleBarHeightOption::Standard);
            appWindowTitleBar.IconShowOptions(
                muw::IconShowOptions::HideIconAndSystemMenu);
            appWindowTitleBar.PreferredTheme(darkTheme
                    ? muw::TitleBarTheme::Dark
                    : muw::TitleBarTheme::Light);
            ApplyIntegratedTitleBarButtonColors();
            return true;
        }
        catch (const winrt::hresult_error& error)
        {
            SetError(L"Configure integrated settings title bar (" +
                std::to_wstring(
                    static_cast<unsigned int>(error.code().value)) +
                L")");
        }
        catch (...)
        {
            SetError(L"Configure integrated settings title bar failed");
        }
        appWindowTitleBar = nullptr;
        appWindow = nullptr;
        return false;
    }

    void UpdateIntegratedTitleBarInsets() noexcept
    {
        integratedTitleBarInsetsUpdateQueued = false;
        if (shuttingDown || !runtime.IsAttached() || !shell ||
            !window || !IsWindow(window) || !appWindowTitleBar ||
            !appWindowTitleBar.ExtendsContentIntoTitleBar())
        {
            return;
        }

        try
        {
            const UINT dpi = GetDpiForWindow(window);
            const double scale = dpi > 0
                ? static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI
                : 1.0;
            shell->SetIntegratedTitleBarInsets(
                appWindowTitleBar.LeftInset() / scale,
                appWindowTitleBar.RightInset() / scale);
        }
        catch (...)
        {
            // Retain the last valid padding if caption metrics are temporarily
            // unavailable during attach, DPI or restore changes.
        }
    }

    void QueueIntegratedTitleBarInsetsUpdate() noexcept
    {
        if (integratedTitleBarInsetsUpdateQueued || shuttingDown ||
            !window || !IsWindow(window) || !runtime.IsAttached() ||
            !appWindowTitleBar)
        {
            return;
        }
        if (PostMessageW(
                window, kUpdateIntegratedTitleBarInsetsMessage, 0, 0))
        {
            integratedTitleBarInsetsUpdateQueued = true;
        }
    }

    void ResetIntegratedTitleBar() noexcept
    {
        integratedTitleBarInsetsUpdateQueued = false;
        if (appWindowTitleBar)
        {
            try
            {
                appWindowTitleBar.ResetToDefault();
            }
            catch (...)
            {
            }
        }
        appWindowTitleBar = nullptr;
        appWindow = nullptr;
    }

    void ApplyActualTheme(bool isDark) noexcept
    {
        if (shuttingDown || !OnOwnerThread())
            return;

        darkTheme = isDark;
        ApplySettingsWindowChrome(window, darkTheme);
        if (appWindowTitleBar)
        {
            try
            {
                appWindowTitleBar.PreferredTheme(darkTheme
                        ? muw::TitleBarTheme::Dark
                        : muw::TitleBarTheme::Light);
                ApplyIntegratedTitleBarButtonColors();
            }
            catch (...)
            {
            }
        }
    }

    void ShowActionError(const SettingsActionResult& result)
    {
        if (!shell || !controller || result.Succeeded())
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot)
            return;
        std::wstring title = L("settings.status.error");
        if (title.empty())
            title = L"Settings";
        std::wstring message = result.message;
        if (message.empty())
            message = L("settings.status.saveFailed");
        (void)shell->ShowInfoForGeneration(snapshot->generation,
            shell_impl::SettingsShellInfoSeverity::Error,
            std::move(title), std::move(message));
    }

    [[nodiscard]] bool DispatchToOwner(std::function<void()> task)
    {
        if (!task || shuttingDown || !callbacks ||
            !callbacks->alive.load())
        {
            return false;
        }
        try
        {
            if (callbacks->dispatcher.TryEnqueue(task))
                return true;
        }
        catch (...)
        {
        }

        if (!window || !IsWindow(window))
            return false;
        auto* queued = new (std::nothrow) std::function<void()>(
            std::move(task));
        if (!queued)
            return false;
        if (PostMessageW(window, kDispatchOwnerTaskMessage, 0,
                reinterpret_cast<LPARAM>(queued)))
        {
            return true;
        }
        delete queued;
        return false;
    }

    void DiscardPostedOwnerTasks() noexcept
    {
        if (!window)
            return;
        MSG message{};
        while (snowdesktop::TakePendingWindowMessage(message, window,
            kDispatchOwnerTaskMessage) == snowdesktop::PendingWindowMessage::Ready)
        {
            delete reinterpret_cast<std::function<void()>*>(message.lParam);
        }
    }

    void ShowGenerationConfirmation(
        std::uint64_t generation,
        std::wstring title,
        std::wstring message,
        std::function<void(bool)> completed,
        bool destructive = true,
        std::wstring primaryButtonText = {})
    {
        if (!shell || !controller ||
            !controller->IsGenerationCurrent(generation))
        {
            if (completed)
                completed(false);
            return;
        }
        shell_impl::SettingsShellDialogRequest request;
        request.generation = generation;
        request.title = std::move(title);
        request.message = std::move(message);
        request.primaryButtonText = primaryButtonText.empty()
            ? L("settings.dialog.confirm")
            : std::move(primaryButtonText);
        request.closeButtonText = L("settings.dialog.cancel");
        request.destructive = destructive;
        shell->ShowConfirmation(std::move(request), std::move(completed));
    }

    SettingsSearchIndexInput BuildSearchInput()
    {
        SettingsSearchIndexInput input;
        if (options.searchInput)
            input = options.searchInput();

        input.developerToolsVisible = options.developerToolsVisible &&
            options.developerToolsVisible();
        input.debugVisible = DebugPageVisible();
        const bool advancedFeaturesVisible =
            options.advancedFeatureStatus &&
            options.advancedFeatureStatus().cardVisible;
        if (input.languageTag.empty())
            input.languageTag = "runtime";

        if (input.staticSettings.empty())
        {
            PopulateSettingsSearchCatalog(input, [this](std::string_view key) { return L(key); },
                advancedFeaturesVisible, StatusBarSupportsSystemQuickSettings());
        }
        if (!advancedFeaturesVisible)
        {
            for (auto& descriptor : input.staticSettings)
            {
                if (descriptor.focusId == "general.advancedFeatures")
                    descriptor.visible = false;
            }
        }
        return input;
    }


    void RebuildSearchIndex()
    {
        if (!shell)
        {
            searchIndex = {};
            return;
        }
        try
        {
            SettingsSearchIndexInput input = BuildSearchInput();
            searchIndex.Rebuild(input);
            if (shell)
            {
                shell->SetConditionalPagesVisible(
                    input.developerToolsVisible, input.debugVisible);
            }
        }
        catch (...)
        {
            SetError(L"Rebuild settings search index failed");
        }
    }

    void QueueSnapshot(ISettingsController::SnapshotPtr snapshot)
    {
        if (!callbacks || !snapshot)
            return;
        {
            std::lock_guard lock(callbacks->snapshotMutex);
            callbacks->latestSnapshot = std::move(snapshot);
        }
        if (callbacks->snapshotQueued.exchange(true))
            return;

        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            if (!callbacks->dispatcher.TryEnqueue(
                    [weak]() {
                        const auto state = weak.lock();
                        if (!state)
                            return;
                        state->snapshotQueued.store(false);
                        // The immutable snapshot carries generation/revision
                        // stale-result protection. Keeping this queue bound to
                        // one rendered-view epoch could discard a newer
                        // coalesced snapshot after a visible-window Open.
                        if (!state->alive.load() || !state->owner)
                            return;
                        ISettingsController::SnapshotPtr latest;
                        {
                            std::lock_guard lock(state->snapshotMutex);
                            latest = std::move(state->latestSnapshot);
                        }
                        state->owner->ApplySnapshotNow(std::move(latest));
                    }))
            {
                callbacks->snapshotQueued.store(false);
            }
        }
        catch (...)
        {
            callbacks->snapshotQueued.store(false);
        }
    }

    void ApplySnapshotNow(ISettingsController::SnapshotPtr snapshot)
    {
        performance::Scope performanceScope("settings", "snapshot.apply");
        if (!shell || !snapshot || shuttingDown)
            return;
        if (!Visible() && !snapshot->sessionActive)
            return;
        if (applyingSnapshot)
        {
            // Synchronous IPC queries may deliver a newer snapshot while a
            // presenter is being constructed/activated. Render it on the next
            // owner turn rather than rebuilding controls on their own stack.
            QueueSnapshot(std::move(snapshot));
            return;
        }
        applyingSnapshot = true;
        struct ApplyGuard
        {
            bool& applying;
            ~ApplyGuard() { applying = false; }
        } applyGuard{applyingSnapshot};
        try
        {
            if (!shell->ApplySnapshot(*snapshot))
                return;
            // Ordinary controller revisions must not touch the Island's window-
            // level backdrop. In particular, continuous Slider/ColorPicker
            // previews publish here while WinUI owns pointer capture or a Flyout.
            // Backdrop refresh remains tied to attach and system theme/contrast
            // messages, where a window-level material transition is intentional.
            SynchronizePageBackends(*snapshot);
            if (options.homeAboutStatus)
            {
                HomeAboutStatusPatch patch = options.homeAboutStatus(
                    snapshot->generation, snapshot->revision);
                // Preserve the host's status sequence; it also orders direct
                // publications while the controller snapshot is unchanged.
                (void)shell->ApplyHomeAboutStatusPatch(patch);
            }
        }
        catch (const winrt::hresult_error& error)
        {
            SetError(L"Apply settings snapshot: " + std::wstring(error.message().c_str()));
        }
        catch (...)
        {
            SetError(L"Apply settings snapshot failed");
        }
    }

    void QueuePendingFlush()
    {
        if (!callbacks || callbacks->flushQueued.exchange(true))
            return;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            if (!callbacks->dispatcher.TryEnqueue(
                    [weak]() {
                        const auto state = weak.lock();
                        if (!state)
                            return;
                        state->flushQueued.store(false);
                        // Controller work belongs to the application session,
                        // not to one rendered route. A visible-window Open can
                        // legitimately advance the rendered-view epoch first;
                        // runs; dropping the only pending notification there
                        // would strand preview/commit work until shutdown.
                        if (!state->alive.load() || !state->owner)
                            return;
                        state->owner->FlushPendingNow();
                    }))
            {
                callbacks->flushQueued.store(false);
            }
        }
        catch (...)
        {
            callbacks->flushQueued.store(false);
        }
    }

    void FlushPendingNow()
    {
        if (!controller || shuttingDown)
            return;
        const SettingsActionResult result = controller->FlushPending();
        if (!result.Succeeded())
            ShowActionError(result);
        // A successful preview/commit already publishes revisioned snapshots
        // for the affected presenters. Refreshing every localized label here
        // rewrites the active XAML tree on the DispatcherQueue turn following
        // Slider.ValueChanged/ColorChanged, which releases pointer capture and
        // dismisses flyouts after their first value. Localization refreshes are
        // reserved for initialization, reopen, and ApplyLanguageChange.
    }

    std::wstring BackupConfirmationMessage(
        BackupDataConfirmationKind kind) const
    {
        switch (kind)
        {
        case BackupDataConfirmationKind::RestoreLayoutBackup:
            return L("settings.backup.restoreLayout.confirm");
        case BackupDataConfirmationKind::DeleteLayoutBackup:
            return L("settings.backup.deleteLayout.confirm");
        case BackupDataConfirmationKind::ImportAndRestoreFullBackup:
            return L("app.settings.restore_backup_file_confirm");
        case BackupDataConfirmationKind::RestoreFullBackup:
            return L("app.settings.restore_full_backup_confirm");
        case BackupDataConfirmationKind::DeleteFullBackup:
            return L("app.settings.delete_full_backup_confirm");
        case BackupDataConfirmationKind::MigrateData:
            return L("app.settings.migrate_data_confirm");
        case BackupDataConfirmationKind::ClearLayout:
            return L("settings.backup.clearData.confirm");
        }
        return {};
    }

    std::wstring BackupConfirmationTitle(
        BackupDataConfirmationKind kind) const
    {
        switch (kind)
        {
        case BackupDataConfirmationKind::RestoreLayoutBackup:
        case BackupDataConfirmationKind::DeleteLayoutBackup:
            return L("app.settings.layout_backups");
        case BackupDataConfirmationKind::MigrateData:
            return L("app.settings.data_migration");
        case BackupDataConfirmationKind::ClearLayout:
            return L("settings.backup.clearData");
        default:
            return L("app.settings.full_data_backups");
        }
    }

    void RefreshAgentSkillNavigationState() noexcept
    {
        if (!shell)
            return;

        bool updateAvailable = false;
        try
        {
            const auto packagePaths = WidgetEngine::GetWidgetPackagePaths();
            const auto bundledSkill =
                packagePaths.builtin / L"snowdesktop-lua-widget";
            const auto bundledCli =
                bundledSkill / L"bin" / L"snowwidget.exe";
            for (auto target :
                snowdesktop::steam_bridge::DefaultAgentSkillTargets())
            {
                std::string error;
                const auto status =
                    snowdesktop::steam_bridge::InspectAgentSkill(
                        bundledSkill, bundledCli, std::move(target), error);
                if (status.state == snowdesktop::steam_bridge::
                        SkillInstallState::UpdateAvailable)
                {
                    updateAvailable = true;
                    break;
                }
            }
        }
        catch (...)
        {
            updateAvailable = false;
        }
        shell->SetAgentSkillUpdateAvailable(updateAvailable);
    }

    bool SetWidgetDeveloperToolsEnabled(std::uint64_t generation, bool enabled)
    {
            EditGeneral(
                generation, SettingsUpdateMode::PreviewAndCommit,
                [enabled](GeneralSettings& settings) {
                    settings.widgetDeveloperToolsEnabled = enabled;
                });
            const auto snapshot = controller->Snapshot();
            const bool applied = snapshot &&
                snapshot->generation == generation &&
                snapshot->values.general.widgetDeveloperToolsEnabled ==
                    enabled;
            if (applied)
            {
                // Publish the new toggle state back to the cached Widgets
                // presenter before another click can derive its next value.
                // Without this refresh, disabling and then re-enabling from
                // the same page keeps using the stale pre-click snapshot.
                if (widgetsPageBackend)
                    (void)widgetsPageBackend->Refresh();
                // Keep the conditional NavigationView item and its search
                // entries in sync before the presenter optionally navigates
                // to Developer Tools on this same click.
                RebuildSearchIndex();
            }
            return applied;
    }

    void ConfigureWidgetsPageBackend()
    {
        if (!shell || !callbacks || (!widgetEngine && !options.createWidgetsBackend) || widgetsPageBackend)
            return;
        const std::weak_ptr<CallbackState> weak = callbacks;
        auto configured = options.widgetsPage;
        configured.localize = [weak](std::string_view key) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner
                ? state->owner->L(key)
                : std::wstring{};
        };
        configured.dispatchToOwner = [weak](std::function<void()> task) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner &&
                state->owner->DispatchToOwner(std::move(task));
        };
        configured.diagnosticsVisible = [weak]() {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner)
                return false;
            const auto snapshot = state->owner->controller
                ? state->owner->controller->Snapshot() : nullptr;
            if (!snapshot || !snapshot->sessionActive)
                return false;
            const auto& hostOptions = state->owner->options;
            if (snapshot->route.page == SettingsPage::DeveloperTools)
            {
                return hostOptions.developerToolsVisible &&
                    hostOptions.developerToolsVisible();
            }
            if (snapshot->route.page == SettingsPage::Debug)
            {
                return state->owner->DebugPageVisible();
            }
            return false;
        };
        configured.snapshotChanged = [weak](
            std::shared_ptr<const WidgetsPageSnapshot> snapshot) {
            const auto state = weak.lock();
            if (state && state->alive.load() && state->owner &&
                state->owner->shell && snapshot)
            {
                (void)state->owner->shell->ApplyWidgetsPageSnapshot(
                    *snapshot);
            }
        };
        configured.pickPackage = [weak](std::uint64_t generation,
                                     WidgetsPageBackendOptions::
                                         PackagePickerCompletion completed) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                if (completed)
                    completed(std::nullopt);
                return;
            }
            auto selected = ShowOpenPathDialog(state->owner->window,
                state->owner->L("app.settings.widgets_install_package"),
                {{state->owner->L("app.settings.widgets_install_package"),
                    L"*.snowwidget;*.snowtheme"}}, false);
            if (completed)
                completed(std::move(selected));
        };
        configured.exportDevelopmentPackage = [weak](std::uint64_t generation,
            std::filesystem::path projectRoot, std::string packageId, std::string version,
            WidgetsPageBackendOptions::PackageExportCompletion completed) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                if (completed) completed(std::nullopt);
                return;
            }
            const std::wstring title = state->owner->L("app.settings.widgets_export_package");
            auto selected = ShowSavePathDialog(state->owner->window, title,
                projectRoot.filename().wstring() + L"-" + Utf8ToWide(version) + L".snowwidget",
                {{title, L"*.snowwidget"}}, L"snowwidget");
            if (!selected || !state->alive.load() || !state->owner ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                if (completed) completed(std::nullopt);
                return;
            }
            const std::wstring successText = state->owner->L("app.settings.widgets_export_package_success");
            const std::wstring failureText = state->owner->L("app.settings.widgets_export_package_failed");
            const std::wstring operationError = state->owner->L("app.settings.widgets_error_operation_failed");
            const auto formatFeedback = [](std::wstring text, std::wstring_view detail) {
                const auto placeholder = text.find(L"{0}");
                if (placeholder != std::wstring::npos)
                    text.replace(placeholder, 3, detail);
                else
                    text += L"\n" + std::wstring(detail);
                return text;
            };
            const auto dispatcher = state->dispatcher;
            const auto done = std::make_shared<WidgetsPageBackendOptions::PackageExportCompletion>(
                std::move(completed));
            try
            {
                std::thread([weak, dispatcher, done, projectRoot = std::move(projectRoot),
                    packageId = std::move(packageId), version = std::move(version),
                    output = std::move(*selected), successText, failureText, operationError, formatFeedback]() {
                    WidgetsPageHostOperationResult result;
                    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    try
                    {
                        if (FAILED(apartment))
                            throw std::runtime_error("export worker COM initialization failed");
                        const auto exported = snowdesktop::widget::detail::ExportDevelopmentPackageFile(
                            projectRoot, output, packageId, version);
                        result.succeeded = exported.succeeded;
                        std::wstring detail = exported.succeeded ? output.wstring()
                            : Utf8ToWide(exported.report.Ok() ? exported.error : exported.report.ToJson());
                        result.message = formatFeedback(
                            exported.succeeded ? successText : failureText, detail);
                    }
                    catch (const std::exception& exception)
                    {
                        result = WidgetsPageHostOperationResult::Failure(
                            formatFeedback(failureText, Utf8ToWide(exception.what())));
                    }
                    catch (...)
                    {
                        result = WidgetsPageHostOperationResult::Failure(formatFeedback(failureText, operationError));
                    }
                    if (SUCCEEDED(apartment)) CoUninitialize();
                    // The worker owns all file IO inputs. A closed settings
                    // process/view cannot receive a callback through a raw owner.
                    try
                    {
                        (void)dispatcher.TryEnqueue([weak, done, result = std::move(result)]() mutable {
                            const auto live = weak.lock();
                            if (live && live->alive.load() && *done)
                                (*done)(std::move(result));
                        });
                    }
                    catch (...) {} // The settings dispatcher may already be closed.
                }).detach();
            }
            catch (...)
            {
                if (*done) (*done)(WidgetsPageHostOperationResult::Failure(formatFeedback(failureText, operationError)));
            }
        };
        configured.confirmInstall = [weak](std::uint64_t generation,
                                        WidgetInstallConfirmationRequest request,
                                        WidgetsPageBackendOptions::
                                            ConfirmationCompletion completed) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->shell || !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                if (completed)
                    completed(false);
                return;
            }
            state->owner->shell->ShowWidgetInstallConfirmation(generation,
                std::move(request),
                std::move(completed));
        };

        widgetsPageBackend = options.createWidgetsBackend
            ? options.createWidgetsBackend(std::move(configured))
            : std::make_unique<WidgetsPageBackend>(*widgetEngine, std::move(configured));
        WidgetsPageActions actions;
        actions.invoke = [weak](std::uint64_t generation,
                             WidgetsPageRequest request) {
            const auto state = weak.lock();
            if (state && state->alive.load() && state->owner &&
                state->owner->widgetsPageBackend)
            {
                (void)state->owner->widgetsPageBackend->Invoke(
                    generation, std::move(request));
            }
        };
        actions.navigate = [weak](std::uint64_t generation,
                               SettingsRoute route) {
            const auto state = weak.lock();
            if (state && state->alive.load() && state->owner &&
                state->owner->controller &&
                state->owner->controller->IsGenerationCurrent(generation))
            {
                state->owner->RequestRoute(route);
            }
        };
        actions.setDeveloperToolsEnabled = [weak](
            std::uint64_t generation, bool enabled) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return false;
            }
            return state->owner->SetWidgetDeveloperToolsEnabled(generation, enabled);
        };
        actions.reloadWidgetInstance = [weak](
                                           std::uint64_t generation,
                                           std::wstring instanceId) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation) ||
                !state->owner->options.developerToolsVisible ||
                !state->owner->options.developerToolsVisible())
            {
                return;
            }
            SettingsHostActions::Request request;
            request.action =
                SettingsHostActions::Action::ReloadWidgetInstance;
            request.widgetInstanceId = std::move(instanceId);
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
            if (result.Succeeded() && state->owner->widgetsPageBackend)
                (void)state->owner->widgetsPageBackend->Refresh();
        };
        actions.confirm = [weak](std::uint64_t generation,
                              std::wstring title,
                              std::wstring message,
                              std::wstring primaryButtonText,
                              WidgetsPageActions::ConfirmationCompletion done) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner)
            {
                if (done)
                    done(false);
                return;
            }
            state->owner->ShowGenerationConfirmation(generation,
                std::move(title), std::move(message), std::move(done), true,
                std::move(primaryButtonText));
        };
        actions.editPermissions = [weak](std::uint64_t generation,
                                      WidgetPermissionEditorRequest request,
                                      WidgetsPageActions::
                                          PermissionEditorCompletion done) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->shell || !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                if (done) done({});
                return;
            }
            state->owner->shell->ShowWidgetPermissionEditor(generation,
                std::move(request), std::move(done));
        };
        shell->SetWidgetsPageActions(std::move(actions));
    }

    void ConfigureBackupDataPageBackend()
    {
        if (!shell || !callbacks || !controller || backupDataPageBackend)
            return;
        const std::weak_ptr<CallbackState> weak = callbacks;
        auto configured = options.backupDataPage;
        if (!configured.openPath)
        {
            configured.openPath = [this](HWND owner, const std::filesystem::path& path) {
                // This same-executable helper is also available in the
                // independent settings process. Success means dispatched.
                return snowdesktop::ShellLaunchWorker::ExecuteInteractive(owner, path.wstring(), nullptr)
                    ? SettingsActionResult::Success()
                    : SettingsActionResult::Failure(L("settings.backup.error.openLocation"));
            };
        }
        configured.ownerWindow = [weak]() -> HWND {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner
                ? state->owner->window
                : nullptr;
        };
        configured.postToUi = [weak](std::function<void()> task) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner &&
                state->owner->DispatchToOwner(std::move(task));
        };
        configured.localize = [weak](std::string_view key) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner
                ? state->owner->L(key)
                : std::wstring{};
        };
        configured.confirm = [weak](HWND,
                                 BackupDataConfirmationRequest request,
                                 BackupDataPageActions::
                                     ConfirmationCompletion completed) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller)
            {
                if (completed)
                    completed(false);
                return;
            }
            const auto snapshot = state->owner->controller->Snapshot();
            if (!snapshot)
            {
                if (completed)
                    completed(false);
                return;
            }
            state->owner->ShowGenerationConfirmation(snapshot->generation,
                state->owner->BackupConfirmationTitle(request.kind),
                state->owner->BackupConfirmationMessage(request.kind),
                std::move(completed), true,
                request.kind == BackupDataConfirmationKind::ClearLayout
                    ? state->owner->L("settings.backup.clearData.action")
                    : std::wstring{});
        };
        configured.pickPath = [weak](HWND owner,
                                  BackupDataPickerRequest request,
                                  BackupDataPageActions::PickerCompletion done) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner)
            {
                if (done)
                    done(std::nullopt);
                return;
            }
            std::optional<std::filesystem::path> selected;
            switch (request.kind)
            {
            case BackupDataPickerKind::ImportFullBackupArchive:
                selected = ShowOpenPathDialog(owner,
                    state->owner->L(
                        "app.settings.restore_from_backup_file"),
                    {{state->owner->L(
                          "app.settings.backup_archive_file_type"),
                         L"*.snowbackup;*.zip"},
                        {state->owner->L("app.settings.snowbackup_file_type"),
                         L"*.snowbackup"},
                        {state->owner->L("app.settings.zip_file_type"),
                         L"*.zip"}},
                    false);
                break;
            case BackupDataPickerKind::ExportFullBackupArchive:
                selected = ShowSavePathDialog(owner,
                    state->owner->L("app.settings.export_backup"),
                    request.suggestedFileName,
                    {{state->owner->L("app.settings.snowbackup_file_type"),
                         L"*.snowbackup"},
                        {state->owner->L("app.settings.zip_file_type"),
                         L"*.zip"}},
                    L"snowbackup");
                break;
            case BackupDataPickerKind::MigrationSourceDirectory:
                selected = ShowOpenPathDialog(owner,
                    state->owner->L("app.settings.select_migration_data"),
                    {}, true);
                break;
            }
            if (done)
                done(std::move(selected));
        };

        backupDataPageBackend = options.createBackupBackend
            ? options.createBackupBackend(std::move(configured))
            : std::make_unique<BackupDataPageBackend>(*controller, std::move(configured));
        backupDataPageBackend->SetSnapshotChangedCallback(
            [weak](const BackupDataPageSnapshot& snapshot) {
                const auto state = weak.lock();
                if (state && state->alive.load() && state->owner &&
                    state->owner->shell)
                {
                    (void)state->owner->shell->ApplyBackupDataPageSnapshot(
                        snapshot);
                }
            });
        shell->SetBackupDataPageActions(backupDataPageBackend->Actions());
    }

    void EnsurePageBackends()
    {
        ConfigureBackupDataPageBackend();
        ConfigureWidgetsPageBackend();
    }

    void DisposePageBackends() noexcept
    {
        widgetsPageActive = false;
        widgetsBackendPage = SettingsPage::Home;
        backupDataPageActive = false;
        if (widgetsPageBackend)
        {
            auto backend = std::move(widgetsPageBackend);
            backend->SetSnapshotChangedCallback({});
            backend->Close();
        }
        if (backupDataPageBackend)
        {
            // Must precede ISettingsController::CloseSession: a completed
            // replacement can discard dirty state or reload the layout here.
            backupDataPageBackend->Close();
            backupDataPageBackend.reset();
        }
        if (shell)
        {
            shell->SetWidgetsPageActions({});
            shell->SetBackupDataPageActions({});
        }
    }

    void SynchronizePageBackends(const SettingsSnapshot& snapshot)
    {
        if (themeTask && (!snapshot.sessionActive || snapshot.externalReplacementPending || snapshot.generation != themeTask->generation))
        {
            auto task = themeTask; task->cancel.store(true);
            CompleteThemeTask(task, false, "cancelled");
        }
        if (!snapshot.sessionActive || shuttingDown)
            return;
        EnsurePageBackends();

        const bool showWidgets = IsWidgetsBackendPage(snapshot.route.page);
        if (showWidgets && widgetsPageBackend)
        {
            if (widgetsPageActive &&
                widgetsBackendPage != snapshot.route.page)
            {
                widgetsPageBackend->Deactivate();
                widgetsPageActive = false;
            }
            if (!widgetsPageActive ||
                !widgetsPageBackend->IsGenerationCurrent(
                    snapshot.generation))
            {
                widgetsPageActive = widgetsPageBackend->Activate(
                    snapshot.generation,
                    snapshot.route.page == SettingsPage::Widgets);
                if (widgetsPageActive)
                    widgetsBackendPage = snapshot.route.page;
            }
        }
        else if (widgetsPageActive && widgetsPageBackend)
        {
            widgetsPageBackend->Deactivate();
            widgetsPageActive = false;
            widgetsBackendPage = SettingsPage::Home;
        }

        const bool showBackup =
            snapshot.route.page == SettingsPage::BackupAndData;
        if (showBackup && backupDataPageBackend)
        {
            const auto current = backupDataPageBackend->CurrentSnapshot();
            if (!backupDataPageActive || !current.initialized ||
                current.generation != snapshot.generation)
            {
                backupDataPageBackend->Activate(snapshot.generation);
                backupDataPageActive = true;
            }
        }
        else if (backupDataPageActive && backupDataPageBackend)
        {
            backupDataPageBackend->Deactivate();
            backupDataPageActive = false;
        }
    }

    void ConfigurePageActions()
    {
        if (!shell || !callbacks)
            return;
        const std::weak_ptr<CallbackState> weak = callbacks;

        GeneralPageActions general;
        general.commitGeneral = [weak](std::uint64_t generation,
                                      GeneralPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, SettingsUpdateMode::Commit, std::move(edit));
            }
        };
        general.commitNavigation = [weak](
            std::uint64_t generation,
            GeneralPageActions::NavigationEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditNavigation(
                    generation, SettingsUpdateMode::Commit, std::move(edit));
            }
        };
        general.commitDock = [weak](std::uint64_t generation,
                                   GeneralPageActions::DockEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditDock(
                    generation, SettingsUpdateMode::Commit, std::move(edit));
            }
        };
        general.probeHotkey = [weak](
            SettingsHostActions::HotkeyTarget target,
            HotkeyChord chord,
            std::uint64_t generation,
            std::uint64_t,
            HotkeyRecorder::AvailabilityCompletion completion) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                completion(false, L"");
                return;
            }
            SettingsHostActions::Request request;
            request.action = SettingsHostActions::Action::
                ProbeHotkeyAvailability;
            request.hotkeyTarget = target;
            request.modifiers = chord.modifiers;
            request.virtualKey = chord.virtualKey;
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            completion(result.Succeeded(), result.message);
        };
        general.languageCatalog = [weak]() {
            std::vector<SettingsLanguageOption> result;
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->options.languageCatalog)
            {
                return result;
            }
            for (auto&& [code, label] :
                state->owner->options.languageCatalog())
            {
                result.push_back(
                    {std::move(code), std::move(label)});
            }
            return result;
        };
        general.setAutoStart = [weak](
                                   std::uint64_t generation,
                                   bool enabled) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            SettingsHostActions::Request request;
            request.action =
                SettingsHostActions::Action::SetAutoStartEnabled;
            request.boolValue = enabled;
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        general.queryStartupConflict = [weak]() {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->options.startupConflict)
            {
                return GeneralStartupConflict{};
            }
            return state->owner->options.startupConflict();
        };
        general.queryAdvancedFeatureStatus = [weak]() {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->options.advancedFeatureStatus)
            {
                return GeneralAdvancedFeatureStatus{};
            }
            return state->owner->options.advancedFeatureStatus();
        };
        general.registerAdvancedFeatures = [weak]() {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->options.registerAdvancedFeatures)
            {
                return;
            }
            state->owner->options.registerAdvancedFeatures();
        };
        general.openAdvancedFeaturesStore = [weak]() {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner)
                return;
            const std::wstring uri = snowdesktop::SnowDesktopSteamStoreUrl();
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                    state->owner->window, L"open", uri.c_str(), nullptr,
                    nullptr, SW_SHOWNORMAL)) <= 32)
            {
                state->owner->ShowActionError(
                    SettingsActionResult::Failure(
                        state->owner->L(
                            "settings.about.link.openFailed")));
            }
        };
        general.onboarding.begin = [weak](std::uint64_t generation,
            usage_guide::Topic topic) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation)) return;
            const auto* lesson = usage_guide::Find(topic);
            if (!lesson) return;
            auto& owner = *state->owner;
            if (topic == usage_guide::Topic::Workshop)
            {
                if (!owner.options.widgetsPage.workshopAvailable ||
                    !owner.options.widgetsPage.workshopAvailable() || !owner.options.widgetsPage.openWorkshop)
                {
                    owner.ShowActionError(SettingsActionResult::Failure(owner.L("app.settings.widgets_error_workshop_unavailable")));
                    return;
                }
                try
                {
                    const auto result = owner.options.widgetsPage.openWorkshop("steam-workshop");
                    if (state->alive.load() && state->owner && !result.succeeded)
                        state->owner->ShowActionError(SettingsActionResult::Failure(result.message));
                }
                catch (...)
                {
                    if (state->alive.load() && state->owner)
                        state->owner->ShowActionError(SettingsActionResult::Failure(
                            state->owner->L("settings.widgets.workshop.openFailed")));
                }
                return;
            }
            if (topic == usage_guide::Topic::Develop)
            {
                if (owner.SetWidgetDeveloperToolsEnabled(generation, true))
                    owner.RequestRoute(usage_guide::SettingsDestination(*lesson));
                return;
            }
            if (!lesson->practice)
            {
                state->owner->RequestRoute(usage_guide::SettingsDestination(*lesson));
                return;
            }
            const std::string_view key = lesson->key;
            SettingsHostActions::Request request;
            request.action = SettingsHostActions::Action::StartUsageGuidePractice;
            request.value = std::wstring(key.begin(), key.end());
            const auto result = state->owner->controller->InvokeHostAction(request);
            if (!state->alive.load() || !state->owner) return;
            state->owner->ShowActionError(result);
            // Keep the session and its presenters alive. Only the child may
            // minimize after the parent's synchronous start RPC has returned.
            if (result.Succeeded())
            {
                ShowWindow(state->owner->window, SW_MINIMIZE);
            }
        };
        general.onboarding.expandedChanged = [weak](std::uint64_t generation, bool expanded) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner || !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation)) return;
            SettingsHostActions::Request request;
            request.action = SettingsHostActions::Action::SetUsageGuideExpanded;
            request.boolValue = expanded;
            const auto result = state->owner->controller->InvokeHostAction(request);
            if (!state->alive.load() || !state->owner) return;
            state->owner->ShowActionError(result);
        };
        general.onboarding.navigate = [weak](const SettingsRoute& route) {
            if (const auto state = weak.lock(); state && state->alive.load() && state->owner)
                state->owner->RequestRoute(route);
        };
        general.onboarding.settingsIndex = [weak]() {
            if (const auto state = weak.lock(); state && state->alive.load() && state->owner)
            {
                auto entries = state->owner->BuildSearchInput().staticSettings;
                const auto& widgets = state->owner->options.widgetsPage;
                const bool workshop = widgets.workshopAvailable && widgets.workshopAvailable() && widgets.openWorkshop;
                for (auto& entry : entries)
                    if (entry.focusId == "widgets.workshop") entry.visible = entry.visible && workshop;
                return entries;
            }
            return std::vector<StaticSettingSearchDescriptor>{};
        };
        auto calendar = options.calendarPage;
        calendar.commitGeneral = general.commitGeneral;
        shell->SetCalendarPageActions(std::move(calendar));
        shell->SetGeneralPageActions(std::move(general));

        PageLayoutPageActions pageLayout = options.pageLayoutPage;
        pageLayout.confirm = [weak](
                                 std::wstring title,
                                 std::wstring message,
                                 std::wstring primaryButtonText,
                                 PageLayoutPageActions::
                                     ConfirmationCompletion completed) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->shell)
            {
                if (completed)
                    completed(false);
                return;
            }
            state->owner->ShowGenerationConfirmation(
                state->owner->shell->CurrentGeneration(),
                std::move(title), std::move(message),
                std::move(completed), true,
                std::move(primaryButtonText));
        };
        shell->SetPageLayoutPageActions(std::move(pageLayout));
        shell->SetLargeIconSettingsAction([weak](LargeIconSettingsRequest request) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner || !state->owner->options.largeIconSettings)
                return LargeIconSettingsSnapshot{};
            if (request.action == "import" && request.path.empty())
            {
                auto selected = ShowOpenPathDialog(state->owner->window,
                    state->owner->L("largeIcon.image"),
                    {{state->owner->L("largeIcon.image"), L"*.png;*.jpg;*.jpeg;*.bmp;*.ico"}}, false);
                if (selected) request.path = selected->wstring();
                else request.action = "status";
            }
            if (!state->alive.load() || !state->owner) return LargeIconSettingsSnapshot{};
            return state->owner->options.largeIconSettings(std::move(request));
        });

        PersonalizationPageActions personalization;
        personalization.themeLibrary = [weak](std::uint64_t generation, const ThemeLibraryRequest& request) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner) return ThemeLibraryResult{};
            return state->owner->ThemeOperation(generation, request);
        };
        personalization.contextMenu = options.contextMenu;
        personalization.themeAsync = [weak](std::uint64_t generation, ThemeLibraryRequest request, auto completed) {
            const auto state = weak.lock();
            if (state && state->alive.load() && state->owner)
                state->owner->BeginThemeTask(generation, std::move(request), std::move(completed));
            else if (completed) completed({});
        };
        personalization.appliedFont = options.appliedFont;
        personalization.restartApplication = [weak](std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation) ||
                !state->owner->FlushPendingChanges()) return;
            SettingsHostActions::Request request;
            request.action = SettingsHostActions::Action::RestartApplication;
            const auto result = state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        personalization.listFonts = [] {
            return app_fonts::List(std::filesystem::path(GetExecutableDirectoryPath()) / L"Assets", GetDataDirectoryPath());
        };
        personalization.importFonts = [weak](bool folder, std::string& error) {
            std::vector<app_fonts::Choice> choices;
            if (const auto state = weak.lock(); state && state->alive.load() && state->owner)
            {
                const auto path = ShowOpenPathDialog(state->owner->window,
                    state->owner->L("font.title"), {{L"TTF / OTF / TTC", L"*.ttf;*.otf;*.ttc"}}, folder);
                if (path) app_fonts::Import(*path, GetDataDirectoryPath(), choices, error);
            }
            return choices;
        };
        personalization.update = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            PersonalizationPageActions::Edit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditPersonalization(
                    generation, mode, std::move(edit));
            }
        };
        personalization.updateGeneral = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            PersonalizationPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, mode, std::move(edit));
            }
        };
        personalization.updateDock = [weak](std::uint64_t generation, SettingsUpdateMode mode, PersonalizationPageActions::DockEdit edit) {
            if (const auto state = weak.lock(); state && state->alive.load() && state->owner)
                state->owner->EditDock(generation, mode, std::move(edit));
        };
        personalization.updateNavigation = [weak](std::uint64_t generation, SettingsUpdateMode mode, PersonalizationPageActions::NavigationEdit edit) {
            if (const auto state = weak.lock(); state && state->alive.load() && state->owner)
                state->owner->EditNavigation(generation, mode, std::move(edit));
        };
        personalization.navigate = [weak](const SettingsRoute& route) {
            if (const auto state = weak.lock(); state && state->alive.load() && state->owner) state->owner->RequestRoute(route);
        };
        shell->SetPersonalizationPageActions(std::move(personalization));

        DesktopPageActions desktop;
        desktop.updateDesktop = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DesktopPageActions::DesktopEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditDesktop(
                    generation, mode, std::move(edit));
            }
        };
        desktop.updateCategory = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DesktopPageActions::CategoryEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditCategory(
                    generation, mode, std::move(edit));
            }
        };
        desktop.updatePersonalization = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DesktopPageActions::PersonalizationEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditPersonalization(
                    generation, mode, std::move(edit));
            }
        };
        desktop.commitCategory = [weak](std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            state->owner->controller->RequestCommit(SettingsDomain::Category);
        };
        shell->SetDesktopPageActions(std::move(desktop));

        DockPageActions dock;
        dock.previewAppearance = [weak](std::uint64_t generation, std::string target) {
            if (const auto state = weak.lock(); state && state->alive.load() && state->owner)
            {
                ThemeLibraryRequest request; request.command = ThemeLibraryCommand::Preview; request.target = std::move(target);
                state->owner->BeginThemeTask(generation, std::move(request), {});
            }
        };
        dock.updateGeneral = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DockPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, mode, std::move(edit));
            }
        };
        dock.updateDock = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DockPageActions::DockEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditDock(
                    generation, mode, std::move(edit));
            }
        };
        dock.invokeHost = [weak](
            std::uint64_t generation,
            SettingsHostActions::Request request) -> SettingsActionResult {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return SettingsActionResult::Busy(L"The settings page is no longer active.");
            }
            SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            // The optional animation dialog follows an accepted layout commit,
            // not merely a queued draft or a failed host action.
            if (result.Succeeded() && request.action == SettingsHostActions::Action::ApplyDesktopStylePreset)
                result = state->owner->controller->FlushPending();
            state->owner->ShowActionError(result);
            return result;
        };
        dock.openTaskbarSettings = [weak](std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                    state->owner->window, L"open", L"ms-settings:taskbar",
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                state->owner->ShowActionError(SettingsActionResult::Failure(
                    state->owner->L("settings.about.link.openFailed")));
            }
        };
        dock.confirm = [weak](
            std::uint64_t generation,
            std::wstring title,
            std::wstring message,
            DockPageActions::ConfirmationCompletion completion) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->shell ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                completion(false);
                return;
            }
            shell_impl::SettingsShellDialogRequest request;
            request.generation = generation;
            request.title = std::move(title);
            request.message = std::move(message);
            request.primaryButtonText =
                state->owner->L("settings.dialog.confirm");
            request.closeButtonText =
                state->owner->L("settings.dialog.cancel");
            request.destructive = true;
            state->owner->shell->ShowConfirmation(
                std::move(request), std::move(completion));
        };
        shell->SetDockPageActions(std::move(dock));

        HomeAboutPageActions homeAbout;
        homeAbout.navigate = [weak](const SettingsRoute& route) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->RequestRoute(route);
            }
        };
        homeAbout.invoke = [weak](
            std::uint64_t generation,
            HomeAboutCommand command) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }

            SettingsHostActions::Request request;
            switch (command)
            {
            case HomeAboutCommand::CheckForUpdates:
                request.action = SettingsHostActions::Action::CheckForUpdates;
                break;
            case HomeAboutCommand::OpenProject:
                request.action = SettingsHostActions::Action::OpenProject;
                break;
            case HomeAboutCommand::OpenLicense:
                request.action = SettingsHostActions::Action::OpenLicense;
                break;
            case HomeAboutCommand::OpenThirdPartyNotices:
                request.action =
                    SettingsHostActions::Action::OpenThirdPartyNotices;
                break;
            }
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        homeAbout.openLink = [weak](
                                 std::uint64_t generation,
                                 HomeAboutLink link) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            const std::wstring_view uri = HomeAboutLinkUri(link);
            if (uri.empty() || reinterpret_cast<INT_PTR>(ShellExecuteW(
                    state->owner->window, L"open",
                    std::wstring(uri).c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL)) <= 32)
            {
                state->owner->ShowActionError(
                    SettingsActionResult::Failure(
                        state->owner->L(
                            "settings.about.link.openFailed")));
            }
        };
        homeAbout.updateGeneral = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            HomeAboutPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, mode, std::move(edit));
            }
        };
        homeAbout.setAnimationDiagnostics = [weak](
            std::uint64_t generation, bool enabled) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            SettingsHostActions::Request request;
            request.action =
                SettingsHostActions::Action::SetAnimationDiagnostics;
            request.boolValue = enabled;
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        const auto invokeDebugProfile = [weak](std::uint64_t generation,
            SettingsHostActions::Action action, bool enabled, std::wstring value) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller || !state->owner->DebugPageVisible() ||
                !state->owner->controller->IsGenerationCurrent(generation)) return;
            SettingsHostActions::Request request;
            request.action = action;
            request.boolValue = enabled;
            request.value = std::move(value);
            const auto result = state->owner->controller->InvokeHostAction(request);
            if (!state->alive.load() || !state->owner) return;
            state->owner->ShowActionError(result);
            if (state->owner->shell) state->owner->shell->RefreshRuntimeState();
        };
        homeAbout.setDebugProfileEnabled = [invokeDebugProfile](std::uint64_t generation, bool enabled) {
            invokeDebugProfile(generation, SettingsHostActions::Action::SetDebugProfileEnabled, enabled, {});
        };
        homeAbout.chooseDebugDesktop = [weak, invokeDebugProfile](std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner || !state->owner->controller ||
                !state->owner->DebugPageVisible() || !state->owner->controller->IsGenerationCurrent(generation)) return;
            const auto selected = ShowOpenPathDialog(state->owner->window,
                state->owner->L("settings.debug.profile.desktop"), {}, true);
            if (selected) invokeDebugProfile(generation,
                SettingsHostActions::Action::SetDebugDesktopDirectory, false, selected->wstring());
        };
        homeAbout.clearDebugProfile = [weak, invokeDebugProfile](std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner || !state->owner->controller ||
                !state->owner->DebugPageVisible() || !state->owner->controller->IsGenerationCurrent(generation)) return;
            auto& owner = *state->owner;
            if (!owner.options.homeAboutStatus) return;
            const auto status = owner.options.homeAboutStatus(generation, owner.controller->Snapshot()->revision);
            if (!status.debugDataDirectory || status.debugDataDirectory->empty()) return;
            const auto root = std::filesystem::path(*status.debugDataDirectory).parent_path();
            const auto detail = owner.L("settings.debug.profile.clearConfirm") + L"\n\n" +
                *status.debugDataDirectory + L"\n" + (root / L"FullBackups").wstring() +
                L"\n" + (root / L"TempState").wstring() + L"\n" + (root / L"PrivateState").wstring();
            owner.ShowGenerationConfirmation(generation, owner.L("settings.debug.profile.clear"), detail,
                [invokeDebugProfile, generation](bool confirmed) {
                    if (confirmed) invokeDebugProfile(generation, SettingsHostActions::Action::ClearDebugProfile, false, {});
                }, true, owner.L("settings.debug.profile.clear"));
        };
        homeAbout.setTemporaryInitialization = [weak](std::uint64_t generation, bool enabled) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller || !state->owner->DebugPageVisible() ||
                !state->owner->controller->IsGenerationCurrent(generation)) return;
            SettingsHostActions::Request request;
            request.action = SettingsHostActions::Action::SetTemporaryGridInitialization;
            request.boolValue = enabled;
            const auto result = state->owner->controller->InvokeHostAction(request);
            if (!state->alive.load() || !state->owner) return;
            state->owner->ShowActionError(result);
            if (state->owner->shell) state->owner->shell->RefreshRuntimeState();
        };
        homeAbout.unlockDebug = [weak](std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return false;
            }
            state->owner->debugSession.Unlock();
            state->owner->RebuildSearchIndex();
            return state->owner->DebugPageVisible();
        };
        homeAbout.requestResetUnlockConfirmation = [weak](
            std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->DebugPageVisible())
                return;
            state->owner->ShowGenerationConfirmation(
                generation,
                state->owner->L("settings.debug.resetUnlock"),
                state->owner->L("settings.debug.resetUnlock.description"),
                [weak, generation](bool confirmed) {
                    if (!confirmed) return;
                    const auto current = weak.lock();
                    if (!current || !current->alive.load() ||
                        !current->owner || !current->owner->controller ||
                        !current->owner->shell ||
                        !current->owner->controller->IsGenerationCurrent(
                            generation) ||
                        !current->owner->DebugPageVisible())
                        return;
                    auto& owner = *current->owner;
                    const bool reset = owner.options.resetAdvancedFeatures &&
                        owner.options.resetAdvancedFeatures();
                    owner.shell->RefreshRuntimeState();
                    (void)owner.shell->ShowInfoForGeneration(generation,
                        reset ? shell_impl::SettingsShellInfoSeverity::Success
                              : shell_impl::SettingsShellInfoSeverity::Error,
                        owner.L("settings.debug.resetUnlock"),
                        owner.L(reset ? "settings.debug.resetUnlock.success"
                                      : "settings.debug.resetUnlock.failed"));
                }, true, state->owner->L("settings.debug.resetUnlock"));
        };
        homeAbout.requestCrashTestConfirmation = [weak](
            std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->DebugPageVisible())
            {
                return;
            }
            state->owner->ShowGenerationConfirmation(
                generation,
                state->owner->L("app.settings.crash_test"),
                state->owner->L("app.settings.crash_test_desc"),
                [weak, generation](bool confirmed) {
                    if (!confirmed)
                        return;
                    const auto current = weak.lock();
                    if (!current || !current->alive.load() ||
                        !current->owner || !current->owner->controller ||
                        !current->owner->controller->IsGenerationCurrent(
                            generation) ||
                        !current->owner->DebugPageVisible())
                    {
                        return;
                    }
                    SettingsHostActions::Request request;
                    request.action =
                        SettingsHostActions::Action::TriggerCrashTest;
                    (void)current->owner->controller->InvokeHostAction(
                        request);
                },
                true);
        };
        shell->SetHomeAboutPageActions(std::move(homeAbout));
    }

    void EditGeneral(std::uint64_t generation, SettingsUpdateMode mode,
        GeneralPageActions::GeneralEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        GeneralSettings value = snapshot->values.general;
        edit(value);
        if (value.quickNavigationAppearance.mode == -1 && snapshot->values.general.quickNavigationAppearance.mode != -1 &&
            value.globalQuickNavigationAppearance.customized)
        {
            themes::Library library;
            std::string error;
            auto navigation = snapshot->values.navigation;
            if (themes::Load(themes::LibraryPath(), library, error) && themes::FollowQuickBinding(library, navigation))
                controller->UpdateNavigation(std::move(navigation), mode);
        }
        controller->UpdateGeneral(std::move(value), mode);
    }

    void EditNavigation(std::uint64_t generation, SettingsUpdateMode mode,
        GeneralPageActions::NavigationEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        NavigationSettings value = snapshot->values.navigation;
        edit(value);
        controller->UpdateNavigation(std::move(value), mode);
    }

    void EditDock(std::uint64_t generation, SettingsUpdateMode mode,
        DockPageActions::DockEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        DockSettings value = snapshot->values.dock;
        edit(value);
        controller->UpdateDock(std::move(value), mode);
    }

    ThemeLibraryResult ThemeOperation(std::uint64_t generation, const ThemeLibraryRequest& request)
    {
        ThemeLibraryResult result;
        result.sharingAvailable = ThemeSharingAvailable();
        if (!controller || !controller->IsGenerationCurrent(generation)) return result;
        const auto current = controller->Snapshot();
        if (!current || current->externalReplacementPending) return result;
        std::string error;
        const auto feedback = [&]() {
            result.message = result.succeeded ? L("themeLibrary.success") : L(themes::ErrorLocalizationKey(error));
            if (result.message.empty()) result.message = L("themeLibrary.error.invalidPackage");
        };
        if (request.command == ThemeLibraryCommand::Refresh)
        {
            result.succeeded = themes::Load(themes::LibraryPath(), result.library, error);
            if (!result.succeeded) feedback();
            return result;
        }
        if (!controller->FlushAll().Succeeded())
        { error = "writeFailed"; feedback(); return result; }
        if (!themes::Load(themes::LibraryPath(), result.library, error)) { feedback(); return result; }
        themes::ReconcileReferences(result.library, current->values);
        if (request.command == ThemeLibraryCommand::Export)
        {
            themes::Package package;
            if (!themes::Export(result.library, request.id, package, error)) { feedback(); return result; }
            const auto path = ShowSavePathDialog(window, L("themeLibrary.export"), L"theme.snowtheme",
                {{L("themeLibrary.title"), L"*.snowtheme"}}, L"snowtheme");
            if (!path) { result.succeeded = true; return result; }
            if (!controller->IsGenerationCurrent(generation)) return result;
            result.succeeded = themes::WritePackage(*path, package, error); feedback(); return result;
        }
        if (request.command == ThemeLibraryCommand::Apply)
        {
            const auto apply = [&](const SettingsValues& values) {
                controller->UpdatePersonalization(values.personalization, SettingsUpdateMode::PreviewAndCommit);
                controller->UpdateDock(values.dock, SettingsUpdateMode::PreviewAndCommit);
                controller->UpdateNavigation(values.navigation, SettingsUpdateMode::PreviewAndCommit);
                controller->UpdateGeneral(values.general, SettingsUpdateMode::PreviewAndCommit);
                return controller->FlushAll();
            };
            bool settingsTouched = false;
            // Keep the file's last successful reference untouched until settings
            // persist. The lock also binds application and snapshot to the same
            // library values; a failed write never needs a second library rollback.
            result.succeeded = themes::Transact(themes::LibraryPath(), [&](auto& library, auto& detail) {
                themes::ReconcileReferences(library, current->values);
                auto next = current->values;
                if (!themes::ApplyTarget(library, request.target, request.id, next, detail) ||
                    !themes::Select(library, request.target, request.id, themes::TargetKind(request.target),
                        themes::TargetScope(request.target), detail)) return false;
                settingsTouched = true;
                if (apply(next).Succeeded()) return true;
                detail = "writeFailed";
                return false;
            }, result.library, error);
            if (!result.succeeded && settingsTouched) (void)apply(current->values);
            feedback(); return result;
        }
        result.succeeded = themes::Transact(themes::LibraryPath(), [&](auto& library, auto& detail) {
            themes::ReconcileReferences(library, current->values);
            if (request.command == ThemeLibraryCommand::Remove)
                return themes::Remove(library, request.id, request.replacement, true, detail);
            auto theme = themes::CaptureTarget(request.target, current->values);
            theme.id = request.id; theme.name = request.name; theme.scopes = request.scopes;
            themes::Package dependencies;
            if (theme.kind == themes::Kind::Global)
            {
                const auto dependency = [&](themes::Kind kind, const std::string& selected) {
                    if (!selected.empty()) return selected;
                    auto child = themes::CaptureTarget(kind == themes::Kind::QuickPanel ? "quickPanel" : "popup", current->values);
                    child.id = themes::CreateId(); child.name = request.name + (kind == themes::Kind::QuickPanel ? " / " + winrt::to_string(L("themeLibrary.quickPanel")) : " / " + winrt::to_string(L("themeLibrary.popup")));
                    dependencies.emplace(child.id, child); return child.id;
                };
                theme.quickPanel = dependency(themes::Kind::QuickPanel, request.quickPanel);
                theme.popup = dependency(themes::Kind::Popup, request.popup);
            }
            return themes::Save(library, std::move(theme), dependencies,
                request.command == ThemeLibraryCommand::Update, result.savedId, detail, themes::CreateId, true);
        }, result.library, error);
        if (result.succeeded && request.command == ThemeLibraryCommand::Update)
        {
            auto next = current->values;
            const auto targets = themes::ApplySavedUpdate(result.library, result.savedId, next, error);
            bool applied = true;
            if (!targets.empty())
            {
                controller->UpdatePersonalization(next.personalization, SettingsUpdateMode::PreviewAndCommit);
                controller->UpdateDock(next.dock, SettingsUpdateMode::PreviewAndCommit);
                controller->UpdateNavigation(next.navigation, SettingsUpdateMode::PreviewAndCommit);
                controller->UpdateGeneral(next.general, SettingsUpdateMode::PreviewAndCommit);
                applied = controller->FlushAll().Succeeded();
                if (!applied)
                {
                    controller->UpdatePersonalization(current->values.personalization, SettingsUpdateMode::PreviewAndCommit);
                    controller->UpdateDock(current->values.dock, SettingsUpdateMode::PreviewAndCommit);
                    controller->UpdateNavigation(current->values.navigation, SettingsUpdateMode::PreviewAndCommit);
                    controller->UpdateGeneral(current->values.general, SettingsUpdateMode::PreviewAndCommit);
                    (void)controller->FlushAll();
                }
            }
            std::vector<std::string> completed = applied ? targets : std::vector<std::string>{};
            for (const auto& [target, reference] : result.library.references)
            {
                if (!target.starts_with("widget/") || reference.id != result.savedId) continue;
                if (!widgetSettingsService) { applied = false; continue; }
                const auto previous = themes::Resolve(reference.snapshot, reference.id);
                const auto theme = themes::Resolve(result.library.themes, reference.id);
                const auto loaded = widgetSettingsService->Load(Utf8ToWide(target.substr(7)));
                if (!loaded.Succeeded() || !loaded.snapshot || !previous || !theme ||
                    !themes::WidgetMatches(loaded.snapshot->hostAppearance, *previous))
                { applied = false; continue; }
                const auto changed = widgetSettingsService->UpdateHostAppearance(
                    widget_runtime::WidgetSettingMutationGuard::FromSnapshot(*loaded.snapshot), themes::WidgetPatch(*theme));
                if (changed.Succeeded()) completed.push_back(target);
                else applied = false;
            }
            if (!completed.empty())
            {
                themes::Library refreshed;
                const bool recorded = themes::Transact(themes::LibraryPath(), [&](auto& library, auto& detail) {
                    for (const auto& target : completed)
                    {
                        const auto found = library.references.find(target);
                        if (found == library.references.end() || found->second.id.empty()) continue;
                        const auto reference = found->second;
                        if (!themes::Select(library, target, reference.id, reference.kind, reference.scope, detail)) return false;
                    }
                    return true;
                }, refreshed, error);
                if (recorded) result.library = std::move(refreshed);
                else applied = false;
            }
            if (!applied) { result.message = L("themeLibrary.savedPending"); return result; }
        }
        feedback(); return result;
    }

    void EditPersonalization(std::uint64_t generation,
        SettingsUpdateMode mode, PersonalizationPageActions::Edit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        PersonalizationSettings value = snapshot->values.personalization;
        edit(value);
        if (value.backgroundPreset != kAppearancePresetCustom && value.backgroundPreset != snapshot->values.personalization.backgroundPreset)
        {
            auto general = snapshot->values.general;
            general.globalQuickNavigationAppearance = {};
            general.globalCollectionPopupAppearance = {};
            controller->UpdateGeneral(std::move(general), mode);
        }
        controller->UpdatePersonalization(std::move(value), mode);
    }

    void EditDesktop(std::uint64_t generation, SettingsUpdateMode mode,
        DesktopPageActions::DesktopEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        DesktopDisplaySettings value = snapshot->values.desktop;
        edit(value);
        controller->UpdateDesktop(std::move(value), mode);
    }

    void EditCategory(std::uint64_t generation, SettingsUpdateMode mode,
        DesktopPageActions::CategoryEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        CategorySettings value = snapshot->values.category;
        edit(value);
        controller->UpdateCategory(std::move(value), mode);
    }

    [[nodiscard]] bool CommitRoute(const SettingsRoute& route,
        SettingsActionResult* controllerResult = nullptr)
    {
        performance::Scope performanceScope("settings.navigate", SettingsPageKey(route.page));
        if (!controller || !route.IsValid() || shuttingDown)
            return false;
        WriteDiagnosticLogEntry((L"SettingsUI navigate pid=" +
            std::to_wstring(GetCurrentProcessId()) + L" page=" +
            std::to_wstring(static_cast<int>(route.page))).c_str());
        if ((route.page == SettingsPage::DeveloperTools &&
                (!options.developerToolsVisible ||
                    !options.developerToolsVisible())) ||
            (route.page == SettingsPage::Debug &&
                !DebugPageVisible()))
        {
            SetError(L"The requested conditional settings page is hidden.");
            return false;
        }

        const auto previous = controller->Snapshot();
        if (previous && previous->sessionActive && shell &&
            previous->route.page == SettingsPage::WidgetSettings &&
            (route.page != SettingsPage::WidgetSettings ||
                route.widgetInstanceId !=
                    previous->route.widgetInstanceId))
        {
            const auto flushed = shell->FlushPendingWidgetSettings();
            if (!flushed.Succeeded())
            {
                std::wstring message;
                if (!flushed.message.empty())
                    message = winrt::to_hstring(flushed.message).c_str();
                if (message.empty())
                    message = L("settings.widget.saveFailed");
                ShowActionError(SettingsActionResult::Failure(
                    std::move(message)));
                // A component draft may be unsaved, but navigation must not
                // trap the user on this page. Deactivation closes the failed
                // component-settings session and discards that draft.
            }
        }

        std::optional<widget_runtime::WidgetSettingsSnapshot>
            widgetSnapshot;
        if (route.page == SettingsPage::WidgetSettings)
        {
            try
            {
                if (!widgetSettingsService ||
                    (options.ensureWidgetSettingsInstance &&
                        !options.ensureWidgetSettingsInstance(
                            route.widgetInstanceId)))
                {
                    ShowActionError(SettingsActionResult::Failure(
                        L("settings.widget.loadFailed")));
                    return false;
                }

                const auto loaded = widgetSettingsService->Load(
                    route.widgetInstanceId);
                const auto current = widgetSettingsService->Snapshot(
                    route.widgetInstanceId);
                if (!loaded.Succeeded() || !loaded.snapshot || !current ||
                    loaded.snapshot->widgetId != route.widgetInstanceId ||
                    current->widgetId != loaded.snapshot->widgetId ||
                    current->generation != loaded.snapshot->generation ||
                    current->revision != loaded.snapshot->revision)
                {
                    std::wstring message;
                    if (!loaded.message.empty())
                        message = winrt::to_hstring(loaded.message).c_str();
                    if (message.empty())
                        message = L("settings.widget.loadFailed");
                    ShowActionError(SettingsActionResult::Failure(
                        std::move(message)));
                    return false;
                }
                widgetSnapshot = *loaded.snapshot;
            }
            catch (...)
            {
                ShowActionError(SettingsActionResult::Failure(
                    L("settings.widget.loadFailed")));
                return false;
            }
        }

        const SettingsActionResult opened = controller->Open(route);
        if (controllerResult)
            *controllerResult = opened;
        const auto snapshot = controller->Snapshot();
        if (!IsUsableControllerSnapshot(snapshot) ||
            !snapshot->sessionActive || snapshot->route != route)
        {
            ShowActionError(opened);
            return false;
        }

        ApplySnapshotNow(snapshot);
        if (widgetSnapshot &&
            (!shell || !shell->ApplyWidgetSettingsSnapshot(
                *widgetSnapshot)))
        {
            ShowActionError(SettingsActionResult::Failure(
                L("settings.widget.loadFailed")));
            return false;
        }
        return true;
    }

    void RequestRoute(const SettingsRoute& route)
    {
        SettingsActionResult result;
        if (CommitRoute(route, &result) && !result.Succeeded())
            ShowActionError(result);
    }

    void RequestSearch(std::wstring query, std::uint64_t generation,
        std::uint64_t requestId)
    {
        if (!callbacks)
            return;
        const std::uint64_t expectedEpoch = viewEpoch;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            (void)callbacks->dispatcher.TryEnqueue(
                [weak, query = std::move(query), generation, requestId,
                    expectedEpoch]() mutable {
                    const auto state = weak.lock();
                    if (!state || !state->alive.load() || !state->owner ||
                        state->owner->viewEpoch != expectedEpoch ||
                        !state->owner->shell ||
                        !state->owner->controller ||
                        !state->owner->controller->IsGenerationCurrent(
                            generation))
                    {
                        return;
                    }
                    auto results = state->owner->searchIndex.Search(query);
                    (void)state->owner->shell->SetSearchResults(
                        std::move(results), generation, requestId);
                });
        }
        catch (...)
        {
        }
    }

    void RefreshLocalizedPresentation()
    {
        if (!shell)
            return;
        shell->RefreshLocalizedText();
        if (widgetsPageActive && widgetsPageBackend)
            (void)widgetsPageBackend->Refresh();
        if (backupDataPageActive && backupDataPageBackend)
            backupDataPageBackend->Refresh();
        // Component schemas are supplied by the reloaded runtime. Rebuild
        // search last so both the current editor and the management snapshot
        // have observed the new language generation first.
        RebuildSearchIndex();
        std::wstring title = L("settings.shell.title");
        if (title.empty())
            title = options.windowTitle;
        if (window && IsWindow(window))
            SetWindowTextW(window, title.c_str());
    }

    [[nodiscard]] bool PrepareLanguageChange()
    {
        if (!controller || !shell)
            return true;
        const auto current = controller->Snapshot();
        if (!current || !current->sessionActive ||
            current->route.page != SettingsPage::WidgetSettings)
        {
            return true;
        }

        const auto flushed = shell->FlushPendingWidgetSettings();
        if (flushed.Succeeded())
            return true;

        std::wstring message;
        if (!flushed.message.empty())
            message = winrt::to_hstring(flushed.message).c_str();
        if (message.empty())
            message = L("settings.widget.saveFailed");
        ShowActionError(SettingsActionResult::Failure(std::move(message)));
        return false;
    }

    void ReloadActiveWidgetSettingsForLanguageChange()
    {
        if (!controller || !shell || !widgetSettingsService)
            return;
        const auto controllerSnapshot = controller->Snapshot();
        if (!controllerSnapshot || !controllerSnapshot->sessionActive ||
            controllerSnapshot->route.page != SettingsPage::WidgetSettings)
        {
            return;
        }

        const std::wstring instanceId =
            controllerSnapshot->route.widgetInstanceId;
        const auto disableStaleEditor = [&]() {
            try
            {
                widgetSettingsService->Close(instanceId);
                const auto currentController = controller->Snapshot();
                if (currentController && currentController->sessionActive &&
                    currentController->route ==
                        controllerSnapshot->route &&
                    shell->CurrentRoute() == controllerSnapshot->route)
                {
                    // The old runtime generation must never remain editable.
                    // A normal Open resumes interaction and retries the load.
                    shell->SuspendInteraction();
                    interactionSuspended = true;
                }
            }
            catch (...)
            {
            }
        };

        try
        {
            const auto loaded = widgetSettingsService->Reload(instanceId);
            const auto current = widgetSettingsService->Snapshot(instanceId);
            if (!loaded.Succeeded() || !loaded.snapshot || !current ||
                loaded.snapshot->widgetId != instanceId ||
                current->widgetId != loaded.snapshot->widgetId ||
                current->generation != loaded.snapshot->generation ||
                current->revision != loaded.snapshot->revision ||
                !shell->ApplyWidgetSettingsSnapshot(*loaded.snapshot))
            {
                std::wstring message;
                if (!loaded.message.empty())
                    message = winrt::to_hstring(loaded.message).c_str();
                if (message.empty())
                    message = L("settings.widget.loadFailed");
                ShowActionError(
                    SettingsActionResult::Failure(std::move(message)));
                disableStaleEditor();
            }
        }
        catch (...)
        {
            ShowActionError(SettingsActionResult::Failure(
                L("settings.widget.loadFailed")));
            disableStaleEditor();
        }
    }

    void ResumeInteraction()
    {
        if (!shell || !interactionSuspended)
            return;
        shell->ResumeInteraction();
        interactionSuspended = false;
    }

    void SuspendInteraction()
    {
        if (!shell || interactionSuspended)
            return;
        shell->SuspendInteraction();
        interactionSuspended = true;
    }

    [[nodiscard]] bool FlushPendingChanges()
    {
        if (!controller || shuttingDown)
            return false;
        if (shell)
        {
            const auto widgetResult =
                shell->FlushPendingWidgetSettings();
            if (!widgetResult.Succeeded())
            {
                const auto snapshot = controller->Snapshot();
                std::wstring message;
                if (!widgetResult.message.empty())
                    message = winrt::to_hstring(widgetResult.message).c_str();
                if (message.empty())
                    message = L("settings.widget.saveFailed");
                if (snapshot)
                {
                    (void)shell->ShowInfoForGeneration(
                        snapshot->generation,
                        shell_impl::SettingsShellInfoSeverity::Error,
                        L("settings.status.error"), std::move(message));
                }
                // Component settings are isolated from the application
                // settings controller. Report the failed component draft but
                // continue flushing the remaining settings so Close can
                // always leave the component editor.
            }
        }

        const SettingsActionResult result = controller->FlushAll();
        if (!result.Succeeded())
        {
            ShowActionError(result);
            return false;
        }
        return true;
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(
                hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self)
                self->window = hwnd;
        }

        if (!self)
            return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message)
        {
        case kApplyXamlBackdropMessage:
            self->ApplyDeferredSystemBackdrop();
            return 0;
        case kUpdateIntegratedTitleBarInsetsMessage:
            self->UpdateIntegratedTitleBarInsets();
            return 0;
        case kRefreshExternalStateMessage:
            self->ApplyDeferredExternalStateRefresh();
            return 0;
        case kDispatchOwnerTaskMessage:
        {
            std::unique_ptr<std::function<void()>> task(
                reinterpret_cast<std::function<void()>*>(lParam));
            if (task && *task)
            {
                try
                {
                    (*task)();
                }
                catch (...)
                {
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (self->HandleWorkingSetTrimTimer(
                    static_cast<UINT_PTR>(wParam)))
            {
                return 0;
            }
            break;
        case WM_NCMOUSELEAVE:
        {
            // The integrated title bar keeps Windows-owned caption buttons.
            // DWM must observe the non-client leave before a close hides this
            // reusable HWND, otherwise its Close-button hover can survive the
            // next ShowWindow call.
            LRESULT dwmResult = 0;
            if (DwmDefWindowProc(
                    hwnd, message, wParam, lParam, &dwmResult))
            {
                return dwmResult;
            }
            break;
        }
        case WM_CLOSE:
            (void)self->HideWindow();
            return 0;
        case WM_ACTIVATEAPP:
            if (wParam != FALSE)
                self->QueueExternalStateRefresh();
            break;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT windowDpi = GetDpiForWindow(hwnd);
            const UINT dpi = windowDpi != 0 ? windowDpi : 96;
            const int minimumClientWidth = MulDiv(
                kMinimumClientWidth, static_cast<int>(dpi), 96);
            const int minimumClientHeight = MulDiv(
                kMinimumClientHeight, static_cast<int>(dpi), 96);
            RECT minimumBounds{0, 0,
                minimumClientWidth, minimumClientHeight};
            const DWORD style = static_cast<DWORD>(
                GetWindowLongPtrW(hwnd, GWL_STYLE));
            const DWORD extendedStyle = static_cast<DWORD>(
                GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            if (AdjustWindowRectExForDpi(&minimumBounds, style, FALSE,
                    extendedStyle, dpi))
            {
                info->ptMinTrackSize.x =
                    minimumBounds.right - minimumBounds.left;
                info->ptMinTrackSize.y =
                    minimumBounds.bottom - minimumBounds.top;
            }
            else
            {
                info->ptMinTrackSize.x = minimumClientWidth;
                info->ptMinTrackSize.y = minimumClientHeight;
            }
            return 0;
        }
        case WM_DPICHANGED:
        {
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            if (suggested)
            {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
            }
            self->QueueIntegratedTitleBarInsetsUpdate();
            break;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplySettingsWindowChrome(hwnd, self->darkTheme);
            self->ApplyIntegratedTitleBarButtonColors();
            self->QueueSystemBackdropUpdate();
            self->QueueIntegratedTitleBarInsetsUpdate();
            break;
        case WM_NCDESTROY:
            self->CancelWorkingSetTrim();
            self->systemBackdropUpdateQueued = false;
            self->integratedTitleBarInsetsUpdateQueued = false;
            self->externalStateRefreshQueued = false;
            self->runtime.HandleWindowMessage(message, wParam, lParam);
            self->window = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, message, wParam, lParam);
        default:
            break;
        }

        self->runtime.HandleWindowMessage(message, wParam, lParam);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    [[nodiscard]] bool RegisterWindowClass()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        if (!windowClass.hIcon)
            windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hIconSm = windowClass.hIcon;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kSettingsWindowClassName;
        if (RegisterClassExW(&windowClass) != 0)
            return true;
        const DWORD error = GetLastError();
        if (error == ERROR_CLASS_ALREADY_EXISTS)
            return true;
        SetError(FormatWin32Error(L"Register settings window class", error));
        return false;
    }

    [[nodiscard]] bool CreateHostWindow()
    {
        const UINT dpi = GetDpiForSystem();
        RECT bounds{0, 0,
            MulDiv(kDefaultClientWidth, static_cast<int>(dpi), 96),
            MulDiv(kDefaultClientHeight, static_cast<int>(dpi), 96)};
        AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE,
            WS_EX_APPWINDOW, dpi);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
        const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);

        window = CreateWindowExW(WS_EX_APPWINDOW,
            kSettingsWindowClassName, options.windowTitle.c_str(),
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            x, y, width, height, nullptr, nullptr, instance, this);
        if (!window)
        {
            SetError(FormatWin32Error(
                L"Create settings window", GetLastError()));
            return false;
        }
        ApplySettingsWindowChrome(window, darkTheme);
        return true;
    }

    void ReleaseView() noexcept
    {
        if (themeTask) { themeTask->cancel.store(true); themeTask->completed = {}; themeTask.reset(); }
        viewReleaseQueued = false;
        CancelWorkingSetTrim();
        settingsSessionWorkingSetBaseline.reset();
        systemBackdropUpdateQueued = false;
        integratedTitleBarInsetsUpdateQueued = false;
        externalStateRefreshQueued = false;
        interactionSuspended = true;

        if (shell)
            shell->SetActualThemeChangedCallback({});
        if (shell)
            shell->SetWidgetSettingsService(nullptr);
        DisposePageBackends();
        if (shell)
            shell->SetSystemBackdropActive(false);
        if (shell)
        {
            shell->Close();
            shell = nullptr;
        }
        runtime.Detach();
        ResetIntegratedTitleBar();
        searchIndex = {};
    }

    void ReleaseSessionView() noexcept
    {
        viewReleaseQueued = false;
        systemBackdropUpdateQueued = false;
        integratedTitleBarInsetsUpdateQueued = false;
        externalStateRefreshQueued = false;
        interactionSuspended = true;

        // Keep one process-lifetime Shell and Island. WinUI retains SVG file,
        // mapping, and composition handles when either root is reconstructed;
        // route presenters and their controls remain safe to release here.
        if (shell)
            shell->ReleaseSessionResources();
        DisposePageBackends();
        searchIndex = {};
        QueueWorkingSetTrim();
    }

    void QueueViewRelease() noexcept
    {
        if (viewReleaseQueued || shuttingDown || Visible() || !callbacks ||
            !callbacks->alive.load())
        {
            return;
        }

        viewReleaseQueued = true;
        const std::uint64_t expectedEpoch = viewEpoch;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            if (callbacks->dispatcher.TryEnqueue([weak, expectedEpoch]() {
                    const auto state = weak.lock();
                    if (!state || !state->alive.load() || !state->owner)
                        return;
                    auto* owner = state->owner;
                    owner->viewReleaseQueued = false;
                    if (owner->shuttingDown || owner->Visible() ||
                        owner->viewEpoch != expectedEpoch)
                    {
                        return;
                    }
                    owner->ReleaseSessionView();
                }))
            {
                return;
            }
        }
        catch (...)
        {
        }
        viewReleaseQueued = false;
    }

    void QueueWorkingSetTrim() noexcept
    {
        if (workingSetTrimQueued || shuttingDown || Visible() ||
            !window || !IsWindow(window) ||
            !settingsSessionWorkingSetBaseline)
        {
            return;
        }

        const auto current = QueryCurrentProcessWorkingSet();
        if (!current || !HasSignificantWorkingSetGrowth(
                *current, *settingsSessionWorkingSetBaseline))
        {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        if (lastWorkingSetTrimTick != 0 &&
            now - lastWorkingSetTrimTick < kWorkingSetTrimCooldownMs)
        {
            return;
        }

        do
        {
            ++nextWorkingSetTrimTimerId;
        } while (nextWorkingSetTrimTimerId == 0);
        const UINT_PTR timerId = SetTimer(window,
            nextWorkingSetTrimTimerId, kWorkingSetTrimDelayMs, nullptr);
        if (timerId != 0)
        {
            workingSetTrimQueued = true;
            workingSetTrimEpoch = viewEpoch;
            activeWorkingSetTrimTimerId = timerId;
            return;
        }
        workingSetTrimQueued = false;
        workingSetTrimEpoch = 0;
        activeWorkingSetTrimTimerId = 0;
    }

    void CancelWorkingSetTrim() noexcept
    {
        if (activeWorkingSetTrimTimerId != 0 &&
            window && IsWindow(window))
        {
            KillTimer(window, activeWorkingSetTrimTimerId);
        }
        workingSetTrimQueued = false;
        workingSetTrimEpoch = 0;
        activeWorkingSetTrimTimerId = 0;
    }

    bool HandleWorkingSetTrimTimer(UINT_PTR timerId) noexcept
    {
        // KillTimer does not remove an already-posted WM_TIMER. Ignore stale
        // request IDs without touching a newer timer scheduled after reopen.
        if (!workingSetTrimQueued || timerId == 0 ||
            timerId != activeWorkingSetTrimTimerId)
        {
            return false;
        }
        if (window && IsWindow(window))
            KillTimer(window, timerId);

        const std::uint64_t expectedEpoch = workingSetTrimEpoch;
        workingSetTrimQueued = false;
        workingSetTrimEpoch = 0;
        activeWorkingSetTrimTimerId = 0;
        if (shuttingDown || Visible() || expectedEpoch == 0 ||
            viewEpoch != expectedEpoch ||
            !settingsSessionWorkingSetBaseline)
        {
            return true;
        }

        const ULONGLONG now = GetTickCount64();
        if (lastWorkingSetTrimTick != 0 &&
            now - lastWorkingSetTrimTick < kWorkingSetTrimCooldownMs)
        {
            return true;
        }

        const auto current = QueryCurrentProcessWorkingSet();
        if (!current || !HasSignificantWorkingSetGrowth(
                *current, *settingsSessionWorkingSetBaseline))
        {
            return true;
        }

        if (TrimProcessWorkingSet())
            lastWorkingSetTrimTick = GetTickCount64();
        return true;
    }

    [[nodiscard]] bool CreateView()
    {
        performance::Scope performanceScope("settings", "view.create");
        if (shell && runtime.IsAttached())
            return true;
        if (!window || !IsWindow(window) || !runtime.IsInitialized() ||
            !callbacks)
        {
            SetError(L"Create settings view before host initialization");
            return false;
        }

        // A partial view cannot be repaired in place. Keep the process-level
        // XAML runtime and stable top-level HWND, then rebuild only the Island
        // and its complete visual tree.
        ReleaseView();
        if (!ConfigureIntegratedTitleBar())
            return false;

        try
        {
            shell = winrt::make_self<shell_impl::SettingsShell>();
            const std::weak_ptr<CallbackState> weak = callbacks;
            shell->SetLocalizer([weak](std::string_view key) {
                const auto state = weak.lock();
                return state && state->alive.load() && state->owner
                    ? state->owner->L(key)
                    : std::wstring{};
            });
            shell->SetRouteRequestedCallback(
                [weak](const SettingsRoute& route) {
                    if (const auto state = weak.lock();
                        state && state->alive.load() && state->owner)
                    {
                        state->owner->RequestRoute(route);
                    }
                });
            shell->SetSearchRequestedCallback(
                [weak](std::wstring query, std::uint64_t generation,
                       std::uint64_t requestId) {
                    if (const auto state = weak.lock();
                        state && state->alive.load() && state->owner)
                    {
                        state->owner->RequestSearch(
                            std::move(query), generation, requestId);
                    }
                });
            shell->SetCancelOperationCallback([weak](std::uint64_t generation) {
                if (const auto state = weak.lock(); state && state->alive.load() && state->owner && state->owner->themeTask &&
                    state->owner->themeTask->generation == generation) state->owner->themeTask->cancel.store(true);
            });
            shell->SetWidgetSettingsService(widgetSettingsService);
            ConfigurePageActions();
            RebuildSearchIndex();

            if (!runtime.Attach(window, shell.as<mux::UIElement>()))
            {
                SetError(runtime.LastError());
                ReleaseView();
                return false;
            }
            shell->SetSystemBackdropActive(false);
            shell->SetActualThemeChangedCallback(
                [weak](bool isDark) {
                    if (const auto state = weak.lock();
                        state && state->alive.load() && state->owner)
                    {
                        state->owner->ApplyActualTheme(isDark);
                    }
                });
            QueueIntegratedTitleBarInsetsUpdate();
            QueueSystemBackdropUpdate();
            interactionSuspended = true;
            return true;
        }
        catch (const winrt::hresult_error& error)
        {
            SetError(
                L"Initialize WinUI settings shell (" +
                std::to_wstring(
                    static_cast<unsigned int>(error.code().value)) +
                L")");
        }
        catch (...)
        {
            SetError(L"Initialize WinUI settings shell failed");
        }

        ReleaseView();
        return false;
    }

    [[nodiscard]] bool HideWindow()
    {
        performance::Scope performanceScope("settings", "hide");
        if (!controller || !window || shuttingDown)
            return false;
        WriteDiagnosticLogEntry((L"SettingsUI close begin pid=" +
            std::to_wstring(GetCurrentProcessId())).c_str());
        if (!FlushPendingChanges())
            return false;
        ++viewEpoch;
        SuspendInteraction();
        DisposePageBackends();
        const SettingsActionResult result = controller->CloseSession();
        if (!result.Succeeded())
        {
            --viewEpoch;
            ResumeInteraction();
            ApplySnapshotNow(controller->Snapshot());
            ShowActionError(result);
            return false;
        }
        if (widgetSettingsService)
            widgetSettingsService->CloseAll();
        ShowWindow(window, SW_HIDE);
        // AppWindow keeps caption-button interaction state with its customized
        // title bar even while the HWND is hidden. Reset only that platform
        // object after hiding; the XAML runtime and settings host stay alive.
        ResetIntegratedTitleBar();
        // Releasing route controls from WM_CLOSE can unwind controls that are
        // still on the XAML input stack. Defer session cleanup to the next
        // DispatcherQueue turn. A newer Open advances viewEpoch and cancels
        // this stale release safely.
        QueueViewRelease();
        if (options.sessionClosed) options.sessionClosed();
        return true;
    }
};

SettingsWindowHost::SettingsWindowHost()
    : impl_(std::make_unique<Impl>())
{
}

SettingsWindowHost::~SettingsWindowHost()
{
    Shutdown();
}

bool SettingsWindowHost::Initialize(
    HINSTANCE instance,
    ISettingsController& controller,
    widget_runtime::IWidgetSettingsService* widgetSettingsService,
    SettingsWindowHostOptions options)
{
    if (impl_->initialized)
        return impl_->OnOwnerThread();
    if (!instance)
    {
        impl_->SetError(L"Settings window initialization requires HINSTANCE");
        return false;
    }

    impl_->ownerThreadId = GetCurrentThreadId();
    impl_->instance = instance;
    impl_->controller = &controller;
    impl_->widgetSettingsService = widgetSettingsService;
    impl_->options = std::move(options);
    impl_->lastError.clear();

    if (!impl_->runtime.Initialize() || !impl_->RegisterWindowClass() ||
        !impl_->CreateHostWindow())
    {
        if (impl_->lastError.empty())
            impl_->lastError = impl_->runtime.LastError();
        Shutdown();
        return false;
    }

    try
    {
        impl_->callbacks =
            std::make_shared<Impl::CallbackState>();
        impl_->callbacks->owner = impl_.get();
        impl_->callbacks->dispatcher =
            mud::DispatcherQueue::GetForCurrentThread();
        if (!impl_->callbacks->dispatcher)
            winrt::throw_hresult(E_UNEXPECTED);
        if (!impl_->CreateView())
        {
            Shutdown();
            return false;
        }

        const std::weak_ptr<Impl::CallbackState> weak = impl_->callbacks;
        controller.SetSnapshotChangedCallback(
            [weak](ISettingsController::SnapshotPtr snapshot) {
                if (const auto state = weak.lock();
                    state && state->alive.load() && state->owner)
                {
                    state->owner->QueueSnapshot(std::move(snapshot));
                }
            });
        controller.SetPendingWorkCallback([weak]() {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->QueuePendingFlush();
            }
        });

        impl_->initialized = true;
        impl_->ApplySnapshotNow(controller.Snapshot());
        impl_->RefreshLocalizedPresentation();
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(
            L"Initialize WinUI settings shell (" +
            std::to_wstring(static_cast<unsigned int>(error.code().value)) +
            L")");
    }
    catch (...)
    {
        impl_->SetError(L"Initialize WinUI settings shell failed");
    }

    Shutdown();
    return false;
}

void SettingsWindowHost::Shutdown() noexcept
{
    if (!impl_ || impl_->shuttingDown)
        return;
    if (impl_->initialized && impl_->OnOwnerThread() && impl_->controller)
        (void)impl_->FlushPendingChanges();
    impl_->shuttingDown = true;

    if (impl_->shell)
        impl_->shell->SetActualThemeChangedCallback({});
    if (impl_->shell)
        impl_->shell->SetWidgetSettingsService(nullptr);
    impl_->DisposePageBackends();
    if (impl_->controller)
    {
        impl_->controller->SetSnapshotChangedCallback({});
        impl_->controller->SetPendingWorkCallback({});
        (void)impl_->controller->CloseSession();
    }
    if (impl_->widgetSettingsService)
        impl_->widgetSettingsService->CloseAll();

    if (impl_->callbacks)
    {
        impl_->callbacks->alive.store(false);
        impl_->callbacks->owner = nullptr;
        {
            std::lock_guard lock(impl_->callbacks->snapshotMutex);
            impl_->callbacks->latestSnapshot.reset();
        }
    }

    impl_->ReleaseView();
    impl_->DiscardPostedOwnerTasks();
    if (impl_->window && IsWindow(impl_->window))
        DestroyWindow(impl_->window);
    impl_->window = nullptr;
    impl_->runtime.Shutdown();

    impl_->callbacks.reset();
    impl_->widgetEngine = nullptr;
    impl_->widgetSettingsService = nullptr;
    impl_->controller = nullptr;
    impl_->instance = nullptr;
    impl_->initialized = false;
    impl_->interactionSuspended = true;
    impl_->ownerThreadId = 0;
    impl_->shuttingDown = false;
}

bool SettingsWindowHost::Open(const SettingsRoute& route)
{
    performance::Scope performanceScope("settings.open", SettingsPageKey(route.page));
    impl_->lastError.clear();
    if (!impl_->initialized)
    {
        impl_->SetError(L"Open settings before host initialization");
        return false;
    }
    if (!impl_->OnOwnerThread())
    {
        impl_->SetError(L"Open settings from a different thread");
        return false;
    }
    if (!route.IsValid())
    {
        impl_->SetError(L"Open settings with an invalid route");
        return false;
    }
    if (!impl_->window || !IsWindow(impl_->window))
    {
        impl_->SetError(L"Open settings with an invalid host window");
        return false;
    }

    ++impl_->viewEpoch;
    impl_->viewReleaseQueued = false;
    impl_->CancelWorkingSetTrim();
    const bool reopening = !impl_->Visible();
    SettingsActionResult reloadResult = SettingsActionResult::Success();
    if (reopening)
    {
        if ((!impl_->shell || !impl_->runtime.IsAttached()) &&
            !impl_->CreateView())
        {
            return false;
        }
        // Sample only after any stable Shell/Island recovery so retained root
        // pages are not mistaken for settings-session growth.
        impl_->settingsSessionWorkingSetBaseline =
            QueryCurrentProcessWorkingSet();
        if (!impl_->appWindowTitleBar)
        {
            if (!impl_->ConfigureIntegratedTitleBar())
                return false;
            impl_->QueueIntegratedTitleBarInsetsUpdate();
        }
        reloadResult = impl_->controller->Reload(
            SettingsReloadPolicy::PreservePendingChanges);
        impl_->RefreshLocalizedPresentation();
    }

    // Re-opening an already visible settings HWND must still observe changes
    // made by another SnowDesktop build or by Windows Startup Apps settings.
    impl_->RefreshExternalStateNow();

    const bool refreshActiveWidgetsPage = !reopening &&
        impl_->widgetsPageActive && impl_->widgetsBackendPage == route.page;
    SettingsActionResult openResult;
    if (!impl_->CommitRoute(route, &openResult))
    {
        if (impl_->lastError.empty())
            impl_->SetError(L"Commit settings route failed");
        return false;
    }
    const auto snapshot = impl_->controller->Snapshot();
    impl_->ApplySnapshotNow(snapshot);
    if (refreshActiveWidgetsPage && impl_->widgetsPageBackend)
        (void)impl_->widgetsPageBackend->Refresh();
    impl_->RefreshAgentSkillNavigationState();
    impl_->ResumeInteraction();
    if (IsIconic(impl_->window))
        ShowWindow(impl_->window, SW_RESTORE);
    else
        ShowWindow(impl_->window, SW_SHOWNORMAL);
    (void)ActivateSettingsWindow(impl_->window);

    if (!IsWindowVisible(impl_->window))
    {
        impl_->SetError(L"Settings host window remained hidden after show");
        return false;
    }

    if (snapshot && impl_->shell &&
        route.page == SettingsPage::General &&
        route.focusId == "general.advancedFeatures.unlockRequired" &&
        impl_->options.advancedFeatureStatus)
    {
        // Opening settings is asynchronous; unlocking may have finished meanwhile.
        const auto status = impl_->options.advancedFeatureStatus();
        if (status.bridgeAvailable && !status.registered)
        {
            (void)impl_->shell->ShowInfoForGeneration(snapshot->generation,
                shell_impl::SettingsShellInfoSeverity::Informational,
                impl_->L("settings.general.advancedFeatures"),
                impl_->L("settings.general.advancedFeatures.unlockRequired"));
        }
    }

    if (!reloadResult.Succeeded())
        impl_->ShowActionError(reloadResult);
    if (!openResult.Succeeded())
        impl_->ShowActionError(openResult);
    return true;
}

bool SettingsWindowHost::Hide()
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->HideWindow();
}

bool SettingsWindowHost::FlushPendingChanges()
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->FlushPendingChanges();
}

void SettingsWindowHost::SetWidgetSettingsService(
    widget_runtime::IWidgetSettingsService* service) noexcept
{
    if (impl_->shell)
        impl_->shell->SetWidgetSettingsService(service);
    impl_->widgetSettingsService = service;
}

void SettingsWindowHost::SetWidgetEngine(WidgetEngine* engine)
{
    if (!impl_->OnOwnerThread() || impl_->widgetEngine == engine)
        return;
    impl_->widgetsPageActive = false;
    impl_->widgetsBackendPage = SettingsPage::Home;
    if (impl_->widgetsPageBackend)
    {
        impl_->widgetsPageBackend->Close();
        impl_->widgetsPageBackend.reset();
    }
    if (impl_->shell)
        impl_->shell->SetWidgetsPageActions({});
    impl_->widgetEngine = engine;
    if (!impl_->initialized || !impl_->controller)
        return;
    impl_->ConfigureWidgetsPageBackend();
    const auto snapshot = impl_->controller->Snapshot();
    if (snapshot && snapshot->sessionActive)
        impl_->SynchronizePageBackends(*snapshot);
    impl_->RebuildSearchIndex();
}

void SettingsWindowHost::RefreshWidgetsPage()
{
    if (!impl_->initialized || !impl_->OnOwnerThread())
        return;
    if (impl_->widgetsPageBackend)
        (void)impl_->widgetsPageBackend->Refresh();
    impl_->RebuildSearchIndex();
}

void SettingsWindowHost::RefreshGeneralRuntimeState()
{
    if (!impl_->initialized || !impl_->OnOwnerThread() || !impl_->shell)
        return;
    impl_->shell->RefreshRuntimeState();
}

bool SettingsWindowHost::PrepareLanguageChange()
{
    if (!impl_->initialized)
        return true;
    return impl_->OnOwnerThread() && impl_->PrepareLanguageChange();
}

void SettingsWindowHost::ApplyLanguageChange(bool widgetRuntimeReloaded)
{
    if (impl_->initialized && impl_->OnOwnerThread())
    {
        if (widgetRuntimeReloaded)
            impl_->ReloadActiveWidgetSettingsForLanguageChange();
        impl_->RefreshLocalizedPresentation();
    }
}

bool SettingsWindowHost::PublishHomeAboutStatus(
    HomeAboutStatusPatch patch)
{
    const bool applied = impl_->initialized && impl_->OnOwnerThread() && impl_->shell &&
        impl_->controller &&
        impl_->controller->IsGenerationCurrent(patch.generation) &&
        impl_->shell->ApplyHomeAboutStatusPatch(patch);
    return applied;
}

bool SettingsWindowHost::PreTranslateMessage(MSG* message) noexcept
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->runtime.PreTranslateMessage(message);
}

bool SettingsWindowHost::ProcessTabNavigation(MSG* message) noexcept
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->runtime.ProcessTabNavigation(message);
}

bool SettingsWindowHost::IsHotkeyCaptureActive() const noexcept
{
    return impl_->initialized && impl_->shell &&
        impl_->shell->IsHotkeyCaptureActive();
}

void SettingsWindowHost::CaptureRegisteredHotkey(
    UINT modifiers, UINT virtualKey)
{
    if (impl_->initialized && impl_->shell)
        impl_->shell->CaptureRegisteredHotkey(modifiers, virtualKey);
}

void SettingsWindowHost::ShowExitConfirmation(
    std::function<void(bool)> completed)
{
    if (!impl_->initialized || !impl_->shell || !impl_->controller)
    {
        if (completed)
            completed(false);
        return;
    }
    const auto snapshot = impl_->controller->Snapshot();
    if (!snapshot || !snapshot->sessionActive)
    {
        if (completed)
            completed(false);
        return;
    }
    shell_impl::SettingsShellDialogRequest request;
    request.generation = snapshot->generation;
    request.title = impl_->L("app.settings.exit_confirm");
    request.message = impl_->L("app.settings.exit_confirm_text") + L"\n\n" +
        impl_->L("app.settings.exit_restore_text");
    request.primaryButtonText = impl_->L("app.settings.exit_ok");
    request.closeButtonText = impl_->L("app.settings.cancel");
    request.destructive = true;
    impl_->shell->ShowConfirmation(std::move(request), std::move(completed));
}

bool SettingsWindowHost::IsInitialized() const noexcept
{
    return impl_->initialized;
}

bool SettingsWindowHost::IsVisible() const noexcept
{
    return impl_->Visible();
}

HWND SettingsWindowHost::Window() const noexcept
{
    return impl_->window;
}

const std::wstring& SettingsWindowHost::LastError() const noexcept
{
    return impl_->lastError;
}

} // namespace snowdesktop::winui
