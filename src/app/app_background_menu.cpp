#include "app.h"
#include "../menu_fluent_glyphs.h"
#include "../modern_menu.h"
#include "../search_match.h"
#include "../widget_preview_stage.h"

#include <cstring>
#include <unordered_map>

// Grid adjustment and desktop-background context menus.

namespace
{

constexpr size_t kLuaWidgetMenuPageSize = 8;

enum class LuaWidgetMenuSource
{
    Builtin,
    Installed,
    Development,
};

enum class LuaWidgetMenuFilter
{
    All,
    Builtin,
    Installed,
    Development,
};

struct LuaWidgetMenuEntry
{
    std::wstring packageId;
    std::wstring displayName;
    std::wstring searchText;
    LuaWidgetMenuSource source = LuaWidgetMenuSource::Installed;
};

std::vector<LuaWidgetMenuEntry> BuildLuaWidgetMenuEntries()
{
    const auto packages = WidgetEngine::ListWidgetPackages();
    std::unordered_map<std::wstring, const snowdesktop::widget::InstalledPackage*>
        packagesById;
    for (const auto& package : packages)
    {
        if (!package.active || !package.enabled)
            continue;
        packagesById[Utf8ToWide(package.manifest.id)] = &package;
    }

    std::vector<LuaWidgetMenuEntry> entries;
    for (const auto& packageId : WidgetEngine::ListAvailable())
    {
        LuaWidgetMenuEntry entry;
        entry.packageId = packageId;
        entry.displayName = WidgetEngine::GetWidgetDisplayName(packageId);
        if (entry.displayName.empty()) entry.displayName = packageId;
        entry.searchText = entry.displayName + L"\n" + packageId;
        const LuaWidgetManifest manifest =
            WidgetEngine::GetWidgetManifest(packageId);
        entry.searchText += L"\n" + Utf8ToWide(manifest.description);
        entry.searchText += L"\n" + Utf8ToWide(manifest.publisher);

        if (const auto found = packagesById.find(packageId);
            found != packagesById.end())
        {
            if (found->second->builtin)
                entry.source = LuaWidgetMenuSource::Builtin;
            else if (found->second->development)
                entry.source = LuaWidgetMenuSource::Development;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool LuaWidgetMenuSourceMatches(
    LuaWidgetMenuSource source, LuaWidgetMenuFilter filter)
{
    switch (filter)
    {
    case LuaWidgetMenuFilter::Builtin:
        return source == LuaWidgetMenuSource::Builtin;
    case LuaWidgetMenuFilter::Installed:
        return source == LuaWidgetMenuSource::Installed;
    case LuaWidgetMenuFilter::Development:
        return source == LuaWidgetMenuSource::Development;
    case LuaWidgetMenuFilter::All:
    default:
        return true;
    }
}

std::optional<LuaWidgetMenuFilter> LuaWidgetMenuFilterForCommand(
    UINT command)
{
    switch (command)
    {
    case kContextAddLuaWidgetFilterAll:
        return LuaWidgetMenuFilter::All;
    case kContextAddLuaWidgetFilterBuiltin:
        return LuaWidgetMenuFilter::Builtin;
    case kContextAddLuaWidgetFilterInstalled:
        return LuaWidgetMenuFilter::Installed;
    case kContextAddLuaWidgetFilterDevelopment:
        return LuaWidgetMenuFilter::Development;
    default:
        return std::nullopt;
    }
}

std::vector<LuaWidgetMenuEntry> FilterLuaWidgetMenuEntries(
    const std::vector<LuaWidgetMenuEntry>& entries,
    const std::wstring& search, LuaWidgetMenuFilter filter)
{
    std::vector<LuaWidgetMenuEntry> filtered;
    for (const auto& entry : entries)
    {
        if (!LuaWidgetMenuSourceMatches(entry.source, filter))
            continue;
        if (!search.empty() &&
            !NameMatchesQuery(entry.searchText, search))
            continue;
        filtered.push_back(entry);
    }
    if (!search.empty())
    {
        std::ranges::stable_sort(filtered,
            [&](const auto& left, const auto& right) {
                const int leftName = NameSearchMatchRank(
                    left.displayName, search);
                const int rightName = NameSearchMatchRank(
                    right.displayName, search);
                const int leftRank = leftName < kNameSearchNoMatchRank
                    ? leftName
                    : NameSearchMatchRank(left.searchText, search);
                const int rightRank = rightName < kNameSearchNoMatchRank
                    ? rightName
                    : NameSearchMatchRank(right.searchText, search);
                return leftRank < rightRank;
            });
    }
    return filtered;
}

size_t LuaWidgetMenuPageCount(size_t widgetCount)
{
    return std::max<size_t>(
        1, (widgetCount + kLuaWidgetMenuPageSize - 1) /
            kLuaWidgetMenuPageSize);
}

std::vector<snowdesktop::modern_menu::Item> BuildAddWidgetMenuItems(
    const std::vector<LuaWidgetMenuEntry>& allLuaWidgets,
    const std::vector<LuaWidgetMenuEntry>& luaWidgets, size_t page,
    const std::wstring& search, LuaWidgetMenuFilter filter)
{
    using snowdesktop::modern_menu::Item;

    std::vector<Item> items;
    UINT inlineGroup = 1;
    auto appendPair = [&](UINT command, UINT previewCommand,
                          const wchar_t* label, const wchar_t* glyph) {
        Item item;
        item.command = command;
        item.label = label;
        item.glyph = glyph;
        item.inlineAction = true;
        item.inlineGroup = inlineGroup;
        items.push_back(std::move(item));

        Item preview;
        preview.command = previewCommand;
        preview.label = _LW("app.menu.preview");
        preview.inlineAction = true;
        preview.inlineGroup = inlineGroup++;
        preview.compactInlineAction = true;
        items.push_back(std::move(preview));
    };
    appendPair(kContextAddCollectionWidget,
        kContextPreviewCollectionWidget,
        _LW("app.menu.collection"),
        snowdesktop::menu_fluent_glyphs::kCollection);
    appendPair(kContextAddFileCategoryWidget,
        kContextPreviewFileCategoryWidget,
        _LW("app.menu.file_categories"),
        snowdesktop::menu_fluent_glyphs::kDesktopFiles);
    appendPair(kContextAddFolderMappingWidget,
        kContextPreviewFolderMappingWidget,
        _LW("app.menu.folder_mapping"),
        snowdesktop::menu_fluent_glyphs::kFolderMapping);
    appendPair(kContextAddCollectionGroupWidget,
        kContextPreviewCollectionGroupWidget,
        _LW("app.menu.collection_group"),
        snowdesktop::menu_fluent_glyphs::kCollectionGroup);
    appendPair(kContextAddFileGroupWidget,
        kContextPreviewFileGroupWidget,
        _LW("app.menu.file_group"),
        snowdesktop::menu_fluent_glyphs::kFileGroup);

    if (allLuaWidgets.empty())
        return items;

    items.push_back({ 0, L"", L"", false, false, true });

    Item searchItem;
    searchItem.command = kContextAddLuaWidgetSearch;
    searchItem.label = _LW("app.settings.widgets_search_hint");
    searchItem.glyph = L"\uF68F";
    searchItem.textInput = true;
    searchItem.inputText = search;
    items.push_back(std::move(searchItem));

    const auto sourceCount = [&](LuaWidgetMenuSource source) {
        return static_cast<int>(std::ranges::count_if(allLuaWidgets,
            [source](const auto& entry) { return entry.source == source; }));
    };
    const int builtinCount = sourceCount(LuaWidgetMenuSource::Builtin);
    const int installedCount = sourceCount(LuaWidgetMenuSource::Installed);
    const int developmentCount = sourceCount(
        LuaWidgetMenuSource::Development);
    UINT filterGroup = inlineGroup++;
    const auto appendFilter = [&](UINT command, const wchar_t* label,
                                  int count, LuaWidgetMenuFilter itemFilter) {
        Item item;
        item.command = command;
        item.label = label;
        item.label += L" " + std::to_wstring(count);
        item.checked = filter == itemFilter;
        item.inlineAction = true;
        item.inlineGroup = filterGroup;
        item.horizontalScrollAction = true;
        items.push_back(std::move(item));
    };
    appendFilter(kContextAddLuaWidgetFilterAll,
        _LW("app.settings.widgets_filter_all"),
        static_cast<int>(allLuaWidgets.size()), LuaWidgetMenuFilter::All);
    if (builtinCount > 0)
        appendFilter(kContextAddLuaWidgetFilterBuiltin,
            _LW("app.settings.widgets_filter_builtin"), builtinCount,
            LuaWidgetMenuFilter::Builtin);
    if (installedCount > 0)
        appendFilter(kContextAddLuaWidgetFilterInstalled,
            _LW("app.settings.widgets_filter_installed"), installedCount,
            LuaWidgetMenuFilter::Installed);
    if (developmentCount > 0)
        appendFilter(kContextAddLuaWidgetFilterDevelopment,
            _LW("app.settings.widgets_filter_development"), developmentCount,
            LuaWidgetMenuFilter::Development);

    items.push_back({ 0, L"", L"", false, false, true });
    if (luaWidgets.empty())
    {
        Item empty;
        empty.command = kContextAddLuaWidgetEmpty;
        empty.label = _LW("app.settings.widgets_filter_empty");
        empty.glyph = snowdesktop::menu_fluent_glyphs::kCollectionGroup;
        empty.enabled = false;
        items.push_back(std::move(empty));
        return items;
    }

    const size_t pageCount = LuaWidgetMenuPageCount(luaWidgets.size());
    page = std::min(page, pageCount - 1);
    const size_t first = page * kLuaWidgetMenuPageSize;
    const size_t last = std::min(
        luaWidgets.size(), first + kLuaWidgetMenuPageSize);
    for (size_t i = first; i < last; ++i)
    {
        Item item;
        item.command = kContextAddLuaWidgetFirst +
            static_cast<UINT>(i - first);
        item.label = luaWidgets[i].displayName;
        item.glyph = L"\uEE65";
        item.inlineAction = true;
        item.inlineGroup = inlineGroup;
        items.push_back(std::move(item));

        Item preview;
        preview.command = kContextPreviewLuaWidgetFirst +
            static_cast<UINT>(i - first);
        preview.label = _LW("app.menu.preview");
        preview.inlineAction = true;
        preview.inlineGroup = inlineGroup++;
        preview.compactInlineAction = true;
        items.push_back(std::move(preview));
    }

    if (pageCount > 1)
    {
        items.push_back({ 0, L"", L"", false, false, true });
        Item previous;
        previous.command = kContextAddLuaWidgetPreviousPage;
        previous.glyph = L"\uF15B";
        previous.enabled = page > 0;
        previous.inlineAction = true;
        previous.inlineGroup = inlineGroup;
        items.push_back(std::move(previous));

        Item status;
        status.command = kContextAddLuaWidgetPageStatus;
        status.label = _LFW("app.menu.lua_widgets_page",
            std::to_wstring(page + 1), std::to_wstring(pageCount));
        status.enabled = false;
        status.inlineAction = true;
        status.inlineGroup = inlineGroup;
        items.push_back(std::move(status));

        Item next;
        next.command = kContextAddLuaWidgetNextPage;
        next.glyph = L"\uF181";
        next.enabled = page + 1 < pageCount;
        next.inlineAction = true;
        next.inlineGroup = inlineGroup;
        items.push_back(std::move(next));
    }
    return items;
}

UINT AddCommandForPreviewCommand(UINT command)
{
    switch (command)
    {
    case kContextPreviewCollectionWidget:
        return kContextAddCollectionWidget;
    case kContextPreviewFileCategoryWidget:
        return kContextAddFileCategoryWidget;
    case kContextPreviewFolderMappingWidget:
        return kContextAddFolderMappingWidget;
    case kContextPreviewCollectionGroupWidget:
        return kContextAddCollectionGroupWidget;
    case kContextPreviewFileGroupWidget:
        return kContextAddFileGroupWidget;
    default:
        break;
    }
    if (command >= kContextPreviewLuaWidgetFirst &&
        command < kContextPreviewLuaWidgetFirst +
            static_cast<UINT>(kLuaWidgetMenuPageSize))
    {
        return kContextAddLuaWidgetFirst +
            (command - kContextPreviewLuaWidgetFirst);
    }
    return 0;
}

bool IsAddWidgetMenuCommand(UINT command)
{
    if (AddCommandForPreviewCommand(command) != 0)
        return true;
    if (command >= kContextAddLuaWidgetFirst &&
        command < kContextAddLuaWidgetFirst +
            static_cast<UINT>(kLuaWidgetMenuPageSize))
        return true;
    switch (command)
    {
    case kContextAddCollectionWidget:
    case kContextAddCollectionGroupWidget:
    case kContextAddFileGroupWidget:
    case kContextAddFileCategoryWidget:
    case kContextAddFolderMappingWidget:
    case kContextAddLuaWidgetSearch:
    case kContextAddLuaWidgetFilterAll:
    case kContextAddLuaWidgetFilterBuiltin:
    case kContextAddLuaWidgetFilterInstalled:
    case kContextAddLuaWidgetFilterDevelopment:
    case kContextAddLuaWidgetPreviousPage:
    case kContextAddLuaWidgetNextPage:
        return true;
    default:
        return false;
    }
}

bool ReplaceAddWidgetSubmenu(
    std::vector<snowdesktop::modern_menu::Item>& rootItems,
    std::vector<snowdesktop::modern_menu::Item> replacement)
{
    for (auto& item : rootItems)
    {
        const bool isAddWidgetSubmenu = std::ranges::any_of(
            item.children, [](const auto& child) {
                return child.command == kContextAddCollectionWidget;
            });
        if (isAddWidgetSubmenu)
        {
            item.children = std::move(replacement);
            return true;
        }
    }
    return false;
}

} // namespace

void DesktopApp::ShowGridAdjustmentMenu(POINT screenPoint, UINT initialCommand)
{
    PrepareMenuIconsForPoint(screenPoint);

    struct MonitorSizeRange
    {
        const wchar_t* label;
        float representativeInches;
    };
    const MonitorSizeRange kMonitorSizeRanges[] = {
        { _LW("app.menu.monitor_1316"), 15.0f },
        { _LW("app.menu.monitor_1721"), 19.0f },
        { _LW("app.menu.monitor_2225"), 24.0f },
        { _LW("app.menu.monitor_2630"), 27.0f },
        { _LW("app.menu.monitor_31plus"), 34.0f },
    };

    auto isRecommendedCommand = [](UINT value) {
        return (value >= kContextGridRecommended169First &&
                value <= kContextGridRecommended169Last) ||
            (value >= kContextGridRecommended1610First &&
                value <= kContextGridRecommended1610Last);
    };
    auto recommendedDimensions = [&](UINT value) {
        const bool isSixteenTen = value >= kContextGridRecommended1610First;
        const UINT first = isSixteenTen
            ? kContextGridRecommended1610First
            : kContextGridRecommended169First;
        const size_t rangeIndex = static_cast<size_t>(value - first);
        return CalculateRecommendedGridDimensions(
            16, isSixteenTen ? 10 : 9,
            kMonitorSizeRanges[rangeIndex].representativeInches);
    };

    auto applyAdjustment = [&](UINT command) {
        if (isRecommendedCommand(command))
        {
            const GridSpan recommended = recommendedDimensions(command);
            SetGridDimensions(recommended.columns, recommended.rows);
            return true;
        }
        switch (command)
        {
        case kContextGridAddRow: AdjustGridRows(1); return true;
        case kContextGridRemoveRow: AdjustGridRows(-1); return true;
        case kContextGridAddColumn: AdjustGridColumns(1); return true;
        case kContextGridRemoveColumn: AdjustGridColumns(-1); return true;
        default: return false;
        }
    };

    auto buildItems = [&]() {
        using snowdesktop::modern_menu::Item;
        std::vector<Item> items;
        POINT clientPoint = lastContextMenuScreenPoint_;
        ScreenToClient(hwnd_, &clientPoint);
        const GridPage* page = GridPageFromPoint(clientPoint);
        std::wstring status = _LW("app.menu.grid_current_label");
        status += L"\t";
        status += std::to_wstring(page ? page->columns : 0);
        status += L" × ";
        status += std::to_wstring(page ? page->rows : 0);
        items.push_back({ 0, std::move(status), L"", false });
        items.push_back({ 0, L"", L"", false, false, true });
        items.push_back({ kContextGridAddRow,
            _LW("app.menu.add_row"), L"\uF109", true });
        items.push_back({ kContextGridRemoveRow,
            _LW("app.menu.remove_row"), L"\uEBD0", true });
        items.push_back({ kContextGridAddColumn,
            _LW("app.menu.add_col"), L"\uF109", true });
        items.push_back({ kContextGridRemoveColumn,
            _LW("app.menu.remove_col"), L"\uEBD0", true });
        items.push_back({ 0, L"", L"", false, false, true });

        auto appendRecommendedItem = [&](int aspectHeight,
            UINT firstCommand, const wchar_t* label) {
            Item parent;
            parent.label = label;
            parent.glyph = L"\uF462";
            for (size_t i = 0; i < std::size(kMonitorSizeRanges); ++i)
            {
                const GridSpan recommended = CalculateRecommendedGridDimensions(
                    16, aspectHeight, kMonitorSizeRanges[i].representativeInches);
                std::wstring childLabel = kMonitorSizeRanges[i].label;
                childLabel += L"\t";
                childLabel += std::to_wstring(recommended.columns);
                childLabel += L" × ";
                childLabel += std::to_wstring(recommended.rows);
                Item child;
                child.command = firstCommand + static_cast<UINT>(i);
                child.label = std::move(childLabel);
                child.glyph = L"\uF462";
                child.checked = page &&
                    page->columns == recommended.columns &&
                    page->rows == recommended.rows;
                parent.children.push_back(std::move(child));
            }
            items.push_back(std::move(parent));
        };
        appendRecommendedItem(9, kContextGridRecommended169First,
            _LW("app.menu.recommend_169"));
        appendRecommendedItem(10, kContextGridRecommended1610First,
            _LW("app.menu.recommend_1610"));
        items.push_back({ 0, L"", L"", false, false, true });
        items.push_back({ kContextGridAdjustmentDone,
            _LW("app.menu.end_adjust"), L"\uF294", true });
        return items;
    };

    if (initialCommand != 0)
        applyAdjustment(initialCommand);

    std::vector<snowdesktop::modern_menu::Item> items = buildItems();
    snowdesktop::modern_menu::Options options;
    options.owner = hwnd_;
    options.anchor = screenPoint;
    options.dpi = menuIconDpi_;
    options.lightTheme = menuLightTheme_;
    options.appearance = static_cast<
        snowdesktop::modern_menu::Appearance>(menuAppearanceStyle_);
    ConfigureModernMenuEventPump(options);
    options.onCommand = [&](UINT command, auto& currentItems) {
        if (!applyAdjustment(command))
            return false;
        currentItems = buildItems();
        return true;
    };

    SetForegroundWindow(hwnd_);
    snowdesktop::modern_menu::Show(items, options);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();
    RestoreInteractionInputFocus();
}

snowdesktop::component_preview::Bitmap
DesktopApp::RenderWidgetMenuPreview(
    const std::shared_ptr<snowdesktop::WidgetPreviewScene>& scene,
    const std::wstring& rootWidgetId,
    const std::unordered_map<std::string, std::string>& previewStorage,
    int width, int height, UINT dpi,
    const snowdesktop::component_preview::StagePlacement& stage,
    bool hovered)
{
    using snowdesktop::component_preview::Bitmap;
    Bitmap result;
    if (!scene || !d2dDevice_ || !dwriteFactory_ ||
        width <= 0 || height <= 0)
        return result;
    DesktopWidget* data = scene->FindWidget(rootWidgetId);
    if (!data) return result;
    result.width = width;
    result.height = height;

    ComPtr<ID2D1DeviceContext> context;
    if (FAILED(d2dDevice_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)) || !context)
    {
        result = {};
        return result;
    }
    const D2D1_BITMAP_PROPERTIES1 targetProperties =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> target;
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(result.width),
        static_cast<UINT32>(result.height));
    if (FAILED(context->CreateBitmap(size, nullptr, 0,
            &targetProperties, &target)) || !target)
    {
        result = {};
        return result;
    }
    context->SetTarget(target.Get());
    const float renderDpi = static_cast<float>(
        dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
    context->SetDpi(renderDpi, renderDpi);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    data->selected = false;
    const RECT desktopFrame = GetStandaloneWidgetFrameRect(*data);
    if (IsRectEmptyRect(desktopFrame) ||
        desktopFrame.right - desktopFrame.left != width ||
        desktopFrame.bottom - desktopFrame.top != height)
        return {};
    std::unique_ptr<WidgetEngine> previewEngine;
    if (data->type == DesktopWidgetType::LuaScript)
    {
        previewEngine = std::make_unique<WidgetEngine>();
        if (!previewEngine->InitPreview(context.Get(), dwriteFactory_.Get()) ||
            !previewEngine->EnsureWidgetPreviewLoaded(
                data->id, data->packageId, previewStorage))
            return {};
    }
    std::unique_ptr<Widget> widget = CreateWidget(data, this);
    if (!widget) return {};

    const PersonalizationSettings globalAppearance = CurrentPersonalization();
    PersonalizationSettings stageAppearance = globalAppearance;
    bool customStyle = false;
    if (previewEngine)
    {
        customStyle = previewEngine->HasCustomStyle(data->id);
        if (customStyle)
        {
            const std::string follow = previewEngine->RuntimeGetStorageValue(
                data->id, "followPersonalization");
            if (follow == "1" || follow == "true")
                customStyle = false;
        }
        if (customStyle)
        {
            stageAppearance = PersonalizationSettings::DarkPreset();
            float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f, alpha = 0.0f;
            float borderR = 0.0f, borderG = 0.0f, borderB = 0.0f;
            float borderAlpha = 0.0f;
            float borderWidth = 1.0f;
            bool edgeHighlightEnabled = false;
            float edgeHighlightWidth = kDefaultEdgeHighlightWidth;
            float edgeHighlightStrength =
                kDefaultEdgeHighlightStrength;
            float gradientEndA = stageAppearance.gradientEndA;
            bool glass = false, acrylic = false;
            if (previewEngine->ReadCustomColors(data->id,
                    bgR, bgG, bgB, alpha,
                    borderR, borderG, borderB, borderAlpha,
                    borderWidth, edgeHighlightEnabled,
                    edgeHighlightWidth, edgeHighlightStrength,
                    gradientEndA, glass, acrylic))
            {
                stageAppearance.widgetBorderWidth = borderWidth;
                stageAppearance.widgetEdgeHighlightEnabled =
                    edgeHighlightEnabled;
                stageAppearance.widgetEdgeHighlightWidth =
                    edgeHighlightWidth;
                stageAppearance.widgetEdgeHighlightStrength =
                    edgeHighlightStrength;
                stageAppearance.glassEnabled = glass;
                stageAppearance.acrylicEnabled = glass && acrylic;
            }
            stageAppearance.glassBlurRadius =
                globalAppearance.glassBlurRadius;
            stageAppearance.contentTheme = globalAppearance.contentTheme;
            const std::string storedTheme =
                previewEngine->RuntimeGetStorageValue(
                    data->id, "__contentTheme");
            if (storedTheme == "0" || storedTheme == "1")
                stageAppearance.contentTheme = storedTheme[0] - '0';
        }
    }
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    const RECT stageBounds{ 0, 0, width, height };
    if (!stage.transparent)
    {
        snowdesktop::widget_preview::DrawStage(context.Get(), stageBounds,
            { stage.lightTheme, stageAppearance.glassEnabled,
                stageAppearance.glassBlurRadius,
                static_cast<float>(ScaleWidgetCu(
                    globalAppearance.cornerRadius, data->cellScale)) },
            { stage.canvasWidth, stage.canvasHeight,
                stage.offsetX, stage.offsetY }, stage.wallpaper);
    }
    context->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(-desktopFrame.left),
        static_cast<float>(-desktopFrame.top)));
    snowdesktop::WidgetRenderOptions options;
    options.previewScene = scene.get();
    options.luaEngine = previewEngine.get();
    options.pointer = hovered
        ? POINT{ (desktopFrame.left + desktopFrame.right) / 2,
            (desktopFrame.top + desktopFrame.bottom) / 2 }
        : POINT{ -32000, -32000 };
    options.frame = desktopFrame;
    options.dpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    options.interactive = true;
    options.registerBackdrop = false;
    widget->DrawPreview(context.Get(), options.frame, options);
    const HRESULT drawResult = context->EndDraw();
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->SetTarget(nullptr);
    for (const auto& sample : scene->Items())
    {
        if (DesktopItem* item = scene->FindDesktopItem(sample.key))
            EraseD2DIconCacheForBitmap(item->iconBitmap);
        if (FolderEntry* entry = scene->FindFolderEntry(sample.key))
            EraseD2DIconCacheForBitmap(entry->iconBitmap);
    }
    if (FAILED(drawResult))
    {
        result = {};
        return result;
    }

    const D2D1_BITMAP_PROPERTIES1 readProperties =
        D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ |
                D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> readback;
    if (FAILED(context->CreateBitmap(size, nullptr, 0,
            &readProperties, &readback)) || !readback ||
        FAILED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr)))
    {
        result = {};
        return result;
    }
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
    {
        result = {};
        return result;
    }
    result.pixels.resize(static_cast<size_t>(result.width) * result.height);
    for (int y = 0; y < result.height; ++y)
    {
        std::memcpy(result.pixels.data() +
                static_cast<size_t>(y) * result.width,
            mapped.bits + static_cast<size_t>(y) * mapped.pitch,
            static_cast<size_t>(result.width) * sizeof(std::uint32_t));
    }
    readback->Unmap();
    return result;
}

snowdesktop::component_preview::Model
DesktopApp::BuildAddWidgetMenuPreview(
    UINT command, const std::wstring& packageId)
{
    using snowdesktop::component_preview::Card;
    using snowdesktop::component_preview::Model;
    using snowdesktop::component_preview::ApplyKind;
    using snowdesktop::component_preview::Option;
    using snowdesktop::component_preview::OptionSetting;
    Model model;
    model.resizeHint = _LW("app.widget_preview.resize_hint");
    model.applyLabel = _LW("app.widget_preview.add_to_desktop");

    POINT previewPoint = lastContextMenuScreenPoint_;
    ScreenToClient(hwnd_, &previewPoint);
    const GridPage* previewPage = GridPageFromPoint(previewPoint);
    GridCell previewCell = CellFromPoint(previewPoint);
    if (!previewPage)
    {
        previewPage = GetFirstPageGridPage();
        if (previewPage)
        {
            previewCell.pageId = previewPage->id;
            previewCell.column = 0;
            previewCell.row = 0;
        }
    }

    const PersonalizationSettings appearance = CurrentPersonalization();
    const std::wstring appearanceKey =
        std::to_wstring(menuIconDpi_) + L":" +
        std::to_wstring(menuLightTheme_) + L":" +
        std::to_wstring(appearance.widgetBgR) + L":" +
        std::to_wstring(appearance.widgetBgG) + L":" +
        std::to_wstring(appearance.widgetBgB) + L":" +
        std::to_wstring(appearance.widgetAlpha) + L":" +
        std::to_wstring(appearance.widgetBorderR) + L":" +
        std::to_wstring(appearance.widgetBorderG) + L":" +
        std::to_wstring(appearance.widgetBorderB) + L":" +
        std::to_wstring(appearance.widgetBorderAlpha) + L":" +
        std::to_wstring(appearance.widgetBorderWidth) + L":" +
        std::to_wstring(appearance.widgetEdgeHighlightEnabled) + L":" +
        std::to_wstring(appearance.widgetEdgeHighlightWidth) + L":" +
        std::to_wstring(appearance.widgetEdgeHighlightStrength) + L":" +
        std::to_wstring(appearance.gradientEndA) + L":" +
        std::to_wstring(appearance.cornerRadius) + L":" +
        std::to_wstring(appearance.barHeight) + L":" +
        std::to_wstring(appearance.categorizedTabHeight) + L":" +
        std::to_wstring(appearance.backgroundPreset) + L":" +
        std::to_wstring(appearance.glassEnabled) + L":" +
        std::to_wstring(appearance.acrylicEnabled) + L":" +
        std::to_wstring(appearance.glassBlurRadius) + L":" +
        std::to_wstring(appearance.contentTheme);
    auto makeScene = [&](bool applications = false) {
        auto scene = std::make_shared<snowdesktop::WidgetPreviewScene>();
        struct PreviewItemVisual
        {
            const wchar_t* glyph;
            std::uint32_t backgroundRgb;
        };
        static constexpr std::array<PreviewItemVisual, 12>
            applicationVisuals{
                PreviewItemVisual{ L"\uE855", 0x7A6B9B }, // music
                PreviewItemVisual{ L"\uF4F9", 0x4F7F7C }, // location
                PreviewItemVisual{ L"\uF489", 0x9A6B69 }, // image
                PreviewItemVisual{ L"\uF507", 0x5A74A8 }, // mail
                PreviewItemVisual{ L"\uE24F", 0x7A6B9B }, // calendar
                PreviewItemVisual{ L"\uF86F", 0x607B91 }, // weather
                PreviewItemVisual{ L"\uF56C", 0x9A784F }, // note
                PreviewItemVisual{ L"\uF472", 0x7A6B9B }, // headphones
                PreviewItemVisual{ L"\uE179", 0x5A74A8 }, // book
                PreviewItemVisual{ L"\uE233", 0x697986 }, // calculator
                PreviewItemVisual{ L"\uF255", 0x9A6B69 }, // camera
                PreviewItemVisual{ L"\uE6B2", 0x4F7F7C }, // globe
            };
        static constexpr std::array<PreviewItemVisual, 12> fileVisuals{
            PreviewItemVisual{ L"\uF379", 0x5A74A8 }, // document
            PreviewItemVisual{ L"\uF489", 0x9A6B69 }, // image
            PreviewItemVisual{ L"\uF3AD", 0x98645E }, // PDF
            PreviewItemVisual{ L"\uE644", 0x9A784F }, // folder
            PreviewItemVisual{ L"\uE54C", 0x5F7D6E }, // table
            PreviewItemVisual{ L"\uF489", 0x9A6B69 }, // image
            PreviewItemVisual{ L"\uE558", 0x697986 }, // text document
            PreviewItemVisual{ L"\uE644", 0x9A784F }, // folder
            PreviewItemVisual{ L"\uF56C", 0x697986 }, // note
            PreviewItemVisual{ L"\uF489", 0x9A6B69 }, // image
            PreviewItemVisual{ L"\uF444", 0x9A784F }, // food
            PreviewItemVisual{ L"\uF2C3", 0x4F7F7C }, // city
        };
        const std::wstring fileTitles[] = {
            _LW("app.widget_preview.item_travel_plans"),
            _LW("app.widget_preview.item_seaside_sunset"),
            _LW("app.widget_preview.item_reading_list"),
            _LW("app.widget_preview.item_weekend_photos"),
            _LW("app.widget_preview.item_home_budget"),
            _LW("app.widget_preview.item_favorite_cafe"),
            _LW("app.widget_preview.item_birthday_list"),
            _LW("app.widget_preview.item_travel_photos"),
            _LW("app.widget_preview.item_fitness_notes"),
            _LW("app.widget_preview.item_mountain_view"),
            _LW("app.widget_preview.item_restaurants"),
            _LW("app.widget_preview.item_city_lights"),
        };
        const std::wstring applicationTitles[] = {
            _LW("app.widget_preview.app_music"),
            _LW("app.widget_preview.app_maps"),
            _LW("app.widget_preview.app_photos"),
            _LW("app.widget_preview.app_mail"),
            _LW("app.widget_preview.app_calendar"),
            _LW("app.widget_preview.app_weather"),
            _LW("app.widget_preview.app_notes"),
            _LW("app.widget_preview.app_podcasts"),
            _LW("app.widget_preview.app_books"),
            _LW("app.widget_preview.app_calculator"),
            _LW("app.widget_preview.app_camera"),
            _LW("app.widget_preview.app_browser"),
        };
        for (int i = 0; i < 12; ++i)
        {
            const PreviewItemVisual& visual = applications
                ? applicationVisuals[static_cast<size_t>(i)]
                : fileVisuals[static_cast<size_t>(i)];
            snowdesktop::WidgetPreviewItem item;
            item.key = L"__preview_item_" + std::to_wstring(i);
            item.glyph = visual.glyph;
            item.title = applications
                ? applicationTitles[i] : fileTitles[i];
            item.categoryId = applications
                ? L"others"
                : (i % 2 == 0 ? L"documents" : L"images");
            item.dateGroup = i < 3 ? L"today" : L"earlier";
            item.directory = !applications && (i == 3 || i == 7);
            item.backgroundRgb = visual.backgroundRgb;
            scene->AddItem(std::move(item));
        }
        const int bitmapSize = previewPage
            ? std::max(64, GetGridPageItemIconSize(*previewPage) * 2)
            : 128;
        scene->PreparePlaceholderModels(
            bitmapSize, IsLightContentTheme());
        return scene;
    };

    auto fillItemKeys = [](DesktopWidget& widget,
        const snowdesktop::WidgetPreviewScene& scene, size_t count) {
        for (size_t i = 0;
             i < std::min(count, scene.Items().size()); ++i)
            widget.itemKeys.push_back(scene.Items()[i].key);
    };

    auto addCard = [&](const char* titleKey, const char* descriptionKey,
                       const std::wstring& modeKey,
                       const std::shared_ptr<
                           snowdesktop::WidgetPreviewScene>& scene,
                       DesktopWidget root,
                       std::unordered_map<std::string, std::string>
                           storage = {}) {
        root.id = L"__component_preview_" + modeKey;
        root.selected = false;
        root.showTitle = true;
        if (previewPage)
        {
            root.gridSpan.columns = std::clamp(root.gridSpan.columns,
                1, std::max(1, previewPage->columns));
            root.gridSpan.rows = std::clamp(root.gridSpan.rows,
                1, std::max(1, previewPage->rows));
            root.gridCell = previewCell;
            root.gridCell.pageId = previewPage->id;
            root.gridCell.column = std::clamp(root.gridCell.column, 0,
                std::max(0, previewPage->columns - root.gridSpan.columns));
            root.gridCell.row = std::clamp(root.gridCell.row, 0,
                std::max(0, previewPage->rows - root.gridSpan.rows));
            root.cellScale = GetGridPageCuScale(*previewPage);
            root.bounds = GetGridRect(
                gridPages_, root.gridCell, root.gridSpan);
        }
        else
        {
            root.gridCell = {};
            root.cellScale = 1.0f;
            root.bounds = { 0, 0,
                std::max(1, root.gridSpan.columns) * kCellWidth,
                std::max(1, root.gridSpan.rows) * kMinCellHeight };
        }
        const RECT desktopFrame = GetStandaloneWidgetFrameRect(root);
        const std::wstring rootId = root.id;
        scene->AddWidget(std::move(root));
        Card card;
        card.title = _LW(titleKey);
        card.description = descriptionKey && *descriptionKey
            ? _LW(descriptionKey) : L"";
        const DesktopWidget* rootData = scene->FindWidget(rootId);
        card.columns = rootData
            ? std::max(1, rootData->gridSpan.columns) : 1;
        card.rows = rootData
            ? std::max(1, rootData->gridSpan.rows) : 1;
        card.previewWidth = std::max<LONG>(
            1, desktopFrame.right - desktopFrame.left);
        card.previewHeight = std::max<LONG>(
            1, desktopFrame.bottom - desktopFrame.top);
        card.lightStage = false;
        card.useDesktopWallpaperStage = true;
        card.sizeLabel = _LFW("app.widget_preview.size",
            std::to_wstring(card.columns), std::to_wstring(card.rows));
        card.cacheKey = modeKey + L":" + appearanceKey + L":" +
            (previewPage ? previewPage->id : L"fallback") + L":" +
            std::to_wstring(card.previewWidth) + L"x" +
            std::to_wstring(card.previewHeight) + L":" +
            std::to_wstring(rootData ? rootData->cellScale : 1.0f);
        card.render = [this, scene, rootId, storage](
                int width, int height, UINT dpi,
                const snowdesktop::component_preview::StagePlacement& stage,
                const snowdesktop::component_preview::ApplySettings& settings,
                bool hovered) {
            if (DesktopWidget* preview = scene->FindWidget(rootId))
            {
                preview->listMode = settings.listMode;
                preview->scrollContainerMode = settings.scrollContainerMode;
                preview->largeFolderTitleless =
                    settings.largeFolderTitleless;
                preview->dateHeaders = settings.dateHeaders;
                preview->showFileCategories = settings.showFileCategories;
                preview->showSearchBox = settings.showSearchBox;
            }
            return RenderWidgetMenuPreview(
                scene, rootId, storage, width, height, dpi, stage, hovered);
        };
        if (rootData)
        {
            switch (rootData->type)
            {
            case DesktopWidgetType::Collection:
                card.applySettings.kind = ApplyKind::Collection; break;
            case DesktopWidgetType::CollectionGroup:
                card.applySettings.kind = ApplyKind::CollectionGroup; break;
            case DesktopWidgetType::FileGroup:
                card.applySettings.kind = ApplyKind::FileGroup; break;
            case DesktopWidgetType::FileCategories:
                card.applySettings.kind = ApplyKind::FileCategories; break;
            case DesktopWidgetType::FolderMapping:
                card.applySettings.kind = ApplyKind::FolderMapping; break;
            case DesktopWidgetType::LuaScript:
                card.applySettings.kind = ApplyKind::LuaScript; break;
            default:
                break;
            }
            card.applySettings.packageId = rootData->packageId;
            card.applySettings.columns = card.columns;
            card.applySettings.rows = card.rows;
            card.applySettings.listMode = rootData->listMode;
            card.applySettings.scrollContainerMode =
                rootData->scrollContainerMode;
            card.applySettings.largeFolderTitleless =
                rootData->largeFolderTitleless;
            card.applySettings.dateHeaders = rootData->dateHeaders;
            card.applySettings.showFileCategories =
                rootData->showFileCategories;
            card.applySettings.showSearchBox = rootData->showSearchBox;
        }
        model.cards.push_back(std::move(card));
    };
    auto option = [&](OptionSetting setting, const char* labelKey,
                      const char* offKey, const char* onKey) {
        Option value;
        value.setting = setting;
        value.label = _LW(labelKey);
        value.offLabel = _LW(offKey);
        value.onLabel = _LW(onKey);
        return value;
    };
    auto layoutOption = [&]() {
        return option(OptionSetting::ListMode,
            "app.widget_preview.setting_layout",
            "app.widget_preview.mode_icons",
            "app.widget_preview.mode_list");
    };
    auto collectionModeOption = [&]() {
        return option(OptionSetting::ScrollContainerMode,
            "app.widget_preview.setting_collection_mode",
            "app.widget_preview.mode_large_folder",
            "app.widget_preview.mode_scroll_container");
    };
    auto collectionTitlelessOption = [&]() {
        return option(OptionSetting::LargeFolderTitleless,
            "app.interact.large_folder_titleless",
            "app.interact.off",
            "app.interact.on");
    };

    switch (command)
    {
    case kContextAddCollectionWidget:
    {
        model.title = _LW("app.menu.collection");
        model.introduction = _LW("app.widget_preview.collection_intro");
        auto compactScene = makeScene(true);
        DesktopWidget compact;
        compact.type = DesktopWidgetType::Collection;
        compact.title = model.title;
        compact.gridSpan = { 1, 1 };
        fillItemKeys(compact, *compactScene, 5);
        addCard("app.widget_preview.collection_compact",
            "app.widget_preview.collection_compact_hint", L"collection:compact",
            compactScene, std::move(compact));

        auto scrollGridScene = makeScene(true);
        DesktopWidget scrollGrid;
        scrollGrid.type = DesktopWidgetType::Collection;
        scrollGrid.title = model.title;
        scrollGrid.gridSpan = { 3, 3 };
        fillItemKeys(scrollGrid, *scrollGridScene, 12);
        addCard("app.widget_preview.collection_scroll_grid",
            "app.widget_preview.collection_scroll_grid_hint",
            L"collection:scroll-grid", scrollGridScene,
            std::move(scrollGrid));
        model.cards.back().options = { collectionModeOption(),
            collectionTitlelessOption(), layoutOption() };
        model.initialCard = 1;
        return model;
    }
    case kContextAddFileCategoryWidget:
    {
        model.title = _LW("app.menu.file_categories");
        model.introduction = _LW("app.widget_preview.file_categories_intro");
        auto gridScene = makeScene();
        DesktopWidget grid;
        grid.type = DesktopWidgetType::FileCategories;
        grid.title = model.title;
        grid.gridSpan = { 3, 3 };
        grid.activeCategoryId = L"documents";
        grid.showFileCategories = true;
        grid.showSearchBox = true;
        fillItemKeys(grid, *gridScene, 8);
        addCard("app.widget_preview.category_grid",
            "app.widget_preview.category_grid_hint", L"categories:grid",
            gridScene, std::move(grid));
        model.cards.back().options = {
            layoutOption(),
            option(OptionSetting::DateHeaders,
                "app.widget_preview.setting_date_headers",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
            option(OptionSetting::ShowFileCategories,
                "app.widget_preview.setting_category_tabs",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
            option(OptionSetting::ShowSearchBox,
                "app.widget_preview.setting_search_box",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
        };
        return model;
    }
    case kContextAddFolderMappingWidget:
    {
        model.title = _LW("app.menu.folder_mapping");
        model.introduction = _LW("app.widget_preview.folder_mapping_intro");
        auto addEntries = [](DesktopWidget& widget,
            const snowdesktop::WidgetPreviewScene& scene) {
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            for (size_t i = 0; i < scene.Items().size(); ++i)
            {
                FolderEntry entry;
                entry.name = scene.Items()[i].title;
                entry.fullPath = scene.Items()[i].key;
                entry.isDirectory = scene.Items()[i].directory;
                entry.lastWriteTime = i < 3 ? now : FILETIME{};
                widget.folderEntries.push_back(std::move(entry));
            }
        };
        auto gridScene = makeScene();
        DesktopWidget grid;
        grid.type = DesktopWidgetType::FolderMapping;
        grid.title = model.title;
        grid.gridSpan = { 3, 3 };
        addEntries(grid, *gridScene);
        addCard("app.widget_preview.folder_grid",
            "app.widget_preview.folder_grid_hint", L"folder:grid",
            gridScene, std::move(grid));
        model.cards.back().options = {
            layoutOption(),
            option(OptionSetting::DateHeaders,
                "app.widget_preview.setting_date_headers",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
            option(OptionSetting::ShowFileCategories,
                "app.widget_preview.setting_category_tabs",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
            option(OptionSetting::ShowSearchBox,
                "app.widget_preview.setting_search_box",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
        };
        return model;
    }
    case kContextAddCollectionGroupWidget:
    {
        model.title = _LW("app.menu.collection_group");
        model.introduction = _LW("app.widget_preview.collection_group_intro");
        auto addGroupCard = [&]() {
            auto scene = makeScene(true);
            DesktopWidget first;
            first.id = L"__preview_collection_a";
            first.type = DesktopWidgetType::Collection;
            first.title = _LW("app.widget_preview.collection_a");
            fillItemKeys(first, *scene, 5);
            DesktopWidget second;
            second.id = L"__preview_collection_b";
            second.type = DesktopWidgetType::Collection;
            second.title = _LW("app.widget_preview.collection_b");
            for (size_t i = 3; i < scene->Items().size(); ++i)
                second.itemKeys.push_back(scene->Items()[i].key);
            scene->AddWidget(std::move(first));
            scene->AddWidget(std::move(second));
            DesktopWidget group;
            group.type = DesktopWidgetType::CollectionGroup;
            group.title = model.title;
            group.gridSpan = { 3, 3 };
            group.childWidgetIds = {
                L"__preview_collection_a", L"__preview_collection_b" };
            group.activeCategoryId = L"__preview_collection_a";
            addCard("app.widget_preview.group_grid",
                "app.widget_preview.group_grid_hint",
                L"collection-group:grid",
                scene, std::move(group));
        };
        addGroupCard();
        model.cards.back().options = {
            layoutOption(),
            option(OptionSetting::ShowSearchBox,
                "app.widget_preview.setting_search_box",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
        };
        return model;
    }
    case kContextAddFileGroupWidget:
    {
        model.title = _LW("app.menu.file_group");
        model.introduction = _LW("app.widget_preview.file_group_intro");
        auto addFileGroupCard = [&]() {
            auto scene = makeScene();
            DesktopWidget categories;
            categories.id = L"__preview_file_categories";
            categories.type = DesktopWidgetType::FileCategories;
            categories.title = _LW("app.widget_preview.desktop");
            categories.activeCategoryId = L"documents";
            fillItemKeys(categories, *scene, 8);
            DesktopWidget folder;
            folder.id = L"__preview_folder_source";
            folder.type = DesktopWidgetType::FolderMapping;
            folder.title = _LW("app.widget_preview.folder");
            for (const auto& item : scene->Items())
            {
                FolderEntry entry;
                entry.name = item.title;
                entry.fullPath = item.key;
                entry.isDirectory = item.directory;
                folder.folderEntries.push_back(std::move(entry));
            }
            scene->AddWidget(std::move(categories));
            scene->AddWidget(std::move(folder));
            DesktopWidget group;
            group.type = DesktopWidgetType::FileGroup;
            group.title = model.title;
            group.gridSpan = { 3, 3 };
            group.showFileCategories = true;
            group.showSearchBox = false;
            group.childWidgetIds = {
                L"__preview_file_categories", L"__preview_folder_source" };
            group.activeCategoryId = L"__preview_file_categories";
            addCard("app.widget_preview.source_grid",
                "app.widget_preview.source_grid_hint",
                L"file-group:grid",
                scene, std::move(group));
        };
        addFileGroupCard();
        model.cards.back().options = {
            layoutOption(),
            option(OptionSetting::DateHeaders,
                "app.widget_preview.setting_date_headers",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
            option(OptionSetting::ShowFileCategories,
                "app.widget_preview.setting_category_tabs",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
            option(OptionSetting::ShowSearchBox,
                "app.widget_preview.setting_search_box",
                "app.widget_preview.setting_hide",
                "app.widget_preview.setting_show"),
        };
        return model;
    }
    default:
        break;
    }

    if (packageId.empty()) return model;
    const LuaWidgetManifest manifest =
        WidgetEngine::GetWidgetManifest(packageId);
    model.title = Utf8ToWide(manifest.name);
    if (model.title.empty()) model.title = packageId;
    model.introduction = Utf8ToWide(
        manifest.previewIntroduction.empty()
            ? manifest.description : manifest.previewIntroduction);
    std::vector<snowdesktop::widget::PreviewVariant> variants =
        manifest.previewVariants;
    if (variants.empty())
    {
        snowdesktop::widget::PreviewVariant variant;
        variant.id = "default";
        variant.title = WideToUtf8(
            _LW("app.widget_preview.default_mode"));
        variant.columns = manifest.defaultColumns;
        variant.rows = manifest.defaultRows;
        variants.push_back(std::move(variant));
    }
    for (size_t i = 0; i < variants.size() && i < 4; ++i)
    {
        auto& variant = variants[i];
        auto scene = makeScene();
        DesktopWidget root;
        root.type = DesktopWidgetType::LuaScript;
        root.packageId = packageId;
        root.title = model.title;
        root.gridSpan = {
            std::clamp(variant.columns, 1, 8),
            std::clamp(variant.rows, 1, 8) };
        const std::wstring title = Utf8ToWide(variant.title).empty()
            ? _LW("app.widget_preview.default_mode")
            : Utf8ToWide(variant.title);
        const std::wstring description =
            Utf8ToWide(variant.description);
        const std::wstring modeKey = L"lua:" + packageId + L":" +
            Utf8ToWide(variant.id.empty()
                ? std::to_string(i) : variant.id);
        addCard("app.widget_preview.default_mode", nullptr, modeKey,
            scene, std::move(root), variant.storage);
        model.cards.back().title = title;
        model.cards.back().description = description;
    }
    return model;
}

/**
 * @brief 打开保留原结构、支持 Lua 分页与显式预览操作的添加组件菜单。
 */
void DesktopApp::ShowAddWidgetMenu(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    PrepareMenuIconsForPoint(screenPoint);
    snowdesktop::component_preview::Window previewWindow;
    previewWindow.PrefetchDesktopWallpaperBackdrop(hwnd_, screenPoint);
    const auto allLuaWidgets = BuildLuaWidgetMenuEntries();
    std::wstring luaSearch;
    LuaWidgetMenuFilter luaFilter = LuaWidgetMenuFilter::All;
    auto luaWidgets = FilterLuaWidgetMenuEntries(
        allLuaWidgets, luaSearch, luaFilter);
    size_t luaPage = 0;
    auto items = BuildAddWidgetMenuItems(
        allLuaWidgets, luaWidgets, luaPage, luaSearch, luaFilter);
    UINT previewCacheCommand = 0;
    std::wstring previewCachePackage;
    snowdesktop::component_preview::Model previewCache;
    std::optional<snowdesktop::component_preview::ApplySettings>
        previewApply;
    auto applyPreview = [&](const auto& settings) {
        previewApply = settings;
        snowdesktop::modern_menu::DismissActive();
    };
    snowdesktop::modern_menu::HoverInfo previewAnchor;
    auto showPreview = [&](UINT previewCommand) {
        const UINT addCommand =
            AddCommandForPreviewCommand(previewCommand);
        if (addCommand == 0) return false;
        std::wstring packageId;
        if (addCommand >= kContextAddLuaWidgetFirst &&
            addCommand < kContextAddLuaWidgetFirst +
                static_cast<UINT>(kLuaWidgetMenuPageSize))
        {
            const size_t index = luaPage * kLuaWidgetMenuPageSize +
                static_cast<size_t>(
                    addCommand - kContextAddLuaWidgetFirst);
            if (index >= luaWidgets.size()) return true;
            packageId = luaWidgets[index].packageId;
        }
        if (addCommand != previewCacheCommand ||
            packageId != previewCachePackage)
        {
            previewCacheCommand = addCommand;
            previewCachePackage = packageId;
            previewCache = BuildAddWidgetMenuPreview(
                addCommand, packageId);
        }
        if (!previewCache.Empty())
        {
            previewWindow.Show(previewCache,
                previewAnchor.popupScreenRect, hwnd_, menuIconDpi_,
                menuLightTheme_, applyPreview,
                previewAnchor.itemScreenRect,
                static_cast<snowdesktop::modern_menu::Appearance>(
                    menuAppearanceStyle_));
        }
        return true;
    };

    snowdesktop::modern_menu::Options options;
    options.owner = hwnd_;
    options.anchor = screenPoint;
    options.dpi = menuIconDpi_;
    options.lightTheme = menuLightTheme_;
    options.appearance = static_cast<
        snowdesktop::modern_menu::Appearance>(menuAppearanceStyle_);
    ConfigureModernMenuEventPump(options);
    options.onCommand = [&](UINT command, auto& currentItems) {
        if (showPreview(command)) return true;
        if (const auto selectedFilter =
            LuaWidgetMenuFilterForCommand(command))
        {
            luaFilter = *selectedFilter;
            luaPage = 0;
            luaWidgets = FilterLuaWidgetMenuEntries(
                allLuaWidgets, luaSearch, luaFilter);
            previewWindow.Hide();
            previewCacheCommand = 0;
            previewCachePackage.clear();
            previewCache = {};
            currentItems = BuildAddWidgetMenuItems(allLuaWidgets,
                luaWidgets, luaPage, luaSearch, luaFilter);
            return true;
        }
        const size_t pageCount = LuaWidgetMenuPageCount(luaWidgets.size());
        if (command == kContextAddLuaWidgetPreviousPage && luaPage > 0)
            --luaPage;
        else if (command == kContextAddLuaWidgetNextPage &&
            luaPage + 1 < pageCount)
            ++luaPage;
        else
            return false;
        previewCacheCommand = 0;
        previewCachePackage.clear();
        previewCache = {};
        currentItems = BuildAddWidgetMenuItems(allLuaWidgets,
            luaWidgets, luaPage, luaSearch, luaFilter);
        return true;
    };
    options.onTextChanged = [&](UINT command, const std::wstring& text,
                                auto& currentItems) {
        if (command != kContextAddLuaWidgetSearch)
            return;
        luaSearch = text;
        luaPage = 0;
        luaWidgets = FilterLuaWidgetMenuEntries(
            allLuaWidgets, luaSearch, luaFilter);
        previewWindow.Hide();
        previewCacheCommand = 0;
        previewCachePackage.clear();
        previewCache = {};
        currentItems = BuildAddWidgetMenuItems(allLuaWidgets,
            luaWidgets, luaPage, luaSearch, luaFilter);
    };
    options.onHover = [&](const snowdesktop::modern_menu::HoverInfo& hover) {
        if (hover.command != 0)
            previewAnchor = hover;
    };

    SetForegroundWindow(hwnd_);
    const UINT command = snowdesktop::modern_menu::Show(items, options).command;
    previewWindow.Close();
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (previewApply)
    {
        ApplyWidgetPreviewSettings(screenPoint, *previewApply);
        return;
    }

    if (command >= kContextAddLuaWidgetFirst &&
        command < kContextAddLuaWidgetFirst +
            static_cast<UINT>(kLuaWidgetMenuPageSize))
    {
        const size_t index = luaPage * kLuaWidgetMenuPageSize +
            static_cast<size_t>(command - kContextAddLuaWidgetFirst);
        if (index < luaWidgets.size())
            AddLuaWidgetAt(screenPoint, luaWidgets[index].packageId);
        RestoreInteractionInputFocus();
        return;
    }

    switch (command)
    {
    case kContextAddCollectionWidget:
        AddCollectionWidgetAt(screenPoint); break;
    case kContextAddCollectionGroupWidget:
        AddCollectionGroupWidgetAt(screenPoint); break;
    case kContextAddFileGroupWidget:
        AddFileGroupWidgetAt(screenPoint); break;
    case kContextAddFileCategoryWidget:
        AddFileCategoryWidgetAt(screenPoint); break;
    case kContextAddFolderMappingWidget:
        AddFolderMappingWidgetAt(screenPoint); break;
    default:
        break;
    }
    RestoreInteractionInputFocus();
}

/**
 * @brief 显示桌面背景右键菜单。
 *        在屏幕坐标处弹出菜单，包含粘贴、新建、刷新、排序方式、
 *        行列调整、添加组件、图标间距等选项。菜单项均带图标。
 *        选中 Lua 组件或间距预设时直接处理，其余通过命令 ID 分发。
 * @param screenPoint 菜单弹出的屏幕坐标。
 */
void DesktopApp::ShowBackgroundContextMenu(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    PrepareMenuIconsForPoint(screenPoint);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu,
        HasPasteableFileClipboardData()
            ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextPasteCommand, _LW("app.menu.paste"));
    AppendMenuW(menu, MF_STRING, kContextNewMenu, _LW("app.menu.new"));
    AppendMenuW(menu, MF_STRING, kContextRefreshCommand, _LW("app.menu.refresh"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextMoreCommand, _LW("app.menu.more_options"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU sortMenu = CreatePopupMenu();
    HMENU nameSortMenu = nullptr, typeSortMenu = nullptr;
    if (sortMenu)
    {
        nameSortMenu = CreatePopupMenu();
        if (nameSortMenu)
        {
            AppendMenuW(nameSortMenu, MF_STRING, kContextSortByNameCommand,     _LW("app.menu.sort_asc"));
            AppendMenuW(nameSortMenu, MF_STRING, kContextSortByNameDescCommand,     _LW("app.menu.sort_desc"));
            AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(nameSortMenu), _LW("app.menu.sort_name"));
        }
        typeSortMenu = CreatePopupMenu();
        if (typeSortMenu)
        {
            AppendMenuW(typeSortMenu, MF_STRING, kContextSortByTypeCommand, _LW("app.menu.sort_asc"));
            AppendMenuW(typeSortMenu, MF_STRING, kContextSortByTypeDescCommand, _LW("app.menu.sort_desc"));
            AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(typeSortMenu), _LW("app.menu.sort_type"));
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), _LW("app.menu.sort_by"));
    }

    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* gridPage = GridPageFromPoint(clientPoint);

    HMENU displaySettingsMenu = CreatePopupMenu();
    if (displaySettingsMenu)
    {
        const std::wstring gridLabel = _LFW("app.menu.grid_adjust",
            std::to_wstring(gridPage ? gridPage->columns : 0),
            std::to_wstring(gridPage ? gridPage->rows : 0));
        AppendMenuW(displaySettingsMenu, MF_STRING, kContextGridAdjustmentMenu,
            gridLabel.c_str());

        HMENU spacingMenu = CreatePopupMenu();
        if (spacingMenu)
        {
            const int presets[] = { 50, 70, 80, 90, 100, 110, 120, 130, 150, 200 };
            const int currentSpacingPercent = static_cast<int>(
                std::round(iconSpacingScale_ * 100.0f));
            for (int pct : presets)
            {
                wchar_t label[16]{};
                swprintf_s(label, L"%d%%", pct);
                UINT flags = MF_STRING;
                if (currentSpacingPercent == pct) flags |= MF_CHECKED;
                AppendMenuW(spacingMenu, flags,
                    kContextSpacingPresetFirst + static_cast<UINT>(pct), label);
            }
            AppendMenuW(spacingMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(spacingMenu, MF_STRING, kContextSpacingIncrease, _LW("app.menu.inc_spacing"));
            AppendMenuW(spacingMenu, MF_STRING, kContextSpacingDecrease, _LW("app.menu.dec_spacing"));
            const std::wstring spacingLabel = _LFW("app.menu.layout_spacing_pct",
                std::to_wstring(currentSpacingPercent));
            AppendMenuW(displaySettingsMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(spacingMenu), spacingLabel.c_str());
            SetMenuItemIcon(displaySettingsMenu, reinterpret_cast<UINT_PTR>(spacingMenu), L"");
            SetMenuItemIcon(spacingMenu, kContextSpacingIncrease, L"");
            SetMenuItemIcon(spacingMenu, kContextSpacingDecrease, L"");
        }

        HMENU fontSizeMenu = CreatePopupMenu();
        if (fontSizeMenu)
        {
            const int currentFontSize = static_cast<int>(std::round(itemFontSizeCu_));
            auto addFontSizeItem = [&](UINT id, const wchar_t* label, int size) {
                UINT flags = MF_STRING;
                if (currentFontSize == size) flags |= MF_CHECKED;
                AppendMenuW(fontSizeMenu, flags, id, label);
            };
            addFontSizeItem(kContextFontSizeSmall, _LW("app.menu.font_small"), 12);
            addFontSizeItem(kContextFontSizeMedium, _LW("app.menu.font_medium"), 18);
            addFontSizeItem(kContextFontSizeLarge, _LW("app.menu.font_large"), 24);
            const std::wstring fontSizeLabel = _LFW("app.menu.title_font_size_cu",
                std::to_wstring(currentFontSize));
            AppendMenuW(displaySettingsMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(fontSizeMenu), fontSizeLabel.c_str());
            SetMenuItemIcon(displaySettingsMenu, reinterpret_cast<UINT_PTR>(fontSizeMenu), L"");
        }

        HMENU listFontSizeMenu = CreatePopupMenu();
        if (listFontSizeMenu)
        {
            const int currentFontSize = static_cast<int>(
                std::round(listItemFontSizeCu_));
            auto addFontSizeItem = [&](UINT id, const wchar_t* label, int size) {
                UINT flags = MF_STRING;
                if (currentFontSize == size) flags |= MF_CHECKED;
                AppendMenuW(listFontSizeMenu, flags, id, label);
            };
            addFontSizeItem(kContextListFontSizeSmall,
                _LW("app.menu.font_small"), 12);
            addFontSizeItem(kContextListFontSizeMedium,
                _LW("app.menu.font_medium"), 18);
            addFontSizeItem(kContextListFontSizeLarge,
                _LW("app.menu.font_large"), 24);
            const std::wstring fontSizeLabel = _LFW(
                "app.menu.list_font_size_cu",
                std::to_wstring(currentFontSize));
            AppendMenuW(displaySettingsMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(listFontSizeMenu),
                fontSizeLabel.c_str());
            SetMenuItemIcon(displaySettingsMenu,
                reinterpret_cast<UINT_PTR>(listFontSizeMenu), L"");
        }

        HMENU fontWeightMenu = CreatePopupMenu();
        if (fontWeightMenu)
        {
            auto addWeightItem = [&](UINT id, const wchar_t* label, DWRITE_FONT_WEIGHT weight) {
                UINT flags = MF_STRING;
                if (itemFontWeight_ == weight) flags |= MF_CHECKED;
                AppendMenuW(fontWeightMenu, flags, id, label);
            };
            addWeightItem(kContextFontWeightBold, _LW("app.menu.font_weight_bold_label"), DWRITE_FONT_WEIGHT_BOLD);
            addWeightItem(kContextFontWeightMedium, _LW("app.menu.font_weight_medium_label"), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            addWeightItem(kContextFontWeightFine, _LW("app.menu.font_weight_light_label"), DWRITE_FONT_WEIGHT_NORMAL);
            const wchar_t* weightLabel = _LW("app.menu.font_weight_medium");
            if (itemFontWeight_ == DWRITE_FONT_WEIGHT_BOLD)
                weightLabel = _LW("app.menu.font_weight_bold");
            else if (itemFontWeight_ == DWRITE_FONT_WEIGHT_NORMAL)
                weightLabel = _LW("app.menu.font_weight_light");
            AppendMenuW(displaySettingsMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(fontWeightMenu), weightLabel);
            SetMenuItemIcon(displaySettingsMenu, reinterpret_cast<UINT_PTR>(fontWeightMenu), L"");
        }

        AppendMenuW(displaySettingsMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(displaySettingsMenu, MF_STRING,
            kContextDisplayAppearanceMore,
            _LW("app.menu.more_appearance_options"));
        SetMenuItemIcon(displaySettingsMenu,
            kContextDisplayAppearanceMore, L"");

        AppendMenuW(menu, MF_POPUP,
            reinterpret_cast<UINT_PTR>(displaySettingsMenu), _LW("app.menu.display_settings"));
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(displaySettingsMenu), L"");
        SetMenuItemIcon(displaySettingsMenu, kContextGridAdjustmentMenu, L"");
    }

    const auto allLuaWidgets = BuildLuaWidgetMenuEntries();
    std::wstring luaSearch;
    LuaWidgetMenuFilter luaFilter = LuaWidgetMenuFilter::All;
    auto luaWidgets = FilterLuaWidgetMenuEntries(
        allLuaWidgets, luaSearch, luaFilter);
    size_t luaPage = 0;
    HMENU widgetMenu = CreatePopupMenu();
    if (widgetMenu)
    {
        const auto widgetItems =
            BuildAddWidgetMenuItems(allLuaWidgets, luaWidgets, luaPage,
                luaSearch, luaFilter);
        for (const auto& item : widgetItems)
        {
            if (item.separator)
            {
                AppendMenuW(widgetMenu, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            UINT flags = item.enabled
                ? MF_STRING : MF_STRING | MF_GRAYED;
            if (item.checked) flags |= MF_CHECKED;
            AppendMenuW(widgetMenu, flags, item.command,
                item.label.c_str());
            if (item.textInput)
            {
                SetMenuItemTextInput(
                    widgetMenu, item.command, item.inputText);
            }
            if (item.inlineAction)
            {
                SetMenuItemInlineAction(widgetMenu, item.command,
                    item.inlineGroup, item.compactInlineAction,
                    item.horizontalScrollAction);
            }
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(widgetMenu), _LW("app.menu.add_widget"));
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ── 条件：分页导航 ──
    const GridPage* clickedPage = GridPageFromPoint(clientPoint);
    std::wstring clickedPageId = clickedPage ? clickedPage->id : L"";
    std::wstring clickedMonitorId = clickedPage ? clickedPage->monitorId : L"";
    std::wstring firstPageId = savedPageIds_.empty() ? L"" : savedPageIds_[0];
    bool isFirstPage = !clickedPageId.empty() && clickedPageId == firstPageId;
    int maxOff = MaxPageOffset();
    const size_t monitorCount = gridPages_.size();
    // 单物理屏同时承担首屏和末屏，也应提供末屏的分页导航菜单。
    const bool showPageNavigation = !isFirstPage || monitorCount == 1;

    // ── 首屏/末屏锁定开关（持久化、互斥，仅多屏时显示） ──
    HMENU pinPageMenu = nullptr;
    if (monitorCount >= 2)
    {
        pinPageMenu = CreatePopupMenu();
        if (pinPageMenu)
        {
            UINT fFlags = MF_STRING;
            if (!firstPageMonitorId_.empty() && firstPageMonitorId_ == clickedMonitorId)
                fFlags |= MF_CHECKED;
            AppendMenuW(pinPageMenu, fFlags, kContextPinFirstPage, _LW("app.menu.page_pin_first"));

            UINT lFlags = MF_STRING;
            if (!lastPageMonitorId_.empty() && lastPageMonitorId_ == clickedMonitorId)
                lFlags |= MF_CHECKED;
            AppendMenuW(pinPageMenu, lFlags, kContextPinLastPage, _LW("app.menu.page_pin_last"));

            AppendMenuW(menu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(pinPageMenu), _LW("app.menu.page_pin_both"));
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    HMENU jumpMenu = nullptr;
    if (showPageNavigation)
    {
        // 当前右键点击的显示器显示的页索引
        int clickedPageIdx = 0;
        {
            auto it = std::ranges::find(savedPageIds_, clickedPageId);
            if (it != savedPageIds_.end())
                clickedPageIdx = static_cast<int>(it - savedPageIds_.begin());
        }

        if (pageOffset_ > 0)
            AppendMenuW(menu, MF_STRING, kContextPagePrev, _LW("app.menu.prev_page"));
        if (pageOffset_ < maxOff)
            AppendMenuW(menu, MF_STRING, kContextPageNext, _LW("app.menu.next_page"));

        jumpMenu = CreatePopupMenu();
        if (jumpMenu)
        {
            // 计算当前所有显示器上显示的页面索引
            std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
            std::unordered_set<int> pagesOnMonitors;
            for (size_t mi = 0; mi < monitorOrder.size(); ++mi)
            {
                int displayIdx = static_cast<int>(mi);
                if (mi == monitorOrder.size() - 1)
                    displayIdx += pageOffset_;
                if (displayIdx < static_cast<int>(savedPageIds_.size()))
                    pagesOnMonitors.insert(displayIdx);
            }


            for (int i = 0; static_cast<size_t>(i) < savedPageIds_.size(); ++i)
            {
                if (!PageHasContent(savedPageIds_[i]) && !pagesOnMonitors.contains(i)) continue;
                std::wstring label = GetPageDisplayName(i);
                UINT flags = MF_STRING;
                if (pagesOnMonitors.contains(i))
                    flags |= MF_GRAYED;
                if (i == clickedPageIdx)
                    flags |= MF_CHECKED;
                AppendMenuW(jumpMenu, flags,
                    kContextPageJumpFirst + static_cast<UINT>(i), label.c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(jumpMenu), _LW("app.menu.goto_page"));
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_STRING, kContextPageAdd, _LW("app.menu.add_page"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextSettingsCommand, _LW("app.menu.settings"));

    SetMenuItemIcon(menu, kContextNewMenu,
        snowdesktop::menu_fluent_glyphs::kNewItem,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextRefreshCommand, L"");
    SetMenuItemIcon(menu, kContextPasteCommand, L"");
    SetMenuItemQuickAction(menu, kContextPasteCommand);
    SetMenuItemQuickAction(menu, kContextNewMenu);
    SetMenuItemQuickAction(menu, kContextRefreshCommand);
    SetMenuItemIcon(menu, kContextMoreCommand,
        snowdesktop::menu_fluent_glyphs::kMoreOptions,
        MenuIconFont::FluentRegular);
    if (sortMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(sortMenu),
            snowdesktop::menu_fluent_glyphs::kSort,
            MenuIconFont::FluentRegular);
        if (nameSortMenu)
        {
            SetMenuItemIcon(sortMenu,
                reinterpret_cast<UINT_PTR>(nameSortMenu),
                snowdesktop::menu_fluent_glyphs::kSortName,
                MenuIconFont::FluentRegular);
            SetMenuItemIcon(nameSortMenu, kContextSortByNameCommand,
                snowdesktop::menu_fluent_glyphs::kSortNameAscending,
                MenuIconFont::FluentRegular);
            SetMenuItemIcon(nameSortMenu, kContextSortByNameDescCommand,
                snowdesktop::menu_fluent_glyphs::kSortNameDescending,
                MenuIconFont::FluentRegular);
        }
        if (typeSortMenu)
        {
            SetMenuItemIcon(sortMenu,
                reinterpret_cast<UINT_PTR>(typeSortMenu),
                snowdesktop::menu_fluent_glyphs::kSortType,
                MenuIconFont::FluentRegular);
            SetMenuItemIcon(typeSortMenu, kContextSortByTypeCommand,
                snowdesktop::menu_fluent_glyphs::kSortTypeAscending,
                MenuIconFont::FluentRegular);
            SetMenuItemIcon(typeSortMenu, kContextSortByTypeDescCommand,
                snowdesktop::menu_fluent_glyphs::kSortTypeDescending,
                MenuIconFont::FluentRegular);
        }
    }
    if (widgetMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(widgetMenu),
            L"\uF136", MenuIconFont::FluentRegular);
        SetMenuItemIcon(widgetMenu, kContextAddCollectionWidget,
            snowdesktop::menu_fluent_glyphs::kCollection,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(widgetMenu, kContextAddCollectionGroupWidget,
            snowdesktop::menu_fluent_glyphs::kCollectionGroup,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(widgetMenu, kContextAddFileGroupWidget,
            snowdesktop::menu_fluent_glyphs::kFileGroup,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(widgetMenu, kContextAddFileCategoryWidget,
            snowdesktop::menu_fluent_glyphs::kDesktopFiles,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(widgetMenu, kContextAddFolderMappingWidget,
            snowdesktop::menu_fluent_glyphs::kFolderMapping,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(widgetMenu, kContextAddLuaWidgetSearch,
            L"\uF68F", MenuIconFont::FluentRegular);
        SetMenuItemIcon(widgetMenu, kContextAddLuaWidgetEmpty,
            snowdesktop::menu_fluent_glyphs::kCollectionGroup,
            MenuIconFont::FluentRegular);
        for (UINT i = 0; i < kLuaWidgetMenuPageSize; ++i)
        {
            SetMenuItemIcon(widgetMenu, kContextAddLuaWidgetFirst + i,
                L"\uEE65",
                MenuIconFont::FluentRegular);
        }
        SetMenuItemIcon(widgetMenu, kContextAddLuaWidgetPreviousPage,
            L"\uF15B", MenuIconFont::FluentRegular);
        SetMenuItemIcon(widgetMenu, kContextAddLuaWidgetNextPage,
            L"\uF181", MenuIconFont::FluentRegular);
    }
    if (pinPageMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(pinPageMenu), L"");
    }
    SetMenuItemIcon(menu, kContextSettingsCommand, L"");
    if (pageOffset_ > 0)
        SetMenuItemIcon(menu, kContextPagePrev, L"");
    if (pageOffset_ < maxOff)
        SetMenuItemIcon(menu, kContextPageNext, L"");
    SetMenuItemIcon(menu, kContextPageAdd, L"");
    if (jumpMenu)
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(jumpMenu), L"");

    gridAdjustmentMenuAnchorValid_ = false;
    SetForegroundWindow(hwnd_);
    snowdesktop::component_preview::Window previewWindow;
    bool wallpaperPrefetchStarted = false;
    UINT previewCacheCommand = 0;
    std::wstring previewCachePackage;
    snowdesktop::component_preview::Model previewCache;
    std::optional<snowdesktop::component_preview::ApplySettings>
        previewApply;
    auto applyPreview = [&](const auto& settings) {
        previewApply = settings;
        snowdesktop::modern_menu::DismissActive();
    };
    snowdesktop::modern_menu::HoverInfo previewAnchor;
    auto showPreview = [&](UINT previewCommand) {
        const UINT addCommand =
            AddCommandForPreviewCommand(previewCommand);
        if (addCommand == 0) return false;
        std::wstring packageId;
        if (addCommand >= kContextAddLuaWidgetFirst &&
            addCommand < kContextAddLuaWidgetFirst +
                static_cast<UINT>(kLuaWidgetMenuPageSize))
        {
            const size_t index = luaPage * kLuaWidgetMenuPageSize +
                static_cast<size_t>(
                    addCommand - kContextAddLuaWidgetFirst);
            if (index >= luaWidgets.size()) return true;
            packageId = luaWidgets[index].packageId;
        }
        if (addCommand != previewCacheCommand ||
            packageId != previewCachePackage)
        {
            previewCacheCommand = addCommand;
            previewCachePackage = packageId;
            previewCache = BuildAddWidgetMenuPreview(
                addCommand, packageId);
        }
        if (!previewCache.Empty())
        {
            previewWindow.Show(previewCache,
                previewAnchor.popupScreenRect, hwnd_, menuIconDpi_,
                menuLightTheme_, applyPreview,
                previewAnchor.itemScreenRect,
                static_cast<snowdesktop::modern_menu::Appearance>(
                    menuAppearanceStyle_));
        }
        return true;
    };
    auto changeLuaWidgetPage = [&](UINT command, auto& rootItems) {
        if (showPreview(command)) return true;
        if (const auto selectedFilter =
            LuaWidgetMenuFilterForCommand(command))
        {
            luaFilter = *selectedFilter;
            luaPage = 0;
            luaWidgets = FilterLuaWidgetMenuEntries(
                allLuaWidgets, luaSearch, luaFilter);
            previewWindow.Hide();
            previewCacheCommand = 0;
            previewCachePackage.clear();
            previewCache = {};
            return ReplaceAddWidgetSubmenu(rootItems,
                BuildAddWidgetMenuItems(allLuaWidgets, luaWidgets,
                    luaPage, luaSearch, luaFilter));
        }
        const size_t pageCount = LuaWidgetMenuPageCount(luaWidgets.size());
        if (command == kContextAddLuaWidgetPreviousPage && luaPage > 0)
            --luaPage;
        else if (command == kContextAddLuaWidgetNextPage &&
            luaPage + 1 < pageCount)
            ++luaPage;
        else
            return false;
        previewCacheCommand = 0;
        previewCachePackage.clear();
        previewCache = {};
        return ReplaceAddWidgetSubmenu(rootItems,
            BuildAddWidgetMenuItems(allLuaWidgets, luaWidgets,
                luaPage, luaSearch, luaFilter));
    };
    auto searchLuaWidgets = [&](UINT command, const std::wstring& text,
                                auto& rootItems) {
        if (command != kContextAddLuaWidgetSearch)
            return;
        luaSearch = text;
        luaPage = 0;
        luaWidgets = FilterLuaWidgetMenuEntries(
            allLuaWidgets, luaSearch, luaFilter);
        previewWindow.Hide();
        previewCacheCommand = 0;
        previewCachePackage.clear();
        previewCache = {};
        ReplaceAddWidgetSubmenu(rootItems,
            BuildAddWidgetMenuItems(allLuaWidgets, luaWidgets,
                luaPage, luaSearch, luaFilter));
    };
    auto previewWidgetMenuItem = [&](const snowdesktop::modern_menu::HoverInfo& hover) {
        if (!wallpaperPrefetchStarted &&
            IsAddWidgetMenuCommand(hover.command))
        {
            const POINT capturePoint{
                (hover.popupScreenRect.left +
                    hover.popupScreenRect.right) / 2,
                (hover.popupScreenRect.top +
                    hover.popupScreenRect.bottom) / 2,
            };
            previewWindow.PrefetchDesktopWallpaperBackdrop(
                hwnd_, capturePoint);
            wallpaperPrefetchStarted = true;
        }
        if (hover.command != 0)
            previewAnchor = hover;
    };
    UINT command = ShowModernMenu(menu, screenPoint, hwnd_,
        false, false, nullptr, changeLuaWidgetPage,
        previewWidgetMenuItem, searchLuaWidgets);
    previewWindow.Close();

    POINT adjustmentMenuPoint = gridAdjustmentMenuAnchorValid_
        ? gridAdjustmentMenuAnchor_ : screenPoint;

    if (sortMenu) DestroyMenu(sortMenu);
    if (widgetMenu) DestroyMenu(widgetMenu);
    if (displaySettingsMenu) DestroyMenu(displaySettingsMenu);
    if (pinPageMenu) DestroyMenu(pinPageMenu);
    if (jumpMenu) DestroyMenu(jumpMenu);
    DestroyMenu(menu);
    newMenuContextMenu_.Reset();
    ClearMenuIcons();

    if (previewApply)
    {
        RestoreDesktopWindowLayer();
        ApplyWidgetPreviewSettings(screenPoint, *previewApply);
        return;
    }

    bool needsDesktopFocus = true;
    if (command >= kContextAddLuaWidgetFirst &&
        command < kContextAddLuaWidgetFirst +
            static_cast<UINT>(kLuaWidgetMenuPageSize))
    {
        const size_t index = luaPage * kLuaWidgetMenuPageSize +
            static_cast<size_t>(command - kContextAddLuaWidgetFirst);
        if (index < luaWidgets.size())
            AddLuaWidgetAt(screenPoint, luaWidgets[index].packageId);
    }
    else if (command >= kContextSpacingPresetFirst &&
        command <= kContextSpacingPresetFirst + 200)
    {
        SetIconSpacing(
            static_cast<float>(command - kContextSpacingPresetFirst) / 100.0f);
    }
    else if (command >= kContextPageJumpFirst && command <= kContextPageJumpLast)
    {
        int pageIdx = static_cast<int>(command - kContextPageJumpFirst);
        if (pageIdx >= 0 && static_cast<size_t>(pageIdx) < savedPageIds_.size())
        {
            int visiblePageCount = static_cast<int>(std::min(savedPageIds_.size(), gridPages_.size()));
            int targetOffset = pageIdx - (visiblePageCount - 1);
            JumpToPageOffset(targetOffset);
        }
    }
    else
    {
        switch (command)
        {
        case kContextRefreshCommand: ReloadItems(); break;
        case kContextSortByNameCommand: SortIconsByName(true); break;
        case kContextSortByNameDescCommand: SortIconsByName(false); break;
        case kContextSortByTypeCommand: SortIconsByType(true); break;
        case kContextSortByTypeDescCommand: SortIconsByType(false); break;
        case kContextGridAdjustmentMenu:
            needsDesktopFocus = false;
            ShowGridAdjustmentMenu(adjustmentMenuPoint, 0);
            break;
        case kContextGridAddRow:
        case kContextGridRemoveRow:
        case kContextGridAddColumn:
        case kContextGridRemoveColumn:
        {
            POINT legacyAdjustmentMenuPoint{};
            GetCursorPos(&legacyAdjustmentMenuPoint);
            needsDesktopFocus = false;
            ShowGridAdjustmentMenu(legacyAdjustmentMenuPoint, command);
            break;
        }
        case kContextSpacingIncrease: AdjustIconSpacing(+0.1f); break;
        case kContextSpacingDecrease: AdjustIconSpacing(-0.1f); break;
        case kContextPinFirstPage: ToggleFirstPagePin(screenPoint); break;
        case kContextPinLastPage:  ToggleLastPagePin(screenPoint);  break;
        case kContextAddCollectionWidget: AddCollectionWidgetAt(screenPoint); break;
        case kContextAddCollectionGroupWidget: AddCollectionGroupWidgetAt(screenPoint); break;
        case kContextAddFileGroupWidget: AddFileGroupWidgetAt(screenPoint); break;
        case kContextAddFileCategoryWidget: AddFileCategoryWidgetAt(screenPoint); break;
        case kContextAddFolderMappingWidget: AddFolderMappingWidgetAt(screenPoint); break;
        case kContextNewMenu:
        {
            wchar_t desktopPath[MAX_PATH]{};
            if (SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE))
            {
                ShowNewMenuAndInvoke(screenPoint, desktopPath);
                ReloadItems();
            }
            break;
        }
        case kContextPasteCommand:
            PasteClipboardToDesktop();
            break;
        case kContextMoreCommand:
            needsDesktopFocus = false;
            ShowDesktopBackgroundContextMenu(screenPoint);
            break;
        case kContextSettingsCommand:
            needsDesktopFocus = false;
            ShowSettingsWindow(); break;
        case kContextFontSizeSmall: SetItemFontSize(12.0f); break;
        case kContextFontSizeMedium: SetItemFontSize(18.0f); break;
        case kContextFontSizeLarge: SetItemFontSize(24.0f); break;
        case kContextListFontSizeSmall: SetListItemFontSize(12.0f); break;
        case kContextListFontSizeMedium: SetListItemFontSize(18.0f); break;
        case kContextListFontSizeLarge: SetListItemFontSize(24.0f); break;
        case kContextFontWeightBold: SetItemFontWeight(DWRITE_FONT_WEIGHT_BOLD); break;
        case kContextFontWeightMedium: SetItemFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD); break;
        case kContextFontWeightFine: SetItemFontWeight(DWRITE_FONT_WEIGHT_NORMAL); break;
        case kContextDisplayAppearanceMore:
            needsDesktopFocus = false;
            ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
                snowdesktop::SettingsPage::Personalization));
            break;
        case kContextPagePrev: NavigatePageOffset(-1); break;
        case kContextPageNext: NavigatePageOffset(1); break;
        case kContextPageAdd: AddNewPage(); break;
        default: break;
        }
    }
    RestoreDesktopWindowLayer();
    if (needsDesktopFocus)
        RestoreInteractionInputFocus();
}

/**
 * @brief 显示桌面图标（文件/快捷方式）的右键菜单。
 *        包含打开、重命名、剪切、复制、删除及"展开更多选项"。
 *        仅在选中项为文件系统项时可操作（非系统图标如此电脑等）。
 *        剪切/复制通过 IDataObject 与 OLE 剪贴板交互。
 * @param screenPoint 菜单弹出的屏幕坐标。
 * @param itemIndex   当前右键点击的桌面项索引。
 */
