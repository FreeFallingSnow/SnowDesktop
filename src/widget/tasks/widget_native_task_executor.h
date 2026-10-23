#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct NativeRequestOptions
{
    std::string helper, operation, payload = "{}";
    int timeoutMs = 15000;
    bool streaming = false; // Internal task mode; never accepted as a Lua argument.
};
bool ParseNativeRequestOptions(const std::unordered_map<std::string, std::string>& arguments,
    NativeRequestOptions& options);

struct NativeTaskMessage
{
    std::uint64_t id = 0;
    bool final = false, ok = false;
    std::string error, json;
    // Host-generated metadata only; never copies helper stdout/stderr contents.
    std::string diagnostic;
};

// Only manages external PE executables. No third-party DLL or code is loaded
// into the host. Each request owns its pipes, job, process and deadline.
class WidgetNativeTaskExecutor
{
public:
    WidgetNativeTaskExecutor();
    ~WidgetNativeTaskExecutor();
    void SetCompletionCallback(std::function<void()> callback);
    bool Start(std::uint64_t id, std::filesystem::path root,
        std::string authorizedFingerprint, NativeRequestOptions options);
    bool Cancel(std::uint64_t id);
    std::vector<NativeTaskMessage> DrainMessages();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
