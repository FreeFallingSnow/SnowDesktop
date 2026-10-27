#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace snowdesktop::performance
{
struct PerfettoSessionOptions
{
    std::filesystem::path output;
    std::uint32_t bufferKiB = 32768;
    std::uint64_t maximumFileBytes = 256ULL * 1024 * 1024;
    bool threadCpu = false;
    std::vector<std::string> categories{
        "snow.resource", "snow.lua.callback", "snow.task", "snow.frame" };
};

// Internal SDK boundary. All methods belong on the capture worker, never in a
// window-message handler. The SDK owns event storage, clocks and slice nesting.
// This class only manages acknowledgements and an exclusively reserved file.
class PerfettoSession final
{
public:
    PerfettoSession();
    ~PerfettoSession();
    PerfettoSession(const PerfettoSession&) = delete;
    PerfettoSession& operator=(const PerfettoSession&) = delete;

    bool Start(const PerfettoSessionOptions& options, std::string& error);
    bool Stop(std::string& error);
    bool Active() const noexcept;
    static bool Initialized() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
