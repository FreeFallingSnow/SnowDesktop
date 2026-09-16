#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop::onboarding
{
// Private host/UI state, not a widget API or a layout schema.
enum class Task : std::uint8_t { Collection, Application, Layout, Files };
inline constexpr std::uint32_t kCollection = 1, kApplication = 2,
    kMoved = 4, kResized = 8, kFiles = 16, kAllSteps = 31;

inline constexpr std::string_view TaskKey(Task task)
{
    switch (task)
    {
    case Task::Collection: return "collection";
    case Task::Application: return "application";
    case Task::Layout: return "layout";
    case Task::Files: return "files";
    }
    return {};
}

inline std::optional<Task> ParseTask(std::string_view key)
{
    for (auto task : {Task::Collection, Task::Application, Task::Layout, Task::Files})
        if (TaskKey(task) == key) return task;
    return std::nullopt;
}

inline constexpr bool Completed(std::uint32_t steps, Task task)
{
    const auto required = task == Task::Collection ? kCollection
        : task == Task::Application ? kApplication
        : task == Task::Layout ? kMoved | kResized : kFiles;
    return (steps & required) == required;
}

struct State
{
    bool welcomePending = false;
    std::uint32_t steps = 0;
    std::uint32_t deferred = 0;
    std::string collectionId;

    bool Record(std::uint32_t step);
    bool CollectionCreated(std::string id, bool previousCollectionExists);
    bool ApplicationDropped(std::string_view id, bool application,
        bool newlyInserted);
    bool GeometryCommitted(std::string_view id, bool moved, bool resized);
    bool Defer(Task task);
    bool Resume(Task task);
    friend bool operator==(const State&, const State&) = default;
};

// Debug initialization has an independent, in-memory lifetime. Ending it
// restores the real state without merging progress or welcome flags.
struct Session
{
    State regular;
    std::optional<State> experiment;
    State& Current() { return experiment ? *experiment : regular; }
    const State& Current() const { return experiment ? *experiment : regular; }
    void BeginExperiment() { experiment = State{.welcomePending = true}; }
    void EndExperiment() { experiment.reset(); }
};

std::string Encode(const State& state);
bool Decode(std::string_view text, State& state);
bool Load(const std::filesystem::path& path, bool existingUser, State& state,
    std::string* error = nullptr);
bool Save(const std::filesystem::path& path, const State& state,
    std::string* error = nullptr);
}
