#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace snowdesktop::widget_authoring
{

struct PreviewRenderRequest
{
    std::filesystem::path sourceDirectory;
    std::filesystem::path outputPng;
    int columns = 1;
    int rows = 1;
    unsigned dpi = 96;
    std::string locale = "en-US";
    std::string theme = "dark";
    std::string appearance = "dark";
    std::string dataState = "ready";
    std::filesystem::path backgroundImage;
    int canvasSize = 0;
    int padding = 0;
    bool contentOnly = false;
    std::unordered_map<std::string, std::string> storage;
};

struct PreviewRenderResult
{
    bool ok = false;
    std::string stage;
    std::string error;
    std::filesystem::path outputPng;
    int width = 0;
    int height = 0;
    int componentWidth = 0;
    int componentHeight = 0;
    int canvasSize = 0;
    int padding = 0;
    int placementX = 0;
    int placementY = 0;
    int placementWidth = 0;
    int placementHeight = 0;
    int cornerRadius = 0;
    int columns = 0;
    int rows = 0;
    unsigned dpi = 0;
    std::string locale;
    std::string theme;
    std::string appearance;
    int contentTheme = -1;
    std::string foregroundTheme;
    std::string dataState;
    std::filesystem::path backgroundImage;
    bool contentOnly = false;

    std::string ToJson() const;
};

/** Render one validated development package through WidgetEngine API v2. */
PreviewRenderResult RenderWidgetPreview(
    const PreviewRenderRequest& request);

/**
 * Handle SnowDesktop's private out-of-process authoring render command.
 * Returns the command exit code and sets handled only when the switch exists.
 */
int TryRunWidgetAuthorPreviewHostCommand(bool& handled);

} // namespace snowdesktop::widget_authoring
