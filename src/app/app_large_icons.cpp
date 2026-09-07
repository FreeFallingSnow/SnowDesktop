#include "app.h"
#include "../animation_settings.h"
#include "../large_icon_steam.h"
#include "../large_icon_render_rules.h"

void DesktopApp::RequestLargeIconAsset(size_t index, bool refresh, std::filesystem::path importPath, int variant)
{
    if (index >= items_.size() || !items_[index].largeIcon || IsItemInAnyWidget(items_[index])) return;
    auto& item = items_[index];
    const auto& config = *item.largeIcon;
    snowdesktop::LargeIconAssetRequest request;
    request.itemKey = item.layoutKey; request.parsingName = item.parsingName;
    request.variant = variant;
    request.content = config.content; request.reference = config.image; request.lastGood = config.cachedCover;
    request.refresh = refresh; request.localOnly = config.localOnly; request.importPath = std::move(importPath);
    request.language = snowdesktop::large_icon_steam::Language(Locale::Instance().GetEffectiveLanguage());
    const auto frame = GetLargeIconFrameRect(item);
    const int width = std::max<LONG>(1, frame.right - frame.left), height = std::max<LONG>(1, frame.bottom - frame.top);
    request.pixels = config.content == 0 ? snowdesktop::icon_render_rules::SourcePixelsForTarget(
        static_cast<int>(std::min(width, height) * config.contentScale)) : std::clamp(std::max(width, height), 256, 2048);
    request.portrait = config.steamOrientation == 2 || (config.steamOrientation == 0 && double(width) / height < 1.195);
    if (variant) { request.content = 2; request.pixels = 256; request.portrait = variant == 2; }
    if (request.content == 2)
    {
        wchar_t url[2048]{};
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, static_cast<DWORD>(std::size(url)), item.parsingName.c_str());
        request.appId = snowdesktop::large_icon_steam::AppId(url).value_or(0);
    }
    auto& runtime = largeIconRuntime_[item.layoutKey];
    std::error_code stampError;
    const auto stamp = std::filesystem::last_write_time(item.parsingName, stampError);
    const std::wstring signature = item.parsingName + L":" + std::to_wstring(config.content) + L":" +
        Utf8ToWide(config.image) + L":" + std::to_wstring(request.appId) + L":" + std::to_wstring(request.pixels) +
        L":" + std::to_wstring(request.portrait) + L":" + std::to_wstring(request.localOnly) + L":" + Utf8ToWide(request.language) +
        L":" + (stampError ? L"" : std::to_wstring(stamp.time_since_epoch().count())) + L":" + std::to_wstring(item.sysIconIndex);
    auto& savedSignature = variant ? runtime.previewSignatures[variant - 1] : runtime.signature;
    if (!refresh && request.importPath.empty() && savedSignature == signature &&
        (variant || runtime.pending || runtime.retryAt == 0 ||
            snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() < runtime.retryAt)) return;
    if (!variant && request.importPath.empty() && config.content == 0 && runtime.asset && runtime.asset->source != "original")
    { EraseD2DIconCacheForBitmap(runtime.asset->bitmap); runtime.asset.reset(); }
    savedSignature = signature;
    if (!variant) runtime.pending = true;
    request.generation = ++largeIconAssetSerial_;
    (variant ? runtime.previewGenerations[variant - 1] : runtime.generation) = request.generation;
    if (!largeIconAssets_)
        largeIconAssets_ = std::make_unique<snowdesktop::LargeIconAssets>(GetDataSubdirectoryPath(L"large-icons"),
            [window = hwnd_] { PostMessageW(window, kLargeIconAssetsReadyMessage, 0, 0); });
    std::vector<std::string> retained;
    for (const auto& current : items_) if (current.largeIcon)
    { retained.push_back(current.largeIcon->image); retained.push_back(current.largeIcon->cachedCover); }
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
            if (!CanEditLargeIcons()) continue;
            auto config = *items_[index].largeIcon;
            config.content = 1; config.image = result.asset->reference; config.fit = 1;
            if (!SetLargeIconConfig(index, config)) { state.error = "largeIcon.saveFailed"; continue; }
        }
        if (state.asset && state.asset != result.asset) EraseD2DIconCacheForBitmap(state.asset->bitmap);
        state.asset = std::move(result.asset);
        if (items_[index].largeIcon->content == 2 && items_[index].largeIcon->cachedCover != state.asset->reference)
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
    if (config && (!CanEditLargeIcons() || !snowdesktop::ValidateLargeIconConfig(*config) ||
        IsItemInAnyWidget(item) || item.gridCell.pageId == kDockPageId)) return false;
    if (config && config->content == 2)
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
    const auto previous = item.largeIcon;
    const auto previousSpan = item.gridSpan;
    const auto previousRecords = layoutRecords_;
    item.largeIcon = std::move(config);
    item.gridSpan = span;
    if (!SaveLayoutSlots())
    {
        item.largeIcon = previous;
        item.gridSpan = previousSpan;
        layoutRecords_ = previousRecords;
        return false;
    }
    if (!item.largeIcon && largeIconEdit_.key == item.layoutKey) largeIconEdit_ = {};
    else if (largeIconEdit_.key == item.layoutKey) ++largeIconEdit_.revision;
    LayoutItems();
    if (items_[index].largeIcon) RequestLargeIconAsset(index);
    else if (const auto runtime = largeIconRuntime_.find(items_[index].layoutKey); runtime != largeIconRuntime_.end())
    {
        if (largeIconAssets_) largeIconAssets_->Cancel(items_[index].layoutKey);
        if (runtime->second.asset) EraseD2DIconCacheForBitmap(runtime->second.asset->bitmap);
        largeIconRuntime_.erase(runtime);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
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
    wchar_t steamUrl[2048]{};
    GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", steamUrl, static_cast<DWORD>(std::size(steamUrl)), item.parsingName.c_str());
    result.steam = snowdesktop::large_icon_steam::AppId(steamUrl).has_value();
    result.maxColumns = std::max(item.largeIcon->columns, item.gridSpan.columns);
    result.maxRows = std::max(item.largeIcon->rows, item.gridSpan.rows);
    if (const auto* page = FindGridPage(gridPages_, item.gridCell.pageId))
    {
        result.maxColumns = page->columns - item.gridCell.column;
        result.maxRows = page->rows - item.gridCell.row;
    }
    if (!result.editable)
    {
        largeIconEdit_.preview.reset();
        result.error = "largeIcon.locked";
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (request.action == "read")
    {
        largeIconEdit_ = { request.key, ++largeIconSessionSerial_, 1, {} };
        result.succeeded = true;
    }
    else if (request.action == "status" && request.key == largeIconEdit_.key && request.session == largeIconEdit_.token)
        result.succeeded = true;
    else if (request.session != largeIconEdit_.token || request.key != largeIconEdit_.key ||
        request.revision != largeIconEdit_.revision)
        result.error = "largeIcon.stale";
    else if (request.action == "cancel")
    {
        largeIconEdit_.preview.reset();
        result.succeeded = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (request.action == "refresh")
    {
        RequestLargeIconAsset(index, true);
        if (result.steam) for (int variant = 1; variant <= 2; ++variant) RequestLargeIconAsset(index, true, {}, variant);
        result.succeeded = true;
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
    if (result.editable && result.steam) for (int variant = 1; variant <= 2; ++variant) RequestLargeIconAsset(index, false, {}, variant);
    if (const auto runtime = largeIconRuntime_.find(item.layoutKey); runtime != largeIconRuntime_.end())
    {
        if (runtime->second.asset)
        {
            const auto path = std::filesystem::path(GetDataSubdirectoryPath(L"large-icons")) / Utf8ToWide(runtime->second.asset->previewReference);
            result.imagePath = L"file:///" + path.generic_wstring();
            result.source = runtime->second.asset->source;
            result.accent = runtime->second.asset->accent;
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
    RequestLargeIconAsset(FindItemIndexByKey(item.layoutKey));
    const auto& c = EffectiveLargeIconConfig(item);
    DesktopWidget geometry;
    geometry.bounds = bounds;
    geometry.gridCell = item.gridCell;
    const auto frame = GetStandaloneWidgetFrameRect(geometry);
    const float scale = GetItemLayoutScale(bounds);
    const float radius = std::min(static_cast<float>(c.radius) * scale,
        static_cast<float>(std::min(frame.right - frame.left, frame.bottom - frame.top)) / 2);
    const auto runtime = largeIconRuntime_.find(item.layoutKey);
    const auto asset = runtime != largeIconRuntime_.end() ? runtime->second.asset : nullptr;
    const float hover = runtime != largeIconRuntime_.end() ? runtime->second.hover : 0;
    const bool cover = c.content != 0 && asset;
    const float alpha = item.isCut ? .4f : (state == 3 ? .6f : 1.f);
    auto color = [](std::uint32_t rgb, double opacity) { return D2D1::ColorF(rgb, static_cast<float>(opacity)); };
    const auto neutral = IsLightContentTheme() ? 0xc9ced6u : 0x414751u;
    const auto accent = asset ? asset->accent : neutral;
    const auto mixChannel = [&](int shift) { return static_cast<UINT>(((neutral >> shift) & 255) * (1 - c.colorMix) + ((accent >> shift) & 255) * c.colorMix); };
    const auto background = c.autoColor ? (mixChannel(16) << 16) | (mixChannel(8) << 8) | mixChannel(0) : c.manualColor;
    if (c.shadow || (c.hoverFrame == 3 && hover > 0))
        for (int spread = 5; spread >= 1; --spread)
        {
            RECT shadow = frame; InflateRect(&shadow, spread, spread); OffsetRect(&shadow, 0, 2);
            DrawD2DRoundedRectangle(context, shadow, radius + spread, color(0,
                c.shadowStrength * (c.shadow ? .06 : 0) + (c.hoverFrame == 3 ? hover * .035 : 0)), color(0, 0));
        }
    DrawD2DRoundedRectangle(context, frame, radius,
        color(background, alpha * (c.opacity + (c.hoverFrame == 1 ? (c.hoverOpacity - c.opacity) * hover : 0))), color(0, 0));
    const int edge = std::max(1, static_cast<int>(std::min(frame.right - frame.left, frame.bottom - frame.top) * c.contentScale));
    RECT icon = { (frame.left + frame.right - edge) / 2, (frame.top + frame.bottom - edge) / 2, 0, 0 };
    icon.right = icon.left + edge;
    icon.bottom = icon.top + edge;
    const bool animate = snowdesktop::animation::RuntimeAnimationsEnabled();
    const bool leftReveal = animate && !cover && c.content == 0 && (c.titleMode == 1 || c.hoverContent == 1) &&
        snowdesktop::large_icon_render_rules::CanRevealTitle(static_cast<float>(frame.right - frame.left),
            static_cast<float>(frame.bottom - frame.top), static_cast<float>(edge), static_cast<float>(c.titleSize) * scale, scale);
    float contentZoom = 1;
    if (animate && ((cover && c.coverHover == 1) || (!cover && c.hoverContent == 2))) contentZoom += .06f * hover * static_cast<float>(c.amplitude);
    if (c.press && animate && mouseDown_ && PtInRect(&frame, lastMousePoint_) && !dragSession_.IsActive()) contentZoom *= .96f;
    if (leftReveal) OffsetRect(&icon, static_cast<int>((frame.left + 12 * scale - icon.left) * hover), 0);
    else if (animate && !cover && c.hoverContent == 3) OffsetRect(&icon, 0, static_cast<int>(-6 * scale * hover * c.amplitude));
    if (runtime != largeIconRuntime_.end() && runtime->second.launchStart > 0 && c.launch == 1)
    {
        const double progress = std::clamp((snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - runtime->second.launchStart) / 450., 0., 1.);
        OffsetRect(&icon, 0, static_cast<int>(-std::sin(progress * 3.141592653589793) * 9 * scale * c.amplitude));
    }
    InflateRect(&icon, static_cast<int>(edge * (contentZoom - 1) / 2), static_cast<int>(edge * (contentZoom - 1) / 2));
    ComPtr<ID2D1Factory> factory; context->GetFactory(&factory);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    const auto target = D2D1::RectF(static_cast<float>(frame.left), static_cast<float>(frame.top), static_cast<float>(frame.right), static_cast<float>(frame.bottom));
    if (factory) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(target, radius, radius), &clip);
    if (clip) context->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), nullptr);
    if (ShouldUseDemoIdentity(item))
        DrawDemoIdentityIcon(context, item.layoutKey, icon, alpha);
    else if (auto* bitmap = GetOrCreateD2DBitmap(context, asset ? asset->bitmap : nullptr, false))
    {
        if (!cover) DrawIconBitmap(context, bitmap, icon, alpha);
        else
        {
            const auto size = bitmap->GetSize();
            const float fw = target.right - target.left, fh = target.bottom - target.top;
            if (c.fit == 0)
            {
                const float factor = std::min(fw / size.width, fh / size.height) * contentZoom;
                const float w = size.width * factor, h = size.height * factor;
                const auto destination = D2D1::RectF((target.left + target.right - w) / 2, (target.top + target.bottom - h) / 2,
                    (target.left + target.right + w) / 2, (target.top + target.bottom + h) / 2);
                context->DrawBitmap(bitmap, destination, alpha);
            }
            else
            {
                const float factor = std::max(fw / size.width, fh / size.height) * contentZoom;
                const float w = std::min(size.width, fw / factor), h = std::min(size.height, fh / factor);
                const float left = static_cast<float>((size.width - w) * c.focusX), top = static_cast<float>((size.height - h) * c.focusY);
                const auto source = D2D1::RectF(left, top, left + w, top + h);
                context->DrawBitmap(bitmap, target, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
            }
        }
    }
    else DrawPlaceholderIcon(context, item.sysIconIndex, icon, alpha, false);
    if ((leftReveal || (cover && c.coverHover == 2)) && hover > 0)
    {
        RECT title = frame;
        title.left = leftReveal ? static_cast<LONG>(frame.left + edge + 24 * scale) : frame.left;
        title.right -= static_cast<LONG>(12 * scale);
        if (cover) title.top = static_cast<LONG>(frame.bottom - c.titleSize * scale * 3.2);
        DrawD2DRoundedRectangle(context, title, 4 * scale,
            color(snowdesktop::large_icon_render_rules::TitleBackdrop(c.autoTitleColor ? 0xffffff : c.titleColor), .88 * hover), color(0, 0));
        ComPtr<IDWriteTextFormat> format;
        dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(c.titleSize) * scale, L"", &format);
        ComPtr<ID2D1SolidColorBrush> brush;
        context->CreateSolidColorBrush(color(c.autoTitleColor ? 0xffffff : c.titleColor, hover), &brush);
        if (format && brush)
        {
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis; dwriteFactory_->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
            format->SetTrimming(&trimming, ellipsis.Get());
            const auto name = ShouldUseDemoIdentity(item) ? GetDemoIdentityTitle(item.layoutKey) : item.name;
            const float middle = static_cast<float>(title.top + title.bottom) / 2;
            context->DrawText(name.c_str(), static_cast<UINT32>(name.size()), format.Get(),
                D2D1::RectF(static_cast<float>(title.left + 6 + (1 - hover) * 6), middle - static_cast<float>(c.titleSize) * scale * 1.35f,
                    static_cast<float>(title.right - 6), middle + static_cast<float>(c.titleSize) * scale * 1.35f), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    if (clip) context->PopLayer();
    if (c.border)
        DrawD2DRoundedRectangle(context, frame, radius, color(0, 0), color(c.borderColor,
            alpha * std::min(1., c.borderOpacity + (c.hoverFrame == 2 ? hover * .4 : 0))), static_cast<float>(c.borderWidth) * scale);
    if (runtime != largeIconRuntime_.end() && runtime->second.launchStart > 0 && c.launch == 2)
    {
        const double progress = std::clamp((snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - runtime->second.launchStart) / 450., 0., 1.);
        DrawD2DRoundedRectangle(context, frame, radius, color(0, 0), color(0xffffff, std::sin(progress * 3.141592653589793) * .65), 2 * scale);
    }
    if (item.selected)
    {
        DrawD2DRoundedRectangle(context, frame, radius,
            D2D1::ColorF(0, 0.f), D2D1::ColorF(0x75baff, .95f));
        RECT marker{frame.left + 5, frame.top + 5, frame.left + 13, frame.top + 13};
        DrawD2DRoundedRectangle(context, marker, 4, D2D1::ColorF(0x75baff), D2D1::ColorF(0x75baff));
    }
}

void DesktopApp::DrawLargeIconTitles(ID2D1RenderTarget* context)
{
    if (dragSession_.IsActive() || marqueeActive_ || widgetAction_ != WidgetAction::None || largeIconGesture_ || HasActiveContextMenuSession()) return;
    for (const auto& item : items_)
    {
        if (!item.largeIcon || IsItemInAnyWidget(item) || IsRectEmptyRect(item.bounds)) continue;
        const RECT frame = GetLargeIconFrameRect(item);
        const auto* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (!page) continue;
        const auto& c = EffectiveLargeIconConfig(item);
        const auto runtime = largeIconRuntime_.find(item.layoutKey);
        const float hover = runtime != largeIconRuntime_.end() ? runtime->second.hover : 0;
        if (hover <= .01f) continue;
        const float scale = GetItemLayoutScale(item.bounds);
        const int width = std::min<int>(page->bounds.right - page->bounds.left, std::max(160, static_cast<int>(260 * scale)));
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(c.titleSize) * scale, L"", &format))) continue;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
        const auto name = ShouldUseDemoIdentity(item) ? GetDemoIdentityTitle(item.layoutKey) : item.name;
        const float edge = std::min(frame.right - frame.left, frame.bottom - frame.top) * static_cast<float>(c.contentScale);
        if (snowdesktop::animation::RuntimeAnimationsEnabled() && c.content == 0 && (c.titleMode == 1 || c.hoverContent == 1) &&
            snowdesktop::large_icon_render_rules::CanRevealTitle(static_cast<float>(frame.right - frame.left),
                static_cast<float>(frame.bottom - frame.top), edge, static_cast<float>(c.titleSize) * scale, scale))
        {
            ComPtr<IDWriteTextLayout> inner;
            DWRITE_TEXT_METRICS metrics{};
            const float available = static_cast<float>(frame.right - frame.left) - edge - 48 * scale;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(name.c_str(), static_cast<UINT32>(name.size()), format.Get(),
                available, 100000.f, &inner)) && SUCCEEDED(inner->GetMetrics(&metrics)) && metrics.lineCount <= 2) continue;
        }
        ComPtr<IDWriteTextLayout> text;
        if (FAILED(dwriteFactory_->CreateTextLayout(name.c_str(), static_cast<UINT32>(name.size()), format.Get(),
            static_cast<float>(width - 12), 100000.f, &text))) continue;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(text->GetMetrics(&metrics))) continue;
        const int height = std::min(static_cast<int>(page->bounds.bottom - page->bounds.top), static_cast<int>(std::ceil(metrics.height)) + 12);
        const int left = std::clamp(static_cast<int>((frame.left + frame.right - width) / 2),
            static_cast<int>(page->bounds.left), static_cast<int>(page->bounds.right - width));
        int top = frame.bottom + 5;
        if (top + height > page->bounds.bottom) top = frame.top - height - 5;
        top = std::clamp(top, static_cast<int>(page->bounds.top), static_cast<int>(page->bounds.bottom - height));
        RECT title{left, top, left + width, top + height};
        DrawD2DRoundedRectangle(context, title, 6 * scale,
            D2D1::ColorF(snowdesktop::large_icon_render_rules::TitleBackdrop(c.autoTitleColor ? 0xffffff : c.titleColor), .96f * hover),
            D2D1::ColorF(0xffffff, .18f * hover));
        ComPtr<ID2D1SolidColorBrush> brush;
        context->CreateSolidColorBrush(D2D1::ColorF(c.autoTitleColor ? 0xffffff : c.titleColor, hover), &brush);
        if (brush)
        {
            context->PushAxisAlignedClip(ToD2DRect(title), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            context->DrawTextLayout(D2D1::Point2F(static_cast<float>(title.left + 6), static_cast<float>(title.top + 6)), text.Get(), brush.Get());
            context->PopAxisAlignedClip();
        }
    }
}
