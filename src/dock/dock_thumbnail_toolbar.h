#pragma once
#include "taskbar_hook/thumbnail_toolbar_protocol.h"
#include <memory>
#include <vector>

namespace snowdesktop::thumbnail_toolbar
{
Target Capture(HWND window);
struct Click { Target target; std::uint64_t identity = 0; DWORD button = 0; };
struct Update
{
    std::vector<Snapshot> snapshots;
    std::vector<Target> dismiss;
};
// Native queries stay off the preview/UI thread; hidden previews stop sampling.
class Client
{
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    void SetTargets(const std::vector<HWND>& windows);
    void Invoke(Click click);
    Update Take();
private:
    struct State;
    std::unique_ptr<State> state_;
};
DWORD QueryNative(HWND taskband, HMODULE entry, const Adapter& adapter, const Target& target,
    Snapshot& snapshot, const Click* click = nullptr, bool* dismiss = nullptr);
}
