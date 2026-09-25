// The production click state machine consumes hit-test/selection snapshots and
// a clock. These cases substitute only those inputs; no native desktop HWND is
// automated. A wrong trigger, missed trigger or stale target fails Check.
void TestSlowRenameClicks()
{
    const RECT label{10, 70, 100, 110};
    const POINT name{30, 80};
    const POINT icon{30, 20};
    const RenameClickTarget file{RenameTargetKind::DesktopItem, L"file-a", L"desktop"};
    const RenameClickTarget other{RenameTargetKind::DesktopItem, L"file-b", L"desktop"};
    RenameClickController click;
    click.Press(file, label, icon, false, true);
    Check(!click.Release(file, label, icon, true, 1020, 500),
        "the first selecting click must not rename");
    click.Move({-1200, 2400}, 4, 4);
    click.Move(name, 4, 4);
    click.Press(file, label, name, true, true);
    Check(click.Release(file, label, name, true, 2020, 500),
        "moving far between clicks must not block a click on the selected name");
    Check(!click.TakeReady(file, label, true, 2519),
        "rename must wait until the double-click interval has passed");
    Check(click.TakeReady(file, label, true, 2520) == file,
        "the selected file must become editable after the delay");
    Check(!click.TakeReady(file, label, true, 3000),
        "a timer must not open the same editor twice");

    // Windows can deliver another ordinary down within 500 ms (for example,
    // outside its double-click rectangle or on a different input HWND).
    click.Press(file, label, name, false, true);
    click.Release(file, label, name, true, 4020, 500);
    click.Press(file, label, name, true, true);
    Check(click.Release(file, label, name, true, 4120, 500),
        "ordinary name clicks must not be rejected by a duplicate time-only gate");
    Check(click.TakeReady(file, label, true, 4620) == file,
        "a click not classified as WM_LBUTTONDBLCLK must allow rename");

    click.Press(file, label, name, true, true);
    click.Release(file, label, name, true, 4700, 500);
    click.Cancel(); // WM_LBUTTONDBLCLK replaces, rather than follows, the next down.
    Check(!click.Release(file, label, name, true, 4800, 500) &&
            !click.TakeReady(file, label, true, 5300),
        "a real double-click must cancel rename including its final button-up");

    for (int scenario = 0; scenario < 5; ++scenario)
    {
        click.Cancel();
        click.Press(file, label, scenario == 0 ? icon : name,
            scenario != 1, scenario != 2);
        if (scenario == 3)
        {
            click.Move({40, 80}, 4, 4);
            click.Move(name, 4, 4);
        }
        Check(!click.Release(scenario == 4 ? other : file,
                label, name, true, 5020, 500),
            "icon clicks, multiselection, modifiers, drag-return and changed targets cannot rename");
    }

    for (const auto kind : {RenameTargetKind::DesktopItem,
            RenameTargetKind::FolderEntry, RenameTargetKind::DockFolderEntry})
    {
        const RenameClickTarget target{kind, L"file-a", L"surface-a"};
        for (int scenario = 0; scenario < 6; ++scenario)
        {
            click.Cancel();
            click.Press(target, label, name, true, true);
            Check(click.Release(target, label, name, true, 6020, 500),
                "every supported file surface uses the same delayed trigger");
            auto current = target;
            RECT currentLabel = label;
            if (scenario == 0) current.key = L"replacement";
            if (scenario == 1) current.surface = L"different-popup";
            if (scenario == 2) currentLabel.top += 2;
            if (scenario == 4) click.Cancel(); // double-click, key, menu, focus/capture loss
            if (scenario == 5) click.Press(other, label, name, false, true);
            Check(!click.TakeReady(current, currentLabel, scenario != 3, 6520),
                "timer revalidates identity, surface, geometry, selection and cancellation");
        }
    }
    click.Press(file, label, name, true, true);
    click.Release(file, label, name, true, 7020, 500);
    click.Move(icon, 4, 4);
    Check(click.TakeReady(file, label, true, 7520) == file,
        "moving after button-up must not cancel a completed name click");

    click.Press(file, label, name, true, true);
    click.Release(file, label, name, true, 8000, 500);
    Check(click.RemainingDelay(8484) == 16 && !click.TakeReady(file, label, true, 8484),
        "an early timer must wait only the remaining 16 ms, not another 500 ms");
    Check(click.RemainingDelay(8500) == 0 && click.TakeReady(file, label, true, 8500) == file,
        "rescheduling an early timer must preserve the original deadline");
}
