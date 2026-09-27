// The real Dock uses one hover owner for optional growth and its title. These
// cases protect visibility/transition input gates without treating a disabled
// growth preference as a disabled title. No native desktop is involved.
void CheckDockHoverTitleCases()
{
    namespace hover = snowdesktop::dock_magnification;
    const RECT base{100, 200, 176, 276};
    for (const int effect : {0, 1, 2})
    {
        Check(!hover::ShouldSuppressMagnification(false, false, false, true),
            "a settled interactive Dock keeps its semantic hover in every growth mode");
        const float scale = hover::ResolveFocusScale(effect, 1.6f, true);
        const RECT visual = hover::MagnifyRect(base, DockPosition::Bottom,
            scale, 64);
        const RECT title = hover::AnchorTooltipBounds(visual,
            DockPosition::Bottom, 160, 30, 8);
        Check(title.bottom + 8 == visual.top && title.right > title.left,
            "a Dock title stays outside the current icon geometry even when growth is off");
        Check(effect == 0 ? EqualRect(&base, &visual) != FALSE :
                visual.right - visual.left > base.right - base.left,
            "shared chrome must preserve the configured Dock growth effect");
    }
    Check(hover::ShouldSuppressMagnification(false, false, false, false),
        "hidden or transitioning Dock input must suppress both hover growth and titles");
    for (const bool inputAvailable : {false, true})
        Check(hover::ShouldSuppressMagnification(true, false, false, inputAvailable) &&
                hover::ShouldSuppressMagnification(false, true, false, inputAvailable) &&
                hover::ShouldSuppressMagnification(false, false, true, inputAvailable),
            "dragging or moving/resizing a widget must suppress Dock hover in either visibility state");
}
