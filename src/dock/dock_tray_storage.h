#pragma once

#include "dock_tray_rules.h"
#include "common/json_value.h"
#include "common/utils.h"
#include <ostream>

namespace snowdesktop::dock_tray
{
inline bool DecodeRules(const JsonValue& document, std::vector<DockTrayRule>& rules)
{
    const auto* value = document.Find("trayApplications");
    if (!value) { rules = Defaults(); return true; }
    if (!value->IsArray() || value->array.size() > 128) return false;
    std::vector<DockTrayRule> decoded;
    for (const auto& item : value->array)
    {
        const auto* path = item.Find("executable");
        const auto* matchPath = item.Find("matchPath");
        const auto* enabled = item.Find("enabled");
        const auto* doubleClick = item.Find("doubleClick");
        if (!path || !path->IsString() || path->string.empty() || path->string.size() > 131072 ||
            !matchPath || !matchPath->IsBoolean() || !enabled || !enabled->IsBoolean() ||
            !doubleClick || !doubleClick->IsBoolean()) return false;
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            path->string.data(), static_cast<int>(path->string.size()), nullptr, 0);
        if (!length) return false;
        std::wstring target(static_cast<size_t>(length), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path->string.data(),
            static_cast<int>(path->string.size()), target.data(), length) != length) return false;
        target = NormalizeTarget(std::move(target), matchPath->boolean);
        if (target.empty()) return false;
        decoded.push_back({std::move(target), matchPath->boolean, enabled->boolean, doubleClick->boolean});
    }
    NormalizeRules(decoded);
    rules = std::move(decoded);
    return true;
}

inline bool WriteRules(std::ostream& output, const std::vector<DockTrayRule>& rules)
{
    if (rules.size() > 128) return false;
    for (const auto& rule : rules)
        if (NormalizeTarget(rule.executable, rule.matchPath).empty()) return false;
    output << "  \"trayApplications\": [";
    for (size_t i = 0; i < rules.size(); ++i)
    {
        const auto& rule = rules[i];
        if (i) output << ',';
        output << "{\"executable\":\"" << JsonEscapeUtf8(rule.executable)
            << "\",\"matchPath\":" << (rule.matchPath ? "true" : "false")
            << ",\"enabled\":" << (rule.enabled ? "true" : "false")
            << ",\"doubleClick\":" << (rule.doubleClick ? "true" : "false") << '}';
    }
    output << "],\n";
    return true;
}
}
