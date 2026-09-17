#pragma once
#include "pending_drop_completion.h"

namespace snowdesktop::new_item_placement
{
enum class Result { Pending, Discarded, Applied };

// Resolve stable identities only when the actual created item is enumerated.
// This also corrects provisional auto-collection into a different widget.
template<class Normalize>
Result Apply(std::vector<DesktopWidget>& widgets, std::vector<DesktopItem>& items,
    const std::wstring& widgetId, const std::wstring& path, size_t insertIndex,
    Normalize&& normalize)
{
    const auto target = std::find_if(widgets.begin(), widgets.end(),
        [&](const auto& widget) { return widget.id == widgetId; });
    if (target == widgets.end() || target->type != DesktopWidgetType::FileCategories)
        return Result::Discarded;
    const auto item = std::find_if(items.begin(), items.end(), [&](const auto& candidate) {
        return pending_drop::MatchesExactPath(candidate.parsingName, path);
    });
    if (item == items.end()) return Result::Pending;
    const auto key = normalize(item->layoutKey);
    if (key.empty()) return Result::Pending;
    PendingLandingEntry landing;
    landing.kind = DropLandingKind::WidgetIndex;
    landing.widgetId = widgetId;
    landing.insertIndex = insertIndex;
    return pending_drop::CommitKeyedLanding(widgets, *item, landing, key)
        ? Result::Applied : Result::Discarded;
}
}
