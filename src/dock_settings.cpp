#include "dock_settings.h"

#include "data_paths.h"
#include "taskbar_hook/taskbar_hook_protocol.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
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

std::vector<HWND> FindSystemTaskbarWindows()
{
    std::vector<HWND> taskbars;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        wchar_t className[64]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
            reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
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

class TaskbarBackdropController
{
public:
    ~TaskbarBackdropController()
    {
        if (state_)
            UnmapViewOfFile(state_);
        if (mapping_)
            CloseHandle(mapping_);
    }

    bool Apply(bool enabled, const DockSettings& settings,
        const PersonalizationSettings& appearance)
    {
        std::lock_guard lock(mutex_);
        if (!OpenState())
            return false;

        HWND primaryTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD explorerProcessId = 0;
        if (primaryTaskbar)
            GetWindowThreadProcessId(primaryTaskbar, &explorerProcessId);

        state_->enabled = enabled ? TRUE : FALSE;
        // The material is rendered inside Explorer's taskbar XAML layer so
        // ordinary windows cannot appear between the icons and their backdrop.
        state_->style = appearance.glassEnabled
            ? snowdesktop::taskbar_hook::kStyleGlassBackdrop : 0;
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
        MemoryBarrier();
        InterlockedIncrement(&state_->generation);

        const UINT applyMessage = RegisterWindowMessageW(
            snowdesktop::taskbar_hook::kApplyMessageName);
        for (HWND taskbar : FindSystemTaskbarWindows())
            PostMessageW(taskbar, applyMessage, 0, 0);

        if (!enabled)
            return true;
        if (!IsWindows11OrGreater() || !primaryTaskbar || !explorerProcessId)
            return false;

        if (state_->explorerProcessId == explorerProcessId &&
            state_->status >= snowdesktop::taskbar_hook::kStatusInjecting)
            return true;
        const ULONGLONG now = GetTickCount64();
        if (state_->explorerProcessId == explorerProcessId &&
            now - lastInjectionAttemptTick_ < 60000)
            return false;

        state_->explorerProcessId = explorerProcessId;
        lastInjectionAttemptTick_ = now;
        InterlockedExchange(&state_->status,
            snowdesktop::taskbar_hook::kStatusInjecting);
        InterlockedExchange(&state_->lastError, ERROR_SUCCESS);
        return Inject(primaryTaskbar);
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

    bool Inject(HWND taskbar)
    {
        const std::filesystem::path hookPath =
            std::filesystem::path(GetExecutableDirectoryPath()) /
            L"SnowDesktopTaskbarHook.dll";
        if (!std::filesystem::is_regular_file(hookPath))
        {
            InterlockedExchange(&state_->status,
                snowdesktop::taskbar_hook::kStatusFailed);
            InterlockedExchange(&state_->lastError, ERROR_FILE_NOT_FOUND);
            return false;
        }

        HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE,
            snowdesktop::taskbar_hook::kReadyEventName);
        if (!readyEvent)
            return Fail(GetLastError());
        ResetEvent(readyEvent);

        HMODULE module = LoadLibraryW(hookPath.c_str());
        if (!module)
        {
            const DWORD error = GetLastError();
            CloseHandle(readyEvent);
            return Fail(error);
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
            CloseHandle(readyEvent);
            return Fail(error ? error : ERROR_INVALID_FUNCTION);
        }

        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(taskbar, WM_NULL, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored);
        const DWORD waitResult = WaitForSingleObject(readyEvent, 35000);
        UnhookWindowsHookEx(hook);
        FreeLibrary(module);
        CloseHandle(readyEvent);

        if (waitResult != WAIT_OBJECT_0)
            return Fail(waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        return state_->status >= snowdesktop::taskbar_hook::kStatusConnected;
    }

    bool Fail(DWORD error)
    {
        InterlockedExchange(&state_->lastError, static_cast<LONG>(error));
        InterlockedExchange(&state_->status,
            snowdesktop::taskbar_hook::kStatusFailed);
        return false;
    }

    std::mutex mutex_;
    HANDLE mapping_ = nullptr;
    snowdesktop::taskbar_hook::SharedState* state_ = nullptr;
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
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    return (SHAppBarMessage(ABM_GETSTATE, &data) & ABS_AUTOHIDE) != 0;
}

bool SetSystemTaskbarAutoHideEnabled(bool enabled)
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
    return IsSystemTaskbarAutoHideEnabled() == enabled;
}

bool IsWindowsSystemLightThemeEnabled()
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

bool SetWindowsSystemLightThemeEnabled(bool enabled)
{
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
    return IsWindowsSystemLightThemeEnabled() == enabled;
}

SystemTaskbarBackdropRuntimeState GetSystemTaskbarBackdropRuntimeState()
{
    return GetTaskbarBackdropController().RuntimeState();
}

bool ApplySystemTaskbarBackdrop(bool enabled, const DockSettings& settings,
    const PersonalizationSettings& appearance)
{
    return GetTaskbarBackdropController().Apply(enabled, settings, appearance);
}

bool LoadDockSettings(const wchar_t* path, DockSettings& settings)
{
    // Older configuration files do not contain this system setting. Start
    // from the current Windows state so upgrading never changes it silently.
    settings.systemTaskbarAutoHide = IsSystemTaskbarAutoHideEnabled();
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
    ReadBoolField(text, "showWindowsButton", settings.showWindowsButton);
    ReadBoolField(text, "followPersonalization", settings.followPersonalization);
    ReadBoolField(text, "showFrequentItems", settings.showFrequentItems);
    if (ReadDoubleField(text, "frequentItemCount", value))
        settings.frequentItemCount = std::clamp(static_cast<int>(value), 1, 8);
    ReadBoolField(text, "systemTaskbarAutoHide", settings.systemTaskbarAutoHide);
    ReadBoolField(text, "systemTaskbarBackdropEnabled",
        settings.systemTaskbarBackdropEnabled);
    ReadBoolField(text, "systemTaskbarFollowPersonalization",
        settings.systemTaskbarFollowPersonalization);
    if (settings.systemTaskbarBackdropEnabled)
        settings.systemTaskbarAutoHide = false;
    PersonalizationSettings& style = settings.appearance;
    if (ReadDoubleField(text, "backgroundR", value)) style.widgetBgR = static_cast<float>(value);
    if (ReadDoubleField(text, "backgroundG", value)) style.widgetBgG = static_cast<float>(value);
    if (ReadDoubleField(text, "backgroundB", value)) style.widgetBgB = static_cast<float>(value);
    if (ReadDoubleField(text, "borderR", value)) style.widgetBorderR = static_cast<float>(value);
    if (ReadDoubleField(text, "borderG", value)) style.widgetBorderG = static_cast<float>(value);
    if (ReadDoubleField(text, "borderB", value)) style.widgetBorderB = static_cast<float>(value);
    if (ReadDoubleField(text, "backgroundAlpha", value)) style.widgetAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "borderAlpha", value)) style.widgetBorderAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "cornerRadius", value)) style.cornerRadius = static_cast<float>(value);
    if (ReadDoubleField(text, "highlightAlpha", value)) style.highlightAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "noiseAlpha", value)) style.noiseAlpha = static_cast<float>(value);
    ReadBoolField(text, "glassEnabled", style.glassEnabled);

    PersonalizationSettings& taskbarStyle = settings.systemTaskbarAppearance;
    bool hasTaskbarAppearance = false;
    if (ReadDoubleField(text, "taskbarBackgroundR", value))
    {
        taskbarStyle.widgetBgR = static_cast<float>(value);
        hasTaskbarAppearance = true;
    }
    if (ReadDoubleField(text, "taskbarBackgroundG", value)) taskbarStyle.widgetBgG = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBackgroundB", value)) taskbarStyle.widgetBgB = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBorderR", value)) taskbarStyle.widgetBorderR = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBorderG", value)) taskbarStyle.widgetBorderG = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBorderB", value)) taskbarStyle.widgetBorderB = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBackgroundAlpha", value)) taskbarStyle.widgetAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarBorderAlpha", value)) taskbarStyle.widgetBorderAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarGlassBlurRadius", value)) taskbarStyle.glassBlurRadius = static_cast<float>(value);
    if (ReadDoubleField(text, "taskbarGlassRefreshMode", value))
        taskbarStyle.glassRefreshMode = std::clamp(static_cast<int>(value), 0, 3);
    ReadBoolField(text, "taskbarGlassEnabled", taskbarStyle.glassEnabled);
    if (!hasTaskbarAppearance)
    {
        // 首次升级时以旧版 Dock 独立外观为起点；保存后两者完全独立。
        taskbarStyle = style;
        taskbarStyle.cornerRadius = 0.0f;
        taskbarStyle.shadowAlpha = 0.0f;
    }
    return true;
}

bool SaveDockSettings(const wchar_t* path, const DockSettings& settings)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    const PersonalizationSettings& style = settings.appearance;
    const PersonalizationSettings& taskbarStyle = settings.systemTaskbarAppearance;
    file << "{\n";
    file << "  \"position\": " << static_cast<int>(settings.position) << ",\n";
    file << "  \"edgeAttached\": "
         << (settings.edgeAttached ? "true" : "false") << ",\n";
    file << "  \"showWindowsButton\": "
         << (settings.showWindowsButton ? "true" : "false") << ",\n";
    file << "  \"followPersonalization\": "
         << (settings.followPersonalization ? "true" : "false") << ",\n";
    file << "  \"showFrequentItems\": "
         << (settings.showFrequentItems ? "true" : "false") << ",\n";
    file << "  \"frequentItemCount\": " << settings.frequentItemCount << ",\n";
    file << "  \"systemTaskbarAutoHide\": "
         << (settings.systemTaskbarAutoHide ? "true" : "false") << ",\n";
    file << "  \"systemTaskbarBackdropEnabled\": "
         << (settings.systemTaskbarBackdropEnabled ? "true" : "false") << ",\n";
    file << "  \"systemTaskbarFollowPersonalization\": "
         << (settings.systemTaskbarFollowPersonalization ? "true" : "false") << ",\n";
    file << "  \"backgroundR\": " << style.widgetBgR << ",\n";
    file << "  \"backgroundG\": " << style.widgetBgG << ",\n";
    file << "  \"backgroundB\": " << style.widgetBgB << ",\n";
    file << "  \"borderR\": " << style.widgetBorderR << ",\n";
    file << "  \"borderG\": " << style.widgetBorderG << ",\n";
    file << "  \"borderB\": " << style.widgetBorderB << ",\n";
    file << "  \"backgroundAlpha\": " << style.widgetAlpha << ",\n";
    file << "  \"borderAlpha\": " << style.widgetBorderAlpha << ",\n";
    file << "  \"cornerRadius\": " << style.cornerRadius << ",\n";
    file << "  \"highlightAlpha\": " << style.highlightAlpha << ",\n";
    file << "  \"noiseAlpha\": " << style.noiseAlpha << ",\n";
    file << "  \"glassEnabled\": " << (style.glassEnabled ? "true" : "false") << ",\n";
    file << "  \"taskbarBackgroundR\": " << taskbarStyle.widgetBgR << ",\n";
    file << "  \"taskbarBackgroundG\": " << taskbarStyle.widgetBgG << ",\n";
    file << "  \"taskbarBackgroundB\": " << taskbarStyle.widgetBgB << ",\n";
    file << "  \"taskbarBorderR\": " << taskbarStyle.widgetBorderR << ",\n";
    file << "  \"taskbarBorderG\": " << taskbarStyle.widgetBorderG << ",\n";
    file << "  \"taskbarBorderB\": " << taskbarStyle.widgetBorderB << ",\n";
    file << "  \"taskbarBackgroundAlpha\": " << taskbarStyle.widgetAlpha << ",\n";
    file << "  \"taskbarBorderAlpha\": " << taskbarStyle.widgetBorderAlpha << ",\n";
    file << "  \"taskbarGlassEnabled\": "
         << (taskbarStyle.glassEnabled ? "true" : "false") << ",\n";
    file << "  \"taskbarGlassBlurRadius\": " << taskbarStyle.glassBlurRadius << ",\n";
    file << "  \"taskbarGlassRefreshMode\": " << taskbarStyle.glassRefreshMode << "\n";
    file << "}\n";
    return true;
}
