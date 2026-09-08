#include "app.h"
#include "../animation_settings.h"
#include "../large_icon_steam.h"
#include "../large_icon_render_rules.h"
#include "../large_icon_renderer.h"
#include "../large_icon_edit_rules.h"

void DesktopApp::RequestLargeIconAsset(size_t index, bool refresh, std::filesystem::path importPath, int variant)
{
    if (index >= items_.size() || !items_[index].largeIcon || IsItemInAnyWidget(items_[index])) return;
    auto& item = items_[index];
    const auto& config = EffectiveLargeIconConfig(item);
    snowdesktop::LargeIconAssetRequest request;
    request.itemKey = item.layoutKey; request.parsingName = item.parsingName;
    request.variant = variant;
    request.content = snowdesktop::LargeIconActiveContent(config);
    request.reference = snowdesktop::LargeIconActiveImage(config); request.lastGood = config.cachedCover;
    request.fillLayer = snowdesktop::IsLargeIconFill(config);
    request.refresh = refresh; request.localOnly = config.localOnly; request.importPath = std::move(importPath);
    if (request.content == 1 && request.reference.empty() && request.importPath.empty()) request.content = 0;
    request.language = snowdesktop::large_icon_steam::Language(Locale::Instance().GetEffectiveLanguage());
    const auto frame = GetLargeIconFrameRect(item);
    const int width = std::max<LONG>(1, frame.right - frame.left), height = std::max<LONG>(1, frame.bottom - frame.top);
    request.pixels = !request.fillLayer && request.content == 0 ? snowdesktop::icon_render_rules::SourcePixelsForTarget(
        static_cast<int>(std::min(width, height) * config.contentScale)) : std::clamp(std::max(width, height), 256, 2048);
    request.portrait = config.steamOrientation == 2 || (config.steamOrientation == 0 && double(width) / height < 1.195);
    if (variant) { request.content = 2; request.pixels = 256; request.portrait = variant == 2; }
    auto& runtime = largeIconRuntime_[item.layoutKey];
    const auto stamp = item.modifiedTime ? (std::uint64_t(item.modifiedTime->dwHighDateTime) << 32) |
        item.modifiedTime->dwLowDateTime : 0;
    request.sourceStamp = stamp;
    request.sourceIconIndex = item.sysIconIndex;
    const std::wstring signature = item.parsingName + L":" + std::to_wstring(request.content) + L":" + std::to_wstring(request.fillLayer) + L":" +
        Utf8ToWide(request.reference) + L":" + std::to_wstring(request.pixels) +
        L":" + std::to_wstring(request.portrait) + L":" + std::to_wstring(request.localOnly) + L":" + Utf8ToWide(request.language) +
        L":" + std::to_wstring(stamp) + L":" + std::to_wstring(item.sysIconIndex);
    auto& savedSignature = variant ? runtime.previewSignatures[variant - 1] : runtime.signature;
    if (!refresh && request.importPath.empty() && savedSignature == signature &&
        (variant || runtime.pending || runtime.retryAt == 0 ||
            snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() < runtime.retryAt)) return;
    // Paint/animation frames compare only model values. Read shortcut contents
    // only when a new resource generation is actually needed.
    if (request.content == 2)
    {
        wchar_t url[2048]{};
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, static_cast<DWORD>(std::size(url)), item.parsingName.c_str());
        request.appId = snowdesktop::large_icon_steam::AppId(url).value_or(0);
    }
    if (!variant && request.importPath.empty() && runtime.asset &&
        (runtime.sourceContent != request.content || runtime.sourceReference != request.reference))
    { EraseD2DIconCacheForBitmap(runtime.asset->bitmap); runtime.asset.reset(); }
    savedSignature = signature;
    if (!variant)
    {
        runtime.pending = true; runtime.error.clear(); runtime.retryAt = 0;
        runtime.sourceContent = request.content; runtime.sourceReference = request.reference;
    }
    request.generation = ++largeIconAssetSerial_;
    (variant ? runtime.previewGenerations[variant - 1] : runtime.generation) = request.generation;
    if (!largeIconAssets_)
        largeIconAssets_ = std::make_unique<snowdesktop::LargeIconAssets>(GetDataSubdirectoryPath(L"large-icons"),
            [window = hwnd_] { PostMessageW(window, kLargeIconAssetsReadyMessage, 0, 0); });
    std::vector<std::string> retained;
    for (const auto& current : items_) if (current.largeIcon)
    { retained.push_back(current.largeIcon->image); retained.push_back(current.largeIcon->foregroundImage); retained.push_back(current.largeIcon->cachedCover); }
    largeIconAssets_->RetainReferences(std::move(retained));
    largeIconAssets_->Request(std::move(request));
}

void DesktopApp::ProcessLargeIconAssets()
{
    if (!largeIconAssets_) return;
    bool persist = false;
    for (auto& result : largeIconAssets_->TakeCompleted())
    {
        const auto index = FindItemIndexByKey(result.request.itemKey);
        auto runtime = largeIconRuntime_.find(result.request.itemKey);
        if (index >= items_.size() || !items_[index].largeIcon || IsItemInAnyWidget(items_[index]) ||
            runtime == largeIconRuntime_.end()) continue;
        auto& state = runtime->second;
        if (result.request.variant)
        {
            const auto i = result.request.variant - 1;
            if (state.previewGenerations[i] == result.request.generation) state.previews[i] = std::move(result.asset);
            continue;
        }
        if (state.generation != result.request.generation) continue;
        state.pending = false; state.error = result.error;
        state.retryAt = result.error.empty() ? 0 : snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() + 120000;
        if (state.retryAt) SetTimer(hwnd_, kLargeIconRetryTimerId, 120000, nullptr);
        if (!result.asset) continue;
        if (!result.request.importPath.empty())
        {
            if (!CanEditLargeIcons() || snowdesktop::IsLargeIconFill(*items_[index].largeIcon) != result.request.fillLayer) continue;
            auto config = *items_[index].largeIcon;
            if (result.request.fillLayer) { config.content = 1; config.image = result.asset->reference; }
            else { config.foregroundContent = 1; config.foregroundImage = result.asset->reference; }
            if (!SetLargeIconConfig(index, config)) { state.error = "largeIcon.saveFailed"; continue; }
        }
        if (state.asset && state.asset != result.asset) EraseD2DIconCacheForBitmap(state.asset->bitmap);
        state.asset = std::move(result.asset);
        if (snowdesktop::IsLargeIconFill(*items_[index].largeIcon) && items_[index].largeIcon->content == 2 && state.asset->reference.starts_with("steam-") &&
            items_[index].largeIcon->cachedCover != state.asset->reference)
        {
            items_[index].largeIcon->cachedCover = state.asset->reference;
            persist = true;
        }
    }
    if (persist) SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool DesktopApp::CanEditLargeIcons() const
{
    return steamEntitlementService_ && steamEntitlementService_->IsRegistered();
}

bool DesktopApp::SetLargeIconConfig(size_t index, std::optional<snowdesktop::LargeIconConfig> config)
{
    if (index >= items_.size()) return false;
    auto& item = items_[index];
    const bool desktop = !IsItemInAnyWidget(item) && item.gridCell.pageId != kDockPageId;
    if (!snowdesktop::large_icon_edit_rules::CanStore(config, CanEditLargeIcons(), desktop)) return false;
    if (config && snowdesktop::IsLargeIconFill(*config) && config->content == 2)
    {
        wchar_t url[2048]{};
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, static_cast<DWORD>(std::size(url)), item.parsingName.c_str());
        if (!snowdesktop::large_icon_steam::AppId(url)) return false;
    }
    const bool sameDesiredSize = config && item.largeIcon && config->columns == item.largeIcon->columns && config->rows == item.largeIcon->rows;
    const GridSpan span = sameDesiredSize ? item.gridSpan : config ? GridSpan{config->columns, config->rows} : GridSpan{1, 1};
    if (config && !sameDesiredSize)
    {
        const auto* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (!page || item.gridCell.column + span.columns > page->columns ||
            item.gridCell.row + span.rows > page->rows) return false;
        std::unordered_set<std::wstring> occupied;
        for (size_t i = 0; i < items_.size(); ++i)
            if (i != index && !IsItemInAnyWidget(items_[i]))
                MarkGridArea(occupied, items_[i].gridCell, items_[i].gridSpan);
        for (const auto& widget : widgets_)
            if (!IsGroupedWidget(widget)) MarkGridArea(occupied, widget.gridCell, widget.gridSpan);
        if (AreGridSlotsMarked(occupied, item.gridCell, span)) return false;
    }
    const auto previousRecords = layoutRecords_;
    if (!snowdesktop::large_icon_edit_rules::Store(item, std::move(config), span, CanEditLargeIcons(), desktop,
        [this] { return SaveLayoutSlots(); }))
    {
        layoutRecords_ = previousRecords;
        return false;
    }
    if (!item.largeIcon && largeIconEdit_.key == item.layoutKey) largeIconEdit_ = {};
    else if (largeIconEdit_.key == item.layoutKey)
    { largeIconEdit_.preview.reset(); ++largeIconEdit_.revision; }
    LayoutItems();
    if (items_[index].largeIcon) RequestLargeIconAsset(index);
    else if (const auto runtime = largeIconRuntime_.find(items_[index].layoutKey); runtime != largeIconRuntime_.end())
    {
        desktopBackdropCompositor_.RemovePanel(runtime->second.backdropFrame);
        if (largeIconAssets_) largeIconAssets_->Cancel(items_[index].layoutKey);
        if (runtime->second.asset) EraseD2DIconCacheForBitmap(runtime->second.asset->bitmap);
        largeIconRuntime_.erase(runtime);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

snowdesktop::LargeIconConfig DesktopApp::MakeLargeIconDefaults(size_t index)
{
    snowdesktop::LargeIconConfig config;
    const auto& style = CurrentPersonalization();
    config.radius = style.cornerRadius;
    config.titleSize = itemFontSizeCu_;
    config.material = style.glassEnabled ? (style.acrylicEnabled ? 2 : 1) : 0;
    config.componentTheme = style.contentTheme;
    config.blurRadius = style.glassBlurRadius;
    config.manualColor = (static_cast<UINT>(style.widgetBgR * 255) << 16) |
        (static_cast<UINT>(style.widgetBgG * 255) << 8) | static_cast<UINT>(style.widgetBgB * 255);
    config.opacity = style.widgetAlpha;
    config.border = style.widgetBorderAlpha > 0;
    config.edgeHighlight = style.widgetEdgeHighlightEnabled;
    config.edgeWidth = style.widgetEdgeHighlightWidth; config.edgeStrength = style.widgetEdgeHighlightStrength;
    config.gradient = style.panelGradient;
    config.borderWidth = style.widgetBorderWidth;
    config.borderOpacity = style.widgetBorderAlpha;
    config.borderColor = (static_cast<UINT>(style.widgetBorderR * 255) << 16) |
        (static_cast<UINT>(style.widgetBorderG * 255) << 8) | static_cast<UINT>(style.widgetBorderB * 255);
    if (index < items_.size())
    {
        wchar_t url[2048]{};
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, static_cast<DWORD>(std::size(url)), items_[index].parsingName.c_str());
        if (snowdesktop::large_icon_steam::AppId(url)) { config.backgroundStyle = -2; config.content = 2; config.fit = 1; }
    }
    return config;
}

void DesktopApp::OpenLargeIconSettings(size_t index)
{
    if (!CanEditLargeIcons())
    {
        ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
            snowdesktop::SettingsPage::General, "general.advancedFeatures"));
        return;
    }
    if (index >= items_.size() || !items_[index].largeIcon) return;
    auto route = snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::LargeIcon);
    route.itemKey = items_[index].layoutKey;
    ShowSettingsWindow(std::move(route));
}

snowdesktop::LargeIconSettingsSnapshot DesktopApp::EditLargeIcon(snowdesktop::LargeIconSettingsRequest request)
{
    snowdesktop::LargeIconSettingsSnapshot result;
    if (request.action == "close")
    {
        largeIconEdit_ = {};
        InvalidateRect(hwnd_, nullptr, FALSE);
        result.succeeded = true;
        return result;
    }
    result.key = request.key;
    const size_t index = FindItemIndexByKey(request.key);
    if (index >= items_.size() || !items_[index].largeIcon || IsItemInAnyWidget(items_[index]) ||
        items_[index].gridCell.pageId == kDockPageId)
    {
        if (largeIconEdit_.key == request.key) largeIconEdit_ = {};
        result.error = "largeIcon.unavailable";
        return result;
    }
    auto& item = items_[index];
    result.available = true;
    result.editable = CanEditLargeIcons();
    result.name = item.name;
    result.defaultConfig = snowdesktop::EncodeLargeIconConfig(MakeLargeIconDefaults(index));
    const auto frame = GetLargeIconFrameRect(item);
    result.frameWidth = std::max<LONG>(1, frame.right - frame.left);
    result.frameHeight = std::max<LONG>(1, frame.bottom - frame.top);
    result.frameColumns = item.gridSpan.columns; result.frameRows = item.gridSpan.rows;
    result.unitScale = GetItemLayoutScale(item.bounds);
    result.durationScale = snowdesktop::animation::RuntimeDurationScale();
    result.frameLimit = snowdesktop::animation::RuntimeFrameLimit();
    result.animations = snowdesktop::animation::RuntimeAnimationsEnabled();
    result.neutral = IsLightContentTheme() ? 0xc9ced6u : 0x414751u;
    wchar_t steamUrl[2048]{};
    GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", steamUrl, static_cast<DWORD>(std::size(steamUrl)), item.parsingName.c_str());
    result.steam = snowdesktop::large_icon_steam::AppId(steamUrl).has_value();
    result.maxColumns = std::max(item.largeIcon->columns, item.gridSpan.columns);
    result.maxRows = std::max(item.largeIcon->rows, item.gridSpan.rows);
    if (const auto* page = FindGridPage(gridPages_, item.gridCell.pageId))
    {
        result.maxColumns = page->columns - item.gridCell.column;
        result.maxRows = page->rows - item.gridCell.row;
        for (int columns = 1; columns <= result.maxColumns; ++columns)
        {
            DesktopWidget geometry; geometry.gridCell = item.gridCell;
            geometry.bounds = GetGridRect(gridPages_, item.gridCell, {columns, 1});
            const auto rect = GetStandaloneWidgetFrameRect(geometry);
            result.frameWidths.push_back(rect.right - rect.left);
        }
        for (int rows = 1; rows <= result.maxRows; ++rows)
        {
            DesktopWidget geometry; geometry.gridCell = item.gridCell;
            geometry.bounds = GetGridRect(gridPages_, item.gridCell, {1, rows});
            const auto rect = GetStandaloneWidgetFrameRect(geometry);
            result.frameHeights.push_back(rect.bottom - rect.top);
        }
    }
    if (const auto error = snowdesktop::large_icon_edit_rules::CheckRequest(largeIconEdit_, request, result.editable); !error.empty())
    {
        result.error = error;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (request.action == "read")
    {
        largeIconEdit_ = { request.key, ++largeIconSessionSerial_, 1, {} };
        result.succeeded = true;
    }
    else if (request.action == "status")
        result.succeeded = true;
    else if (request.action == "cancel")
    {
        largeIconEdit_.preview.reset();
        result.succeeded = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (request.action == "refresh")
    {
        if (!snowdesktop::IsLargeIconFill(*item.largeIcon) || item.largeIcon->content != 2 || !result.steam)
            result.error = "largeIcon.invalid";
        else
        {
            RequestLargeIconAsset(index, true);
            for (int variant = 1; variant <= 2; ++variant) RequestLargeIconAsset(index, true, {}, variant);
            result.succeeded = true;
        }
    }
    else if (request.action == "import")
    {
        std::filesystem::path path = request.path;
        if (path.empty())
        {
            ComPtr<IFileOpenDialog> picker;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker))))
            {
                COMDLG_FILTERSPEC filter{_LW("largeIcon.image"), L"*.png;*.jpg;*.jpeg;*.bmp;*.ico"};
                picker->SetFileTypes(1, &filter);
                picker->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
                if (SUCCEEDED(picker->Show(settingsWindow_ ? settingsWindow_->Window() : hwnd_)))
                {
                    ComPtr<IShellItem> selected; PWSTR selectedPath = nullptr;
                    if (SUCCEEDED(picker->GetResult(&selected)) && SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)))
                    { path = selectedPath; CoTaskMemFree(selectedPath); }
                }
            }
        }
        if (!path.empty()) RequestLargeIconAsset(index, true, std::move(path));
        result.succeeded = true;
    }
    else if (request.action == "commit" || request.action == "preview")
    {
        JsonValue json;
        snowdesktop::LargeIconConfig config;
        if (!ParseJson(request.config, json) || !snowdesktop::DecodeLargeIconConfig(json, config))
            result.error = "largeIcon.invalid";
        else if (request.action == "preview")
        {
            largeIconEdit_.preview = std::move(config);
            result.succeeded = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else if (SetLargeIconConfig(index, std::move(config)))
        {
            largeIconEdit_.preview.reset();
            result.succeeded = true;
        }
        else result.error = "largeIcon.saveFailed";
    }
    else result.error = "largeIcon.invalid";
    result.session = largeIconEdit_.token;
    result.revision = largeIconEdit_.revision;
    result.config = snowdesktop::EncodeLargeIconConfig(EffectiveLargeIconConfig(item));
    RequestLargeIconAsset(index);
    if (result.editable && result.steam && snowdesktop::IsLargeIconFill(EffectiveLargeIconConfig(item)) &&
        EffectiveLargeIconConfig(item).content == 2) for (int variant = 1; variant <= 2; ++variant) RequestLargeIconAsset(index, false, {}, variant);
    if (const auto runtime = largeIconRuntime_.find(item.layoutKey); runtime != largeIconRuntime_.end())
    {
        result.loading = runtime->second.pending;
        if (runtime->second.asset)
        {
            const auto path = std::filesystem::path(GetDataSubdirectoryPath(L"large-icons")) / Utf8ToWide(runtime->second.asset->previewReference);
            result.imagePath = L"file:///" + path.generic_wstring();
            result.source = runtime->second.asset->source;
            result.accent = runtime->second.asset->accent;
            result.hasEdgeColor = runtime->second.asset->hasEdgeColor;
            result.imageWidth = runtime->second.asset->width;
            result.imageHeight = runtime->second.asset->height;
        }
        for (int i = 0; i < 2; ++i) if (const auto& asset = runtime->second.previews[i])
        {
            const auto path = std::filesystem::path(GetDataSubdirectoryPath(L"large-icons")) / Utf8ToWide(asset->previewReference);
            (i == 0 ? result.landscapePath : result.portraitPath) = L"file:///" + path.generic_wstring();
            (i == 0 ? result.landscapeSource : result.portraitSource) = asset->source;
        }
        if (result.error.empty() && !runtime->second.error.empty()) result.error = runtime->second.error;
    }
    return result;
}

const snowdesktop::LargeIconConfig& DesktopApp::EffectiveLargeIconConfig(const DesktopItem& item) const
{
    if (largeIconEdit_.key == item.layoutKey && largeIconEdit_.preview && CanEditLargeIcons())
        return *largeIconEdit_.preview;
    return *item.largeIcon;
}

RECT DesktopApp::GetLargeIconFrameRect(const DesktopItem& item) const
{
    DesktopWidget geometry;
    geometry.bounds = item.bounds;
    geometry.gridCell = item.gridCell;
    return GetStandaloneWidgetFrameRect(geometry);
}

void DesktopApp::DrawLargeIcon(ID2D1RenderTarget* context, const DesktopItem& item, RECT bounds, int state)
{
    // Match component resizing: only the grid placeholder is painted until
    // commit/cancel; do not request or redraw content from pointer updates.
    if (largeIconGesture_ && largeIconGesture_->resizing && largeIconGesture_->key == item.layoutKey)
    {
        if (const auto found = largeIconRuntime_.find(item.layoutKey); found != largeIconRuntime_.end())
            desktopBackdropCompositor_.RemovePanel(found->second.backdropFrame);
        return;
    }
    RequestLargeIconAsset(FindItemIndexByKey(item.layoutKey));
    const auto& config = EffectiveLargeIconConfig(item);
    DesktopWidget geometry; geometry.bounds = bounds; geometry.gridCell = item.gridCell;
    snowdesktop::large_icon_renderer::View view;
    view.frame = GetStandaloneWidgetFrameRect(geometry);
    view.scale = GetItemLayoutScale(bounds);
    view.opacity = (item.isCut ? .4f : 1.f) * (state == 3 ? .6f : 1.f);
    view.animations = snowdesktop::animation::RuntimeAnimationsEnabled();
    view.neutral = IsLightContentTheme() ? 0xc9ced6u : 0x414751u;
    const bool demo = ShouldUseDemoIdentity(item);
    const auto name = demo ? GetDemoIdentityTitle(item.layoutKey) : item.name;
    view.name = name; view.selected = item.selected;
    if (const auto runtime = largeIconRuntime_.find(item.layoutKey); runtime != largeIconRuntime_.end())
    {
        // The first cached ghost may be captured before the desktop's hover
        // reset. Always capture its idle pose so inner text never sticks there.
        view.hover = state == 3 ? 0 : runtime->second.motion.hover;
        if (const auto& asset = runtime->second.asset; asset && !demo)
        {
            view.bitmap = GetOrCreateD2DBitmap(context, asset->bitmap, false);
            view.original = asset->source == "original" || asset->reference.starts_with("raw-");
            view.accent = asset->accent;
            view.hasEdgeColor = asset->hasEdgeColor; view.edgeColor = asset->edgeColor;
        }
    }
    view.placeholder = [&](ID2D1RenderTarget* drawing, RECT rect, float opacity) {
        if (demo) DrawDemoIdentityIcon(drawing, item.layoutKey, rect, opacity);
        else DrawPlaceholderIcon(drawing, item.sysIconIndex, rect, opacity, false);
    };
    auto appearance = CurrentPersonalization();
    if (config.backgroundStyle >= 0 && config.backgroundStyle != kAppearancePresetCustom)
        appearance = MakeAppearancePreset(config.backgroundStyle);
    else if (config.backgroundStyle == kAppearancePresetCustom)
    {
        appearance.widgetBgR = ((config.manualColor >> 16) & 255) / 255.f;
        appearance.widgetBgG = ((config.manualColor >> 8) & 255) / 255.f;
        appearance.widgetBgB = (config.manualColor & 255) / 255.f;
        appearance.widgetAlpha = static_cast<float>(config.opacity);
        appearance.widgetBorderR = ((config.borderColor >> 16) & 255) / 255.f;
        appearance.widgetBorderG = ((config.borderColor >> 8) & 255) / 255.f;
        appearance.widgetBorderB = (config.borderColor & 255) / 255.f;
        appearance.widgetBorderAlpha = config.border ? static_cast<float>(config.borderOpacity) : 0;
        appearance.widgetBorderWidth = static_cast<float>(config.borderWidth);
        appearance.glassEnabled = config.material != 0; appearance.acrylicEnabled = config.material == 2;
        appearance.glassBlurRadius = static_cast<float>(config.blurRadius);
        appearance.contentTheme = config.componentTheme;
        appearance.widgetEdgeHighlightEnabled = config.edgeHighlight;
        appearance.widgetEdgeHighlightWidth = static_cast<float>(config.edgeWidth);
        appearance.widgetEdgeHighlightStrength = static_cast<float>(config.edgeStrength);
        appearance.panelGradient = config.gradient;
    }
    else if (config.backgroundStyle == -3)
    {
        const auto automatic = snowdesktop::large_icon_render_rules::DefaultBackground(config, view.accent, view.hasEdgeColor, view.edgeColor);
        appearance.widgetBgR = ((automatic.color >> 16) & 255) / 255.f;
        appearance.widgetBgG = ((automatic.color >> 8) & 255) / 255.f;
        appearance.widgetBgB = (automatic.color & 255) / 255.f;
        appearance.widgetAlpha = static_cast<float>(automatic.opacity);
        appearance.glassEnabled = appearance.acrylicEnabled = appearance.widgetEdgeHighlightEnabled = false;
        appearance.widgetBorderAlpha = 0;
        appearance.panelGradient = automatic.gradient;
    }
    const auto colorByte = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.f, 1.f) * 255 + .5f); };
    view.backgroundResolved = true;
    view.background.color = colorByte(appearance.widgetBgR) << 16 | colorByte(appearance.widgetBgG) << 8 | colorByte(appearance.widgetBgB);
    view.background.opacity = appearance.widgetAlpha; view.background.gradient = appearance.panelGradient;
    view.componentForeground = appearance.contentTheme == 1 ? 0x161616 : 0xffffff;
    const bool fill = snowdesktop::IsLargeIconFill(config);
    auto& runtime = largeIconRuntime_[item.layoutKey];
    if (state != 3)
    {
        if (fill || !appearance.glassEnabled)
        { desktopBackdropCompositor_.RemovePanel(runtime.backdropFrame); runtime.backdropFrame = {}; }
        else runtime.backdropFrame = view.frame;
    }
    ComPtr<ID2D1DeviceContext> device;
    context->QueryInterface(IID_PPV_ARGS(&device));
    if (!fill && device)
        view.drawBackground = [this, appearance, state, scale = view.scale, owner = &runtime](ID2D1RenderTarget* target, RECT rect, float radius, float opacity) mutable {
            ComPtr<ID2D1DeviceContext> drawing;
            if (FAILED(target->QueryInterface(IID_PPV_ARGS(&drawing)))) return;
            auto style = appearance;
            style.widgetAlpha *= opacity; style.widgetBorderAlpha *= opacity;
            for (auto& stop : style.panelGradient.stops) stop.opacity *= opacity;
            DrawWidgetPanelBackground(drawing.Get(), rect, radius,
                D2D1::ColorF(style.widgetBgR, style.widgetBgG, style.widgetBgB, style.widgetAlpha),
                D2D1::ColorF(style.widgetBorderR, style.widgetBorderG, style.widgetBorderB, style.widgetBorderAlpha),
                false, style.widgetBorderWidth * scale, &style, state != 3,
                reinterpret_cast<std::uintptr_t>(owner), scale);
        };
    const auto transform = snowdesktop::large_icon_transform::Resolve(
        static_cast<float>(view.frame.right - view.frame.left), static_cast<float>(view.frame.bottom - view.frame.top),
        runtime.motion.tiltX * view.hover, runtime.motion.tiltY * view.hover, static_cast<float>(config.amplitude));
    if (config.effect == 1 && view.animations && state != 3 && transform.active && device)
    {
        if (!runtime.cardResources) runtime.cardResources = std::make_shared<snowdesktop::large_icon_renderer::CardResources>();
        if (snowdesktop::large_icon_renderer::DrawCard3D(device.Get(), dwriteFactory_.Get(), config, view, transform, *runtime.cardResources))
        {
            if (!fill && appearance.glassEnabled)
                desktopBackdropCompositor_.SetPanelTransform(reinterpret_cast<std::uintptr_t>(&runtime), transform.matrix,
                    snowdesktop::large_icon_transform::Bounds(view.frame, transform.matrix));
            return;
        }
    }
    else runtime.cardResources.reset();
    snowdesktop::large_icon_renderer::DrawFrame(context, dwriteFactory_.Get(), config, view);
}
