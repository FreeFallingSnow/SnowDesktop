#pragma once
#include "json_value.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::system_control
{
using Arguments = std::map<std::string, std::string>;
struct Snapshot
{
    bool available = false;
    JsonValue value;
    std::string error;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
};
// Host input only. This type is never serialized into Lua, logs or settings.
class Secret
{
public:
    Secret() = default;
    explicit Secret(std::wstring_view text);
    ~Secret();
    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    std::wstring_view View() const;
    void Clear();
private:
    std::vector<wchar_t> bytes_;
};
struct Request
{
    std::string name;
    Arguments arguments;
    Secret password;
    bool hostConfirmed = false;
};
struct Result
{
    bool ok = false;
    std::string error;
    std::uint32_t platformCode = 0;
};
struct Completion : Result { std::uint64_t id = 0; };
struct Cancellation
{
    std::shared_ptr<std::atomic_bool> canceled;
    std::chrono::steady_clock::time_point deadline;
    bool Canceled() const { return canceled && canceled->load(); }
    bool Expired() const { return std::chrono::steady_clock::now() >= deadline; }
    bool Stop() const { return Canceled() || Expired(); }
    Result Failure() const { return {false, Canceled() ? "canceled" : "timeout", 0}; }
};
class Backend
{
public:
    virtual ~Backend() = default;
    // Sample a physical source once; all related topics share its result.
    virtual std::map<std::string, Snapshot> Sample(std::string_view source, const Cancellation& cancel) = 0;
    virtual Result Execute(const Request& request, const Cancellation& cancel) = 0;
    virtual void Release(std::string_view source) = 0;
};
std::shared_ptr<Backend> CreateWindowsBackend();
bool SupportsTopic(std::string_view topic);
bool SupportsTask(std::string_view task);
bool ValidateRequest(const Request& request);
bool RequiresConfirmation(std::string_view task);
bool RequiresPasswordPrompt(const Request& request);
std::string_view Source(std::string_view topicOrTask);

// Application-owned service. Sampling and control execution run independently
// from resource counters and from each other; both native UI and Lua use this.
class Service
{
public:
    using Wake = std::function<void()>;
    explicit Service(std::shared_ptr<Backend> backend = {});
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    bool Subscribe(std::string consumer, std::string topic, std::chrono::milliseconds interval);
    void Unsubscribe(std::string_view consumer, std::string_view topic);
    void RemoveConsumer(std::string_view consumer);
    std::optional<Snapshot> Current(std::string_view topic) const;
    std::vector<std::string> DrainChangedTopics();
    std::uint64_t Start(std::string consumer, Request request);
    bool Cancel(std::uint64_t id);
    std::vector<Completion> DrainCompletions(std::string_view consumer);
    void SetWake(Wake wake);
    void Invalidate(std::string_view source = {});
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace json
{
inline JsonValue Object() { JsonValue value; value.type = JsonValue::Type::Object; return value; }
inline JsonValue Array() { JsonValue value; value.type = JsonValue::Type::Array; return value; }
inline JsonValue Text(std::string text) { JsonValue value; value.type = JsonValue::Type::String; value.string = std::move(text); return value; }
inline JsonValue Number(double number) { JsonValue value; value.type = JsonValue::Type::Number; value.number = number; return value; }
inline JsonValue Boolean(bool boolean) { JsonValue value; value.type = JsonValue::Type::Boolean; value.boolean = boolean; return value; }
inline std::string String(const JsonValue& value, std::string_view field)
{ const auto* found = value.Find(field); return found && found->IsString() ? found->string : std::string{}; }
inline double Numeric(const JsonValue& value, std::string_view field, double fallback = 0)
{ const auto* found = value.Find(field); return found && found->IsNumber() ? found->number : fallback; }
inline bool Flag(const JsonValue& value, std::string_view field)
{ const auto* found = value.Find(field); return found && found->IsBoolean() && found->boolean; }
}
}
