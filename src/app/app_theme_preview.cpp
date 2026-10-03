#include "app.h"
#include "../preview_png_writer.h"
#include "../widget_preview_stage.h"
#include "../status_bar_view.h"
#include "../panel_gradient_renderer.h"
#include "../taskbar_hook/taskbar_material_render.h"
#include <cstring>
#include <stdexcept>

snowdesktop::native_component_preview::Result DesktopApp::ExportThemeSurfacePreview(
    const snowdesktop::native_component_preview::Request& request)
{
    using namespace snowdesktop;
    native_component_preview::Result result; result.request = request; result.stage = "theme-surface.render";
    const auto require = [](HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("offscreen theme renderer failed"); };
    try
    {
        const auto appearance = request.theme ? request.theme->appearance : personalizationSettings_;
        const bool light = appearance.contentTheme == 1;
        items_.clear(); widgets_.clear(); dockEntries_.clear(); gridPages_.clear();
        virtualLeft_ = virtualTop_ = 0; virtualWidth_ = request.canvasWidth; virtualHeight_ = request.canvasHeight;
        layoutWorkArea_ = {0, 0, virtualWidth_, virtualHeight_};
        GridPage page; page.id = L"theme-preview"; page.bounds = page.workArea = page.visualWorkArea = layoutWorkArea_;
        page.dpiX = page.dpiY = 96; page.cellWidth = page.itemPitchWidth = kCellWidth;
        page.cellHeight = page.itemPitchHeight = kMinCellHeight; page.columns = 16; page.rows = 12; gridPages_.push_back(page);
        generalSettings_ = {}; iconBeautifySettings_ = {}; iconBeautifySettings_.enabled = false;
        lastMousePoint_ = {LONG_MIN, LONG_MIN};
        const SHSTOCKICONID icons[]{SIID_APPLICATION, SIID_WORLD, SIID_FOLDER, SIID_IMAGEFILES, SIID_DOCASSOC, SIID_AUDIOFILES};
        DesktopWidget widget; widget.type = DesktopWidgetType::Collection; widget.id = L"theme-preview";
        widget.title = L"SnowDesktop"; widget.gridCell.pageId = page.id;
        for (std::size_t i = 0; i < std::size(icons); ++i)
        {
            DesktopItem item; item.name = L"Example " + std::to_wstring(i + 1); // l10n-allow: fixed offscreen demo data
            item.layoutKey = item.parsingName = L"C:\\ThemePreview\\" + item.name;
            SHSTOCKICONINFO stock{sizeof(stock)};
            require(SHGetStockIconInfo(icons[i], SHGSI_ICON | SHGSI_LARGEICON, &stock));
            item.iconBitmap = CreateAlphaBitmapFromIcon(stock.hIcon, 48, 48, item.iconBitmapSize); DestroyIcon(stock.hIcon);
            item.iconState = IconState::FullQuality; widget.itemKeys.push_back(item.layoutKey);
            DockEntry entry; entry.type = DockEntryType::DesktopItem; entry.reference = item.layoutKey; dockEntries_.push_back(entry);
            items_.push_back(std::move(item));
        }
        widgets_.push_back(std::move(widget)); RefreshDesktopItemIndexCache();
        ComPtr<ID2D1DeviceContext> context; require(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        const auto size = D2D1::SizeU(request.canvasWidth, request.canvasHeight);
        const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        ComPtr<ID2D1Bitmap1> target; require(context->CreateBitmap(size, nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96), &target));
        auto stage = widget_preview::GenerateWallpaper(request.canvasWidth, request.canvasHeight, false);
        ComPtr<ID2D1Bitmap> backdrop; require(context->CreateBitmap(size, stage.pixels.data(), request.canvasWidth * 4,
            D2D1::BitmapProperties(format), &backdrop));
        context->SetTarget(target.Get()); context->SetDpi(96, 96);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->BeginDraw(); context->DrawBitmap(backdrop.Get());
        brushCache_.clear(); brushCacheContext_ = context.Get();
        RECT bounds{};
        const auto material = [&](RECT r, const PersonalizationSettings& p, float radius) {
            if (p.glassEnabled && !widget_preview::DrawStage(context.Get(), r,
                {light, true, p.glassBlurRadius, radius}, {request.canvasWidth, request.canvasHeight, r.left, r.top}, &stage))
                throw std::runtime_error("offscreen material backdrop failed");
        };
        if (request.component == "popup")
        {
            popupWidgetIndex_ = 0; popupHasAnchor_ = popupAnchoredToDock_ = false;
            collectionPopupAppearance_ = appearance; collectionPopupLightTheme_ = light;
            collectionPopupGlassTheme_ = appearance.glassEnabled;
            bounds = GetCollectionPopupRect(widgets_.front()); material(bounds, appearance, 18.f);
            DrawCollectionPopup(context.Get(), false); popupWidgetIndex_ = static_cast<std::size_t>(-1);
        }
        else if (request.component == "dock")
        {
            dockSettings_ = {}; dockSettings_.followComponentAppearance = false;
            dockSettings_.appearancePreset = kAppearancePresetCustom; dockSettings_.customAppearance = appearance;
            dockSettings_.position = DockPosition::Bottom;
            DockContainer dock(this, &dockEntries_, {64, request.canvasHeight / 2, request.canvasWidth - 64, request.canvasHeight / 2 + 160});
            bounds = dock.GetBounds(); material(bounds, appearance, appearance.cornerRadius);
            dock.DrawChrome(context.Get(), lastMousePoint_); dock.DrawContents(context.Get());
        }
        else
        {
            bounds = {64, request.canvasHeight / 2 - 24, request.canvasWidth - 64, request.canvasHeight / 2 + 24};
            material(bounds, appearance, 0);
            context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(bounds.left), static_cast<float>(bounds.top)));
            const UINT width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
            auto fill = appearance; fill.widgetBorderAlpha = 0; fill.widgetEdgeHighlightEnabled = false;
            DrawWidgetPanelBackground(context.Get(), {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)}, 0,
                D2D1::ColorF(fill.widgetBgR, fill.widgetBgG, fill.widgetBgB, fill.widgetAlpha), D2D1::ColorF(0, 0.f),
                false, 0, &fill, false, 0, 1);
            if (request.component == "taskbar")
            {
                taskbar_hook::TargetAppearance style;
                style.red = appearance.widgetBgR; style.green = appearance.widgetBgG; style.blue = appearance.widgetBgB; style.alpha = appearance.widgetAlpha;
                style.borderRed = appearance.widgetBorderR; style.borderGreen = appearance.widgetBorderG; style.borderBlue = appearance.widgetBorderB;
                style.borderAlpha = appearance.widgetBorderAlpha; style.edge.borderWidth = appearance.widgetBorderWidth;
                style.edge.highlightEnabled = appearance.widgetEdgeHighlightEnabled; style.edge.highlightWidth = appearance.widgetEdgeHighlightWidth;
                style.edge.highlightStrength = appearance.widgetEdgeHighlightStrength; style.edge.light = appearance.edgeLight;
                require(taskbar_hook::DrawTaskbarEdges(context.Get(), width, height, 1, style, taskbar_hook::TaskbarMaterialEdge::Top));
                // Explorer owns actual taskbar buttons. Only stock fixture icons
                // are drawn here; material and physical edge use production paths.
                for (std::size_t i = 0; i < 4; ++i)
                { const LONG x = static_cast<LONG>(width / 2 - 112 + i * 56); DrawDockEntry(context.Get(), dockEntries_[i], {x, 0, x + 48, 48}, 0); }
            }
            else
            {
                DrawStatusBarEdge(context.Get(), {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)}, appearance, 1, DockPosition::Bottom);
                StatusBarSnapshot data; data.clock = L"2026/10/03   09:09"; data.inputMethod.label = L"EN";
                StatusBarSettings settings; auto entries = BuildStatusBarItems(settings, data);
                StatusBarPalette palette{};
                require(DrawStatusBarContent(context.Get(), dwriteFactory_.Get(), entries, width, height, 1, appearance, palette));
            }
            context->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        require(context->EndDraw()); context->SetTarget(nullptr); brushCache_.clear(); brushCacheContext_ = nullptr;
        ComPtr<ID2D1Bitmap1> readback; require(context->CreateBitmap(size, nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96, 96), &readback));
        require(readback->CopyFromBitmap(nullptr, target.Get(), nullptr));
        D2D1_MAPPED_RECT mapped{}; require(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(request.canvasWidth) * request.canvasHeight);
        for (int y = 0; y < request.canvasHeight; ++y) std::memcpy(pixels.data() + static_cast<std::size_t>(y) * request.canvasWidth,
            mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(request.canvasWidth) * 4);
        readback->Unmap(); const auto path = request.outputDirectory / (request.component + ".png");
        if (!preview_png::Save(path, request.canvasWidth, request.canvasHeight, pixels, result.error)) return result;
        result.outputs.push_back({request.component, "theme", path, false, false, false, false, false, false,
            static_cast<int>(appearance.cornerRadius), bounds.right - bounds.left, bounds.bottom - bounds.top, bounds.left, bounds.top});
        result.ok = true; result.stage = "complete";
    }
    catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
