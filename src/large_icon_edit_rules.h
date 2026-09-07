#pragma once
#include "large_icon_settings.h"
#include <utility>

namespace snowdesktop::large_icon_edit_rules
{
inline bool CanStore(const std::optional<LargeIconConfig>& config, bool unlocked, bool desktop)
{
    // A null configuration is the always-available return to ordinary icons.
    return !config || (unlocked && desktop && ValidateLargeIconConfig(*config));
}

inline std::string CheckRequest(LargeIconEditSession& edit, const LargeIconSettingsRequest& request, bool unlocked)
{
    if (!unlocked) { edit.preview.reset(); return "largeIcon.locked"; }
    if (request.action == "read") return {};
    if (edit.token == 0 || edit.key != request.key || edit.token != request.session ||
        (request.action != "status" && edit.revision != request.revision)) return "largeIcon.stale";
    if (request.action != "status" && request.action != "preview" && request.action != "commit" &&
        request.action != "cancel" && request.action != "refresh" && request.action != "import") return "largeIcon.invalid";
    return {};
}

template<class Item, class Span, class Save>
bool Store(Item& item, std::optional<LargeIconConfig> config, Span span, bool unlocked, bool desktop, Save&& save)
{
    if (!CanStore(config, unlocked, desktop)) return false;
    const auto previous = item.largeIcon;
    const auto previousSpan = item.gridSpan;
    item.largeIcon = std::move(config); item.gridSpan = span;
    try { if (std::forward<Save>(save)()) return true; }
    catch (...) { /* Failed persistence must leave the committed display intact. */ }
    item.largeIcon = previous; item.gridSpan = previousSpan;
    return false;
}
}
