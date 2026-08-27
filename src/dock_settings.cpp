#include "dock_settings.h"

#include "data_paths.h"
#include "deployment_context.h"
#include "taskbar_hook/taskbar_hook_protocol.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
bool ReadBoolField(const std::string& text, const char* field, bool& out)
{
    const std::string marker = "\"" + std::string(field) + "\"";
    size_t position = text.find(marker);
    if (position == std::string::npos) return false;
    position = text.find(':', position);
    if (position == std::string::npos) return false;
    position = text.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string::npos) return false;
    if (text.compare(position, 4, "true") == 0) { out = true; return true; }
    if (text.compare(position, 5, "false") == 0) { out = false; return true; }
    return false;
}

bool ReadDoubleField(const std::string& text, const char* field, double& out)
{
    const std::string marker = "\"" + std::string(field) + "\"";
    size_t position = text.find(marker);
    if (position == std::string::npos) return false;
    position = text.find(':', position);
    if (position == std::string::npos) return false;
    position = text.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string::npos) return false;
    try
    {
        out = std::stod(text.substr(position));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void ReadDynamicRule(const std::string& text, const char* prefix,
    SystemTaskbarDynamicRule& rule)
{
    const std::string base(prefix);
    const auto key = [&base](const char* suffix) {
        return base + suffix;
    };
    double value = 0.0;
    ReadBoolField(text, key("Enabled").c_str(), rule.enabled);
    if (ReadDoubleField(text, key("Mode").c_str(), value))
    {
        rule.themeMode = static_cast<SystemTaskbarThemeMode>(
            std::clamp(static_cast<int>(value), 0, 9));
    }
    if (ReadDoubleField(text, key("ContentTheme").c_str(), value))
        rule.contentTheme = std::clamp(static_cast<int>(value), -1, 1);

    PersonalizationSettings& style = rule.appearance;
    if (ReadDoubleField(text, key("BackgroundR").c_str(), value))
        style.widgetBgR = static_cast<float>(value);
    if (ReadDoubleField(text, key("BackgroundG").c_str(), value))
        style.widgetBgG = static_cast<float>(value);
    if (ReadDoubleField(text, key("BackgroundB").c_str(), value))
        style.widgetBgB = static_cast<float>(value);
    if (ReadDoubleField(text, key("BorderR").c_str(), value))
        style.widgetBorderR = static_cast<float>(value);
    if (ReadDoubleField(text, key("BorderG").c_str(), value))
        style.widgetBorderG = static_cast<float>(value);
    if (ReadDoubleField(text, key("BorderB").c_str(), value))
        style.widgetBorderB = static_cast<float>(value);
    if (ReadDoubleField(text, key("BackgroundAlpha").c_str(), value))
        style.widgetAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, key("BorderAlpha").c_str(), value))
        style.widgetBorderAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, key("BlurRadius").c_str(), value))
        style.glassBlurRadius = static_cast<float>(value);
    ReadBoolField(text, key("GlassEnabled").c_str(), style.glassEnabled);
    ReadBoolField(text, key("AcrylicEnabled").c_str(),
        style.acrylicEnabled);
    style.backgroundPreset = kAppearancePresetCustom;
}

void WriteDynamicRule(std::ostream& file, const char* prefix,
    const SystemTaskbarDynamicRule& rule)
{
    const PersonalizationSettings& style = rule.appearance;
    file << "  \"" << prefix << "Enabled\": "
         << (rule.enabled ? "true" : "false") << ",\n";
    file << "  \"" << prefix << "Mode\": "
         << static_cast<int>(rule.themeMode) << ",\n";
    file << "  \"" << prefix << "ContentTheme\": "
         << rule.contentTheme << ",\n";
    file << "  \"" << prefix << "BackgroundR\": "
         << style.widgetBgR << ",\n";
    file << "  \"" << prefix << "BackgroundG\": "
         << style.widgetBgG << ",\n";
    file << "  \"" << prefix << "BackgroundB\": "
         << style.widgetBgB << ",\n";
    file << "  \"" << prefix << "BorderR\": "
         << style.widgetBorderR << ",\n";
    file << "  \"" << prefix << "BorderG\": "
         << style.widgetBorderG << ",\n";
    file << "  \"" << prefix << "BorderB\": "
         << style.widgetBorderB << ",\n";
    file << "  \"" << prefix << "BackgroundAlpha\": "
         << style.widgetAlpha << ",\n";
    file << "  \"" << prefix << "BorderAlpha\": "
         << style.widgetBorderAlpha << ",\n";
    file << "  \"" << prefix << "GlassEnabled\": "
         << (style.glassEnabled ? "true" : "false") << ",\n";
    file << "  \"" << prefix << "AcrylicEnabled\": "
         << (style.acrylicEnabled ? "true" : "false") << ",\n";
    file << "  \"" << prefix << "BlurRadius\": "
         << style.glassBlurRadius << ",\n";
}

std::vector<HWND> FindSystemTaskbarWindows()
{
    std::vector<HWND> taskbars;
    if (HWND primaryTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr))
        taskbars.push_back(primaryTaskbar);
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        wchar_t className[64]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
        {
            auto& taskbars =
                *reinterpret_cast<std::vector<HWND>*>(parameter);
            if (std::find(taskbars.begin(), taskbars.end(), window) ==
                taskbars.end())
                taskbars.push_back(window);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&taskbars));
    return taskbars;
}

bool IsWindows11OrGreater()
{
    using RtlGetVersionProc = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionProc>(
        GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!rtlGetVersion) return false;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return rtlGetVersion(&version) == 0 &&
        version.dwMajorVersion >= 10 && version.dwBuildNumber >= 22000;
}

bool ReadSystemTaskbarAutoHideEnabled()
{
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    return (SHAppBarMessage(ABM_GETSTATE, &data) & ABS_AUTOHIDE) != 0;
}

bool ReadSystemTaskbarAlignmentCentered()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            L"TaskbarAl", RRF_RT_REG_DWORD, nullptr,
            &value, &size) != ERROR_SUCCESS)
        return true;
    return value != 0;
}

bool ReadWindowsSystemLightThemeEnabled()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr,
            &value, &size) != ERROR_SUCCESS)
        return true;
    return value != 0;
}

bool ApplySystemTaskbarAutoHideEnabled(bool enabled)
{
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    const UINT_PTR currentState = SHAppBarMessage(ABM_GETSTATE, &data);
    const bool current = (currentState & ABS_AUTOHIDE) != 0;
    if (current == enabled)
        return true;

    data.lParam = static_cast<LPARAM>(enabled
        ? (currentState | ABS_AUTOHIDE)
        : (currentState & ~static_cast<UINT_PTR>(ABS_AUTOHIDE)));
    SHAppBarMessage(ABM_SETSTATE, &data);
    return ReadSystemTaskbarAutoHideEnabled() == enabled;
}

bool ApplySystemTaskbarAlignmentCentered(bool centered)
{
    if (ReadSystemTaskbarAlignmentCentered() == centered)
        return true;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const DWORD value = centered ? 1 : 0;
    const LONG result = RegSetValueExW(key, L"TaskbarAl", 0,
        REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        return false;

    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        reinterpret_cast<LPARAM>(L"TraySettings"),
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 1500, &ignored);
    return ReadSystemTaskbarAlignmentCentered() == centered;
}

bool ApplyWindowsSystemLightThemeEnabled(bool enabled)
{
    if (ReadWindowsSystemLightThemeEnabled() == enabled)
        return true;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const DWORD value = enabled ? 1 : 0;
    const LONG result = RegSetValueExW(key, L"SystemUsesLightTheme", 0,
        REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        return false;

    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 1500, &ignored);
    SendNotifyMessageW(HWND_BROADCAST, WM_THEMECHANGED, 0, 0);
    return ReadWindowsSystemLightThemeEnabled() == enabled;
}

class WindowsShellSettingsController
{
public:
    WindowsShellSettingsController()
        : worker_([this] { Run(); })
    {
    }

    ~WindowsShellSettingsController()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable())
            worker_.join();
    }

    bool RequestAutoHide(bool enabled)
    {
        return Request(autoHide_, enabled);
    }

    bool RequestAlignment(bool centered)
    {
        return Request(alignment_, centered);
    }

    bool RequestSystemTheme(bool enabled)
    {
        return Request(systemTheme_, enabled);
    }

    std::optional<bool> RequestedAutoHide() const
    {
        return Requested(autoHide_);
    }

    std::optional<bool> RequestedAlignment() const
    {
        return Requested(alignment_);
    }

    std::optional<bool> RequestedSystemTheme() const
    {
        return Requested(systemTheme_);
    }

private:
    struct SettingRequest
    {
        bool pending = false;
        bool visible = false;
        bool value = false;
        std::uint64_t generation = 0;
    };

    struct WorkItem
    {
        bool valid = false;
        bool value = false;
        std::uint64_t generation = 0;
    };

    bool Request(SettingRequest& request, bool value)
    {
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
                return false;
            request.pending = true;
            request.visible = true;
            request.value = value;
            ++request.generation;
        }
        wake_.notify_one();
        return true;
    }

    std::optional<bool> Requested(const SettingRequest& request) const
    {
        std::lock_guard lock(mutex_);
        if (!request.visible)
            return std::nullopt;
        return request.value;
    }

    static WorkItem Take(SettingRequest& request)
    {
        if (!request.pending)
            return {};
        request.pending = false;
        return { true, request.value, request.generation };
    }

    void Complete(SettingRequest& request, const WorkItem& work)
    {
        if (!work.valid)
            return;
        std::lock_guard lock(mutex_);
        if (!request.pending && request.generation == work.generation)
            request.visible = false;
    }

    void Run()
    {
        for (;;)
        {
            WorkItem autoHide;
            WorkItem alignment;
            WorkItem systemTheme;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] {
                    return stopping_ || autoHide_.pending ||
                        alignment_.pending || systemTheme_.pending;
                });
                if (stopping_ && !autoHide_.pending &&
                    !alignment_.pending && !systemTheme_.pending)
                    return;
                autoHide = Take(autoHide_);
                alignment = Take(alignment_);
                systemTheme = Take(systemTheme_);
            }

            if (autoHide.valid)
                ApplySystemTaskbarAutoHideEnabled(autoHide.value);
            Complete(autoHide_, autoHide);
            if (alignment.valid)
                ApplySystemTaskbarAlignmentCentered(alignment.value);
            Complete(alignment_, alignment);
            if (systemTheme.valid)
                ApplyWindowsSystemLightThemeEnabled(systemTheme.value);
            Complete(systemTheme_, systemTheme);
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    SettingRequest autoHide_;
    SettingRequest alignment_;
    SettingRequest systemTheme_;
    bool stopping_ = false;
    std::thread worker_;
};

WindowsShellSettingsController& GetWindowsShellSettingsController()
{
    static WindowsShellSettingsController controller;
    return controller;
}

class TaskbarBackdropController
{
public:
    TaskbarBackdropController()
        : injectionCancelEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
    }

    ~TaskbarBackdropController()
    {
        if (injectionCancelEvent_)
            SetEvent(injectionCancelEvent_);
        if (injectionThread_.joinable())
            injectionThread_.join();
        if (injectionCancelEvent_)
            CloseHandle(injectionCancelEvent_);
    }

    void NotifyTaskbarCreated()
    {
        std::lock_guard lock(mutex_);
        if (!OpenState()) return;

        DWORD currentExplorerProcessId = 0;
        if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr))
            GetWindowThreadProcessId(taskbar, &currentExplorerProcessId);

        // A TaskbarCreated broadcast can arrive before Shell_TrayWnd is fully
        // available. Clear only stale-process state; same-process taskbar
        // recreation is still owned by the existing visual-tree watcher.
        const bool explorerProcessChanged = !currentExplorerProcessId ||
            state_->explorerProcessId != currentExplorerProcessId;
        if (explorerProcessChanged)
        {
            if (injectionCancelEvent_)
                SetEvent(injectionCancelEvent_);
            state_->explorerProcessId = 0;
            InterlockedExchange(&state_->status,
                snowdesktop::taskbar_hook::kStatusIdle);
            InterlockedExchange(&state_->lastError, ERROR_SUCCESS);
            InterlockedExchange(&state_->diagnosticStage, 0);
        }
        // A same-process taskbar rebuild should first be handled by the already
        // installed visual-tree watcher. A new Explorer process needs an
        // immediate fresh injection.
        lastInjectionAttemptTick_ = explorerProcessChanged
            ? 0 : GetTickCount64();
    }

    bool Apply(bool hookEnabled, bool defaultEnabled,
        const PersonalizationSettings& appearance,
        const std::vector<SystemTaskbarTargetAppearance>& targets)
    {
        // Reap a completed worker before reusing its std::thread object. The
        // actual injection never runs on the UI thread.
        if (!injectionInFlight_.load(std::memory_order_acquire) &&
            injectionThread_.joinable())
            injectionThread_.join();

        std::lock_guard lock(mutex_);
        if (!OpenState())
            return false;

        HWND primaryTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD explorerProcessId = 0;
        if (primaryTaskbar)
            GetWindowThreadProcessId(primaryTaskbar, &explorerProcessId);

        InterlockedIncrement(&state_->generation); // odd: write in progress
        state_->enabled = hookEnabled ? TRUE : FALSE;
        state_->defaultEnabled = defaultEnabled ? TRUE : FALSE;
        state_->style = appearance.glassEnabled
            ? snowdesktop::taskbar_hook::kStyleGlassBackdrop : 0;
        if (appearance.glassEnabled && appearance.acrylicEnabled)
            state_->style |= snowdesktop::taskbar_hook::kStyleAcrylicBackdrop;
        state_->contentTheme = appearance.contentTheme;
        state_->systemUsesLightTheme = IsWindowsSystemLightThemeEnabled()
            ? TRUE : FALSE;
        state_->ownerProcessId = GetCurrentProcessId();
        state_->red = std::clamp(appearance.widgetBgR, 0.0f, 1.0f);
        state_->green = std::clamp(appearance.widgetBgG, 0.0f, 1.0f);
        state_->blue = std::clamp(appearance.widgetBgB, 0.0f, 1.0f);
        state_->alpha = std::clamp(appearance.widgetAlpha, 0.0f, 1.0f);
        state_->blurAmount = std::clamp(appearance.glassBlurRadius, 0.0f, 48.0f);
        state_->borderRed = std::clamp(appearance.widgetBorderR, 0.0f, 1.0f);
        state_->borderGreen = std::clamp(appearance.widgetBorderG, 0.0f, 1.0f);
        state_->borderBlue = std::clamp(appearance.widgetBorderB, 0.0f, 1.0f);
        state_->borderAlpha = std::clamp(appearance.widgetBorderAlpha, 0.0f, 1.0f);
        const LONG targetCount = static_cast<LONG>(std::min<std::size_t>(
            targets.size(),
            snowdesktop::taskbar_hook::kMaximumTaskbarTargets));
        state_->targetCount = targetCount;
        for (LONG index = 0; index < targetCount; ++index)
        {
            const SystemTaskbarTargetAppearance& source =
                targets[static_cast<std::size_t>(index)];
            snowdesktop::taskbar_hook::TargetAppearance& destination =
                state_->targets[static_cast<std::size_t>(index)];
            destination.taskbar =
                reinterpret_cast<std::uintptr_t>(source.taskbar);
            destination.enabled = source.enabled ? TRUE : FALSE;
            destination.style = source.appearance.glassEnabled
                ? snowdesktop::taskbar_hook::kStyleGlassBackdrop : 0;
            if (source.appearance.glassEnabled &&
                source.appearance.acrylicEnabled)
            {
                destination.style |=
                    snowdesktop::taskbar_hook::kStyleAcrylicBackdrop;
            }
            destination.contentTheme = source.appearance.contentTheme;
            destination.red = std::clamp(source.appearance.widgetBgR,
                0.0f, 1.0f);
            destination.green = std::clamp(source.appearance.widgetBgG,
                0.0f, 1.0f);
            destination.blue = std::clamp(source.appearance.widgetBgB,
                0.0f, 1.0f);
            destination.alpha = std::clamp(source.appearance.widgetAlpha,
                0.0f, 1.0f);
            destination.blurAmount = std::clamp(
                source.appearance.glassBlurRadius, 0.0f, 48.0f);
            destination.borderRed = std::clamp(
                source.appearance.widgetBorderR, 0.0f, 1.0f);
            destination.borderGreen = std::clamp(
                source.appearance.widgetBorderG, 0.0f, 1.0f);
            destination.borderBlue = std::clamp(
                source.appearance.widgetBorderB, 0.0f, 1.0f);
            destination.borderAlpha = std::clamp(
                source.appearance.widgetBorderAlpha, 0.0f, 1.0f);
        }
        for (std::size_t index = static_cast<std::size_t>(targetCount);
             index < snowdesktop::taskbar_hook::kMaximumTaskbarTargets;
             ++index)
        {
            state_->targets[index] =
                snowdesktop::taskbar_hook::TargetAppearance{};
        }
        MemoryBarrier();
        InterlockedIncrement(&state_->generation); // even: snapshot complete

        const UINT applyMessage = RegisterWindowMessageW(
            snowdesktop::taskbar_hook::kApplyMessageName);
        std::vector<HWND> taskbars = FindSystemTaskbarWindows();
        for (const SystemTaskbarTargetAppearance& target : targets)
        {
            if (target.taskbar && IsWindow(target.taskbar) &&
                std::find(taskbars.begin(), taskbars.end(), target.taskbar) ==
                    taskbars.end())
                taskbars.push_back(target.taskbar);
        }
        for (HWND taskbar : taskbars)
        {
            if (hookEnabled)
            {
                PostMessageW(taskbar, applyMessage, 0, 0);
                continue;
            }

            // Process the restore while the shared state and owner process are
            // still alive. The Explorer-side owner watcher remains a crash
            // fallback, but graceful exit must not race an asynchronous post.
            DWORD_PTR ignored = 0;
            SendMessageTimeoutW(taskbar, applyMessage, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored);
        }

        if (!hookEnabled)
        {
            if (injectionCancelEvent_)
                SetEvent(injectionCancelEvent_);
            return true;
        }
        if (!IsWindows11OrGreater() || !primaryTaskbar || !explorerProcessId)
            return false;

        if (state_->explorerProcessId == explorerProcessId &&
            state_->status >= snowdesktop::taskbar_hook::kStatusInjecting)
            return true;
        if (injectionInFlight_.load(std::memory_order_acquire))
            return false;
        const ULONGLONG now = GetTickCount64();
        constexpr ULONGLONG kFailedInjectionRetryDelayMs = 10000;
        if (state_->explorerProcessId == explorerProcessId &&
            now - lastInjectionAttemptTick_ < kFailedInjectionRetryDelayMs)
            return false;

        state_->explorerProcessId = explorerProcessId;
        lastInjectionAttemptTick_ = now;
        InterlockedExchange(&state_->status,
            snowdesktop::taskbar_hook::kStatusInjecting);
        InterlockedExchange(&state_->lastError, ERROR_SUCCESS);
        if (injectionCancelEvent_)
            ResetEvent(injectionCancelEvent_);
        injectionInFlight_.store(true, std::memory_order_release);
        try
        {
            injectionThread_ = std::thread([this, primaryTaskbar, explorerProcessId] {
                const bool injected = Inject(primaryTaskbar, explorerProcessId);
                {
                    std::lock_guard workerLock(mutex_);
                    if (!injected && state_ &&
                        state_->explorerProcessId == explorerProcessId)
                        lastInjectionAttemptTick_ = GetTickCount64();
                }
                injectionInFlight_.store(false, std::memory_order_release);
            });
        }
        catch (...)
        {
            injectionInFlight_.store(false, std::memory_order_release);
            lastInjectionAttemptTick_ = GetTickCount64();
            InterlockedExchange(&state_->lastError, ERROR_NOT_ENOUGH_MEMORY);
            InterlockedExchange(&state_->status,
                snowdesktop::taskbar_hook::kStatusFailed);
            return false;
        }
        return true;
    }

    SystemTaskbarBackdropRuntimeState RuntimeState()
    {
        std::lock_guard lock(mutex_);
        if (!state_ || !state_->enabled)
            return SystemTaskbarBackdropRuntimeState::Disabled;
        if (!IsWindows11OrGreater())
            return SystemTaskbarBackdropRuntimeState::Unsupported;
        if (state_->status >= snowdesktop::taskbar_hook::kStatusApplied)
            return SystemTaskbarBackdropRuntimeState::Active;
        if (state_->status < snowdesktop::taskbar_hook::kStatusIdle)
            return SystemTaskbarBackdropRuntimeState::Failed;
        return SystemTaskbarBackdropRuntimeState::Loading;
    }

private:
    bool OpenState()
    {
        if (state_)
            return true;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(snowdesktop::taskbar_hook::SharedState),
            snowdesktop::taskbar_hook::kSharedStateName);
        if (!mapping_)
            return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        state_ = static_cast<snowdesktop::taskbar_hook::SharedState*>(
            MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                sizeof(snowdesktop::taskbar_hook::SharedState)));
        if (!state_)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        if (created ||
            state_->magic != snowdesktop::taskbar_hook::kSharedStateMagic ||
            state_->version != snowdesktop::taskbar_hook::kSharedStateVersion ||
            state_->size != sizeof(snowdesktop::taskbar_hook::SharedState))
        {
            *state_ = snowdesktop::taskbar_hook::SharedState{};
        }
        return true;
    }

    bool Inject(HWND taskbar, DWORD expectedExplorerProcessId)
    {
        const std::filesystem::path hookPath =
            snowdesktop::deployment::GetTaskbarHookPath();
        if (!std::filesystem::is_regular_file(hookPath))
        {
            return Fail(expectedExplorerProcessId, ERROR_FILE_NOT_FOUND);
        }

        HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE,
            snowdesktop::taskbar_hook::kReadyEventName);
        if (!readyEvent)
            return Fail(expectedExplorerProcessId, GetLastError());
        ResetEvent(readyEvent);

        HANDLE explorerProcess = OpenProcess(SYNCHRONIZE, FALSE,
            expectedExplorerProcessId);

        HMODULE module = LoadLibraryW(hookPath.c_str());
        if (!module)
        {
            const DWORD error = GetLastError();
            if (explorerProcess)
                CloseHandle(explorerProcess);
            CloseHandle(readyEvent);
            return Fail(expectedExplorerProcessId, error);
        }
        auto hookProc = reinterpret_cast<HOOKPROC>(
            GetProcAddress(module, "SnowDesktopTaskbarHookProc"));
        DWORD processId = 0;
        const DWORD threadId = GetWindowThreadProcessId(taskbar, &processId);
        HHOOK hook = hookProc && threadId
            ? SetWindowsHookExW(WH_CALLWNDPROC, hookProc, module, threadId)
            : nullptr;
        if (!hook)
        {
            const DWORD error = GetLastError();
            FreeLibrary(module);
            if (explorerProcess)
                CloseHandle(explorerProcess);
            CloseHandle(readyEvent);
            return Fail(expectedExplorerProcessId,
                error ? error : ERROR_INVALID_FUNCTION);
        }

        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(taskbar, WM_NULL, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored);
        std::array<HANDLE, 3> waitHandles{};
        DWORD waitCount = 0;
        waitHandles[waitCount++] = readyEvent;
        const DWORD explorerWaitIndex = explorerProcess ? waitCount : MAXDWORD;
        if (explorerProcess)
            waitHandles[waitCount++] = explorerProcess;
        const DWORD cancelWaitIndex = injectionCancelEvent_ ? waitCount : MAXDWORD;
        if (injectionCancelEvent_)
            waitHandles[waitCount++] = injectionCancelEvent_;
        const DWORD waitResult = WaitForMultipleObjects(
            waitCount, waitHandles.data(), FALSE, 35000);
        UnhookWindowsHookEx(hook);
        FreeLibrary(module);
        if (explorerProcess)
            CloseHandle(explorerProcess);
        CloseHandle(readyEvent);

        if (cancelWaitIndex != MAXDWORD &&
            waitResult == WAIT_OBJECT_0 + cancelWaitIndex)
            return false;
        if (explorerWaitIndex != MAXDWORD &&
            waitResult == WAIT_OBJECT_0 + explorerWaitIndex)
            return Fail(expectedExplorerProcessId, ERROR_PROCESS_ABORTED);
        if (waitResult != WAIT_OBJECT_0)
            return Fail(expectedExplorerProcessId,
                waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());

        std::lock_guard lock(mutex_);
        return state_ &&
            state_->explorerProcessId == expectedExplorerProcessId &&
            state_->status >= snowdesktop::taskbar_hook::kStatusConnected;
    }

    bool Fail(DWORD expectedExplorerProcessId, DWORD error)
    {
        std::lock_guard lock(mutex_);
        if (state_ && state_->explorerProcessId == expectedExplorerProcessId)
        {
            InterlockedExchange(&state_->lastError, static_cast<LONG>(error));
            InterlockedExchange(&state_->status,
                snowdesktop::taskbar_hook::kStatusFailed);
        }
        return false;
    }

    std::mutex mutex_;
    HANDLE mapping_ = nullptr;
    snowdesktop::taskbar_hook::SharedState* state_ = nullptr;
    HANDLE injectionCancelEvent_ = nullptr;
    std::thread injectionThread_;
    std::atomic<bool> injectionInFlight_{ false };
    ULONGLONG lastInjectionAttemptTick_ = 0;
};

TaskbarBackdropController& GetTaskbarBackdropController()
{
    static TaskbarBackdropController controller;
    return controller;
}
}

std::wstring GetDockSettingsPath()
{
    return GetDataFilePath(L"SnowDesktop.dock.json");
}

bool IsSystemTaskbarAutoHideEnabled()
{
    if (const auto requested =
            GetWindowsShellSettingsController().RequestedAutoHide())
        return *requested;
    return ReadSystemTaskbarAutoHideEnabled();
}

bool RequestSystemTaskbarAutoHideEnabled(bool enabled)
{
    return GetWindowsShellSettingsController().RequestAutoHide(enabled);
}

bool IsSystemTaskbarAlignmentCentered()
{
    if (const auto requested =
            GetWindowsShellSettingsController().RequestedAlignment())
        return *requested;
    return ReadSystemTaskbarAlignmentCentered();
}

bool RequestSystemTaskbarAlignmentCentered(bool centered)
{
    return GetWindowsShellSettingsController().RequestAlignment(centered);
}

bool IsWindowsSystemLightThemeEnabled()
{
    if (const auto requested =
            GetWindowsShellSettingsController().RequestedSystemTheme())
        return *requested;
    return ReadWindowsSystemLightThemeEnabled();
}

bool RequestWindowsSystemLightThemeEnabled(bool enabled)
{
    return GetWindowsShellSettingsController().RequestSystemTheme(enabled);
}

bool RestartWindowsExplorer()
{
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD explorerProcessId = 0;
    if (!taskbar || !GetWindowThreadProcessId(taskbar, &explorerProcessId) ||
        explorerProcessId == 0)
        return false;

    HANDLE explorerProcess = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE, explorerProcessId);
    if (!explorerProcess)
        return false;

    // Ask Explorer to run the same clean shell-exit path used by its hidden
    // "Exit Explorer" command. This lets in-process taskbar hooks and the XAML
    // visual tree unwind before a new shell instance is launched. The command
    // is shell-private, so retain a force-terminate fallback for Windows builds
    // that no longer handle it.
    constexpr UINT kExitExplorerMessage = WM_USER + 436;
    const bool exitRequested = PostMessageW(taskbar,
        kExitExplorerMessage, 0, 0) != FALSE;
    DWORD waitResult = exitRequested
        ? WaitForSingleObject(explorerProcess, 5000)
        : WAIT_TIMEOUT;
    if (waitResult != WAIT_OBJECT_0)
    {
        if (!TerminateProcess(explorerProcess, 0))
        {
            CloseHandle(explorerProcess);
            return false;
        }
        waitResult = WaitForSingleObject(explorerProcess, 5000);
    }
    CloseHandle(explorerProcess);
    if (waitResult != WAIT_OBJECT_0)
        return false;

    wchar_t explorerPath[MAX_PATH]{};
    const UINT windowsPathLength = GetWindowsDirectoryW(
        explorerPath, static_cast<UINT>(std::size(explorerPath)));
    if (!windowsPathLength || windowsPathLength >= std::size(explorerPath))
        return false;
    if (explorerPath[windowsPathLength - 1] != L'\\')
        wcscat_s(explorerPath, L"\\");
    wcscat_s(explorerPath, L"explorer.exe");

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(explorerPath, nullptr, nullptr, nullptr, FALSE, 0,
        nullptr, nullptr, &startupInfo, &processInfo))
        return false;
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

SystemTaskbarBackdropRuntimeState GetSystemTaskbarBackdropRuntimeState()
{
    return GetTaskbarBackdropController().RuntimeState();
}

void NotifySystemTaskbarCreated()
{
    GetTaskbarBackdropController().NotifyTaskbarCreated();
}

bool ApplySystemTaskbarBackdrop(bool hookEnabled, bool defaultEnabled,
    const PersonalizationSettings& appearance,
    const std::vector<SystemTaskbarTargetAppearance>& targets)
{
    return GetTaskbarBackdropController().Apply(hookEnabled, defaultEnabled,
        appearance, targets);
}

PersonalizationSettings MakeTransparentTaskbarAppearance()
{
    PersonalizationSettings appearance =
        PersonalizationSettings::DarkPreset();
    appearance.widgetBgR = 0.0f;
    appearance.widgetBgG = 0.0f;
    appearance.widgetBgB = 0.0f;
    appearance.widgetAlpha = 0.0f;
    appearance.widgetBorderR = 0.0f;
    appearance.widgetBorderG = 0.0f;
    appearance.widgetBorderB = 0.0f;
    appearance.widgetBorderAlpha = 0.0f;
    appearance.gradientEndA = 0.0f;
    appearance.backgroundPreset = kAppearancePresetTaskbarTransparent;
    appearance.glassEnabled = false;
    appearance.acrylicEnabled = false;
    return appearance;
}

bool LoadDockSettings(const wchar_t* path, DockSettings& settings)
{
    NormalizeDockSettings(settings);
    // Older configuration files do not contain this system setting. Start
    // from the current Windows state so upgrading never changes it silently.
    settings.systemTaskbarAutoHide = IsSystemTaskbarAutoHideEnabled();
    settings.systemTaskbarAlignment = IsSystemTaskbarAlignmentCentered() ? 1 : 0;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string text = stream.str();
    if (text.empty()) return false;

    double value = 0.0;
    if (ReadDoubleField(text, "position", value))
        settings.position = static_cast<DockPosition>(std::clamp(static_cast<int>(value), 0, 3));
    ReadBoolField(text, "edgeAttached", settings.edgeAttached);
    ReadBoolField(text, "floatingShortcutMode", settings.floatingShortcutMode);
    if (ReadDoubleField(text, "floatingHotkeyModifiers", value))
    {
        constexpr UINT allowedModifiers =
            MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
        settings.floatingHotkeyModifiers =
            static_cast<UINT>(
                std::max(0, static_cast<int>(value))) &
            allowedModifiers;
    }
    if (ReadDoubleField(text, "floatingHotkeyVirtualKey", value) &&
        value > 0.0 && value <= 0xFF)
    {
        settings.floatingHotkeyVirtualKey =
            static_cast<UINT>(value);
    }
    ReadBoolField(text, "floatingEdgeSwipeEnabled",
        settings.floatingEdgeSwipeEnabled);
    if (ReadDoubleField(text, "monitorScope", value))
    {
        settings.monitorScope = static_cast<DockMonitorScope>(
            std::clamp(static_cast<int>(value), 0, 2));
    }
    else
    {
        // v0.1.21 development builds briefly used a boolean option. Preserve
        // that choice while upgrading to the three-state monitor scope.
        bool showOnAllMonitors = false;
        if (ReadBoolField(text, "showOnAllMonitors", showOnAllMonitors))
        {
            settings.monitorScope = showOnAllMonitors
                ? DockMonitorScope::All : DockMonitorScope::First;
        }
    }
    ReadBoolField(text, "showWindowsButton", settings.showWindowsButton);
    ReadBoolField(text, "showRunningApps", settings.showRunningApps);
    ReadBoolField(text, "showWindowPreviews", settings.showWindowPreviews);
    ReadBoolField(text, "showFrequentItems", settings.showFrequentItems);
    ReadBoolField(text, "keepWhenDesktopHidden",
        settings.keepWhenDesktopHidden);
    if (ReadDoubleField(text, "frequentItemCount", value))
        settings.frequentItemCount = std::clamp(static_cast<int>(value), 1, 8);
    if (ReadDoubleField(text, "thicknessScale", value))
        settings.thicknessScale = ClampDockScale(static_cast<float>(value));
    ReadBoolField(text, "systemTaskbarAutoHide", settings.systemTaskbarAutoHide);
    if (ReadDoubleField(text, "systemTaskbarAlignment", value))
        settings.systemTaskbarAlignment = std::clamp(static_cast<int>(value), 0, 1);
    ReadBoolField(text, "systemTaskbarBackdropEnabled",
        settings.systemTaskbarBackdropEnabled);
    ReadBoolField(text, "systemTaskbarFollowPersonalization",
        settings.systemTaskbarFollowPersonalization);
    PersonalizationSettings& taskbarStyle = settings.systemTaskbarAppearance;
    if (ReadDoubleField(text, "taskbarBackgroundR", value))
        taskbarStyle.widgetBgR = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBackgroundG", value)) taskbarStyle.widgetBgG = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBackgroundB", value)) taskbarStyle.widgetBgB = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBorderR", value)) taskbarStyle.widgetBorderR = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBorderG", value)) taskbarStyle.widgetBorderG = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBorderB", value)) taskbarStyle.widgetBorderB = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBackgroundAlpha", value)) taskbarStyle.widgetAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBorderAlpha", value)) taskbarStyle.widgetBorderAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarGlassBlurRadius", value)) taskbarStyle.glassBlurRadius = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarAppearancePreset", value))
        taskbarStyle.backgroundPreset = NormalizeAppearancePresetId(static_cast<int>(value));
    ReadBoolField(text, "taskbarGlassEnabled", taskbarStyle.glassEnabled);
    ReadBoolField(text, "taskbarAcrylicEnabled", taskbarStyle.acrylicEnabled);
    if (taskbarStyle.backgroundPreset == kAppearancePresetAcrylicDark ||
        taskbarStyle.backgroundPreset == kAppearancePresetAcrylicLight)
    {
        taskbarStyle = MakeAppearancePreset(
            taskbarStyle.backgroundPreset);
    }
    if (ReadDoubleField(text, "taskbarContentTheme", value)) // legacy name
        settings.systemTaskbarContentTheme = std::clamp(static_cast<int>(value), -1, 1);
    if (ReadDoubleField(text, "systemTaskbarContentTheme", value))
        settings.systemTaskbarContentTheme = std::clamp(static_cast<int>(value), -1, 1);
    ReadDynamicRule(text, "systemTaskbarVisibleWindow",
        settings.systemTaskbarVisibleWindow);
    ReadDynamicRule(text, "systemTaskbarMaximizedWindow",
        settings.systemTaskbarMaximizedWindow);
    ReadDynamicRule(text, "systemTaskbarShellUi",
        settings.systemTaskbarShellUi);
    NormalizeDockSettings(settings);
    return true;
}

bool SaveDockSettings(const wchar_t* path, const DockSettings& settings)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    const PersonalizationSettings& taskbarStyle = settings.systemTaskbarAppearance;
    file << "{\n";
    file << "  \"position\": " << static_cast<int>(settings.position) << ",\n";
    file << "  \"edgeAttached\": "
         << (settings.edgeAttached ? "true" : "false") << ",\n";
    file << "  \"floatingShortcutMode\": "
         << (settings.floatingShortcutMode ? "true" : "false") << ",\n";
    file << "  \"floatingHotkeyModifiers\": "
         << settings.floatingHotkeyModifiers << ",\n";
    file << "  \"floatingHotkeyVirtualKey\": "
         << settings.floatingHotkeyVirtualKey << ",\n";
    file << "  \"floatingEdgeSwipeEnabled\": "
         << (settings.floatingEdgeSwipeEnabled ? "true" : "false")
         << ",\n";
    file << "  \"monitorScope\": "
         << static_cast<int>(settings.monitorScope) << ",\n";
    file << "  \"showWindowsButton\": "
         << (settings.showWindowsButton ? "true" : "false") << ",\n";
    // Preserve the legacy keys for downgrade compatibility while migrating
    // every saved configuration to the unconditional feature behavior.
    file << "  \"showRunningApps\": true,\n";
    file << "  \"showWindowPreviews\": true,\n";
    file << "  \"showFrequentItems\": "
         << (settings.showFrequentItems ? "true" : "false") << ",\n";
    file << "  \"keepWhenDesktopHidden\": "
         << (settings.keepWhenDesktopHidden ? "true" : "false") << ",\n";
    file << "  \"frequentItemCount\": " << settings.frequentItemCount << ",\n";
    file << "  \"thicknessScale\": " << settings.thicknessScale << ",\n";
    file << "  \"systemTaskbarAutoHide\": "
         << (settings.systemTaskbarAutoHide ? "true" : "false") << ",\n";
    file << "  \"systemTaskbarAlignment\": " << settings.systemTaskbarAlignment << ",\n";
    file << "  \"systemTaskbarBackdropEnabled\": "
         << (settings.systemTaskbarBackdropEnabled ? "true" : "false") << ",\n";
    file << "  \"systemTaskbarFollowPersonalization\": "
         << (settings.systemTaskbarFollowPersonalization ? "true" : "false") << ",\n";
    file << "  \"taskbarBackgroundR\": " << taskbarStyle.widgetBgR << ",\n";
    file << "  \"taskbarBackgroundG\": " << taskbarStyle.widgetBgG << ",\n";
    file << "  \"taskbarBackgroundB\": " << taskbarStyle.widgetBgB << ",\n";
    file << "  \"taskbarBorderR\": " << taskbarStyle.widgetBorderR << ",\n";
    file << "  \"taskbarBorderG\": " << taskbarStyle.widgetBorderG << ",\n";
    file << "  \"taskbarBorderB\": " << taskbarStyle.widgetBorderB << ",\n";
    file << "  \"taskbarBackgroundAlpha\": " << taskbarStyle.widgetAlpha << ",\n";
    file << "  \"taskbarBorderAlpha\": " << taskbarStyle.widgetBorderAlpha << ",\n";
    file << "  \"taskbarAppearancePreset\": " << taskbarStyle.backgroundPreset << ",\n";
    file << "  \"taskbarGlassEnabled\": "
         << (taskbarStyle.glassEnabled ? "true" : "false") << ",\n";
    file << "  \"taskbarAcrylicEnabled\": "
         << (taskbarStyle.acrylicEnabled ? "true" : "false") << ",\n";
    file << "  \"taskbarGlassBlurRadius\": " << taskbarStyle.glassBlurRadius << ",\n";
    file << "  \"systemTaskbarContentTheme\": "
         << settings.systemTaskbarContentTheme << ",\n";
    WriteDynamicRule(file, "systemTaskbarVisibleWindow",
        settings.systemTaskbarVisibleWindow);
    WriteDynamicRule(file, "systemTaskbarMaximizedWindow",
        settings.systemTaskbarMaximizedWindow);
    WriteDynamicRule(file, "systemTaskbarShellUi",
        settings.systemTaskbarShellUi);
    file << "  \"dynamicTaskbarSchemaVersion\": 1\n";
    file << "}\n";
    return true;
}
