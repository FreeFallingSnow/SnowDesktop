#pragma once

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace snowdesktop::data_cleanup
{
// No UI/model references cross this boundary. New snapshots cancel stale work;
// shutdown does not wait for an unavailable filesystem provider.
class Worker final
{
public:
    using Job = std::function<void(std::stop_token)>;
    Worker();
    ~Worker() { Stop(); }
    void Submit(Job job);
    void Cancel();
    void Stop();
private:
    struct State;
    std::shared_ptr<State> state_;
};

// Only explicitly owned transient data is collected. Unknown directories,
// backups, projects, widget stores and migration markers are never candidates.
std::size_t Collect(const std::filesystem::path& data,
    const std::vector<std::filesystem::path>& referencedPaths, std::stop_token stop = {});
}
