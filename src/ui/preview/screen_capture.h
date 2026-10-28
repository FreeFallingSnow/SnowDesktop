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
    Monitor monitor;
    std::filesystem::path path;
    bool systemCursor = false;
    int width = 0, height = 0;
    std::int64_t startedUnixMs = 0, finishedUnixMs = 0;
    std::int64_t frameTime100ns = 0;
};
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
    void Cancel();
    void Stop() noexcept;
    State Current() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
