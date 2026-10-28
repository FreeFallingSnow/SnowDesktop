#pragma once
#include "scene.h"
#include <filesystem>
#include <memory>

namespace snowdesktop::demo
{
struct ExportState
{
    std::string id, status = "idle", error, sourceExeSha256;
    std::filesystem::path directory;
    unsigned frames = 0, completedFrames = 0;
    std::uint64_t startedUnixMs = 0, finishedUnixMs = 0;
    control::Value manifest;
};
// Runs only the existing private native-preview entry in the trusted host EXE.
// The IPC adapter supplies that EXE and the data-root; neither is client input.
class ExportService final
{
public:
    ExportService();
    ~ExportService();
    ExportService(const ExportService&) = delete;
    bool Start(const Scene& scene, const std::filesystem::path& executable,
        const std::filesystem::path& outputRoot, std::string& error);
    ExportState Current() const;
    void Cancel() noexcept;
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
