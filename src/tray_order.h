#pragma once
#include "status_bar_settings.h"
#include "tray_service.h"
#include "tray_presentation.h"
#include <algorithm>

namespace snowdesktop::tray
{
// Both native bar and overflow use this transaction. Preserve disconnected
// identities, and never persist an HWND as an application identity.
inline bool PlaceIcon(StatusBarSettings& settings, const Snapshot& snapshot,
    std::string_view key, bool pinned, std::string_view beforeKey = {})
{
    const auto source = std::find_if(snapshot.icons.begin(), snapshot.icons.end(),
        [&](const auto& icon) { return icon.key == key && !(icon.state & NIS_HIDDEN); });
    if (source == snapshot.icons.end() || source->persistentKey.empty() || DuplicatesControlCenter(*source)) return false;
    std::string before;
    if(!beforeKey.empty())
    {
        const auto target=std::find_if(snapshot.icons.begin(),snapshot.icons.end(),[&](const auto& icon){return icon.key==beforeKey;});
        // A disappearing insertion target cancels the transaction. Appending
        // instead would contradict the indicator shown while dragging.
        if(target==snapshot.icons.end()||(target->state&NIS_HIDDEN)||target->persistentKey.empty()||DuplicatesControlCenter(*target))return false;
        before=target->persistentKey;
    }
    std::vector<std::string> order;
    auto append = [&](const std::string& id) {
        if (!id.empty() && std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
    };
    for (const auto& id : settings.trayOrder) append(id);
    for (const auto& icon : snapshot.icons) append(icon.persistentKey);
    if (before != source->persistentKey)
    {
        std::erase(order, source->persistentKey);
        order.insert(std::find(order.begin(), order.end(), before), source->persistentKey);
    }
    std::erase(settings.pinnedTrayItems, source->persistentKey);
    if (pinned) settings.pinnedTrayItems.push_back(source->persistentKey);
    settings.trayOrder = std::move(order);
    return true;
}
}
