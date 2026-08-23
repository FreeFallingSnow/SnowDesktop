#include "component_preview.h"
#include "widget_preview_scene.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace
{

void Expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

bool IsVisuallyCentered(const RECT& inner, const RECT& outer)
{
    if (IsRectEmpty(&inner) || IsRectEmpty(&outer)) return false;
    return std::abs(
               (inner.left + inner.right) -
               (outer.left + outer.right)) <= 1 &&
        std::abs(
            (inner.top + inner.bottom) -
            (outer.top + outer.bottom)) <= 1;
}

snowdesktop::component_preview::Bitmap SolidBitmap(
    int width, int height, std::uint32_t color)
{
    snowdesktop::component_preview::Bitmap bitmap;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.pixels.assign(
        static_cast<size_t>(width) * height, color);
    return bitmap;
}

bool PumpMessagesUntil(
    const std::function<bool()>& predicate, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (predicate()) return true;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return predicate();
}

} // namespace

int wmain()
{
    using namespace snowdesktop::component_preview;

    snowdesktop::WidgetPreviewScene scene;
    scene.AddItem({ L"sample-a", L"Sample A", L"A",
        L"documents", L"today", false });
    scene.PreparePlaceholderModels(96, false);
    DesktopWidget child;
    child.id = L"child";
    child.type = DesktopWidgetType::Collection;
    child.itemKeys = { L"sample-a" };
    scene.AddWidget(child);
    DesktopWidget root;
    root.id = L"root";
    root.type = DesktopWidgetType::CollectionGroup;
    root.childWidgetIds = { L"child" };
    scene.AddWidget(root);
    Expect(scene.FindItem(L"sample-a") != nullptr,
        "preview scene resolves temporary items");
    Expect(scene.FindDesktopItem(L"sample-a") != nullptr &&
            scene.FindDesktopItem(L"sample-a")->iconBitmap != nullptr,
        "preview scene materializes a real desktop item with generated icon");
    Expect(scene.FindFolderEntry(L"sample-a") != nullptr &&
            scene.FindFolderEntry(L"sample-a")->iconBitmap != nullptr,
        "preview scene materializes a real folder entry with generated icon");
    BITMAP previewIcon{};
    Expect(GetObjectW(scene.FindDesktopItem(L"sample-a")->iconBitmap,
               sizeof(previewIcon), &previewIcon) != 0 &&
            previewIcon.bmBitsPixel == 32,
        "preview placeholder uses a 32-bit icon bitmap");
    Expect(scene.FindWidget(L"child") != nullptr,
        "preview scene resolves temporary child widgets");
    Expect(scene.FindWidget(L"missing") == nullptr,
        "preview scene never falls through to live data");

    Window window;
    const RECT menuBounds{ 120, 100, 280, 420 };
    int renderCount = 0;
    bool oldFrameVisibleDuringReplacement = false;
    StagePlacement renderedStage;
    for (UINT dpi : { 96u, 120u, 144u, 192u })
    {
        Model model;
        model.title = L"Preview";
        Card card;
        card.title = L"Actual component";
        card.description = L"Exact-size render callback";
        card.sizeLabel = L"3 x 2";
        const int expectedWidth = 276 + static_cast<int>(dpi / 24);
        const int expectedHeight = 232 + static_cast<int>(dpi / 32);
        const bool expectedLightStage = (dpi / 24) % 2 == 0;
        card.previewWidth = expectedWidth;
        card.previewHeight = expectedHeight;
        card.lightStage = expectedLightStage;
        card.cacheKey = L"mode:" + std::to_wstring(dpi);
        card.render = [&, expectedWidth, expectedHeight, dpi](
            int width, int height, UINT callbackDpi,
            const StagePlacement& stage,
            const ApplySettings&, bool) {
            ++renderCount;
            renderedStage = stage;
            Expect(width == expectedWidth && height == expectedHeight,
                "renderer receives the exact desktop component size");
            Expect(callbackDpi == dpi,
                "renderer receives the current DPI");
            if (renderCount > 1)
                oldFrameVisibleDuringReplacement =
                    IsWindowVisible(window.Handle()) != FALSE;
            return SolidBitmap(width, height, 0xff304860u);
        };
        model.cards.push_back(std::move(card));
        Expect(window.Show(model, menuBounds, nullptr, dpi,
                (dpi / 24) % 2 == 0),
            "preview frame commits successfully");
        Expect(IsWindowVisible(window.Handle()) != FALSE,
            "preview remains visible after commit");

        const RECT cardBounds = window.CardBoundsForTesting();
        const RECT previewBounds = window.PreviewBoundsForTesting();
        Expect(renderedStage.canvasWidth ==
                    cardBounds.right - cardBounds.left &&
                renderedStage.canvasHeight ==
                    cardBounds.bottom - cardBounds.top &&
                renderedStage.offsetX ==
                    previewBounds.left - cardBounds.left &&
                renderedStage.offsetY ==
                    previewBounds.top - cardBounds.top &&
                renderedStage.lightTheme == expectedLightStage,
            "component renderer receives its exact position in the full card stage");

        const RECT closeButton = window.CloseBoundsForTesting();
        Expect(!IsRectEmpty(&closeButton),
            "preview exposes a title-bar close button");

        RECT windowRect{};
        Expect(GetWindowRect(window.Handle(), &windowRect) != FALSE,
            "preview window rectangle is available");
        Expect(windowRect.left != 0 || windowRect.top != 0,
            "preview is never committed through screen origin");

        const int beforeCachedShow = renderCount;
        Expect(window.Show(model, menuBounds, nullptr, dpi,
                (dpi / 24) % 2 == 0),
            "cached preview frame recommits successfully");
        Expect(renderCount == beforeCachedShow,
            "component render callback is cached by mode and DPI");

        Model oppositeStageModel = model;
        oppositeStageModel.cards[0].lightStage = !expectedLightStage;
        const int beforeStageChange = renderCount;
        Expect(window.Show(oppositeStageModel, menuBounds, nullptr, dpi,
                (dpi / 24) % 2 == 0),
            "preview recommits after its card stage theme changes");
        Expect(renderCount == beforeStageChange + 1 &&
                renderedStage.lightTheme == !expectedLightStage,
            "card stage theme participates in model and frame caching");
    }

    Expect(oldFrameVisibleDuringReplacement,
        "old frame stays visible while the replacement is rendered");

    Model filledRowsModel;
    filledRowsModel.title = L"Filled rows";
    filledRowsModel.introduction = L"A short introduction";
    filledRowsModel.resizeHint = L"Resize after adding";
    filledRowsModel.applyLabel = L"Add to Desktop";
    Card filledRowsCard;
    filledRowsCard.title = L"Mode name";
    filledRowsCard.description = L"A short mode description";
    filledRowsCard.sizeLabel = L"3 x 3";
    filledRowsCard.previewWidth = 220;
    filledRowsCard.previewHeight = 180;
    filledRowsCard.cacheKey = L"rows:filled";
    filledRowsCard.render = [](int width, int height, UINT,
            const StagePlacement&,
            const ApplySettings&, bool) {
        return SolidBitmap(width, height, 0xff304860u);
    };
    filledRowsModel.cards.push_back(filledRowsCard);
    Expect(window.Show(filledRowsModel, menuBounds, nullptr, 96, false),
        "preview with optional text renders successfully");
    RECT filledRowsBounds{};
    GetClientRect(window.Handle(), &filledRowsBounds);

    Model emptyRowsModel = filledRowsModel;
    emptyRowsModel.title = L"Empty rows";
    emptyRowsModel.introduction = L" \n\t";
    emptyRowsModel.resizeHint.clear();
    emptyRowsModel.cards[0].title.clear();
    emptyRowsModel.cards[0].description = L"  ";
    emptyRowsModel.cards[0].sizeLabel.clear();
    emptyRowsModel.cards[0].cacheKey = L"rows:empty";
    Expect(window.Show(emptyRowsModel, menuBounds, nullptr, 96, false),
        "preview with empty optional text renders successfully");
    RECT emptyRowsBounds{};
    GetClientRect(window.Handle(), &emptyRowsBounds);
    Expect(emptyRowsBounds.bottom < filledRowsBounds.bottom,
        "empty preview rows collapse instead of reserving space");

    Model pagedModel;
    pagedModel.title = L"Paged preview";
    pagedModel.resizeHint = L"Resize after adding";
    pagedModel.applyLabel = L"Add to Desktop";
    int firstPageRenders = 0;
    int secondPageRenders = 0;
    Card firstPage;
    firstPage.title = L"Grid";
    firstPage.previewWidth = 220;
    firstPage.previewHeight = 180;
    firstPage.cacheKey = L"paged:grid";
    firstPage.applySettings.kind = ApplyKind::Collection;
    firstPage.applySettings.columns = 2;
    firstPage.applySettings.rows = 2;
    bool hoveredFrameRendered = false;
    firstPage.render = [&](int width, int height, UINT,
            const StagePlacement&,
            const ApplySettings&, bool hovered) {
        ++firstPageRenders;
        hoveredFrameRendered = hoveredFrameRendered || hovered;
        return SolidBitmap(width, height, 0xff204060u);
    };
    Card secondPage = firstPage;
    secondPage.title = L"Scrolling list";
    secondPage.cacheKey = L"paged:list";
    secondPage.applySettings.columns = 3;
    secondPage.applySettings.rows = 3;
    secondPage.applySettings.listMode = true;
    secondPage.applySettings.scrollContainerMode = true;
    secondPage.render = [&](int width, int height, UINT,
            const StagePlacement&,
            const ApplySettings&, bool) {
        ++secondPageRenders;
        return SolidBitmap(width, height, 0xff604020u);
    };
    pagedModel.cards.push_back(std::move(firstPage));
    pagedModel.cards.push_back(std::move(secondPage));
    std::optional<ApplySettings> applied;
    Expect(window.Show(pagedModel, menuBounds, nullptr, 96, false,
            [&](const ApplySettings& settings) { applied = settings; }),
        "interactive paged preview commits successfully");
    const LONG_PTR extendedStyle = GetWindowLongPtrW(
        window.Handle(), GWL_EXSTYLE);
    Expect((extendedStyle & WS_EX_TRANSPARENT) == 0,
        "preview accepts pointer input");
    Expect(SendMessageW(window.Handle(), WM_NCHITTEST, 0, 0) == HTCLIENT,
        "preview hit testing stays inside the companion window");
    Expect(firstPageRenders == 1 && secondPageRenders == 0,
        "only the current preview page is rendered");
    Expect(IsVisuallyCentered(window.PreviousGlyphBoundsForTesting(),
               window.PreviousBoundsForTesting()) &&
            IsVisuallyCentered(window.NextGlyphBoundsForTesting(),
                window.NextBoundsForTesting()),
        "the visible paging chevrons are centered in their buttons");
    const RECT pagedCard = window.CardBoundsForTesting();
    Expect(window.PreviousBoundsForTesting().top >= pagedCard.bottom &&
            window.NextBoundsForTesting().top >= pagedCard.bottom &&
            window.MetadataBoundsForTesting().top >= pagedCard.bottom &&
            window.ApplyBoundsForTesting().top >= pagedCard.bottom,
        "paging, metadata, and apply controls sit below the captured card");
    RECT pagedBounds{};
    GetClientRect(window.Handle(), &pagedBounds);
    SendMessageW(window.Handle(), WM_MOUSEMOVE, 0,
        MAKELPARAM((pagedBounds.right - pagedBounds.left) / 2,
            (pagedBounds.bottom - pagedBounds.top) / 2));
    Expect(hoveredFrameRendered,
        "component hover produces the real hovered render state");
    SendMessageW(window.Handle(), WM_KEYDOWN, VK_RIGHT, 0);
    Expect(secondPageRenders == 1,
        "switching pages renders the selected mode");
    applied.reset();
    SendMessageW(window.Handle(), WM_LBUTTONUP, 0,
        MAKELPARAM(pagedBounds.right / 2, pagedBounds.bottom / 2));
    Expect(!applied.has_value(),
        "clicking the preview card itself never applies the component");
    const RECT applyButton = window.ApplyBoundsForTesting();
    Expect(!IsRectEmpty(&applyButton),
        "preview exposes an explicit apply button");
    SendMessageW(window.Handle(), WM_LBUTTONUP, 0,
        MAKELPARAM((applyButton.left + applyButton.right) / 2,
            (applyButton.top + applyButton.bottom) / 2));
    Expect(applied.has_value() && applied->columns == 3 &&
            applied->rows == 3 && applied->listMode &&
            applied->scrollContainerMode,
        "applying a page returns its exact size and mode settings");
    const RECT closeButton = window.CloseBoundsForTesting();
    SendMessageW(window.Handle(), WM_LBUTTONUP, 0,
        MAKELPARAM((closeButton.left + closeButton.right) / 2,
            (closeButton.top + closeButton.bottom) / 2));
    Expect(IsWindowVisible(window.Handle()) == FALSE,
        "the title-bar close button hides only the preview window");

    Model optionModel;
    optionModel.title = L"Same-size options";
    optionModel.applyLabel = L"Add to Desktop";
    Card optionCard;
    optionCard.title = L"Configurable component";
    optionCard.previewWidth = 220;
    optionCard.previewHeight = 180;
    optionCard.cacheKey = L"options:file-group";
    optionCard.applySettings.kind = ApplyKind::FileGroup;
    optionCard.applySettings.columns = 3;
    optionCard.applySettings.rows = 3;
    optionCard.options.push_back({ OptionSetting::ListMode,
        L"Layout", L"Icons", L"List" });
    bool optionRenderUsedListMode = false;
    optionCard.render = [&](int width, int height, UINT,
            const StagePlacement&,
            const ApplySettings& settings, bool) {
        optionRenderUsedListMode = settings.listMode;
        return SolidBitmap(width, height, 0xff305070u);
    };
    optionModel.cards.push_back(std::move(optionCard));
    applied.reset();
    Expect(window.Show(optionModel, menuBounds, nullptr, 96, false,
            [&](const ApplySettings& settings) { applied = settings; }),
        "same-size option preview commits successfully");
    const RECT optionListModeButton = window.OptionBoundsForTesting(
        OptionSetting::ListMode, true);
    Expect(!IsRectEmpty(&optionListModeButton),
        "same-size preview exposes its list control");
    Expect(optionListModeButton.top >=
                window.CardBoundsForTesting().bottom &&
            window.ApplyBoundsForTesting().top >=
                window.CardBoundsForTesting().bottom,
        "same-size options and apply control sit below the wallpaper card");
    SendMessageW(window.Handle(), WM_LBUTTONUP, 0,
        MAKELPARAM(
            (optionListModeButton.left + optionListModeButton.right) / 2,
            (optionListModeButton.top + optionListModeButton.bottom) / 2));
    Expect(optionRenderUsedListMode,
        "same-size control rerenders the component with its new setting");
    SendMessageW(window.Handle(), WM_KEYDOWN, VK_RETURN, 0);
    Expect(applied.has_value() && applied->listMode,
        "same-size control value is included when applying the preview");

    Model collectionOptionsModel;
    collectionOptionsModel.title = L"Collection constraints";
    collectionOptionsModel.applyLabel = L"Add to Desktop";
    Card collectionCard;
    collectionCard.title = L"Expanded collection";
    collectionCard.previewWidth = 220;
    collectionCard.previewHeight = 180;
    collectionCard.cacheKey = L"options:collection";
    collectionCard.applySettings.kind = ApplyKind::Collection;
    collectionCard.applySettings.columns = 2;
    collectionCard.applySettings.rows = 2;
    collectionCard.options.push_back({
        OptionSetting::ScrollContainerMode,
        L"Collection mode", L"Large folder", L"Scroll container" });
    collectionCard.options.push_back({
        OptionSetting::ListMode,
        L"Layout", L"Icons", L"List" });
    bool collectionRenderScrolling = false;
    bool collectionRenderList = false;
    collectionCard.render = [&](int width, int height, UINT,
            const StagePlacement&,
            const ApplySettings& settings, bool) {
        collectionRenderScrolling = settings.scrollContainerMode;
        collectionRenderList = settings.listMode;
        return SolidBitmap(width, height, 0xff406080u);
    };
    collectionOptionsModel.cards.push_back(std::move(collectionCard));
    applied.reset();
    Expect(window.Show(collectionOptionsModel, menuBounds, nullptr, 96,
            false,
            [&](const ApplySettings& settings) { applied = settings; }),
        "collection option constraints render successfully");
    RECT largeFolderBounds{};
    GetClientRect(window.Handle(), &largeFolderBounds);
    Expect(!collectionRenderScrolling && !collectionRenderList,
        "large-folder preview starts without a list layout");
    RECT hiddenListModeButton = window.OptionBoundsForTesting(
        OptionSetting::ListMode, true);
    Expect(IsRectEmpty(&hiddenListModeButton),
        "large-folder preview does not expose list controls");
    RECT scrollingModeButton = window.OptionBoundsForTesting(
        OptionSetting::ScrollContainerMode, true);
    Expect(!IsRectEmpty(&scrollingModeButton),
        "expanded collection exposes its scrolling mode control");
    SendMessageW(window.Handle(), WM_LBUTTONUP, 0,
        MAKELPARAM((scrollingModeButton.left + scrollingModeButton.right) / 2,
            (scrollingModeButton.top + scrollingModeButton.bottom) / 2));
    RECT scrollingBounds{};
    GetClientRect(window.Handle(), &scrollingBounds);
    Expect(collectionRenderScrolling &&
            scrollingBounds.bottom > largeFolderBounds.bottom,
        "list layout appears only after enabling scrolling-container mode");
    RECT listModeButton = window.OptionBoundsForTesting(
        OptionSetting::ListMode, true);
    Expect(!IsRectEmpty(&listModeButton),
        "scrolling-container preview exposes list controls");
    SendMessageW(window.Handle(), WM_LBUTTONUP, 0,
        MAKELPARAM((listModeButton.left + listModeButton.right) / 2,
            (listModeButton.top + listModeButton.bottom) / 2));
    Expect(collectionRenderList,
        "scrolling-container mode accepts the list layout");
    RECT largeFolderModeButton = window.OptionBoundsForTesting(
        OptionSetting::ScrollContainerMode, false);
    SendMessageW(window.Handle(), WM_LBUTTONUP, 0,
        MAKELPARAM(
            (largeFolderModeButton.left + largeFolderModeButton.right) / 2,
            (largeFolderModeButton.top + largeFolderModeButton.bottom) / 2));
    RECT restoredLargeFolderBounds{};
    GetClientRect(window.Handle(), &restoredLargeFolderBounds);
    Expect(restoredLargeFolderBounds.bottom == largeFolderBounds.bottom,
        "returning to large-folder mode removes the list option row");
    hiddenListModeButton = window.OptionBoundsForTesting(
        OptionSetting::ListMode, true);
    Expect(IsRectEmpty(&hiddenListModeButton),
        "large-folder preview removes list click targets");
    SendMessageW(window.Handle(), WM_KEYDOWN, VK_RETURN, 0);
    Expect(applied.has_value() && !applied->scrollContainerMode &&
            !applied->listMode,
        "large-folder settings cannot apply a hidden list layout");

    window.Hide();
    const RECT itemBounds{ 130, 200, 260, 232 };
    Expect(window.ScheduleShow(optionModel, menuBounds, nullptr, 96,
            false, {}, itemBounds),
        "preview accepts submenu-style delayed opening");
    Expect(IsWindowVisible(window.Handle()) == FALSE,
        "preview remains hidden during submenu dwell time");
    SendMessageW(window.Handle(), WM_TIMER, 1, 0);
    Expect(IsWindowVisible(window.Handle()) != FALSE,
        "preview opens when the submenu dwell timer expires");
    RECT delayedBounds{};
    GetWindowRect(window.Handle(), &delayedBounds);
    Expect(delayedBounds.top == itemBounds.top - 5,
        "preview aligns to the hovered row like a submenu");

    Model replacementModel = optionModel;
    replacementModel.title = L"Replacement preview";
    replacementModel.cards[0].cacheKey = L"options:replacement";
    bool replacementRendered = false;
    replacementModel.cards[0].render = [&](int width, int height, UINT,
            const StagePlacement&,
            const ApplySettings&, bool) {
        replacementRendered = true;
        return SolidBitmap(width, height, 0xff507030u);
    };
    Expect(window.ScheduleShow(replacementModel, menuBounds, nullptr, 96,
            false, {}, itemBounds),
        "hovering a sibling schedules its preview");
    POINT savedCursor{};
    GetCursorPos(&savedCursor);
    SetCursorPos(0, 0);
    SendMessageW(window.Handle(), WM_TIMER, 2, 0);
    Expect(IsWindowVisible(window.Handle()) != FALSE,
        "a stale close timer keeps the old frame while a sibling is pending");
    SendMessageW(window.Handle(), WM_TIMER, 1, 0);
    SetCursorPos(savedCursor.x, savedCursor.y);
    Expect(replacementRendered && IsWindowVisible(window.Handle()) != FALSE,
        "the sibling preview atomically replaces the old frame");

    Expect(window.Show(replacementModel, menuBounds, nullptr, 96,
            false, {}, itemBounds,
            snowdesktop::modern_menu::Appearance::OpaqueDark),
        "the preview accepts an opaque menu appearance");
    Expect(!window.BlurEnabledForTesting(),
        "an opaque menu also disables blur on its companion preview");
    Expect(window.Show(replacementModel, menuBounds, nullptr, 96,
            true, {}, itemBounds,
            snowdesktop::modern_menu::Appearance::SystemLightBlur),
        "the preview can switch back to a blur menu appearance");
    Expect(window.BlurEnabledForTesting(),
        "an explicit blur theme restores the companion preview backdrop");

    const auto& commits = window.CommittedPositionsForTesting();
    Expect(!commits.empty(),
        "preview records its committed window positions");
    for (const POINT position : commits)
        Expect(position.x != 0 || position.y != 0,
            "no layered-window commit passes through (0,0)");
    window.Close();

    Model wallpaperModel;
    wallpaperModel.title = L"Prefetched wallpaper";
    Card wallpaperCard;
    wallpaperCard.title = L"Wallpaper component";
    wallpaperCard.previewWidth = 120;
    wallpaperCard.previewHeight = 80;
    wallpaperCard.cacheKey = L"wallpaper:prefetch";
    wallpaperCard.useDesktopWallpaperStage = true;
    wallpaperCard.render = [](int width, int height, UINT,
            const StagePlacement&, const ApplySettings&, bool) {
        return SolidBitmap(width, height, 0x80406080u);
    };
    wallpaperModel.cards.push_back(std::move(wallpaperCard));

    std::atomic_int successfulCaptureCount = 0;
    std::atomic_bool successfulCaptureStarted = false;
    std::atomic_bool releaseSuccessfulCapture = false;
    Window successfulPrefetch([&](const RECT& desktopBounds, DWORD,
            const std::atomic_bool* cancelled) {
        ++successfulCaptureCount;
        successfulCaptureStarted.store(true, std::memory_order_relaxed);
        while (!releaseSuccessfulCapture.load(std::memory_order_relaxed) &&
            !(cancelled && cancelled->load(std::memory_order_relaxed)))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        WallpaperBackdropCaptureResult result;
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            return result;
        result.wallpaper.width = 1;
        result.wallpaper.height = 1;
        result.wallpaper.pixels = { 0xff204080u };
        result.desktopBounds = desktopBounds;
        return result;
    });
    successfulPrefetch.PrefetchDesktopWallpaperBackdrop(
        nullptr, POINT{ 160, 160 });
    Expect(PumpMessagesUntil([&] {
                return successfulCaptureStarted.load(
                    std::memory_order_relaxed);
            }, 500),
        "fake wallpaper capture starts without Wallpaper Engine");
    Expect(successfulPrefetch.Show(wallpaperModel, menuBounds, nullptr,
            96, false),
        "preview accepts a pending prefetched wallpaper");
    Expect(successfulPrefetch.WaitingForWallpaperEngineFrameForTesting() &&
            IsWindowVisible(successfulPrefetch.Handle()) == FALSE,
        "first preview stays hidden while wallpaper prefetch is pending");
    releaseSuccessfulCapture.store(true, std::memory_order_relaxed);
    Expect(PumpMessagesUntil([&] {
                return IsWindowVisible(successfulPrefetch.Handle()) != FALSE;
            }, 1000),
        "successful prefetch reveals the waiting first preview");
    Expect(successfulPrefetch.HasWallpaperEngineCacheForTesting() &&
            successfulCaptureCount.load(std::memory_order_relaxed) == 1,
        "successful prefetch is cached once for the menu lifetime");
    successfulPrefetch.Hide();
    Expect(successfulPrefetch.Show(wallpaperModel, menuBounds, nullptr,
            96, false) &&
            IsWindowVisible(successfulPrefetch.Handle()) != FALSE &&
            successfulCaptureCount.load(std::memory_order_relaxed) == 1,
        "later previews reuse the menu-scoped wallpaper frame");
    successfulPrefetch.Close();
    Expect(!successfulPrefetch.HasWallpaperEngineCacheForTesting(),
        "closing the menu preview releases its wallpaper frame");

    std::atomic_bool releaseFailedCapture = false;
    Window failedPrefetch([&](const RECT&, DWORD,
            const std::atomic_bool* cancelled) {
        while (!releaseFailedCapture.load(std::memory_order_relaxed) &&
            !(cancelled && cancelled->load(std::memory_order_relaxed)))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return WallpaperBackdropCaptureResult{};
    });
    failedPrefetch.PrefetchDesktopWallpaperBackdrop(
        nullptr, POINT{ 160, 160 });
    Expect(failedPrefetch.Show(wallpaperModel, menuBounds, nullptr,
            96, false) &&
            failedPrefetch.WaitingForWallpaperEngineFrameForTesting() &&
            IsWindowVisible(failedPrefetch.Handle()) == FALSE,
        "failed prefetch also defers the first preview while pending");
    releaseFailedCapture.store(true, std::memory_order_relaxed);
    Expect(PumpMessagesUntil([&] {
                return IsWindowVisible(failedPrefetch.Handle()) != FALSE;
            }, 1500) &&
            !failedPrefetch.HasWallpaperEngineCacheForTesting(),
        "failed prefetch reveals the preview with its static fallback");
    failedPrefetch.Close();

    std::atomic_bool cancellableCaptureStarted = false;
    std::atomic_bool captureObservedCancellation = false;
    Window cancellablePrefetch([&](const RECT&, DWORD,
            const std::atomic_bool* cancelled) {
        cancellableCaptureStarted.store(true, std::memory_order_relaxed);
        while (!(cancelled &&
            cancelled->load(std::memory_order_relaxed)))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        captureObservedCancellation.store(true,
            std::memory_order_relaxed);
        return WallpaperBackdropCaptureResult{};
    });
    cancellablePrefetch.PrefetchDesktopWallpaperBackdrop(
        nullptr, POINT{ 160, 160 });
    Expect(PumpMessagesUntil([&] {
                return cancellableCaptureStarted.load(
                    std::memory_order_relaxed);
            }, 500),
        "cancellable fake prefetch starts");
    const auto closeStarted = std::chrono::steady_clock::now();
    cancellablePrefetch.Close();
    const auto closeElapsed = std::chrono::steady_clock::now() -
        closeStarted;
    Expect(captureObservedCancellation.load(std::memory_order_relaxed) &&
            closeElapsed < std::chrono::milliseconds(750),
        "closing the menu cancels and joins a pending prefetch promptly");
    return 0;
}
