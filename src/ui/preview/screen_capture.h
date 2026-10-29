#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace snowdesktop::screen_capture
{
struct Monitor
{
    std::wstring id;
    RECT bounds{};
    bool primary = false;
};
std::vector<Monitor> Monitors();
struct State
{
    std::string id, status = "idle", error;
    std::string kind = "screenshot", source = "wgc-monitor";
    Monitor monitor;
    std::filesystem::path path, directory, manifestPath, frameMapPath;
    bool systemCursor = false;
    int width = 0, height = 0;
    unsigned fps = 0, durationMs = 0;
    std::uint64_t frames = 0, capturedFrames = 0, duplicatedFrames = 0, discardedFrames = 0;
    std::int64_t startedUnixMs = 0, finishedUnixMs = 0;
    std::int64_t frameTime100ns = 0;
    std::int64_t firstFrameTime100ns = 0, lastFrameTime100ns = 0;
    std::int64_t captureStartedUnixMs = 0, elapsed100ns = 0, encodedDuration100ns = 0;
};
// CFR duration rounds up by at most one frame. The capture itself remains open
// for durationMs of real time, including when only one output frame is needed.
std::uint64_t RecordingFrameCount(unsigned durationMs, unsigned fps);
// Uses a windowless MTA worker and Windows Graphics Capture. It never activates
// a target, changes user layout, or reads application configuration.
class Service final
{
public:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    bool Screenshot(std::wstring monitorId, std::filesystem::path outputRoot,
        bool systemCursor, std::string& error);
    bool Record(std::wstring monitorId, std::filesystem::path outputRoot,
        bool systemCursor, unsigned durationMs, unsigned fps, std::string& error);
    void Cancel();
    void Stop() noexcept;
    State Current() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
