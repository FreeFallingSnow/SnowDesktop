#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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

std::string_view CMakeCall(
    std::string_view source,
    std::string_view marker)
{
    const std::size_t begin = source.find(marker);
    if (begin == std::string_view::npos)
        return {};
    const std::size_t end = source.find(')', begin);
    if (end == std::string_view::npos)
        return {};
    return source.substr(begin, end + 1 - begin);
}

void TestSettingsEngineHasNoImGui(
    const std::string& header,
    const std::string& source)
{
    Check(!header.empty() && !source.empty(),
        "widget engine sources are readable");
    for (const char* removed : {
             "RenderWidgetEditor", "imguiRender",
             "lua_ImGui", "ImGui::", "#include <imgui",
             "settingsAppTaskExecutor_", "settingsAppSearchStates_",
             "secretSettingDrafts_", "validatedSettingDrafts_",
             "nextSettingsAppSearchTaskId_"})
    {
        Check(header.find(removed) == std::string::npos &&
                source.find(removed) == std::string::npos,
            "the application widget engine has no legacy ImGui settings symbol");
    }
    Check(header.find("ImGuiContext") == std::string::npos &&
            header.find("PersonalizationSettings") == std::string::npos,
        "the widget engine header has no legacy editor forward declarations");
}

void TestBuildTargetBoundary(
    const std::string& rootCMake,
    const std::string& workshopCMake,
    const std::string& workshopManager)
{
    const std::string_view applicationLinks = CMakeCall(
        rootCMake, "target_link_libraries(SnowDesktop PRIVATE");
    const std::string_view workshopLinks = CMakeCall(
        workshopCMake,
        "target_link_libraries(SnowDesktopWorkshopManager PRIVATE");

    Check(!applicationLinks.empty() && !workshopLinks.empty(),
        "application and Workshop Manager link contracts are readable");
    Check(applicationLinks.find("SnowDesktopWorkshopImgui") ==
                std::string_view::npos &&
            applicationLinks.find("SnowDesktopImgui") ==
                std::string_view::npos,
        "SnowDesktop does not link an ImGui target");
    Check(applicationLinks.find("d3d11") != std::string_view::npos &&
            applicationLinks.find("dcomp") != std::string_view::npos &&
            applicationLinks.find("dxgi") != std::string_view::npos,
        "SnowDesktop retains D3D and DirectComposition desktop dependencies");
    Check(rootCMake.find(
              "add_library(SnowDesktopWorkshopImgui STATIC") !=
                std::string::npos &&
            rootCMake.find("SnowDesktopImgui") == std::string::npos,
        "the old shared SnowDesktopImgui target is replaced by a Workshop-only target");
    Check(workshopLinks.find("SnowDesktopWorkshopImgui") !=
                std::string_view::npos &&
            workshopLinks.find("d3d11") != std::string_view::npos &&
            workshopLinks.find("dxgi") != std::string_view::npos,
        "Workshop Manager retains its isolated ImGui DX11 renderer");

    for (const char* lifecycle : {
             "ImGui::CreateContext()", "ImGui_ImplWin32_Init(",
             "ImGui_ImplDX11_Init(", "ImGui_ImplDX11_RenderDrawData(",
             "ImGui_ImplDX11_Shutdown()", "ImGui::DestroyContext()"})
    {
        Check(workshopManager.find(lifecycle) != std::string::npos,
            "Workshop Manager retains the complete isolated ImGui lifecycle");
    }
}

void TestDeclarativeV2Boundary(
    const std::string& publicApi,
    const std::string& presenter)
{
    Check(publicApi.find("imgui") == std::string::npos &&
            publicApi.find("winui.") == std::string::npos,
        "the public v2 Lua API exposes neither imgui nor a WinUI namespace");
    Check(presenter.find("WidgetSettingsService") != std::string::npos &&
            presenter.find("PasswordBox") != std::string::npos &&
            presenter.find("ChooseFilesystemHandle") != std::string::npos,
        "declarative v2 settings remain backed by the typed WinUI presenter");
}
}

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the no-ImGui settings contract");
    if (argc == 2)
    {
        const std::filesystem::path repository(argv[1]);
        TestSettingsEngineHasNoImGui(
            ReadText(repository / "src/widget_engine.h"),
            ReadText(repository / "src/widget_engine.cpp"));
        TestBuildTargetBoundary(
            ReadText(repository / "CMakeLists.txt"),
            ReadText(repository / "steam_bridge/CMakeLists.txt"),
            ReadText(repository / "steam_bridge/src/manager_main.cpp"));
        TestDeclarativeV2Boundary(
            ReadText(repository / "src/widget_public_api.inc"),
            ReadText(repository /
                "src/winui/widget_settings_presenter.cpp"));
    }

    if (failures != 0)
    {
        std::cerr << failures
                  << " no-ImGui settings contract check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "No-ImGui settings boundary checks passed\n";
    return EXIT_SUCCESS;
}
