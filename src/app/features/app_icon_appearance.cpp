#include "app/app.h"
#include "icons/icon_appearance_codec.h"
#include "icons/icon_override_rules.h"
#include "icons/icon_hbitmap_pixels.h"
#include "theme/theme_library_settings.h"
#include <d2d1_1helper.h>
#include <utility>
#include "icons/icon_control_source.h"

bool DesktopApp::IsActiveIconCustomization(const std::wstring& target) const
{
    return snowdesktop::icon_customization::Visible(target, missingIconTargets_);
}

bool DesktopApp::FlushExpiredIconCustomizations()
{
    const auto now = snowdesktop::icon_customization::NowSeconds();
    if (std::none_of(missingIconTargets_.begin(), missingIconTargets_.end(),
        [now](const auto& entry) { return snowdesktop::icon_customization::Expired(entry.second, now); })) return true;
    // Explicit edits must not clear an old deadline and revive an expired
    // sibling customization before the asynchronous maintenance result arrives.
    if (!SaveLayoutSlots()) return false;
    ++iconAppearanceRevision_;
    ClearStyledIconCache(); ClearDockAppearanceImages();
    return true;
}

bool DesktopApp::SaveIconAppearanceReferences()
{
    snowdesktop::themes::Library recorded;
    std::string error;
    const bool saved = snowdesktop::themes::Transact(snowdesktop::themes::LibraryPath(), [&](auto& latest, auto& detail) {
        for (auto it = latest.references.begin(); it != latest.references.end();)
            if (it->first.starts_with("icon-object/")) it = latest.references.erase(it); else ++it;
        for (const auto& [target, record] : iconAppearanceOverrides_)
        {
            if (record.themeId.empty()) continue;
            const auto binding = "icon-object/" + WideToUtf8(target);
            if (binding.size() > 512) continue;
            if (!snowdesktop::themes::Select(latest, binding, record.themeId,
                snowdesktop::themes::Kind::Icon, snowdesktop::themes::Components, detail)) return false;
        }
        return true;
    }, recorded, error);
    iconAppearanceReferencesDirty_ = !saved;
    return saved;
}

void DesktopApp::ScheduleIconCustomizationCleanup()
{
    using namespace snowdesktop::icon_customization;
    if (exitRequested_ || !desktopItemsReady_ || !dataCleanupHasCompleteModel_ || reloading_ ||
        layoutReload_.Pending() || shellFileOperationInFlight_ || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive() || mouseDown_ || widgetAction_ != WidgetAction::None) return;
    const auto tick = GetTickCount64();
    if (tick < nextIconCustomizationProbe_) return;
    nextIconCustomizationProbe_ = tick + 5000;
    if (iconAppearanceReferencesDirty_) (void)SaveIconAppearanceReferences();
    std::map<std::wstring, std::string> targets;
    for (const auto& [target, record] : iconAppearanceOverrides_) { (void)record; targets.try_emplace(target); }
    for (const auto& [target, asset] : iconOverrides_) targets[target] = asset;
    if (targets.empty() && missingIconTargets_.empty()) return;
    struct Request { Observation observation; std::filesystem::path path, asset; };
    std::vector<Request> requests;
    const auto library = std::filesystem::path(GetDataSubdirectoryPath(L"icon-library"));
    for (const auto& [target, asset] : targets)
    {
        Request request;
        request.observation.target = target;
        request.observation.object = IsIconOverrideTarget(target) ? Presence::Present : Presence::Missing;
        if (target.starts_with(L"item:"))
        {
            const std::filesystem::path path(target.substr(5));
            if (path.is_absolute()) request.path = path;
        }
        if (!asset.empty()) request.asset = snowdesktop::IconAssetPath(library, asset);
        requests.push_back(std::move(request));
    }
    const auto revision = iconCustomizationRevision_;
    const auto shellRevision = shellRefreshRevision_.Current();
    shellModelWork_.Submit(L"icon-customization-cleanup", [requests = std::move(requests)] {
        std::vector<Observation> observations;
        for (auto request : requests)
        {
            if (!request.path.empty()) request.observation.object = Probe(request.path);
            if (!request.asset.empty()) request.observation.asset = Probe(request.asset);
            observations.push_back(std::move(request.observation));
        }
        return observations;
    }, [this, revision, shellRevision](const auto& observations) {
        if (exitRequested_ || reloading_ || layoutReload_.Pending() || shellFileOperationInFlight_ ||
            dragSession_.HasContext() || dragDropController_.IsTransportActive() || mouseDown_ ||
            widgetAction_ != WidgetAction::None || revision != iconCustomizationRevision_ ||
            !shellRefreshRevision_.IsCurrent(shellRevision)) return;
        const auto previousAppearanceCount = iconAppearanceOverrides_.size();
        if (!Reconcile(iconOverrides_, iconAppearanceOverrides_, missingIconTargets_, observations, NowSeconds())) return;
        iconAppearanceReferencesDirty_ |= previousAppearanceCount != iconAppearanceOverrides_.size();
        iconAppearancePreview_.Clear(); iconAppearanceTarget_.clear(); ++iconAppearanceSession_; ++iconAppearanceRevision_;
        (void)SaveLayoutSlots();
        if (iconAppearanceReferencesDirty_) (void)SaveIconAppearanceReferences();
        ClearStyledIconCache(); ClearDockAppearanceImages();
        InvalidateDragStaticScene(); InvalidateDockRects(TRUE);
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        if (floatingPopupHwnd_) InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
    }, controlHwnd_, kBackgroundShellReadyMessage);
}

std::wstring DesktopApp::ResolveIconTarget(std::wstring_view identity) const
{
    using namespace snowdesktop::icon_override;
    if (identity == start || identity == search || identity.starts_with(L"widget:")) return std::wstring(identity);
    const auto key = ToUpperInvariant(std::wstring(identity));
    if (key.find(ToUpperInvariant(kDesktopIconClsidRecycleBin)) != std::wstring::npos)
    {
        const auto state = recycleBinPollState_ ? recycleBinPollState_->hasItems.load() : -1;
        return state < 0 ? std::wstring{} : Target(key, state > 0 ? 1 : 0);
    }
    return identity.empty() ? std::wstring{} : Target(key);
}

snowdesktop::IconBeautifySettings DesktopApp::ResolveIconAppearance(std::wstring_view identity, bool media) const
{
    return snowdesktop::icon_appearance::Resolve(iconBeautifySettings_,
        FindIconAppearance(ResolveIconTarget(identity)), media, drawingIconAppearanceSource_,
        beautifyFileThumbnails_);
}
const snowdesktop::IconAppearanceRecord* DesktopApp::FindIconAppearance(std::wstring_view target) const
{
    if (missingIconTargets_.contains(std::wstring(target))) return nullptr;
    const auto found = iconAppearanceOverrides_.find(std::wstring(target));
    return iconAppearancePreview_.Effective(target, found == iconAppearanceOverrides_.end() ? nullptr : &found->second);
}

void DesktopApp::OpenIconAppearance(const std::wstring& target)
{
    if (!IsIconOverrideTarget(target) || !IsActiveIconCustomization(target)) return;
    auto route = snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::AppearanceIconBeautification);
    route.itemKey = target;
    ShowSettingsWindow(std::move(route));
}

snowdesktop::IconAppearanceSnapshot DesktopApp::EditIconAppearance(snowdesktop::IconAppearanceRequest request)
{
    using namespace snowdesktop;
    IconAppearanceSnapshot result;
    const auto repaint = [this](bool preview) {
        ClearStyledIconCache();
        // Folder sources are unstyled. Keep them while rebuilding themed
        // derivatives so preview/apply/cancel never re-enters Shell loading.
        InvalidateDragStaticScene(); InvalidateDockRects(TRUE);
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        if (floatingPopupHwnd_) InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
        if (quickNavigationOpen_) InvalidateQuickNavigationWindow();
        if (preview && hwnd_) { PresentDesktopPointerUpdate(); FlushPendingCompositionCommit(); }
    };
    // Teardown must discard the overlay even if the theme library is missing
    // or damaged, including an unexpected exit of the settings process.
    if (request.action == "close" || request.action == "cancel")
    {
        if (request.action == "close" || (request.session == iconAppearanceSession_ && request.target == iconAppearanceTarget_))
        {
            if (iconAppearancePreview_.Clear()) repaint(true);
            iconAppearanceTarget_.clear(); ++iconAppearanceSession_;
        }
        result.succeeded = true; return result;
    }
    themes::Library library; std::string error;
    if (!themes::Load(themes::LibraryPath(), library, error))
    { result.error = "iconLibrary.unavailable"; return result; }
    if (!FlushExpiredIconCustomizations()) { result.error = "iconLibrary.saveFailed"; return result; }
    const auto previous = iconAppearanceOverrides_;
    const auto previousMissing = missingIconTargets_;
    bool changed = false, previewChanged = false;
    if (request.action == "themes")
    {
        // Keep each processing strategy while updating a named theme. A
        // deleted theme detaches into its last successful local snapshot.
        for (auto& [target, record] : iconAppearanceOverrides_)
        {
            (void)target;
            if (record.themeId.empty()) continue;
            const auto theme = themes::Resolve(library.themes, record.themeId);
            if (!theme || theme->kind != themes::Kind::Icon) { record.themeId.clear(); changed = true; continue; }
            auto style = theme->icon; style.mode = record.settings.mode;
            style.preset = IconBeautifyPreset::Custom;
            if (!icon_beautify::Equal(style, record.settings)) { record.settings = style; changed = true; }
        }
    }
    else if (request.action == "begin")
    {
        if (!IsIconOverrideTarget(request.target) || !IsActiveIconCustomization(request.target)) { result.error = "iconLibrary.stale"; return result; }
        previewChanged = iconAppearancePreview_.Clear();
        iconAppearanceTarget_ = request.target; ++iconAppearanceSession_;
    }
    else if (request.action == "apply" || request.action == "inherit" ||
        request.action == "previewApply" || request.action == "previewInherit")
    {
        const bool preview = request.action == "previewApply" || request.action == "previewInherit";
        const bool independent = request.action == "apply" || request.action == "previewApply";
        const bool guarded = preview || independent || request.session != 0;
        if ((guarded && !icon_appearance::CurrentEdit(request, iconAppearanceTarget_, iconAppearanceSession_, iconAppearanceRevision_)) ||
            ((preview || independent) && (!IsIconOverrideTarget(request.target) || !IsActiveIconCustomization(request.target))))
        { result.error = "iconLibrary.stale"; return result; }
        if (!independent)
        {
            if (preview) previewChanged = iconAppearancePreview_.Update(request, iconAppearanceTarget_, iconAppearanceSession_, iconAppearanceRevision_, std::nullopt);
            else changed = iconAppearanceOverrides_.erase(request.target) != 0;
        }
        else
        {
            if (!icon_appearance::ValidTarget(request.target) || !themes::ValidIconTheme(request.settings))
            { result.error = "iconLibrary.invalid"; return result; }
            if (request.followGlobalTheme) request.themeId.clear();
            if (!request.themeId.empty())
            {
                const auto theme = themes::Resolve(library.themes, request.themeId);
                if (!theme || theme->kind != themes::Kind::Icon) { result.error = "iconLibrary.stale"; return result; }
                const int mode = request.settings.mode; request.settings = theme->icon;
                request.settings.mode = mode; request.settings.preset = IconBeautifyPreset::Custom;
            }
            IconAppearanceRecord record{request.target, IconOverrideTargetName(request.target),
                icon_beautify::Normalize(request.settings), request.themeId, true, request.followGlobalTheme};
            if (preview) previewChanged = iconAppearancePreview_.Update(request, iconAppearanceTarget_, iconAppearanceSession_, iconAppearanceRevision_, std::move(record));
            else { iconAppearanceOverrides_[request.target] = std::move(record); missingIconTargets_.erase(request.target); changed = true; }
        }
    }
    else if (request.action != "read" && request.action != "preview") { result.error = "iconLibrary.invalid"; return result; }
    if (changed)
    {
        if (!SaveLayoutSlots())
        { iconAppearanceOverrides_ = previous; missingIconTargets_ = previousMissing; result.error = "iconLibrary.saveFailed"; return result; }
        const bool referencesSaved = SaveIconAppearanceReferences();
        if (!referencesSaved)
        {
            iconAppearanceOverrides_ = previous; missingIconTargets_ = previousMissing; (void)SaveLayoutSlots();
            result.error = "iconLibrary.saveFailed"; return result;
        }
        ++iconAppearanceRevision_;
    }
    if (request.action == "apply" || (request.action == "inherit" && request.session))
    {
        previewChanged = iconAppearancePreview_.Clear() || previewChanged;
        iconAppearanceTarget_.clear(); ++iconAppearanceSession_;
    }
    if (changed || previewChanged)
        repaint(previewChanged);
    if (request.action == "previewApply" || request.action == "previewInherit")
    { result.succeeded = true; result.session = iconAppearanceSession_; result.revision = iconAppearanceRevision_; return result; }
    if (request.action != "preview") for (const auto& [target, entry] : iconAppearanceOverrides_)
    {
        if (!IsActiveIconCustomization(target)) continue;
        auto record = entry;
        record.settings = icon_appearance::Resolve(iconBeautifySettings_, &entry, false, false);
        record.available = IsIconOverrideTarget(target);
        if (record.available) record.name = IconOverrideTargetName(target);
        result.entries.push_back(std::move(record));
    }
    if (request.action != "preview") for (const auto& [id, theme] : library.themes)
        if (theme.kind == themes::Kind::Icon) result.themes.push_back({id, Utf8ToWide(theme.name), theme.icon});
    result.available = IsIconOverrideTarget(request.target);
    result.current.target = request.target;
    result.current.name = result.available ? IconOverrideTargetName(request.target) : request.target;
    result.globalSettings = iconBeautifySettings_;
    result.current.settings = iconBeautifySettings_;
    result.current.followGlobalTheme = true;
    if (const auto found = iconAppearanceOverrides_.find(request.target); found != iconAppearanceOverrides_.end())
    {
        result.current = found->second;
        result.current.settings = icon_appearance::Resolve(iconBeautifySettings_, &found->second, false, false);
        result.independent = true;
    }
    result.session = iconAppearanceSession_; result.revision = iconAppearanceRevision_;
    if (request.action == "begin" || request.action == "preview")
    {
        icon_image::Image image;
        std::string asset;
        if (request.target.starts_with(L"widget:"))
        {
            const auto index = FindWidgetIndexById(request.target.substr(7));
            if (index < widgets_.size()) asset = widgets_[index].dockIconAssetId;
        }
        else if (const auto found = iconOverrides_.find(request.target); found != iconOverrides_.end()) asset = found->second;
        if (!asset.empty())
        {
            (void)icon_image::Decode(IconAssetPath(GetDataSubdirectoryPath(L"icon-library"), asset), icon_control::ReplacementSourcePixels(request.target, 128), image, error);
            if (!image.pixels.empty()) (void)icon_image::PadSquare(image);
        }
        if (image.pixels.empty() && request.target.starts_with(L"control:"))
        {
            result.nativeControlSource = true;
            image = MakeDockControlIconSource(request.target, CurrentDockAppearance().contentTheme == 1);
        }
        if (image.pixels.empty())
        {
            const auto key = request.target.substr(request.target.find(L':') + 1);
            HBITMAP bitmap = nullptr;
            for (const auto& item : items_) if (ToUpperInvariant(item.layoutKey) == key) { bitmap = item.iconBitmap; break; }
            if (!bitmap) for (const auto& widget : widgets_) for (const auto& entry : widget.folderEntries)
                if (ToUpperInvariant(entry.fullPath) == key) { bitmap = entry.iconBitmap; break; }
            if (!bitmap) for (const auto& entry : dockFolderPopupWidget_.folderEntries)
                if (ToUpperInvariant(entry.fullPath) == key) { bitmap = entry.iconBitmap; break; }
            icon_bitmap_pixels::Buffer pixels;
            if (bitmap && icon_bitmap_pixels::ReadHBitmap(bitmap, pixels))
            { image.width = pixels.width; image.height = pixels.height; image.pixels = std::move(pixels.pixels); }
        }
        if (image.pixels.empty() && request.target.starts_with(L"widget:") && d2dContext_)
        {
            const auto index = FindWidgetIndexById(request.target.substr(7));
            if (index < widgets_.size())
            {
                DockEntry entry;
                entry.type = DockEntryTypeForWidget(widgets_[index].type);
                entry.reference = widgets_[index].id;
                image = CaptureDockIconSource(d2dContext_.Get(), entry);
            }
        }
        if (!image.pixels.empty()) (void)icon_image::EncodePng(image, result.previewPng, error);
    }
    result.succeeded = true;
    return result;
}

snowdesktop::icon_image::Image DesktopApp::MakeDockControlIconSource(std::wstring_view target, bool light) const
{
    return snowdesktop::icon_control::MakeSource(target, light);
}

namespace
{
struct RawIconCapture
{
    bool& value; bool previous;
    explicit RawIconCapture(bool& flag) : value(flag), previous(std::exchange(flag, true)) {}
    ~RawIconCapture() { value = previous; }
};
template<class Draw>
snowdesktop::icon_image::Image CaptureIconSource(ID2D1DeviceContext* ctx, int edge, Draw draw)
{
    using namespace snowdesktop;
    icon_image::Image image;
    if (!ctx) return image;
    ComPtr<ID2D1Device> device; ctx->GetDevice(&device);
    ComPtr<ID2D1DeviceContext> offscreen;
    if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &offscreen))) return image;
    ComPtr<ID2D1Bitmap1> bitmap, readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(offscreen->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(edge),static_cast<UINT32>(edge)), nullptr, 0, &props, &bitmap))) return image;
    offscreen->SetTarget(bitmap.Get()); offscreen->BeginDraw(); offscreen->Clear(D2D1::ColorF(0,0,0,0));
    draw(offscreen.Get());
    const auto drawn = offscreen->EndDraw(); offscreen->SetTarget(nullptr);
    if (FAILED(drawn)) return image;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    if (FAILED(offscreen->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(edge),static_cast<UINT32>(edge)), nullptr, 0, &props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, bitmap.Get(), nullptr))) return image;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return image;
    image.width = image.height = edge; image.pixels.resize(static_cast<size_t>(edge)*edge);
    for (int y=0;y<edge;++y) memcpy(image.pixels.data()+static_cast<size_t>(y)*edge, mapped.bits+y*mapped.pitch, static_cast<size_t>(edge)*4);
    readable->Unmap(); return image;
}
}

snowdesktop::icon_image::Image DesktopApp::CaptureDockIconSource(ID2D1DeviceContext* ctx, const DockEntry& entry)
{
    if (drawingIconAppearanceSource_) return {};
    RawIconCapture raw(drawingIconAppearanceSource_);
    constexpr int edge = snowdesktop::icon_render_rules::kMaximumSourcePixels;
    return CaptureIconSource(ctx, edge, [&](auto* offscreen) { DrawDockEntry(offscreen, entry, {0,0,edge,edge}, 0, edge); });
}

void DesktopApp::ClearDockAppearanceImages()
{
    for (const auto& [key, cached] : dockAppearanceImages_)
    { (void)key; iconGlassBackdrop_.erase(cached.bitmap.Get()); iconBitmapStyles_.erase(cached.bitmap.Get()); }
    dockAppearanceImages_.clear();
}
bool DesktopApp::DrawIndependentDockIcon(ID2D1DeviceContext* ctx, const DockEntry& entry, RECT target, int state)
{
    const auto key = L"widget:" + entry.reference;
    if (drawingIconAppearanceSource_ || (!FindIconAppearance(key) &&
        !(entry.type == DockEntryType::FolderMapping && iconBeautifySettings_.enabled))) return false;
    // Native preview sources share their Shell bitmap identities. A completed
    // async load changes the signature; a library-image completion clears it.
    std::vector<std::uintptr_t> signature;
    const auto append = [&](const DesktopWidget& widget) {
        for (const auto& itemKey : widget.itemKeys)
        {
            if (signature.size() == 4) return;
            const auto index = FindItemIndexByKey(itemKey);
            signature.push_back(index < items_.size() ? reinterpret_cast<std::uintptr_t>(items_[index].iconBitmap) : 0);
        }
        for (const auto& item : widget.folderEntries)
        { if (signature.size() == 4) return; signature.push_back(reinterpret_cast<std::uintptr_t>(item.iconBitmap)); }
    };
    const auto owner = FindWidgetIndexById(entry.reference);
    if (owner >= widgets_.size()) return false;
    if (IsGroupWidgetType(widgets_[owner].type))
        for (const auto& id : widgets_[owner].childWidgetIds)
        { const auto child = FindWidgetIndexById(id); if (child < widgets_.size()) append(widgets_[child]); if (signature.size() == 4) break; }
    else append(widgets_[owner]);
    // Match the editor source at every Dock animation size. A live target
    // bucket changes gaps/antialiasing and can flip the plate classification.
    constexpr int edge = snowdesktop::icon_render_rules::kMaximumSourcePixels;
    auto& cached = dockAppearanceImages_[key + L":" + std::to_wstring(edge)];
    if (!cached.bitmap || cached.sources != signature)
    {
        snowdesktop::icon_image::Image source;
        {
            RawIconCapture raw(drawingIconAppearanceSource_);
            source = CaptureIconSource(ctx, edge, [&](auto* offscreen) { DrawDockEntry(offscreen, entry, {0,0,edge,edge}, 0, edge); });
        }
        if (source.pixels.empty()) return false;
        const auto bitmap = snowdesktop::icon_image::CreateBitmap(source);
        const auto style = ResolveIconAppearance(key);
        auto uploaded = CreateStyledD2DBitmap(bitmap, style, true); DeleteObject(bitmap);
        if (!uploaded) return false;
        iconGlassBackdrop_.erase(cached.bitmap.Get()); iconBitmapStyles_.erase(cached.bitmap.Get());
        cached = {std::move(signature), std::move(uploaded)};
    }
    DrawIconBitmap(ctx, cached.bitmap.Get(), target, 1.f, reinterpret_cast<std::uintptr_t>(&widgets_[owner]));
    if (entry.type == DockEntryType::FolderMapping && ShouldDrawShortcutArrow(true, false)) DrawShortcutArrowOverlay(ctx, target, 1.f);
    // Badges belong to the entry and stay outside the beautified source.
    if (IsGroupDockEntryType(entry.type)) DrawDockGroupBadge(ctx, target);
    if (state == 2) DrawDockSelectionIndicator(ctx, target, CurrentDockAppearance().contentTheme == 1);
    return true;
}
