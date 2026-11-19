#include "widget_network_task_dispatch.h"

#include "common/utils.h"
#include "widget/storage/widget_secret_store.h"
#include "widget_task_broker.h"

#include <windows.h>
#include <charconv>
#include <utility>

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_runtime
{
namespace
{
bool HttpResponseSucceeded(const HttpResponse& response)
{
    return response.error.empty() && response.status >= 200 && response.status < 300;
}
}

void WidgetNetworkTaskDispatch::Start(const TaskBrokerAction& action,
    const std::wstring& widgetId, const std::string& packageId,
    const std::vector<std::string>& domains, WidgetSecretStore* secrets,
    AsyncHttpService* http, WidgetTaskBroker& broker)
{
    if (action.preview)
    {
        HttpResponse response;
        response.status = 200;
        response.body =
            "<?xml version=\"1.0\"?><rss><channel>"
            "<title>Preview feed</title></channel></rss>";
        results_.insert_or_assign(
            action.id, std::move(response));
        (void)broker.Complete(action.id, true);
        return;
    }
    const auto url = action.arguments.find("url");
    const auto method = action.arguments.find("method");
    const auto headerCountValue =
        action.arguments.find("headerCount");
    const auto timeout = action.arguments.find("timeoutMs");
    const auto cache = action.arguments.find("cacheSeconds");
    const auto maximum = action.arguments.find("maxBytes");
    const auto parseInteger = [](const std::string& text,
        int& output) {
        const char* begin = text.data();
        const char* end = begin + text.size();
        const auto parsed = std::from_chars(begin, end, output);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    };
    int timeoutMs = 0;
    int cacheSeconds = 0;
    int maximumBytes = 0;
    int headerCount = 0;
    if (!http || url == action.arguments.end() ||
        method == action.arguments.end() ||
        headerCountValue == action.arguments.end() ||
        timeout == action.arguments.end() ||
        cache == action.arguments.end() ||
        maximum == action.arguments.end() ||
        !parseInteger(headerCountValue->second, headerCount) ||
        !parseInteger(timeout->second, timeoutMs) ||
        !parseInteger(cache->second, cacheSeconds) ||
        !parseInteger(maximum->second, maximumBytes))
    {
        (void)broker.Complete(
            action.id, false, "invalidArguments");
        return;
    }
    if (headerCount < 0 || headerCount > 32)
    {
        (void)broker.Complete(
            action.id, false, "invalidArguments");
        return;
    }

    const auto argument = [&](const std::string& key)
        -> const std::string* {
        const auto found = action.arguments.find(key);
        return found == action.arguments.end()
            ? nullptr : &found->second;
    };
    const auto wipe = [](std::string& value) {
        if (!value.empty())
            SecureZeroMemory(value.data(), value.size());
        value.clear();
    };
    const auto composeValue = [&](const std::string& prefix,
        std::size_t maximumBytes, bool header,
        std::string& output) {
        const std::string* kind = argument(prefix + "kind");
        if (!kind) return false;
        if (*kind == "raw")
        {
            const std::string* value = argument(prefix + "value");
            if (!value || value->size() > maximumBytes)
                return false;
            output = *value;
        }
        else if (*kind == "secret")
        {
            const std::string* reference = argument(prefix + "ref");
            const std::string* valuePrefix =
                argument(prefix + "prefix");
            const std::string* valueSuffix =
                argument(prefix + "suffix");
            if (!secrets || !reference || !valuePrefix ||
                !valueSuffix ||
                valuePrefix->size() > maximumBytes ||
                valueSuffix->size() >
                    maximumBytes - valuePrefix->size())
                return false;
            std::string secret;
            std::string error;
            if (!secrets->Resolve(packageId,
                    action.instanceId, *reference, secret, error) ||
                secret.size() > maximumBytes - valuePrefix->size() -
                    valueSuffix->size())
            {
                wipe(secret);
                return false;
            }
            output.reserve(valuePrefix->size() + secret.size() +
                valueSuffix->size());
            output = *valuePrefix;
            output += secret;
            output += *valueSuffix;
            wipe(secret);
        }
        else
            return false;
        if (header &&
            (output.find_first_of("\r\n\0", 0, 3) !=
                    std::string::npos ||
                (output.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                    output.data(), static_cast<int>(output.size()), nullptr, 0) <= 0)))
        {
            wipe(output);
            return false;
        }
        return true;
    };

    std::string headersUtf8;
    bool requestArgumentsValid = true;
    for (int headerIndex = 0; headerIndex < headerCount;
        ++headerIndex)
    {
        const std::string prefix = "header." +
            std::to_string(headerIndex) + ".";
        const std::string* name = argument(prefix + "name");
        std::string value;
        if (!name || !composeValue(prefix, 16 * 1024, true,
                value) ||
            headersUtf8.size() > 32 * 1024 - name->size() - 4 ||
            value.size() > 32 * 1024 - headersUtf8.size() -
                name->size() - 4)
        {
            wipe(value);
            requestArgumentsValid = false;
            break;
        }
        headersUtf8 += *name;
        headersUtf8 += ": ";
        headersUtf8 += value;
        headersUtf8 += "\r\n";
        wipe(value);
    }
    std::string body;
    if (requestArgumentsValid &&
        !composeValue("body.", 64 * 1024, false, body))
        requestArgumentsValid = false;
    if (!requestArgumentsValid)
    {
        wipe(headersUtf8);
        wipe(body);
        const bool usesSecret =
            action.arguments.contains("usesSecret");
        (void)broker.Complete(action.id, false,
            usesSecret ? "secretUnavailable" :
                "invalidArguments");
        return;
    }
    HttpRequestOptions options;
    options.widgetId = widgetId;
    options.url = Utf8ToWide(url->second);
    options.method = Utf8ToWide(method->second);
    options.headers = Utf8ToWide(headersUtf8);
    options.body = std::move(body);
    options.timeoutMs = timeoutMs;
    options.cacheSeconds =
        method->second == "GET" && options.body.empty() &&
            !action.arguments.contains("usesSecret")
        ? cacheSeconds : 0;
    options.maximumResponseBytes =
        static_cast<std::uint32_t>(maximumBytes);
    options.allowedDomains = domains;
    options.allowAnyHttpOrHttpsUrl = false;
    options.allowHttpAndLocalTargets = true;
    options.sameOriginRedirectsOnly =
        action.arguments.contains("usesSecret");
    const auto retry = action.arguments.find("retryDirect");
    options.retryDirect = retry != action.arguments.end() &&
        retry->second == "true";
    wipe(headersUtf8);
    const int requestId = http->Submit(std::move(options));
    if (requestId <= 0)
    {
        (void)broker.Complete(
            action.id, false, "requestRejected");
        return;
    }
    requests_.insert_or_assign(action.id, requestId);
    tasks_.insert_or_assign(requestId, action.id);
}

void WidgetNetworkTaskDispatch::Cancel(const TaskBrokerAction& action, AsyncHttpService* http)
{
    const auto request = requests_.find(action.id);
    if (http && request != requests_.end())
        (void)http->Cancel(Utf8ToWide(action.instanceId), request->second);

    // Keep the association until the worker reply is drained, even after cancellation.
    DiscardCompletion(action.id);
}

bool WidgetNetworkTaskDispatch::Consume(HttpResponse& response, WidgetTaskBroker* broker)
{
    const auto task = tasks_.find(response.id);
    if (task == tasks_.end()) return false;

    const std::uint64_t taskId = task->second;
    tasks_.erase(task);
    requests_.erase(taskId);
    const bool ok = HttpResponseSucceeded(response);
    std::string error;
    if (!ok)
    {
        if (response.error == "Cancelled")
            error = "canceled";
        else if (response.error == "Response too large")
            error = "responseTooLarge";
        else if (!response.error.empty())
            error = response.error.find("Redirect") !=
                std::string::npos
                ? "redirectRejected" : "networkError";
        else
            error = "httpStatus";
    }
    results_.insert_or_assign(
        taskId, std::move(response));
    if (broker)
        (void)broker->Complete(
            taskId, ok, std::move(error));
    return true;
}

std::optional<HttpResponse> WidgetNetworkTaskDispatch::TakeCompletion(TaskCompletion& completion)
{
    auto result = results_.extract(completion.id);
    if (!result.empty()) return std::move(result.mapped());
    if (completion.ok && completion.name == "network.request")
    {
        completion.ok = false;
        completion.error = "taskResultUnavailable";
    }
    return std::nullopt;
}

void WidgetNetworkTaskDispatch::DiscardCompletion(std::uint64_t id)
{
    results_.erase(id);
}

void WidgetNetworkTaskDispatch::Clear()
{
    requests_.clear();
    tasks_.clear();
    results_.clear();
}

void PushNetworkTaskStatus(lua_State* state, const HttpResponse& response)
{
    lua_pushinteger(state, response.status);
    lua_setfield(state, -2, "status");
}

namespace
{
void PushNetworkTaskFields(lua_State* state, const HttpResponse& response)
{
    PushNetworkTaskStatus(state, response);
    lua_pushlstring(state, response.body.data(), response.body.size());
    lua_setfield(state, -2, "body");
    lua_pushboolean(state, response.fromCache);
    lua_setfield(state, -2, "fromCache");
}
}

void PushNetworkTaskValue(lua_State* state, const HttpResponse& response)
{
    lua_createtable(state, 0, 3);
    PushNetworkTaskFields(state, response);
}

void PushHttpResponseValue(lua_State* state, const HttpResponse& response)
{
    lua_createtable(state, 0, 5);
    PushNetworkTaskFields(state, response);
    lua_pushstring(state, response.error.c_str());
    lua_setfield(state, -2, "error");
    lua_pushboolean(state, HttpResponseSucceeded(response));
    lua_setfield(state, -2, "ok");
}
}
