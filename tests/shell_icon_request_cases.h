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

// Real model/bitmap ownership, with only the D2D cache invalidation substituted.
// A refresh must never display full image -> provisional icon -> full image.
template<class Item>
void CheckIconRefreshPresentation()
{
    using namespace snowdesktop::shell_icon_request;
    Item item;
    int erased = 0;
    const auto erase = [&](HBITMAP bitmap) {
        BITMAP info{};
        Check(GetObjectW(bitmap, sizeof(info), &info) != 0,
            "cached image is invalidated before its GDI handle is destroyed");
        ++erased;
    };
    const auto image = [] { return CreateBitmap(2, 2, 1, 32, nullptr); };
    HBITMAP first = image();
    Check(first != nullptr, "refresh fixture creates its first image");
    if (!first) return;
    ApplyBitmap(item, Phase::Phase1, first, {2, 2}, false, erase);
    ApplyPresentation(item, Phase::Phase1, false, false);
    Check(!first && item.iconBitmap && item.iconState == IconState::IconReady && erased == 0,
        "an empty item displays first pixels immediately and transfers ownership");
    HBITMAP refined = image();
    const auto full = refined;
    Check(refined != nullptr, "refresh fixture creates its refined image");
    if (!refined) return;
    ApplyBitmap(item, Phase::Phase2, refined, {96, 64}, true, erase);
    ApplyPresentation(item, Phase::Phase2, false, false);
    Check(!refined && item.iconBitmap == full && item.iconIsMediaThumbnail && erased == 1,
        "successful refinement replaces first pixels and invalidates their render cache");
    for (const auto state : {IconState::Loading, IconState::FullQuality})
    {
        item.iconState = state;
        HBITMAP provisional = image();
        const auto rejected = provisional;
        Check(provisional != nullptr, "refresh fixture creates a provisional image");
        if (!provisional) continue;
        ApplyBitmap(item, Phase::Phase1, provisional, {2, 2}, false, erase);
        ApplyPresentation(item, Phase::Phase1, false, false);
        BITMAP info{};
        Check(!provisional && !GetObjectW(rejected, sizeof(info), &info) &&
            item.iconBitmap == full && item.iconBitmapSize.cx == 96 &&
            item.iconBitmapSize.cy == 64 && item.iconIsMediaThumbnail && erased == 1,
            "refresh and late first-image callbacks release provisional pixels without flashing the visible thumbnail");
        Check(item.iconState == (state == IconState::FullQuality ? IconState::FullQuality : IconState::IconReady),
            "a refresh can proceed to refinement without downgrading already completed quality");
    }
    HBITMAP missing = nullptr;
    ApplyBitmap(item, Phase::Phase2, missing, {}, false, erase);
    Check(item.iconBitmap == full && item.iconIsMediaThumbnail && erased == 1,
        "failed refinement leaves the previous visible image and its cache intact");
    HBITMAP replacement = image();
    const auto updated = replacement;
    Check(replacement != nullptr, "refresh fixture creates its replacement image");
    if (!replacement) return;
    ApplyBitmap(item, Phase::Phase2, replacement, {128, 128}, false, erase);
    ApplyPresentation(item, Phase::Phase2, false, false);
    Check(!replacement && item.iconBitmap == updated && item.iconBitmapSize.cx == 128 &&
        !item.iconIsMediaThumbnail && item.iconState == IconState::FullQuality && erased == 2,
        "new refined content still replaces the retained image after refresh");
}

void TestIconRefreshPresentation()
{
    CheckIconRefreshPresentation<DesktopItem>();
    CheckIconRefreshPresentation<FolderEntry>();
}
