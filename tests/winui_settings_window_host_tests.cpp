#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::size_t Count(std::string_view text, std::string_view token)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(token, position)) != std::string_view::npos)
    {
        ++count;
        position += token.size();
    }
    return count;
}

void TestHostContract(const std::filesystem::path& repository)
{
    const std::string header = ReadText(
        repository / "src/winui/settings_window_host.h");
    const std::string source = ReadText(
        repository / "src/winui/settings_window_host.cpp");
    const std::string runtime = ReadText(
        repository / "src/winui/winui_runtime.cpp");
    const std::string shellHeader = ReadText(
        repository / "src/winui/SettingsShell.xaml.h");
    const std::string shell = ReadText(
        repository / "src/winui/SettingsShell.xaml.cpp");

    Check(!header.empty() && !source.empty() && !runtime.empty() &&
            !shellHeader.empty() && !shell.empty(),
        "WinUI settings host contract sources are readable");
    Check(source.find("DesktopWindowXamlSource") == std::string::npos &&
            source.find("runtime.Attach(impl_->window") !=
                std::string::npos &&
            source.find("WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN") !=
                std::string::npos,
        "the reusable Win32 top-level HWND delegates its full client to WinUiRuntime");
    Check(source.find("ImGui") == std::string::npos &&
            source.find("ID3D11") == std::string::npos &&
            source.find("IDXGISwapChain") == std::string::npos &&
            source.find("Present(") == std::string::npos,
        "the new settings host has no ImGui, D3D, swap-chain, or manual-present path");

    const std::size_t openBegin = source.find(
        "bool SettingsWindowHost::Open(const SettingsRoute& route)");
    const std::size_t openEnd = source.find(
        "bool SettingsWindowHost::Hide()", openBegin);
    const std::string_view openFunction =
        openBegin != std::string::npos && openEnd != std::string::npos
        ? std::string_view(source).substr(openBegin, openEnd - openBegin)
        : std::string_view{};
    Check(Count(openFunction, "controller->Open(route)") == 1,
        "one host Open request performs exactly one authoritative controller Open");
    Check(source.find("controller->CloseSession()") != std::string::npos &&
            source.find("widgetSettingsService->CloseAll()") !=
                std::string::npos &&
            source.find("ShowWindow(window, SW_HIDE)") !=
                std::string::npos,
        "closing flushes the controller and widget sessions before hiding");
    Check(source.find("viewEpoch") != std::string::npos &&
            source.find("expectedEpoch") != std::string::npos &&
            source.find("DispatcherQueue") != std::string::npos &&
            source.find("latestSnapshot") != std::string::npos,
        "snapshot and async work are coalesced on the DispatcherQueue and gated by view epoch");

    Check(runtime.find("GetAncestor(target, GA_ROOTOWNER)") !=
                std::string::npos &&
            runtime.find("!IsChild(impl_->parentWindow, target)") !=
                std::string::npos &&
            runtime.find("return ::ContentPreTranslateMessage(message)") !=
                std::string::npos,
        "WinUI message preprocessing is restricted to the settings HWND tree");
    Check(shellHeader.find("SuspendInteraction()") != std::string::npos &&
            shellHeader.find("ResumeInteraction()") != std::string::npos &&
            shell.find("generalPage_->Deactivate()") !=
                std::string::npos &&
            shell.find("RenderPageCards(true)") != std::string::npos,
        "hidden settings sessions suspend controls and rebind them when reopened");
    Check(shellHeader.find("SetWidgetSettingsService(") !=
                std::string::npos &&
            shellHeader.find("ApplyWidgetSettingsSnapshot(") !=
                std::string::npos &&
            shell.find("widgetSettingsPage_->EventDispatchers()") !=
                std::string::npos &&
            shell.find("widgetSettingsPage_->Content()") !=
                std::string::npos &&
            shell.find("widgetSettingsPage_->Deactivate()") !=
                std::string::npos &&
            source.find("ApplyWidgetSettingsSnapshot(") !=
                std::string::npos,
        "widget settings snapshots, service events, native content, and close flush are wired through the WinUI shell");
    Check(shellHeader.find("ApplyWidgetsPageSnapshot(") !=
                std::string::npos &&
            shellHeader.find("ApplyBackupDataPageSnapshot(") !=
                std::string::npos &&
            shell.find("widgetsPage_->Content()") !=
                std::string::npos &&
            shell.find("backupDataPage_->Content()") !=
                std::string::npos &&
            shell.find("widgetsPage_->Activate(") !=
                std::string::npos &&
            shell.find("backupDataPage_->Activate()") !=
                std::string::npos,
        "widget management and backup routes render cached native WinUI presenters driven by immutable snapshots");
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the WinUI settings host contract");
    if (argc == 2)
        TestHostContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures << " WinUI settings host check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI settings host checks passed\n";
    return EXIT_SUCCESS;
}
