#pragma once

#include <string>
#include <unordered_set>

namespace snowdesktop::display_topology_refresh
{
enum class Action
{
    None,
    ApplyTopology,
    ResynchronizeWindow,
};

struct Bounds
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

/**
 * @brief Return true when the new virtual desktop exposes pixels outside the
 *        native window's original allocation.
 *
 * A layered DirectComposition child can be resized successfully while its
 * effective input surface remains clipped to the allocation it had when its
 * HWND/target was created. Shrinking or rearranging inside the old bounds does
 * not require that heavier recreation path.
 */
constexpr bool ExtendsBeyond(const Bounds& previous, const Bounds& current)
{
    return current.left < previous.left || current.top < previous.top ||
        current.right > previous.right || current.bottom > previous.bottom;
}

/**
 * @brief Decide whether a display notification needs layout work or only a
 *        second native-window synchronization pass.
 *
 * Explorer can finish resizing its desktop host after the monitor APIs already
 * expose the new topology. The follow-up pass therefore must not be discarded
 * merely because the topology signature is unchanged.
 */
constexpr Action ResolveAction(
    bool topologyChanged,
    bool windowSynchronizationPending,
    bool windowBoundsOutOfSync)
{
    if (topologyChanged)
        return Action::ApplyTopology;
    if (windowSynchronizationPending || windowBoundsOutOfSync)
        return Action::ResynchronizeWindow;
    return Action::None;
}

using PageIdSet = std::unordered_set<std::wstring>;

/**
 * @brief Track pages that disappeared specifically because the active display
 *        topology lost a mapped monitor.
 *
 * Ordinary virtual pages are absent from both mapped-page snapshots and are
 * therefore never added. A page stays classified as topology-hidden until a
 * later display topology maps it again.
 */
inline PageIdSet ReconcileHiddenPages(
    const PageIdSet& hiddenPages,
    const PageIdSet& previousMappedPages,
    const PageIdSet& currentMappedPages)
{
    PageIdSet result = hiddenPages;
    for (const auto& pageId : previousMappedPages)
    {
        if (!pageId.empty() && !currentMappedPages.contains(pageId))
            result.insert(pageId);
    }
    for (const auto& pageId : currentMappedPages)
        result.erase(pageId);
    return result;
}
} // namespace snowdesktop::display_topology_refresh
