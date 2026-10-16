void CheckIconBitmapSizing()
{
    namespace bitmap = snowdesktop::icon_bitmap_renderer;
    // Low-resolution Shell icons used to stay small while the requested
    // desktop/navigation/Dock frame grew. Expectations use the UI frame,
    // independently of the fitting calculation and the source resolution.
    for (const int sourceSize : {16, 32, 64, 256})
        for (const int side : {16, 28, 36, 56, 82, 96, 144})
        {
            const RECT frame{-100, 200, -100 + side, 200 + side};
            const auto plan = bitmap::Prepare(sourceSize, sourceSize, frame);
            Check(EqualRect(&frame, &plan.bounds) != FALSE,
                "icon frames retain their configured size across source resolutions and DPI scales");
        }

    const RECT frame{100, 200, 182, 282};
    const auto landscape = bitmap::Prepare(64, 32, frame);
    Check(landscape.bounds.left == 100 && landscape.bounds.right == 182 &&
            landscape.bounds.top == 220 && landscape.bounds.bottom == 261,
        "landscape collection and navigation previews stay proportional and centered");
    const auto portrait = bitmap::Prepare(32, 64, frame);
    Check(portrait.bounds.left == 120 && portrait.bounds.right == 161 &&
            portrait.bounds.top == 200 && portrait.bounds.bottom == 282,
        "portrait collection and navigation previews stay proportional and centered");
    const auto stretch = bitmap::Prepare(32, 64, frame, false);
    Check(EqualRect(&frame, &stretch.bounds) != FALSE && !stretch.preserveAspectRatio,
        "explicitly stretched generated artwork retains its complete target rectangle");
    for (const auto source : {std::pair{0, 64}, std::pair{64, 0}})
    {
        const auto invalid = bitmap::Prepare(source.first, source.second, frame);
        Check(IsRectEmpty(&invalid.bounds) != FALSE, "invalid bitmap dimensions have no drawable destination");
    }
    const auto invalid = bitmap::Prepare(32, 32, RECT{10, 10, 9, 40});
    Check(IsRectEmpty(&invalid.bounds) != FALSE, "invalid icon frames cannot produce a drawable destination");
}
