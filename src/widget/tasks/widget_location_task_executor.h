#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
namespace snowdesktop::widget_runtime
{
struct LocationOptions { int timeoutMs = 10000; int maximumAgeMs = 300000; };
bool ParseLocationOptions(const std::unordered_map<std::string, std::string>& arguments, LocationOptions& options) noexcept;
struct LocationResult
{
    std::uint64_t id = 0;
    bool ok = false;
    std::string error;
    double latitude = 0, longitude = 0, accuracyMeters = 0;
    std::int64_t timestampMs = 0;
    std::string source;
};
bool ValidLocationResult(const LocationResult& result) noexcept;
// Start on the UI thread. Shared state survives canceled Windows operations;
// shutdown disconnects their callback from the host before releasing the engine.
class WidgetLocationTaskExecutor
{
public:
    WidgetLocationTaskExecutor();
    ~WidgetLocationTaskExecutor();
    WidgetLocationTaskExecutor(const WidgetLocationTaskExecutor&) = delete;
    WidgetLocationTaskExecutor& operator=(const WidgetLocationTaskExecutor&) = delete;
    bool Start(std::uint64_t id, LocationOptions options, bool preview);
    bool Cancel(std::uint64_t id);
    void SetCompletionCallback(std::function<void()> callback);
    std::vector<LocationResult> DrainCompletions();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
