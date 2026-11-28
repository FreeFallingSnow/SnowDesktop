#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "common/l10n.h"
#include "ui/preview/native_component_preview_export.h"
#include "common/utils.h"
#include "ui/render/app_font.h"
#include "ui/preview/preview_png_writer.h"
#include "widget/preview/widget_preview_stage.h"
#include "system/status_bar/status_bar_view.h"
#include "system/status_bar/status_bar_layout.h"
#include "theme/panel_gradient_renderer.h"
#include "taskbar_hook/taskbar_material_render.h"
#include "demo/scene.h"
#include "demo/artifact_io.h"
#include "ui/preview/video_writer.h"
#include "widget/preview/widget_preview_scene.h"
#include <algorithm>
#include <unordered_map>
#include <set>
#include <cstring>
#include <stdexcept>

snowdesktop::native_component_preview::Result DesktopApp::ExportThemeSurfacePreview(
    const snowdesktop::native_component_preview::Request& request)
{
    using namespace snowdesktop;
    native_component_preview::Result result; result.request = request; result.stage = "theme-surface.render";
    const auto require = [](HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("offscreen theme renderer failed"); };
#include "app_demo_scene_render.inc"
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
        if (request.component == "popup-group")
        {
            DesktopWidget group = widgets_.front();
            group.type = DesktopWidgetType::FileGroup;
            group.title = _LW("app.menu.file_group");
            group.itemKeys.clear();
            group.gridSpan = {3, 3};
            group.showSearchBox = group.showFileCategories = true;
            group.childWidgetIds = {L"theme-preview-folder", L"theme-preview-files"};
            group.activeCategoryId = group.childWidgetIds.front();
            DesktopWidget folder;
            folder.id = group.childWidgetIds[0];
            folder.type = DesktopWidgetType::FolderMapping;
            folder.title = _LW("app.widget_preview.folder");
            folder.gridCell.pageId = page.id;
            folder.gridSpan = {3, 3};
            DesktopWidget files;
            files.id = group.childWidgetIds[1];
            files.type = DesktopWidgetType::FileCategories;
            files.title = _LW("app.menu.file_categories");
            files.gridCell.pageId = page.id;
            files.gridSpan = {3, 3};
            files.itemKeys = widgets_.front().itemKeys;
            widgets_.clear();
            widgets_.reserve(3);
            widgets_.push_back(std::move(group));
            widgets_.push_back(std::move(folder));
            widgets_.push_back(std::move(files));
            containers_.push_back(std::make_unique<FileGroup>(&widgets_[0], this));
            containers_.push_back(std::make_unique<FolderMapping>(&widgets_[1], this));
            containers_.push_back(std::make_unique<FileCategories>(&widgets_[2], this));
        }
        ComPtr<ID2D1DeviceContext> context; require(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        const auto size = D2D1::SizeU(request.canvasWidth, request.canvasHeight);
        const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        ComPtr<ID2D1Bitmap1> target; require(context->CreateBitmap(size, nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96), &target));
        auto stage = request.backgroundImage.empty() ?
            widget_preview::GenerateWallpaper(request.canvasWidth, request.canvasHeight, false) :
            widget_preview::GenerateWallpaper(widget_preview::LoadWallpaperImage(request.backgroundImage), request.canvasWidth, request.canvasHeight);
        ComPtr<ID2D1Bitmap> backdrop; require(context->CreateBitmap(size, stage.pixels.data(), request.canvasWidth * 4,
            D2D1::BitmapProperties(format), &backdrop));
        context->SetTarget(target.Get()); context->SetDpi(96, 96);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->BeginDraw(); context->DrawBitmap(backdrop.Get());
        drawing_.Reset();
        RECT bounds{};
        const auto material = [&](RECT r, const PersonalizationSettings& p, float radius) {
            if (p.glassEnabled && !widget_preview::DrawStage(context.Get(), r,
                {light, true, p.glassBlurRadius, radius}, {request.canvasWidth, request.canvasHeight, r.left, r.top}, &stage))
                throw std::runtime_error("offscreen material backdrop failed");
        };
        const auto statusData = [] {
            StatusBarSnapshot data; data.clock = L"2026/10/04   09:09"; data.inputMethod.label = L"EN";
            data.notifications.quiet = false; data.notifications.unreadCount = 3; data.notifications.totalCount = 3;
            widget_runtime::WidgetNetworkStatusDataSnapshot network;
            network.available = true; network.connectivity = "internet"; network.transport = "ethernet"; data.network = network;
            widget_runtime::WidgetAudioOutputVolumeDataSnapshot audio; audio.available = true; audio.volume = .42; data.audio = audio;
            widget_runtime::WidgetPowerDataSnapshot power; power.available = true; power.batteryPercent = 73; data.power = power;
            return data;
        };
        const auto statusContent = [&](RECT frame, bool merged) {
            context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(frame.left), static_cast<float>(frame.top)));
            StatusBarSettings settings; settings.position = merged ? DockPosition::Bottom : DockPosition::Top;
            auto entries = BuildStatusBarItems(settings, statusData());
            require(DrawStatusBarContent(context.Get(), dwriteFactory_.Get(), entries,
                frame.right - frame.left, frame.bottom - frame.top, 1, appearance, {}, {}, false, 0, merged));
            context->SetTransform(D2D1::Matrix3x2F::Identity());
        };
        const auto stripMaterial = [&](RECT frame, DockPosition position) {
            material(frame, appearance, 0);
            const auto fill = StatusBarFillAppearance(appearance);
            DrawWidgetPanelBackground(context.Get(), frame, 0,
                D2D1::ColorF(fill.widgetBgR, fill.widgetBgG, fill.widgetBgB, fill.widgetAlpha), D2D1::ColorF(0, 0.f),
                false, 0, &fill, false, 0, 1);
            DrawStatusBarEdge(context.Get(), frame, appearance, 1, position);
        };
        if (request.component == "popup" || request.component == "popup-group")
        {
            popupWidgetIndex_ = 0; popupHasAnchor_ = popupAnchoredToDock_ = false;
            collectionPopupAppearance_ = appearance; collectionPopupLightTheme_ = light;
            collectionPopupGlassTheme_ = appearance.glassEnabled;
            bounds = GetCollectionPopupRect(widgets_.front()); material(bounds, appearance, 18.f);
            // Keep overflow tracks visible in this isolated renderer so a
            // constrained canvas can inspect popup content/scrollbar spacing.
            popupScrollbarDragging_ = true;
            DrawCollectionPopup(context.Get(), false);
            popupScrollbarDragging_ = false;
            popupWidgetIndex_ = static_cast<std::size_t>(-1);
        }
        else if (request.component == "dock" || request.component == "dock-window-titles")
        {
            const bool windowTitles = request.component == "dock-window-titles";
            dockSettings_ = {}; dockSettings_.followComponentAppearance = false;
            dockSettings_.showWindowTitles = windowTitles;
            if (windowTitles)
            {
                const auto first = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1));
                const auto second = reinterpret_cast<HWND>(static_cast<UINT_PTR>(2));
                const auto third = reinterpret_cast<HWND>(static_cast<UINT_PTR>(3));
                const auto fourth = reinterpret_cast<HWND>(static_cast<UINT_PTR>(4));
                dockTaskWindowTitles_ = {{first, L"设置"}, {second, L"Settings and configuration"}, // l10n-allow: isolated Dock preview window titles.
                    {third, L"Roadmap and release checklist for the next version"}, // l10n-allow: isolated Dock preview window title.
                    {fourth, L"项目配置和下一版本的发布检查清单"}}; // l10n-allow: isolated Dock preview window title.
                dockRunningWindows_[DockItemWindowKey(items_[0])] = {first, false, true, false, {first, second, fourth}, {}};
                dockRunningWindows_[DockItemWindowKey(items_[1])] = {third, false, true, false, {third}, {}};
            }
            dockSettings_.appearancePreset = kAppearancePresetCustom; dockSettings_.customAppearance = appearance;
            dockSettings_.position = DockPosition::Bottom; dockSettings_.thicknessScale = .75f;
            const LONG width = std::min(896, request.canvasWidth - 64), left = (request.canvasWidth - width) / 2;
            const int rows = windowTitles ? 6 : 3;
            const LONG top = std::max(24, (request.canvasHeight - rows * 144) / 2);
            bounds = {left, top, left + width, top + rows * 144};
            ComPtr<IDWriteTextFormat> labelFormat;
            require(app_fonts::CreateTextFormat(dwriteFactory_.Get(), L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18, L"en-US", &labelFormat));
            const char* labels[]{"app.dock.island", "app.dock.edge", "settings.desktopStyle.merged.title",
                "settings.animation.option.wave", "settings.animation.option.wave", "settings.dock.showWindowTitles"};
            for (int row = 0; row < rows; ++row)
            {
                const LONG y = top + row * 144;
                std::wstring label = _LW(labels[row]);
                if (windowTitles && row == 1)
                    label += L" · " + std::wstring(_LW("settings.dock.windowTitles.multiple"));
                const bool waveSample = windowTitles && row >= 2 && row <= 4;
                const wchar_t* waveMarkers[]{L" ◂", L" ●", L" ▸"};
                if (waveSample)
                    label = std::wstring(_LW("app.settings.icon_beautify")) + L" · " + _LW("settings.animation.option.wave") +
                        waveMarkers[row - 2];
                DrawD2DTextEllipsis(context.Get(), label, {left + 8, y, left + width, y + 28},
                    labelFormat.Get(), D2D1::ColorF(0xf0f3f7));
                dockSettings_.edgeAttached = windowTitles ? row == 1 : row != 0;
                dockSettings_.edgeCentered = windowTitles && row == 1;
                dockSettings_.windowTitlesOnlyMultiple = windowTitles && row == 1;
                iconBeautifySettings_.enabled = waveSample;
                iconBeautifySettings_.outlineEnabled = iconBeautifySettings_.enabled;
                RECT area{left, y + 40, left + width, y + 132};
                if (row == 2 && !windowTitles)
                {
                    const RECT strip{left, y + 68, left + width, y + 132};
                    stripMaterial(strip, DockPosition::Bottom); statusContent(strip, true);
                    area = MergedStatusBarCenter(width, strip.bottom - strip.top, 1);
                    OffsetRect(&area, left, strip.top);
                    // The live merged strip paints chrome once. Reuse the
                    // Dock's content renderer inside the same central reservation.
                    DockContainer dock(this, &dockEntries_, area); dock.DrawContents(context.Get());
                }
                else
                {
                    DockContainer dock(this, &dockEntries_, area);
                    if (windowTitles && row == 5)
                    {
                        dockSettings_.showWindowTitles = false;
                        dock.GetSlots();
                        dockSettings_.showWindowTitles = true;
                        dock.InvalidateSlots();
                        dock.GetSlots();
                        dock.AdvanceMagnificationAnimation(UiAnimationScheduler::MonotonicMilliseconds() + 48.0);
                    }
                    if (waveSample)
                    {
                        const auto targets = dock.KeyboardTargets();
                        const auto title = std::ranges::find_if(targets, [](const auto& target) {
                            return target.kind == DockContainer::KeyboardTarget::Kind::WindowTitle && target.index == 2;
                        });
                        if (title != targets.end())
                        {
                            const RECT cell = title->bounds;
                            const LONG positions[]{cell.left + 3, (cell.left + cell.right) / 2, cell.right - 3};
                            lastMousePoint_ = {positions[row - 2], (cell.top + cell.bottom) / 2};
                            dock.GetVisualPanelBounds(lastMousePoint_);
                            dock.AdvanceMagnificationAnimation(UiAnimationScheduler::MonotonicMilliseconds() + 1000.0);
                        }
                    }
                    const auto frame = dock.GetVisualPanelBounds(lastMousePoint_);
                    material(frame, appearance, dockSettings_.edgeAttached ? 0.f : appearance.cornerRadius);
                    dock.DrawChrome(context.Get(), lastMousePoint_); dock.DrawContents(context.Get());
                    lastMousePoint_ = {LONG_MIN, LONG_MIN};
                }
            }
        }
        else
        {
            // A fixed segment keeps text and physical edges legible in the
            // six-surface cover without altering production drawing metrics.
            bounds = request.component == "status-bar" ? RECT{32, 32, request.canvasWidth - 32, 80} :
                RECT{64, request.canvasHeight / 2 - 24,
                    64 + std::min<LONG>(768, request.canvasWidth - 128), request.canvasHeight / 2 + 24};
            material(bounds, appearance, 0);
            context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(bounds.left), static_cast<float>(bounds.top)));
            const UINT width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
            const auto fill = StatusBarFillAppearance(appearance);
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
                DrawStatusBarEdge(context.Get(), {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)}, appearance, 1, DockPosition::Top);
                context->SetTransform(D2D1::Matrix3x2F::Identity());
                statusContent(bounds, false);
            }
            context->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        const HRESULT drawResult = context->EndDraw();
        context->SetTarget(nullptr);
        drawing_.Reset();
        require(drawResult);
        ComPtr<ID2D1Bitmap1> readback; require(context->CreateBitmap(size, nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96, 96), &readback));
        require(readback->CopyFromBitmap(nullptr, target.Get(), nullptr));
        D2D1_MAPPED_RECT mapped{}; require(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(request.canvasWidth) * request.canvasHeight);
        for (int y = 0; y < request.canvasHeight; ++y) std::memcpy(pixels.data() + static_cast<std::size_t>(y) * request.canvasWidth,
            mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(request.canvasWidth) * 4);
        readback->Unmap(); const auto path = request.outputDirectory / (request.component + ".png");
        if (!preview_png::Save(path, request.canvasWidth, request.canvasHeight, pixels, result.error)) return result;
        const bool groupControls = request.component == "popup-group";
        result.outputs.push_back({request.component, "theme", path, false, false, false, false, groupControls, groupControls,
            static_cast<int>(appearance.cornerRadius), bounds.right - bounds.left, bounds.bottom - bounds.top, bounds.left, bounds.top});
        result.ok = true; result.stage = "complete";
    }
    catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
