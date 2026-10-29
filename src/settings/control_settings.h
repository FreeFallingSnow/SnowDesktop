#pragma once

#include "platform/control/control_protocol.h"
#include <cstdint>

namespace snowdesktop::control_settings
{
using control::Value;
struct Target
{
    std::string kind;
    std::string id;
};
inline Target ParseTarget(const Value& params)
{
    const auto* value = params.Find("target");
    if (!value || !value->IsObject()) throw control::Error("invalid_params", "target must be a named object", -32602);
    control::Fields(*value, {"kind", "id"});
    Target target{control::Text(*value, "kind"), control::Text(*value, "id", false)};
    if (target.kind.empty() || target.kind.size() > 64 || target.id.size() > 4096 ||
        target.kind.find('\0') != std::string::npos || target.id.find('\0') != std::string::npos)
        throw control::Error("invalid_params", "Invalid settings target identity", -32602);
    return target;
}
inline Value TargetJson(const Target& target)
{
    auto result = control::Object({{"kind", control::String(target.kind)}});
    if (!target.id.empty()) result.object.emplace("id", control::String(target.id));
    return result;
}
// Content identity for optimistic concurrency, never an authentication token.
inline std::string Revision(const Value& value)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : control::Serialize(value)) { hash ^= byte; hash *= 1099511628211ULL; }
    return std::to_string(hash);
}
inline void RequireRevision(const Value& params, std::string_view revision)
{
    const auto expected = control::Text(params, "ifRevision");
    if (expected != revision) throw control::Error("stale_settings", "Settings changed; query the target again before editing");
}
}
