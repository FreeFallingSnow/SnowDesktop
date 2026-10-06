#pragma once
#include "system_controls.h"
#include <cstdint>
#include <unordered_map>

namespace snowdesktop::widget_runtime
{
enum class SystemControlArgumentKind { Invalid, Text, Number, Boolean };
bool IsSystemControlTask(std::string_view name);
SystemControlArgumentKind SystemControlArgumentType(std::string_view key);
// Host-internal conversion, also used before the no-side-effects preview branch.
bool MakeSystemControlRequest(std::string_view name,
    const std::unordered_map<std::string, std::string>& arguments,
    system_control::Request& request);
// The live closure owns every system effect (including confirmation UI and
// creation of the shared-service bridge). Preview never enters that closure.
system_control::Result DispatchSystemControlTask(system_control::Request& request, bool preview,
    const std::function<system_control::Result(system_control::Request&)>& live);

// UI-thread bridge. The application owns the service and its wake callback;
// this bridge adds no threads, sampler, or recurring timer.
class WidgetSystemControlTasks
{
public:
    struct Completion { std::uint64_t id; bool ok; std::string error; };
    explicit WidgetSystemControlTasks(std::shared_ptr<system_control::Service> service);
    ~WidgetSystemControlTasks();
    bool Start(std::uint64_t id, std::uint64_t owner, system_control::Request request);
    bool Cancel(std::uint64_t id);
    void Forget(std::uint64_t owner);
    std::vector<Completion> Drain();
    std::size_t ActiveCount() const { return active_.size(); }
private:
    struct Active { std::uint64_t serviceId, owner; };
    std::shared_ptr<system_control::Service> service_;
    std::map<std::uint64_t, Active> active_;
    std::vector<Completion> canceled_;
    static std::string Consumer(std::uint64_t owner);
    void ReleaseIdle(std::uint64_t owner);
};
}
