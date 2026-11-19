#pragma once

#include "platform/http_runtime.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace snowdesktop::widget_runtime
{
struct TaskBrokerAction;
struct TaskCompletion;
class WidgetTaskBroker;
class WidgetSecretStore;

class WidgetNetworkTaskDispatch
{
public:
    void Start(const TaskBrokerAction& action, const std::wstring& widgetId,
        const std::string& packageId, const std::vector<std::string>& domains,
        WidgetSecretStore* secrets, AsyncHttpService* http, WidgetTaskBroker& broker);
    void Cancel(const TaskBrokerAction& action, AsyncHttpService* http);
    // Only a response with a registered task ID is moved. The caller can deliver
    // an unclaimed response to its ordinary HTTP callback.
    bool Consume(HttpResponse& response, WidgetTaskBroker* broker);
    std::optional<HttpResponse> TakeCompletion(TaskCompletion& completion);
    void DiscardCompletion(std::uint64_t id);
    void Clear();

private:
    std::unordered_map<std::uint64_t, int> requests_;
    std::unordered_map<int, std::uint64_t> tasks_;
    std::unordered_map<std::uint64_t, HttpResponse> results_;
};

void PushNetworkTaskValue(lua_State* state, const HttpResponse& response);
void PushNetworkTaskStatus(lua_State* state, const HttpResponse& response);
void PushHttpResponseValue(lua_State* state, const HttpResponse& response);
}
