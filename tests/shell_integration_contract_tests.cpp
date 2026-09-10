#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
int failures = 0;

void Check(bool condition, const std::string& message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

std::string Compact(std::string value)
{
    std::erase_if(value, [](unsigned char ch) { return std::isspace(ch) != 0; });
    return value;
}

std::string ReadSource(const std::filesystem::path& root, const char* relative)
{
    std::ifstream input(root / relative, std::ios::binary);
    Check(input.is_open(), std::string(relative) + ": source must be readable");
    std::string source{std::istreambuf_iterator<char>(input), {}};
    Check(!source.empty(), std::string(relative) + ": source must not be empty");
    return Compact(std::move(source));
}

std::string_view Section(const std::string& source, std::string signature,
    std::string nextSignature = {})
{
    signature = Compact(std::move(signature));
    nextSignature = Compact(std::move(nextSignature));
    const auto begin = source.find(signature);
    Check(begin != std::string::npos, "required section is missing: " + signature);
    if (begin == std::string::npos) return {};
    auto end = source.size();
    if (!nextSignature.empty())
    {
        end = source.find(nextSignature, begin + signature.size());
        Check(end != std::string::npos,
            "required section boundary is missing: " + nextSignature);
        if (end == std::string::npos) return {};
    }
    return std::string_view(source).substr(begin, end - begin);
}

// Conservative lexical bans only: comments containing these tokens also fail.
// These checks do not prove execution, call order, thread ownership or recovery.
void Forbid(std::string_view source, const char* token, const char* boundary)
{
    Check(!source.empty() && source.find(token) == std::string_view::npos,
        std::string(boundary) + ": forbidden source token " + token);
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: SnowDesktopShellIntegrationContractTests <source-root>\n";
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    const auto utils = ReadSource(root, "src/utils.cpp");
    const auto lifecycle = ReadSource(root, "src/app/app_lifecycle.cpp");
    const auto settings = ReadSource(root, "src/app/app_settings_apply.cpp");
    const auto startup = ReadSource(root, "src/app/startup_animation.cpp");
    const auto hook = ReadSource(root, "src/taskbar_hook/taskbar_hook.cpp");
    const auto facade = ReadSource(root, "src/settings_window.cpp");
    const auto presenter = ReadSource(root, "src/winui/dock_page_presenter.cpp");
    const auto settingsHost = ReadSource(root, "src/winui/settings_window_host.cpp");

    const auto discovery = Section(utils, "DesktopWindows FindDesktopWindows()",
        "void RestoreExplorerIconLayerNow()");
    Forbid(discovery, "0x052C", "periodic desktop discovery");
    Forbid(discovery, "SendMessageTimeoutW", "periodic desktop discovery");
    const auto watcher = Section(lifecycle, "void DesktopApp::WatchDesktopHost()",
        "void DesktopApp::InvalidateAllWidgetSlots()");
    Forbid(watcher, "EnsureDesktopWorkerWindow(", "periodic desktop watcher");
    const auto recovery = Section(lifecycle,
        "void DesktopApp::RecoverDesktopHostAfterExplorerRestart()",
        "void DesktopApp::WatchDesktopHost()");
    Forbid(recovery, "RequestSystemTaskbar", "desktop host recovery");
    const auto loading = Section(settings, "void DesktopApp::LoadDockSettingsAndApply()",
        "void DesktopApp::SyncSystemTaskbarSettingsFromWindows()");
    Forbid(loading, "RequestSystemTaskbar", "settings loading");

    for (const auto token : {"SetParent(", "AttachThreadInput(",
             "SetForegroundWindow(", "WS_EX_TOPMOST", "Locale::Instance()"})
        Forbid(startup, token, "startup presentation isolation");
    const auto finish = Section(startup, "void StartupAnimation::Finish() noexcept");
    Forbid(finish, "WaitFor", "startup Finish must only request completion");
    Forbid(finish, "join(", "startup Finish must only request completion");

    const auto registryHook = Section(hook,
        "SnowDesktopRegistryQueryHookProc(int code, WPARAM wParam, LPARAM lParam)",
        "_Use_decl_annotations_ STDAPI DllGetClassObject");
    Forbid(registryHook, "StartTaskbarTapIfNeeded(", "registry query hook isolation");
    const auto dllMain = Section(hook,
        "BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)");
    Forbid(dllMain, "CreateThread", "hook loader-lock boundary");
    for (const auto token : {"RegOpenKeyExW", "RegSetValueExW", "RegDeleteValueW"})
        Forbid(settings, token, "settings must use the unvirtualized registry bridge");
    for (const auto token : {"ImGui", "ID3D11", "IDXGISwapChain"})
        Forbid(facade, token, "settings facade rendering ownership");
    Forbid(presenter, "RequestSystemTaskbar", "presenter must use typed host actions");
    Forbid(settingsHost, "SendMessageTimeoutW", "WinUI host Shell broadcast boundary");

    if (failures == 0)
        std::cout << "Shell source boundary checks passed; runtime behavior is not exercised.\n";
    return failures == 0 ? 0 : 1;
}
