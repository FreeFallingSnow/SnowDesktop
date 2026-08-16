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
    int columns = 0;
    int rows = 0;
    unsigned dpi = 0;

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
