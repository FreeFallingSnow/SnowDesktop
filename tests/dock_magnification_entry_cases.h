#include "desktop_hover_rules.h"

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
    single.SetTarget(base, 3000.0, 1.0);
    Check(single.ScaleFor(base) == 1.0f && single.IsAnimating(),
        "single-icon hover enters from the existing icon geometry");
    single.Advance(3080.0);
    const float singleMiddle = single.ScaleFor(base);
    Check(singleMiddle > 1.0f && singleMiddle < magnification::kSingleFocusScale &&
            single.ScaleFor(next) == 1.0f,
        "single-icon entry enlarges only its target through an intermediate frame");
    single.SetTarget(base, 3080.0, 1.0);
    Check(single.ScaleFor(base) == singleMiddle,
        "repeated hit tests do not restart single-icon animation");
    single.SetTarget(next, 3080.0, 1.0);
    Check(single.ScaleFor(base) == singleMiddle && single.ScaleFor(next) == 1.0f,
        "switching icons preserves the outgoing frame rather than jumping the growth to the new target");
    single.Advance(3120.0);
    Check(single.ScaleFor(base) > 1.0f && single.ScaleFor(base) < singleMiddle &&
            single.ScaleFor(next) == 1.0f,
        "the outgoing icon contracts before another icon can magnify");
    single.Advance(3160.0);
    Check(single.ScaleFor(base) == 1.0f && single.ScaleFor(next) == 1.0f && single.IsAnimating(),
        "single-icon ownership changes only at normal size");
    single.Advance(3200.0);
    Check(single.ScaleFor(base) == 1.0f && single.ScaleFor(next) > 1.0f &&
            single.ScaleFor(next) < magnification::kSingleFocusScale,
        "the newly hovered icon grows smoothly while the previous icon stays normal");
    single.Advance(3240.0);
    Check(single.ScaleFor(next) == magnification::kSingleFocusScale && !single.IsAnimating(),
        "a single-icon switch settles and releases its animation frame subscription");
    single.SetTarget({}, 3240.0, 1.0);
    Check(single.ScaleFor(next) == magnification::kSingleFocusScale && single.IsAnimating(),
        "leaving preserves the first exit frame instead of snapping to normal size");
    single.Advance(3320.0);
    const float leavingScale = single.ScaleFor(next);
    Check(leavingScale > 1.0f && leavingScale < magnification::kSingleFocusScale,
        "single-icon exit renders a partially contracted frame without pointer motion");
    single.SetTarget(next, 3320.0, 1.0);
    Check(single.ScaleFor(next) == leavingScale,
        "re-entering the outgoing icon reverses continuously from its current size");
    single.Advance(3480.0);
    single.SetTarget({}, 3480.0, 1.0);
    single.Advance(3640.0);
    Check(!single.IsAnimating() && IsRectEmpty(&single.CurrentRect()) && single.ScaleFor(next) == 1.0f,
        "completed exit restores normal geometry and leaves no perpetual animation");
    single.SetTarget(base, 4000.0, 2.0);
    single.Advance(4160.0);
    Check(single.ScaleFor(base) > 1.0f && single.ScaleFor(base) < magnification::kSingleFocusScale,
        "single-icon animation honors the shared duration preference");
    single.Advance(4320.0);
    for (const auto position : {DockPosition::Bottom, DockPosition::Top,
            DockPosition::Left, DockPosition::Right})
    {
        const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
        RECT before = base, after = base;
        OffsetRect(&before, vertical ? 0 : -76, vertical ? -76 : 0);
        OffsetRect(&after, vertical ? 0 : 76, vertical ? 76 : 0);
        const RECT focused = magnification::MagnifyRect(base, position, single.ScaleFor(base), 64, 0, true);
        const RECT left = magnification::MagnifyRect(before, position, single.ScaleFor(before), 64,
            magnification::SingleFocusAxisShift(-76, 64, single.Scale()), true);
        const RECT right = magnification::MagnifyRect(after, position, single.ScaleFor(after), 64,
            magnification::SingleFocusAxisShift(76, 64, single.Scale()), true);
        Check(focused.left + focused.right == base.left + base.right &&
                focused.top + focused.bottom == base.top + base.bottom,
            "single-icon growth preserves the icon center on every Dock edge");
        Check(left.right - left.left == before.right - before.left &&
                left.bottom - left.top == before.bottom - before.top &&
                right.right - right.left == after.right - after.left &&
                right.bottom - right.top == after.bottom - after.top,
            "neighbors make room without enlarging their icons");
        Check(vertical ? left.bottom <= focused.top && focused.bottom <= right.top :
                left.right <= focused.left && focused.right <= right.left,
            "symmetric single-icon displacement prevents overlap on either side");
        const RECT interaction = magnification::ExpandInteractionBounds(
            base, position, 64, magnification::kSingleFocusScale, true);
        Check(interaction.left <= focused.left && interaction.top <= focused.top &&
                interaction.right >= focused.right && interaction.bottom >= focused.bottom,
            "single-icon input and composition reserve include centered growth in both directions");
    }
}
