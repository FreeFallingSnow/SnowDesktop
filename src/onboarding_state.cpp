#include "onboarding_state.h"
#include "atomic_file.h"
#include "json_value.h"

#include <algorithm>
#include <sstream>

namespace snowdesktop::onboarding
{
bool State::Record(std::uint32_t step)
{
    const auto before = steps;
    steps |= step & kAllSteps;
    for (auto task : {Task::Collection, Task::Application, Task::Layout, Task::Files})
        if (Completed(steps, task)) deferred &= ~(1u << static_cast<unsigned>(task));
    return before != steps;
}

bool State::CollectionCreated(std::string id, bool previousCollectionExists)
{
    if (id.empty()) return false;
    bool changed = Record(kCollection);
    if (collectionId.empty() || !previousCollectionExists)
    {
        changed |= collectionId != id;
        collectionId = std::move(id);
    }
    return changed;
}

bool State::ApplicationDropped(std::string_view id, bool application,
    bool newlyInserted)
{
    return !collectionId.empty() && id == collectionId && application &&
        newlyInserted && Record(kApplication);
}

bool State::GeometryCommitted(std::string_view id, bool moved, bool resized)
{
    if (collectionId.empty() || id != collectionId) return false;
    return Record((moved ? kMoved : 0) | (resized ? kResized : 0));
}

bool State::Defer(Task task)
{
    if (Completed(steps, task)) return false;
    const auto before = deferred;
    deferred |= 1u << static_cast<unsigned>(task);
    return before != deferred;
}

bool State::Resume(Task task)
{
    const auto before = deferred;
    deferred &= ~(1u << static_cast<unsigned>(task));
    return before != deferred;
}

std::string Encode(const State& state)
{
    // IDs originate in MakeNewWidgetId; accept only this non-escaped alphabet
    // when loading as well, so malformed persisted strings cannot become JSON.
    std::ostringstream out;
    out << "{\n  \"version\": 1,\n  \"welcomePending\": "
        << (state.welcomePending ? "true" : "false")
        << ",\n  \"steps\": " << state.steps
        << ",\n  \"deferred\": " << state.deferred
        << ",\n  \"collectionId\": \"" << state.collectionId << "\"\n}\n";
    return out.str();
}

bool Decode(std::string_view text, State& state)
{
    JsonValue root;
    if (!ParseJson(text, root) || !root.IsObject()) return false;
    const auto* version = root.Find("version");
    const auto* pending = root.Find("welcomePending");
    const auto* steps = root.Find("steps");
    const auto* deferred = root.Find("deferred");
    const auto* id = root.Find("collectionId");
    const auto validMask = [](const JsonValue* value, unsigned maximum) {
        return value && value->IsNumber() && value->number >= 0 &&
            value->number <= maximum &&
            value->number == static_cast<unsigned>(value->number);
    };
    if (!version || !version->IsNumber() || version->number != 1 ||
        !pending || !pending->IsBoolean() || !validMask(steps, kAllSteps) ||
        !validMask(deferred, 15) || !id || !id->IsString() ||
        id->string.size() > 128 || !std::all_of(id->string.begin(), id->string.end(),
            [](char c) { return (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; }))
        return false;
    State parsed{pending->boolean, static_cast<std::uint32_t>(steps->number),
        static_cast<std::uint32_t>(deferred->number), id->string};
    // Normalize only stale deferred flags; learned steps are historical.
    parsed.Record(0);
    state = std::move(parsed);
    return true;
}

bool Load(const std::filesystem::path& path, bool existingUser, State& state,
    std::string* error)
{
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) { if (error) *error = ec.message(); return false; }
    if (!exists)
    {
        state = State{.welcomePending = !existingUser};
        // Persist before layout bootstrap creates files: a failed first display
        // must still be offered on the next launch.
        return Save(path, state, error);
    }
    std::string text;
    if (!atomic_file::ReadAll(path, text, error)) return false;
    if (Decode(text, state)) return true;
    if (error) *error = "Invalid onboarding state";
    return false;
}

bool Save(const std::filesystem::path& path, const State& state, std::string* error)
{
    return atomic_file::WriteAll(path, Encode(state), {}, error);
}
}
