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
}
