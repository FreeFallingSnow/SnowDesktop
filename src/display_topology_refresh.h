#pragma once

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
} // namespace snowdesktop::display_topology_refresh
