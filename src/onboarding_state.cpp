#include "onboarding_state.h"
#include "atomic_file.h"
#include "json_value.h"

#include <algorithm>
#include <sstream>

namespace snowdesktop::onboarding
{
bool State::Initialized()
{
    if (dismissed) return false;
    const bool changed = !eligible || !welcomePending;
    eligible = welcomePending = true;
    return changed;
}

bool State::Dismiss()
{
    if (dismissed) return false;
    dismissed = true;
    welcomePending = false;
    return true;
}

bool State::Record(std::uint32_t step)
{
    if (!Visible()) return false;
    const auto before = steps;
    steps |= step & kAllSteps;
    for (auto task : {Task::Collection, Task::Application, Task::Layout, Task::Files})
        if (Completed(steps, task)) deferred &= ~(1u << static_cast<unsigned>(task));
    return before != steps;
}

bool State::CollectionCreated(std::string id, bool previousCollectionExists, bool fromMenu)
{
    if (!Visible() || !fromMenu || id.empty()) return false;
    bool changed = Record(kCollection);
    if (collectionId.empty() || !previousCollectionExists)
    {
        changed |= collectionId != id;
        collectionId = std::move(id);
    }
    return changed;
}

bool Practice::Begin(const State& state, Task task, bool usableCollection)
{
    if (!state.Visible()) return false;
    active = (task == Task::Application || task == Task::Layout) && !usableCollection
        ? Task::Collection : task;
    return true;
}

void Practice::Advance(const State& state)
{
    if (!state.Visible()) { Pause(); return; }
    // Keep the optional Auto collect instruction visible after creating Files.
    if (!active || *active == Task::Files || !Completed(state.steps, *active)) return;
    for (auto task : {Task::Collection, Task::Application, Task::Layout, Task::Files})
        if (!Completed(state.steps, task)) { active = task; return; }
    active = Task::Files;
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

std::string Encode(const State& state)
{
    // IDs originate in MakeNewWidgetId; accept only this non-escaped alphabet
    // when loading as well, so malformed persisted strings cannot become JSON.
    std::ostringstream out;
    out << "{\n  \"version\": 2,\n  \"eligible\": " << (state.eligible ? "true" : "false")
        << ",\n  \"dismissed\": " << (state.dismissed ? "true" : "false")
        << ",\n  \"welcomePending\": "
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
    if (!version || !version->IsNumber() || (version->number != 1 && version->number != 2) ||
        !pending || !pending->IsBoolean() || !validMask(steps, kAllSteps) ||
        !validMask(deferred, 15) || !id || !id->IsString() ||
        id->string.size() > 128 || !std::all_of(id->string.begin(), id->string.end(),
            [](char c) { return (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; }))
        return false;
    State parsed{pending->boolean, static_cast<std::uint32_t>(steps->number),
        static_cast<std::uint32_t>(deferred->number), id->string};
    if (version->number == 1)
    {
        // Old "shown" is not an explicit dismissal. Do not infer eligibility
        // from historical operations made by existing users outside the guide.
        parsed.eligible = parsed.welcomePending;
    }
    else
    {
        const auto* eligible = root.Find("eligible");
        const auto* dismissed = root.Find("dismissed");
        if (!eligible || !eligible->IsBoolean() || !dismissed || !dismissed->IsBoolean()) return false;
        parsed.eligible = eligible->boolean;
        parsed.dismissed = dismissed->boolean;
        parsed.welcomePending &= parsed.Visible();
    }
    // Normalize only stale deferred flags; learned steps are historical.
    parsed.Record(0);
    state = std::move(parsed);
    return true;
}

bool Load(const std::filesystem::path& path, State& state,
    std::string* error)
{
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) { if (error) *error = ec.message(); return false; }
    if (!exists)
    {
        state = State{};
        return true; // Only an actual initialization can enroll a user.
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
