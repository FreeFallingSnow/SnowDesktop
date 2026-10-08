#pragma once
#include <array>
#include <string>
#include <string_view>

namespace snowdesktop::shell_extensions
{
// Only explicit canonical Windows pairs share visibility. Captions and
// arbitrary third-party verbs never establish a paired command identity.
struct StateCommandPair
{
    std::string_view id, forward, reverse;
};
inline constexpr std::array StateCommandPairs{
    StateCommandPair{"state:start-pin", "pintostartscreen", "unpinfromstartscreen"},
    StateCommandPair{"state:taskbar-pin", "taskbarpin", "taskbarunpin"},
    StateCommandPair{"state:home-pin", "pintohome", "unpinfromhome"}};
inline std::string StateCommandLower(std::string_view value)
{
    std::string result(value);
    for (auto &c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}
inline const StateCommandPair *StatePairForVerb(std::string_view verb)
{
    const auto lower = StateCommandLower(verb);
    for (const auto &pair : StateCommandPairs)
        if (lower == pair.forward || lower == pair.reverse) return &pair;
    return nullptr;
}
inline std::string StateVisibilityId(const std::string &id)
{
    const auto lower = StateCommandLower(id);
    for (const auto &pair : StateCommandPairs) if (lower == pair.id) return std::string(pair.id);
    std::string_view verb;
    if (lower.starts_with("verb:")) verb = std::string_view(lower).substr(5);
    else if (lower.starts_with("reg:"))
    {
        const auto shell = lower.rfind("\\shell\\");
        if (shell != std::string::npos) verb = std::string_view(lower).substr(shell + 7);
    }
    if (const auto *pair = StatePairForVerb(verb)) return std::string(pair->id);
    return id;
}
inline const StateCommandPair *StatePairForId(const std::string &id)
{
    const auto key = StateVisibilityId(id);
    for (const auto &pair : StateCommandPairs) if (key == pair.id) return &pair;
    return nullptr;
}
} // namespace snowdesktop::shell_extensions
