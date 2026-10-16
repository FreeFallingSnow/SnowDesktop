void CheckDockIconBitmapScaling()
{
    namespace dock = snowdesktop::dock_icon_render_rules;
    // Application icons were capped at their source bitmap size while the
    // vector Start/Search controls and the surrounding layout kept growing.
    // Exercise the destination used by all Dock bitmap draw paths, including
    // low-resolution Shell icons and non-square collection previews.
    for (const int sourceSize : {16, 32, 64, 256})
        for (const int iconSize : {16, 24, 32, 64, 96})
            for (const int percent : {100, 112, 128, 150, 200})
            {
                const int side = (iconSize * percent + 50) / 100;
                const RECT frame{-100, 200, -100 + side, 200 + side};
                const auto bitmap = dock::BitmapDestination(frame, sourceSize, sourceSize);
                Check(EqualRect(&frame, &bitmap) != FALSE,
                    "Dock bitmap icons fill the same animated frame as Start and Search at every source resolution");
            }

    const RECT frame{100, 200, 182, 282};
    const auto landscape = dock::BitmapDestination(frame, 64, 32);
    Check(landscape.left == 100 && landscape.right == 182 &&
            landscape.top == 220 && landscape.bottom == 261,
        "magnified landscape previews stay proportional and centered");
    const auto portrait = dock::BitmapDestination(frame, 32, 64);
    Check(portrait.left == 120 && portrait.right == 161 &&
            portrait.top == 200 && portrait.bottom == 282,
        "magnified portrait previews stay proportional and centered");
    for (const auto source : {std::pair{0, 64}, std::pair{64, 0}})
    {
        const auto invalid = dock::BitmapDestination(frame, source.first, source.second);
        Check(IsRectEmpty(&invalid) != FALSE, "invalid bitmap dimensions have no drawable destination");
    }
}
