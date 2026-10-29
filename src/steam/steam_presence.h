#pragma once

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <thread>

namespace snowdesktop::steam_presence
{
// Owns only the opt-in Steam session; subscription and entitlement work have
// independent lifetimes. The child also watches the host's real process handle.
class Service final
{
public:
    explicit Service(std::filesystem::path bridge);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    void SetEnabled(bool enabled);
    [[nodiscard]] DWORD ProcessId() const noexcept { return processId_.load(); }
private:
    void Run(std::stop_token stop);
    std::filesystem::path bridge_;
    HANDLE stopEvent_ = nullptr;
    std::jthread worker_;
    std::atomic<DWORD> processId_{0};
};
}
