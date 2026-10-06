#include "desktop/desktop_hover_rules.h"

// Exercise the production timeline and geometry with a controlled clock.
// This protects against full-size first frames and pointer samples restarting
// the entry. Desktop presentation and perceived timing still require runtime QA.
void CheckDockMagnificationEntry()
{
    namespace magnification = snowdesktop::dock_magnification;
    using magnification::HoverEntryAnimation;
    HoverEntryAnimation entry;
    entry.SetHovered(true, 1000.0, 1.0);
    const RECT base{100, 200, 176, 276};
    const auto visual = [&](DockPosition position) {
        const float scale = entry.FocusScale(2.0f);
        return magnification::MagnifyRect(base, position,
            magnification::ScaleForEffect(2, true, 0.0f, 76, scale), 64,
            magnification::AxisShiftForDistance(76, 76, 64, scale));
    };
    for (const auto position : {DockPosition::Bottom, DockPosition::Top,
            DockPosition::Left, DockPosition::Right})
    {
        const RECT first = visual(position);
        Check(EqualRect(&first, &base) && entry.IsAnimating(),
            "entering Dock wave starts at base geometry, not full magnification");
    }
    entry.Advance(1080.0);
    const float middle = entry.FocusScale(2.0f);
    Check(std::abs(middle - 1.5f) < 0.001f && entry.IsAnimating(),
        "a stationary pointer receives an intermediate wave frame");
    const int middleShift = magnification::AxisShiftForDistance(76, 76, 64, middle);
    Check(middleShift > 0 && middleShift <
            magnification::AxisShiftForDistance(76, 76, 64, 2.0f),
        "neighbor displacement enters progressively with icon growth");
    const std::vector<float> packedScales{middle, 1.25f};
    Check(magnification::PackedAxisShift(packedScales, 1, 64, true) > 0 &&
            magnification::PackedAxisShift(packedScales, 1, 64, true) <
                magnification::PackedAxisShift({2.0f, 1.5f}, 1, 64, true),
        "edge-attached wave spacing shares the partial entry amplitude");
    entry.SetHovered(true, 1080.0, 1.0);
    Check(entry.FocusScale(2.0f) == middle,
        "pointer movement and repeated hit tests must not restart Dock entry");
    Check(magnification::ScaleForEffect(2, true, 0.0f, 76, middle) >
            magnification::ScaleForEffect(2, true, 76.0f, 76, middle),
        "wave center still responds immediately while the entry is running");
    entry.Advance(1160.0);
    Check(entry.FocusScale(2.0f) == 2.0f && !entry.IsAnimating(),
        "Dock entry reaches its exact target and retires without more pointer input");
    entry.SetHovered(true, 2000.0, 1.0);
    Check(entry.FocusScale(2.0f) == 2.0f && !entry.IsAnimating(),
        "moving between icons after entry preserves immediate full wave response");
    entry.SetHovered(false, 2000.0, 1.0);
    Check(entry.FocusScale(2.0f) == 1.0f && !entry.IsAnimating(),
        "leaving or suppressing the wave clears its amplitude and pending work");
    entry.SetHovered(true, 2000.0, 1.4);
    Check(entry.FocusScale(2.0f) == 1.0f && entry.IsAnimating(),
        "re-entering after leave starts a fresh wave from normal size");
    entry.Advance(2160.0);
    Check(entry.FocusScale(2.0f) > 1.0f && entry.FocusScale(2.0f) < 2.0f,
        "slow animation preference stretches the Dock entry duration");
    entry.Advance(2224.0);
    Check(entry.FocusScale(2.0f) == 2.0f && !entry.IsAnimating(),
        "slow Dock entry also finishes at its configured deadline");
    Check(entry.FocusScale(magnification::ResolveFocusScale(2, 2.0f, false)) == 1.0f,
        "disabling global motion removes magnification even after entry completes");

    // A minimize/region restack can hand a stationary pointer from the Dock
    // content HWND to its paired backdrop. Exercise the actual leave routing
    // and timelines; only native hit sampling and TrackMouseEvent are replaced.
    magnification::HoverEntryAnimation retainedEntry;
    int rearmed = 0;
    const auto routeLeave = [&](bool onContent, bool onBackdrop) {
        const bool retained = snowdesktop::desktop_hover_rules::RetainPairedSurfaceMouseLeave(
            onContent, onBackdrop, [&] { ++rearmed; });
        if (!retained)
            retainedEntry.SetHovered(false, 1080.0, 1.0);
        return retained;
    };
    retainedEntry.SetHovered(true, 1000.0, 1.0);
    retainedEntry.Advance(1080.0);
    Check(routeLeave(false, true) && rearmed == 0 &&
            std::abs(retainedEntry.FocusScale(2.0f) - 1.5f) < 0.001f && retainedEntry.IsAnimating(),
        "a Dock backdrop handoff preserves a partial wave without rearming content tracking");
    Check(routeLeave(true, false) && rearmed == 1 &&
            std::abs(retainedEntry.FocusScale(2.0f) - 1.5f) < 0.001f,
        "a stale leave over Dock content restores tracking without restarting growth");
    retainedEntry.Advance(1160.0);
    Check(routeLeave(false, true) && rearmed == 1 &&
            retainedEntry.FocusScale(2.0f) == 2.0f && !retainedEntry.IsAnimating(),
        "minimize handoff must not snap a settled Dock wave back to normal size");
    Check(!routeLeave(false, false) && rearmed == 1 &&
            retainedEntry.FocusScale(2.0f) == 1.0f && !retainedEntry.IsAnimating(),
        "leaving the complete Dock pair still clears growth without rearming tracking");

    magnification::SingleFocusAnimation single;
    RECT next = base;
    OffsetRect(&next, 76, 0);
    single.SetTarget(base, 3000.0, 1.0, 138);
    Check(single.Scale() == 1.0f && single.IsAnimating(),
        "single-icon hover enters from the existing icon geometry");
    single.Advance(3040.0);
    const float singleMiddle = single.Scale();
    Check(singleMiddle > 1.0f && singleMiddle < magnification::kSingleFocusScale,
        "single-icon entry receives an intermediate amplitude");
    single.SetTarget(base, 3040.0, 1.0, 157);
    Check(single.Scale() == singleMiddle && single.PointerAxis() == 157,
        "movement inside an icon updates growth distribution without restarting entry");
    single.SetTarget(next, 3040.0, 1.0, 180);
    Check(single.Scale() == singleMiddle,
        "switching semantic hover does not shrink the Dock growth budget");
    single.Advance(3080.0);
    Check(single.Scale() == magnification::kSingleFocusScale && !single.IsAnimating(),
        "entry finishes at its original deadline even across an icon boundary");
    single.SetTarget(base, 3100.0, 1.0, 170);
    Check(single.Scale() == magnification::kSingleFocusScale && !single.IsAnimating(),
        "reversing settled hover keeps full Dock length without a shrink-grow pulse");
    single.SetTarget({}, 3120.0, 1.0, 500);
    Check(single.Scale() == magnification::kSingleFocusScale && single.IsAnimating() &&
            single.PointerAxis() == 170,
        "exit fades the last distribution instead of moving growth to the departing pointer");
    single.Advance(3160.0);
    const float leavingScale = single.Scale();
    Check(leavingScale > 1.0f && leavingScale < magnification::kSingleFocusScale,
        "single-icon exit renders a partially contracted frame without pointer motion");
    single.SetTarget(next, 3160.0, 1.0, 190);
    Check(single.Scale() == leavingScale,
        "re-entering the outgoing icon reverses continuously from its current size");
    single.Advance(3240.0);
    single.SetTarget({}, 3240.0, 1.0, 500);
    single.Advance(3320.0);
    Check(!single.IsAnimating() && IsRectEmpty(&single.CurrentRect()) && single.Scale() == 1.0f,
        "completed exit restores normal geometry and leaves no perpetual animation");
    single.SetTarget(base, 4000.0, 2.0, 138);
    single.Advance(4080.0);
    Check(single.Scale() > 1.0f && single.Scale() < magnification::kSingleFocusScale,
        "single-icon animation honors the shared duration preference");
    single.Advance(4160.0);
    for (const auto position : {DockPosition::Bottom, DockPosition::Top,
            DockPosition::Left, DockPosition::Right})
    {
        const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
        // Independent width budgets catch rounding loss and boundary pulses.
        // Sweep the production geometry, including an unequal separator gap.
        for (const auto [iconSize, expectedGrowth] :
            {std::pair{16, 2}, std::pair{64, 8}, std::pair{75, 9}, std::pair{256, 31}})
        {
            const int pitch = iconSize + 12;
            const RECT origin{-400, -300, -400 + pitch, -300 + pitch};
            std::vector<RECT> candidates{origin, origin, origin, origin};
            for (int index = 1; index < 4; ++index)
            {
                const int offset = index * pitch + (index >= 2 ? 13 : 0);
                OffsetRect(&candidates[index], vertical ? 0 : offset, vertical ? offset : 0);
            }
            const auto center = [&](const RECT& rect) {
                return vertical ? (rect.top + rect.bottom) / 2 : (rect.left + rect.right) / 2;
            };
            std::vector<RECT> previous;
            for (int pointer = center(candidates.front()) - 16;
                 pointer <= center(candidates.back()) + 16; ++pointer)
            {
                const auto geometry = magnification::ResolveSingleFocusGeometry(
                    candidates, vertical, pointer, iconSize, single.Scale());
                std::vector<RECT> visuals;
                int enlargedCount = 0;
                for (const RECT& candidate : candidates)
                {
                    if (geometry.GrowthFor(candidate) > 0) ++enlargedCount;
                    visuals.push_back(magnification::MagnifyRect(candidate, position,
                        geometry.ScaleFor(candidate, iconSize), iconSize,
                        geometry.AxisShiftFor(candidate), true));
                }
                Check(enlargedCount >= 1 && enlargedCount <= 2,
                    "slight growth is confined to the adjacent pair surrounding the pointer");
                Check(vertical ?
                        visuals.front().top == candidates.front().top - expectedGrowth / 2 &&
                        visuals.back().bottom == candidates.back().bottom + expectedGrowth - expectedGrowth / 2 :
                        visuals.front().left == candidates.front().left - expectedGrowth / 2 &&
                        visuals.back().right == candidates.back().right + expectedGrowth - expectedGrowth / 2,
                    "both Dock ends stay constant across the sweep, including odd-pixel growth");
                for (size_t index = 1; index < visuals.size(); ++index)
                    Check(vertical ?
                            visuals[index].top - visuals[index - 1].bottom ==
                                candidates[index].top - candidates[index - 1].bottom :
                            visuals[index].left - visuals[index - 1].right ==
                                candidates[index].left - candidates[index - 1].right,
                        "growth handoff preserves the exact original gaps between icons");
                for (size_t index = 0; index < visuals.size(); ++index)
                {
                    const RECT interaction = magnification::ExpandInteractionBounds(
                        candidates[index], position, iconSize, magnification::kSingleFocusScale, true);
                    Check(interaction.left <= visuals[index].left && interaction.top <= visuals[index].top &&
                            interaction.right >= visuals[index].right && interaction.bottom >= visuals[index].bottom,
                        "transferred growth stays inside the existing input and composition reserve");
                    if (!previous.empty())
                        Check(std::abs(visuals[index].left - previous[index].left) <= 1 &&
                                std::abs(visuals[index].right - previous[index].right) <= 1 &&
                                std::abs(visuals[index].top - previous[index].top) <= 1 &&
                                std::abs(visuals[index].bottom - previous[index].bottom) <= 1,
                            "one-pixel pointer motion cannot jump an icon edge across the handoff");
                }
                previous = visuals;
            }
            const auto atCenter = magnification::ResolveSingleFocusGeometry(
                candidates, vertical, center(candidates[1]), iconSize, single.Scale());
            Check(atCenter.GrowthFor(candidates[1]) == expectedGrowth &&
                    atCenter.GrowthFor(candidates[0]) == 0 && atCenter.GrowthFor(candidates[2]) == 0,
                "at an icon center only that icon receives complete slight growth");
            const int quarter = center(candidates[1]) +
                (center(candidates[2]) - center(candidates[1])) / 4;
            const auto asymmetric = magnification::ResolveSingleFocusGeometry(
                candidates, vertical, quarter, iconSize, single.Scale());
            Check(expectedGrowth <= 2 ||
                    (asymmetric.GrowthFor(candidates[1]) > asymmetric.GrowthFor(candidates[2]) &&
                    asymmetric.GrowthFor(candidates[2]) > 0),
                "movement inside an icon gives the adjacent pair different growth weights");
            const auto alone = magnification::ResolveSingleFocusGeometry(
                {origin}, vertical, center(origin) + 500, iconSize, single.Scale());
            Check(alone.GrowthFor(origin) == expectedGrowth && alone.AxisShiftFor(origin) == 0,
                "a Dock containing one icon keeps centered slight growth");
        }
    }
    const auto empty = magnification::ResolveSingleFocusGeometry({}, false, 0, 64, single.Scale());
    Check(empty.ScaleFor(base, 64) == 1.0f && empty.AxisShiftFor(base) == 0,
        "missing or suppressed candidates do not retain displacement");
}
