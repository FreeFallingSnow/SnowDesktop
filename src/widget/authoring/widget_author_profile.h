#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class WidgetEngine;

namespace snowdesktop::widget_authoring
{
struct PreviewProfileOptions
{
    std::filesystem::path directory;
    unsigned seconds = 5, warmupSeconds = 1, intervalMs = 16;
    bool redraw = true, threadCpu = false, offline = false;
    std::vector<std::string> modules;
    unsigned inputSampleEvery = 1;
};
struct PreviewProfileResult
{
    std::filesystem::path manifest, report;
    std::string quality = "unknown", error;
    std::uint64_t drawCalls = 0;
    unsigned seconds = 0, warmupSeconds = 0, intervalMs = 0;
    bool redraw = false;
    std::vector<std::string> modules;
    unsigned inputSampleEvery = 1;
    std::string ToJson() const;
};

// Runs only in the isolated authoring host. Uses the product capture controller
// and actual WidgetEngine callbacks; it does not install a desktop component.
class WidgetAuthorProfile final
{
public:
    explicit WidgetAuthorProfile(WidgetEngine& engine);
    ~WidgetAuthorProfile();
    bool Initialize(const PreviewProfileOptions& options, std::string& error);
    PreviewProfileResult Measure(const std::function<bool()>& draw);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
