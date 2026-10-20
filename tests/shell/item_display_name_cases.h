// These Shell-boundary cases prevent visible or unavailable metadata from
// exposing .lnk, and prevent a rename from restoring it in live aliases.
void TestShortcutDisplayNames()
{
    struct Case
    {
        const wchar_t* fileName;
        const wchar_t* shellName;
        bool success;
        bool directory;
        const wchar_t* expected;
    };
    const Case cases[] = {
        {L"Tool.LnK", L"Tool.LnK", true, false, L"Tool"},
        {L"Tool.lnk", L"Tool", true, false, L"Tool"},
        {L"Tool.lnk", L"", false, false, L"Tool"},
        {L"Tool.lnk", L"", true, false, L"Tool"},
        {L"Notes.txt.lnk", L"Notes.txt.lnk", true, false, L"Notes.txt"},
        {L"Tool.lnk.lnk", L"Tool.lnk.lnk", true, false, L"Tool.lnk"},
        {L"Tool.lnk.lnk", L"Tool.lnk", true, false, L"Tool.lnk"},
        {L"Tool.LNK.lnk", L"Tool.LNK", true, false, L"Tool.LNK"},
        {L"Tool.lnk", L"本地化标题.LNK", true, false, L"本地化标题"},
        {L"Tool.lnk", L"标题.lnk ", true, false, L"标题.lnk "},
        {L"report.txt", L"report.txt", true, false, L"report.txt"},
        {L"report.txt", L"report", true, false, L"report"},
        {L"report.txt", L"Title.lnk", true, false, L"Title.lnk"},
        {L"Website.url", L"Website.url", true, false, L"Website.url"},
        {L"Folder.lnk", L"Folder.lnk", true, true, L"Folder.lnk"},
        {L"Folder.lnk", L"", false, true, L"Folder.lnk"},
        {L".lnk", L".lnk", true, false, L".lnk"},
    };
    for (const auto& test : cases)
    {
        FolderEntry entry;
        entry.name = test.fileName;
        entry.fullPath = L"C:\\Mapped.lnk\\" + entry.name;
        entry.isDirectory = test.directory;
        entry.displayName = snowdesktop::shell_refresh::ReadFolderDisplayName(
            entry.fullPath, entry.name,
            [&](const std::wstring& path, SHFILEINFOW& info) {
                Check(path == entry.fullPath, "shortcut display lookup retains its full source path");
                wcscpy_s(info.szDisplayName, test.shellName);
                return test.success;
            }, entry.isDirectory);
        Check(entry.DisplayName() == test.expected && entry.name == test.fileName &&
            entry.fullPath == L"C:\\Mapped.lnk\\" + std::wstring(test.fileName),
            "folder labels hide only the shortcut extension without changing filesystem identities");
    }
}

void TestShortcutRenameDisplayNames()
{
    snowdesktop::ShellRenameResult result;
    result.status = S_OK;
    result.sourcePath = L"C:\\Desktop\\Before.lnk";
    result.path = L"C:\\Desktop\\After.LNK";
    result.displayName = L"After.LNK";
    result.attributes.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    result.metadataComplete = true;
    // Only Shell identity creation is substituted; Apply updates real models.
    result.absoluteId = result.desktopChildId = {0, 0};
    std::vector<DesktopItem> items(1);
    items[0].parsingName = result.sourcePath;
    items[0].name = L"Before";
    items[0].selected = true;
    std::vector<DesktopWidget> widgets(2);
    widgets[0].type = DesktopWidgetType::Collection;
    widgets[0].itemKeys = {result.sourcePath};
    widgets[1].type = DesktopWidgetType::FolderMapping;
    widgets[1].id = L"mapping";
    widgets[1].folderEntries.resize(1);
    widgets[1].folderEntries[0].fullPath = result.sourcePath;
    widgets[1].folderEntries[0].name = L"Before.lnk";
    DesktopWidget popup = widgets[1];
    std::vector<DockEntry> dock(1);
    dock[0].type = DockEntryType::DesktopItem;
    dock[0].reference = result.sourcePath;
    const auto changes = snowdesktop::rename_model_update::Apply(
        result, L"C:\\DESKTOP\\AFTER.LNK", items, widgets, dock, &popup);
    Check(!changes.needsReload && changes.desktopItems == std::vector<size_t>{0} &&
        changes.popup && items[0].name == L"After" && items[0].selected &&
        widgets[1].folderEntries[0].DisplayName() == L"After" &&
        popup.folderEntries[0].DisplayName() == L"After",
        "rename cannot reveal .lnk in desktop, mapped-folder or popup aliases");
    Check(items[0].parsingName == result.path &&
        items[0].layoutKey == L"C:\\DESKTOP\\AFTER.LNK" &&
        widgets[0].itemKeys[0] == result.path && dock[0].reference == result.path &&
        widgets[1].folderEntries[0].name == L"After.LNK" &&
        widgets[1].folderEntries[0].fullPath == result.path &&
        popup.folderEntries[0].fullPath == result.path,
        "hidden shortcut labels retain full rename, membership and launch identities");
}
