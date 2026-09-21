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
    click.Press(file, label, icon, false, true, 1000, 500);
    Check(!click.Release(file, label, icon, true, 1020, 500),
        "the first selecting click must not rename");
    click.Press(file, label, name, true, true, 2000, 500);
    Check(click.Release(file, label, name, true, 2020, 500),
        "a slow second click on the selected name must schedule rename");
    Check(!click.TakeReady(file, label, true, 2519),
        "rename must wait until the double-click interval has passed");
    Check(click.TakeReady(file, label, true, 2520) == file,
        "the selected file must become editable after the delay");
    Check(!click.TakeReady(file, label, true, 3000),
        "a timer must not open the same editor twice");

    click.Press(file, label, name, false, true, 4000, 500);
    click.Release(file, label, name, true, 4020, 500);
    click.Press(file, label, name, true, true, 4100, 500);
    Check(!click.Release(file, label, name, true, 4120, 500),
        "fast repeated clicks across HWNDs must not become rename");

    for (int scenario = 0; scenario < 5; ++scenario)
    {
        click.Cancel();
        click.Press(file, label, scenario == 0 ? icon : name,
            scenario != 1, scenario != 2, 5000, 500);
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
            click.Press(target, label, name, true, true, 6000, 500);
            Check(click.Release(target, label, name, true, 6020, 500),
                "every supported file surface uses the same delayed trigger");
            auto current = target;
            RECT currentLabel = label;
            if (scenario == 0) current.key = L"replacement";
            if (scenario == 1) current.surface = L"different-popup";
            if (scenario == 2) currentLabel.top += 2;
            if (scenario == 4) click.Cancel(); // double-click, key, menu, focus/capture loss
            if (scenario == 5) click.Press(other, label, name, false, true, 6200, 500);
            Check(!click.TakeReady(current, currentLabel, scenario != 3, 6520),
                "timer revalidates identity, surface, geometry, selection and cancellation");
        }
    }
    click.Press(file, label, name, true, true, 7000, 500);
    click.Release(file, label, name, true, 7020, 500);
    click.Move(icon, 4, 4);
    Check(!click.TakeReady(file, label, true, 7520),
        "leaving the label cancels a pending edit");
}
