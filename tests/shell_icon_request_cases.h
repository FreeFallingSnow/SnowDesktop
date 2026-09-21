void TestShellIconSourceStamp()
{
    using namespace snowdesktop::shell_icon_request;
    DesktopItem item;
    item.layoutKey = L"SAME-PATH";
    item.modifiedTime = FILETIME{42, 1};
    item.fileSize = 17;
    const auto oldRequest = L"source-generation" + Stamp(item);
    Check(Matches(oldRequest, item), "unchanged desktop icon source can accept its completed image");
    item.modifiedTime->dwLowDateTime = 43;
    Check(!Matches(oldRequest, item), "a file modified at the same path cannot accept a stale icon");
    item.modifiedTime = FILETIME{42, 1};
    item.fileSize = 18;
    Check(!Matches(oldRequest, item), "size changes reject stale results even with equal modification times");
    FolderEntry folder;
    folder.lastWriteTime = FILETIME{42, 1};
    folder.fileSize = 17;
    Check(Matches(oldRequest, folder), "desktop and mapped-folder entries use the same source identity fence");
    folder.lastWriteTime.dwHighDateTime = 2;
    Check(!Matches(oldRequest, folder), "folder icon callbacks reject a changed source before replacing its bitmap");
    item.modifiedTime.reset();
    item.fileSize.reset();
    Check(!Matches(oldRequest, item), "unknown metadata cannot pretend to match a formerly known file stamp");
}
