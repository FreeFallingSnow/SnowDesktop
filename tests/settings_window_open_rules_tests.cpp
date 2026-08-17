#include "settings_window_open_rules.h"

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
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}
}

int main(int argc, char** argv)
{
    using snowdesktop::settings_window_open_rules::RequestState;

    RequestState state;
    Check(!state.Pending(), "new state has no pending request");

    state.Request();
    Check(state.Pending() && state.RetryCount() == 0,
        "request becomes pending and resets retry count");
    Check(state.RecordFailure(3) && state.RetryCount() == 1,
        "first failure schedules a retry");
    Check(state.RecordFailure(3) && state.RetryCount() == 2,
        "second failure schedules a retry");
    Check(state.RecordFailure(3) && state.RetryCount() == 3,
        "third failure schedules the final retry");
    Check(!state.RecordFailure(3) && state.Pending(),
        "retry exhaustion preserves the pending request");

    state.Request();
    Check(state.Pending() && state.RetryCount() == 0,
        "a new user request restores the retry budget");
    state.MarkShown();
    Check(!state.Pending() && state.RetryCount() == 0,
        "successful display clears pending state and retries");
    Check(!state.RecordFailure(3),
        "completed requests cannot schedule retries");

    Check(argc == 2, "source root argument is provided");
    if (argc == 2)
    {
        const std::string source = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "settings_window.cpp");
        const std::size_t newFrame = source.find("ImGui::NewFrame();");
        const std::size_t physicalCursor = source.find("GetCursorPos(&mp);");
        const std::size_t correctedMousePos = source.find(
            "ImGui::GetIO().MousePos = ImVec2((float)mp.x, (float)mp.y);");
        const std::size_t imguiRender = source.find("ImGui::Render();");
        const std::size_t iconSizeDispatch = source.find(
            "DispatchPendingItemIconSizeChange();");
        Check(!source.empty(), "settings window source is readable");
        Check(newFrame != std::string::npos &&
                physicalCursor != std::string::npos &&
                correctedMousePos != std::string::npos &&
                newFrame < physicalCursor && physicalCursor < correctedMousePos,
            "physical cursor correction follows queued ImGui input processing");
        Check(imguiRender != std::string::npos &&
                iconSizeDispatch != std::string::npos &&
                imguiRender < iconSizeDispatch,
            "icon-size slider previews are coalesced after the ImGui frame");

        const std::string pageGridSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_page_grid.cpp");
        const std::size_t previewBegin = pageGridSource.find(
            "void DesktopApp::PreviewIconSpacing(float value)");
        const std::size_t previewEnd = pageGridSource.find(
            "void DesktopApp::SetIconSpacing(float value)", previewBegin);
        const std::string preview = previewBegin == std::string::npos ||
                previewEnd == std::string::npos
            ? std::string{}
            : pageGridSource.substr(previewBegin, previewEnd - previewBegin);
        const std::size_t widgetBounds = preview.find(
            "widget.bounds = GetGridRect(");
        const std::size_t invalidateSlots = preview.find(
            "widgetContainer->InvalidateSlots();");
        const std::size_t synchronousPaint = preview.find(
            "PresentDesktopPointerUpdate();");
        Check(!pageGridSource.empty(), "page-grid source is readable");
        Check(widgetBounds != std::string::npos &&
                invalidateSlots != std::string::npos &&
                synchronousPaint != std::string::npos &&
                widgetBounds < invalidateSlots &&
                invalidateSlots < synchronousPaint,
            "layout-spacing preview refreshes widget item geometry before synchronous painting");

        const std::size_t iconSizePreviewBegin = pageGridSource.find(
            "void DesktopApp::PreviewItemIconSize(float value)");
        const std::size_t iconSizePreviewEnd = pageGridSource.find(
            "void DesktopApp::SetItemIconSize(float value)",
            iconSizePreviewBegin);
        const std::string iconSizePreview =
            iconSizePreviewBegin == std::string::npos ||
                iconSizePreviewEnd == std::string::npos
            ? std::string{}
            : pageGridSource.substr(iconSizePreviewBegin,
                iconSizePreviewEnd - iconSizePreviewBegin);
        const std::size_t refreshSlots = iconSizePreview.find(
            "container->InvalidateSlots();");
        const std::size_t paintIconSize = iconSizePreview.find(
            "PresentDesktopPointerUpdate();");
        Check(refreshSlots != std::string::npos &&
                paintIconSize != std::string::npos &&
                refreshSlots < paintIconSize,
            "icon-size preview refreshes shared item geometry before synchronous painting");
    }

    if (failures == 0)
        std::cout << "All settings window open rule tests passed.\n";
    return failures == 0 ? 0 : 1;
}
