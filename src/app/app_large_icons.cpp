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
    auto& runtime = largeIconRuntime_[item.layoutKey];
    const auto stamp = item.modifiedTime ? (std::uint64_t(item.modifiedTime->dwHighDateTime) << 32) |
        item.modifiedTime->dwLowDateTime : 0;
    request.sourceStamp = stamp;
    request.sourceIconIndex = item.sysIconIndex;
    const std::wstring signature = item.parsingName + L":" + std::to_wstring(config.content) + L":" +
        Utf8ToWide(config.image) + L":" + std::to_wstring(request.pixels) +
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
        if (items_[index].largeIcon->content == 2 && state.asset->reference.starts_with("steam-") &&
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
    const auto frame = GetLargeIconFrameRect(item);
    result.frameWidth = std::max<LONG>(1, frame.right - frame.left);
    result.frameHeight = std::max<LONG>(1, frame.bottom - frame.top);
    result.frameColumns = item.gridSpan.columns; result.frameRows = item.gridSpan.rows;
    result.unitScale = GetItemLayoutScale(item.bounds);
    result.durationScale = snowdesktop::animation::RuntimeDurationScale();
    result.frameLimit = snowdesktop::animation::RuntimeFrameLimit();
    result.animations = snowdesktop::animation::RuntimeAnimationsEnabled();
    result.neutral = result.accent = IsLightContentTheme() ? 0xc9ced6u : 0x414751u;
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
    const auto& config = EffectiveLargeIconConfig(item);
    DesktopWidget geometry; geometry.bounds = bounds; geometry.gridCell = item.gridCell;
    snowdesktop::large_icon_renderer::View view;
    view.frame = GetStandaloneWidgetFrameRect(geometry);
    view.scale = GetItemLayoutScale(bounds);
    view.opacity = (item.isCut ? .4f : 1.f) * (state == 3 ? .6f : 1.f);
    view.animations = snowdesktop::animation::RuntimeAnimationsEnabled();
    view.neutral = view.accent = IsLightContentTheme() ? 0xc9ced6u : 0x414751u;
    const bool demo = ShouldUseDemoIdentity(item);
    const auto name = demo ? GetDemoIdentityTitle(item.layoutKey) : item.name;
    view.name = name; view.selected = item.selected;
    const auto* pressed = dynamic_cast<const DesktopIcon*>(mouseDownHit_);
    view.pressed = mouseDown_ && pressed && pressed->GetDesktopItem() == &item &&
        PtInRect(&view.frame, lastMousePoint_) && !dragSession_.IsActive();
    if (const auto runtime = largeIconRuntime_.find(item.layoutKey); runtime != largeIconRuntime_.end())
    {
        view.hover = runtime->second.motion.hover;
        view.launchWave = runtime->second.motion.LaunchWave(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(),
            snowdesktop::animation::RuntimeDurationScale());
        if (const auto& asset = runtime->second.asset; asset && !demo)
        {
            view.bitmap = GetOrCreateD2DBitmap(context, asset->bitmap, false);
            view.original = config.content == 0 || asset->reference.starts_with("raw-");
            view.accent = asset->accent;
        }
    }
    view.placeholder = [&](RECT rect, float opacity) {
        if (demo) DrawDemoIdentityIcon(context, item.layoutKey, rect, opacity);
        else DrawPlaceholderIcon(context, item.sysIconIndex, rect, opacity, false);
    };
    snowdesktop::large_icon_renderer::DrawFrame(context, dwriteFactory_.Get(), config, view);
}

void DesktopApp::DrawLargeIconTitles(ID2D1RenderTarget* context)
{
    if (dragSession_.IsActive() || marqueeActive_ || widgetAction_ != WidgetAction::None || largeIconGesture_ || HasActiveContextMenuSession()) return;
    for (const auto& item : items_)
    {
        if (!item.largeIcon || IsItemInAnyWidget(item) || IsRectEmptyRect(item.bounds)) continue;
        const auto runtime = largeIconRuntime_.find(item.layoutKey);
        if (runtime == largeIconRuntime_.end() || runtime->second.motion.hover <= .01f) continue;
        const auto* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (!page) continue;
        const auto& config = EffectiveLargeIconConfig(item);
        snowdesktop::large_icon_renderer::View view;
        view.frame = GetLargeIconFrameRect(item); view.scale = GetItemLayoutScale(item.bounds);
        view.animations = snowdesktop::animation::RuntimeAnimationsEnabled();
        view.hover = runtime->second.motion.hover;
        const bool demo = ShouldUseDemoIdentity(item);
        const auto name = demo ? GetDemoIdentityTitle(item.layoutKey) : item.name;
        view.name = name;
        if (const auto& asset = runtime->second.asset; asset && !demo)
        {
            view.bitmap = GetOrCreateD2DBitmap(context, asset->bitmap, false);
            view.original = config.content == 0 || asset->reference.starts_with("raw-");
        }
        snowdesktop::large_icon_renderer::DrawFloatingTitle(context, dwriteFactory_.Get(), config, view, page->bounds);
    }
}
