#pragma once
#include "system_controls.h"
#include <algorithm>
#include <utility>

namespace snowdesktop::system_control
{
// The entire PnP instance identifies a monitor, not its model/friendly name.
// WMI appends an instance ordinal; a monitor interface appends its class GUID.
inline std::wstring BrightnessMonitorIdentity(std::wstring value, bool wmiInstance = false)
{
    for (auto& character : value)
        if (character >= L'a' && character <= L'z') character = static_cast<wchar_t>(character - (L'a' - L'A'));
    if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    std::replace(value.begin(), value.end(), L'#', L'\\');
    if (!value.starts_with(L"DISPLAY\\")) return {};
    const auto modelEnd = value.find(L'\\', 8);
    if (modelEnd == std::wstring::npos || modelEnd == 8 || modelEnd + 1 == value.size()) return {};
    const auto classStart = value.find(L'\\', modelEnd + 1);
    if (classStart != std::wstring::npos)
    {
        if (wmiInstance || classStart == modelEnd + 1 || classStart + 1 == value.size() ||
            value[classStart + 1] != L'{' || value.back() != L'}') return {};
        value.resize(classStart);
    }
    if (wmiInstance)
    {
        const auto ordinal = value.find_last_of(L'_');
        if (ordinal != std::wstring::npos && ordinal > modelEnd + 1 && ordinal + 1 < value.size() &&
            std::all_of(value.begin() + ordinal + 1, value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            value.resize(ordinal);
    }
    return value;
}

struct BrightnessDisplayTarget
{
    std::wstring identity;
    std::wstring name;
    bool active = false;
    bool operator==(const BrightnessDisplayTarget&) const = default;
};

// Clone views can contain several real monitors. The physical API provides no
// identity per handle, so neither array order nor equal names prove a match.
inline std::wstring BrightnessDdcIdentity(const std::vector<BrightnessDisplayTarget>& targets, std::size_t physicalCount)
{
    if (physicalCount != 1) return {};
    const BrightnessDisplayTarget* only = nullptr;
    for (const auto& target : targets)
    {
        if (!target.active) continue;
        if (target.identity.empty() || (only && only->identity != target.identity)) return {};
        only = &target;
    }
    return only ? only->identity : std::wstring{};
}

inline std::optional<bool> BrightnessMonitorActive(std::wstring_view identity, const std::vector<BrightnessDisplayTarget>& targets)
{
    if (identity.empty()) return {};
    std::optional<bool> active;
    for (const auto& target : targets)
        if (target.identity == identity) active = active.value_or(false) || target.active;
    return active;
}

inline std::wstring BrightnessMonitorName(std::wstring friendly, std::wstring description,
    std::wstring_view identity, std::wstring_view displayDevice = {})
{
    const auto usable = [](std::wstring text) {
        const auto first = text.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) return std::wstring{};
        text = text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
        // Backend identifiers belong in id, never in a visible hardware name.
        if (text.find(L'\\') != std::wstring::npos || text.find(L'#') != std::wstring::npos) return std::wstring{};
        for (const auto c : text) if (c < L' ') return std::wstring{};
        return text;
    };
    if (auto name = usable(std::move(friendly)); !name.empty()) return name;
    if (auto name = usable(std::move(description)); !name.empty()) return name;
    if (identity.starts_with(L"DISPLAY\\"))
    {
        const auto end = identity.find(L'\\', 8);
        if (end != std::wstring_view::npos)
            if (auto model = usable(std::wstring(identity.substr(8, end - 8))); !model.empty()) return model;
    }
    if (displayDevice.starts_with(L"\\\\.\\")) displayDevice.remove_prefix(4);
    if (auto name = usable(std::wstring(displayDevice)); !name.empty()) return name;
    return L"Monitor";
}

struct BrightnessMonitorSample
{
    std::wstring identity;
    JsonValue value;
};

inline JsonValue MergeBrightnessMonitors(const std::vector<BrightnessMonitorSample>& endpoints)
{
    std::vector<const BrightnessMonitorSample*> selected;
    const auto rank = [](const JsonValue& value) {
        return (json::Flag(value, "available") ? 2 : 0) + (json::String(value, "kind") == "internal" ? 1 : 0);
    };
    for (const auto& endpoint : endpoints)
    {
        const auto id = json::String(endpoint.value, "id");
        if (id.empty()) continue;
        const auto found = std::find_if(selected.begin(), selected.end(), [&](const auto* previous) {
            return json::String(previous->value, "id") == id ||
                (!endpoint.identity.empty() && endpoint.identity == previous->identity);
        });
        if (found == selected.end()) selected.push_back(&endpoint);
        else if (rank(endpoint.value) > rank((*found)->value)) *found = &endpoint;
    }
    auto result = json::Array();
    for (const auto* endpoint : selected) result.array.push_back(endpoint->value);
    return result;
}

// Presentation merging must never replace the endpoint inventory used here.
// A legacy DDC token still addresses its original physical handle, not a WMI
// endpoint with a potentially different brightness range or implementation.
template<class Range, class Execute>
Result WithBrightnessEndpoint(Range& endpoints, std::string_view id, Execute&& execute)
{
    const auto found = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) { return endpoint.id == id; });
    return found == endpoints.end() ? Result{false, "deviceGone", 1168 /* ERROR_NOT_FOUND */} : std::forward<Execute>(execute)(*found);
}
}
