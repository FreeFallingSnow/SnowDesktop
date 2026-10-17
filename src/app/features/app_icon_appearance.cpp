#include "app/app.h"
#include "icons/icon_appearance_codec.h"
#include "icons/icon_override_rules.h"
#include "icons/icon_hbitmap_pixels.h"
#include "theme/theme_library_settings.h"
#include <d2d1_1helper.h>
#include <utility>
#include "icons/icon_control_source.h"

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
    const auto found = iconAppearanceOverrides_.find(ResolveIconTarget(identity));
    return snowdesktop::icon_appearance::Resolve(iconBeautifySettings_,
        found == iconAppearanceOverrides_.end() ? nullptr : &found->second, media, drawingIconAppearanceSource_);
}

void DesktopApp::OpenIconAppearance(const std::wstring& target)
{
    if (!IsIconOverrideTarget(target)) return;
    auto route = snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::AppearanceIconBeautification);
    route.itemKey = target;
    ShowSettingsWindow(std::move(route));
}

snowdesktop::IconAppearanceSnapshot DesktopApp::EditIconAppearance(snowdesktop::IconAppearanceRequest request)
{
    using namespace snowdesktop;
    IconAppearanceSnapshot result;
    themes::Library library; std::string error;
    if (!themes::Load(themes::LibraryPath(), library, error))
    { result.error = "iconLibrary.unavailable"; return result; }
    const auto previous = iconAppearanceOverrides_;
    bool changed = false;
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
        if (!IsIconOverrideTarget(request.target)) { result.error = "iconLibrary.stale"; return result; }
        iconAppearanceTarget_ = request.target; ++iconAppearanceSession_;
    }
    else if (request.action == "apply" || request.action == "inherit")
    {
        const bool guarded = request.action == "apply" || request.session != 0;
        if ((guarded && !icon_appearance::CurrentEdit(request, iconAppearanceTarget_, iconAppearanceSession_, iconAppearanceRevision_)) ||
            (request.action == "apply" && !IsIconOverrideTarget(request.target)))
        { result.error = "iconLibrary.stale"; return result; }
        if (request.action == "inherit") changed = iconAppearanceOverrides_.erase(request.target) != 0;
        else
        {
            if (!icon_appearance::ValidTarget(request.target) || !themes::ValidIconTheme(request.settings))
            { result.error = "iconLibrary.invalid"; return result; }
            if (!request.themeId.empty())
            {
                const auto theme = themes::Resolve(library.themes, request.themeId);
                if (!theme || theme->kind != themes::Kind::Icon) { result.error = "iconLibrary.stale"; return result; }
                const int mode = request.settings.mode; request.settings = theme->icon;
                request.settings.mode = mode; request.settings.preset = IconBeautifyPreset::Custom;
            }
            iconAppearanceOverrides_[request.target] = {request.target, IconOverrideTargetName(request.target),
                icon_beautify::Normalize(request.settings), request.themeId};
            changed = true;
        }
    }
    else if (request.action == "cancel")
    {
        if (request.session == iconAppearanceSession_ && request.target == iconAppearanceTarget_)
        { iconAppearanceTarget_.clear(); ++iconAppearanceSession_; }
    }
    else if (request.action != "read" && request.action != "preview") { result.error = "iconLibrary.invalid"; return result; }
    if (changed)
    {
        if (!SaveLayoutSlots())
        { iconAppearanceOverrides_ = previous; result.error = "iconLibrary.saveFailed"; return result; }
        themes::Library recorded;
        const bool referencesSaved = themes::Transact(themes::LibraryPath(), [&](auto& latest, auto& detail) {
            for (auto it = latest.references.begin(); it != latest.references.end();)
                if (it->first.starts_with("icon-object/")) it = latest.references.erase(it); else ++it;
            for (const auto& [target, record] : iconAppearanceOverrides_)
            {
                if (record.themeId.empty()) continue;
                const auto binding = "icon-object/" + WideToUtf8(target);
                if (binding.size() > 512) continue; // Layout snapshot still owns long Shell identities.
                if (!themes::Select(latest, binding, record.themeId, themes::Kind::Icon, themes::Components, detail)) return false;
            }
            return true;
        }, recorded, error);
        if (!referencesSaved)
        {
            iconAppearanceOverrides_ = previous; (void)SaveLayoutSlots();
            result.error = "iconLibrary.saveFailed"; return result;
        }
        ++iconAppearanceRevision_; ClearStyledIconCache();
        dockFolderBitmapCache_.Retain([](const auto&) { return false; }); dockIconWork_.Cancel(L"dock-folder:");
        InvalidateDragStaticScene(); InvalidateDockRects();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        if (floatingPopupHwnd_) InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
    }
    if (request.action == "apply" || (request.action == "inherit" && request.session))
    { iconAppearanceTarget_.clear(); ++iconAppearanceSession_; }
    if (request.action != "preview") for (const auto& [target, entry] : iconAppearanceOverrides_)
    {
        auto record = entry;
        record.available = IsIconOverrideTarget(target);
        if (record.available) record.name = IconOverrideTargetName(target);
        result.entries.push_back(std::move(record));
    }
    if (request.action != "preview") for (const auto& [id, theme] : library.themes)
        if (theme.kind == themes::Kind::Icon) result.themes.push_back({id, Utf8ToWide(theme.name), theme.icon});
    result.available = IsIconOverrideTarget(request.target);
    result.current.target = request.target;
    result.current.name = result.available ? IconOverrideTargetName(request.target) : request.target;
    result.current.settings = iconBeautifySettings_;
    if (const auto found = iconAppearanceOverrides_.find(request.target); found != iconAppearanceOverrides_.end())
    { result.current = found->second; result.independent = true; }
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
            image = MakeDockControlIconSource(request.target, CurrentDockAppearance().contentTheme == 1);
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
            for (const auto& entry : dockEntries_) if (entry.reference == request.target.substr(7))
            { image = CaptureDockIconSource(d2dContext_.Get(), entry); break; }
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
    return CaptureIconSource(ctx, 128, [&](auto* offscreen) { DrawDockEntry(offscreen, entry, {0,0,128,128}, 0, 128); });
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
    const auto found = iconAppearanceOverrides_.find(key);
    if (drawingIconAppearanceSource_ || (found == iconAppearanceOverrides_.end() &&
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
    const int edge = std::max(128, snowdesktop::icon_render_rules::SourcePixelsForTarget(target.right - target.left));
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
        const auto style = found == iconAppearanceOverrides_.end() ? iconBeautifySettings_ : found->second.settings;
        auto uploaded = CreateStyledD2DBitmap(bitmap, style, true); DeleteObject(bitmap);
        if (!uploaded) return false;
        iconGlassBackdrop_.erase(cached.bitmap.Get()); iconBitmapStyles_.erase(cached.bitmap.Get());
        cached = {std::move(signature), std::move(uploaded)};
    }
    DrawIconBitmap(ctx, cached.bitmap.Get(), target, 1.f, reinterpret_cast<std::uintptr_t>(&entry));
    if (entry.type == DockEntryType::FolderMapping && ShouldDrawShortcutArrow(true, false)) DrawShortcutArrowOverlay(ctx, target, 1.f);
    // Badges belong to the entry and stay outside the beautified source.
    if (IsGroupDockEntryType(entry.type)) DrawDockGroupBadge(ctx, target);
    if (state == 2) DrawDockSelectionIndicator(ctx, target, CurrentDockAppearance().contentTheme == 1);
    return true;
}
