#pragma once
#include "dock_fullscreen_policy.h"
#include "common/json_value.h"

namespace snowdesktop::dock_fullscreen
{
inline DockFullscreenPolicy DecodePolicy(const JsonValue& document)
{
    std::optional<int> current;
    if (const auto* value = document.Find("fullscreenPolicy"))
        current = value->IsNumber() && (value->number == 0 || value->number == 1 || value->number == 2)
            ? static_cast<int>(value->number) : -1;
    std::optional<bool> legacy;
    if (const auto* value = document.Find("floatingEdgeSwipeBlockFullscreen"); value && value->IsBoolean())
        legacy = value->boolean;
    return LoadPolicy(current, legacy);
}

inline bool DecodeExceptions(const JsonValue& document, std::vector<DockFullscreenException>& entries)
{
    const auto* value = document.Find("fullscreenExceptions");
    if (!value) { entries.clear(); return true; }
    if (!value->IsArray() || value->array.size() > 128) return false;
    std::vector<DockFullscreenException> decoded;
    for (const auto& item : value->array)
    {
        const auto* path = item.Find("executable");
        const auto* policy = item.Find("policy");
        if (!path || !path->IsString() || path->string.empty() || path->string.size() > 131072 ||
            !policy || !policy->IsNumber() ||
            (policy->number != 0 && policy->number != 1 && policy->number != 2)) return false;
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path->string.data(),
            static_cast<int>(path->string.size()), nullptr, 0);
        if (!length) return false;
        std::wstring executable(static_cast<size_t>(length), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path->string.data(),
            static_cast<int>(path->string.size()), executable.data(), length) != length ||
            executable.find(L'\0') != std::wstring::npos) return false;
        executable = NormalizeExecutable(std::move(executable));
        if (executable.empty()) return false;
        decoded.push_back({std::move(executable), Normalize(static_cast<int>(policy->number))});
    }
    NormalizeExceptions(decoded);
    entries = std::move(decoded);
    return true;
}
} // namespace snowdesktop::dock_fullscreen
