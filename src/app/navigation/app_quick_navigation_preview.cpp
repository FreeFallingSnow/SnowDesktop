#include "app/app.h"
#include "settings/settings_search_index.h"
#include "app/navigation/quick_navigation_state.h"
#include "theme/personalization.h"
#include "common/l10n.h"
#include "ui/preview/native_component_preview_export.h"
#include "ui/preview/preview_png_writer.h"
#include "widget/preview/widget_preview_stage.h"
#include "common/utils.h"
#include "navigation/quick_navigation_genie_rules.h"
#include <array>
#include <cstring>
#include <stdexcept>

snowdesktop::native_component_preview::Result DesktopApp::ExportQuickNavigationPreviews(
    const snowdesktop::native_component_preview::Request& request)
{
    using namespace snowdesktop;
    native_component_preview::Result result;
    result.request = request; result.stage = "quick-navigation.render";
    const auto require = [](HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("quick navigation Direct2D rendering failed"); };
    try
    {
        quickNavigation_->preview = true; quickNavigation_->open = true;
        navigationSettings_ = NavigationSettings{};
        quickNavigation_->dpiScale = static_cast<float>(request.dpi) / 96.f;
        const int preset = request.appearance == "light" ? kAppearancePresetLight : request.appearance == "acrylic-light" ? kAppearancePresetAcrylicLight :
            request.appearance == "acrylic-dark" ? kAppearancePresetAcrylicDark : kAppearancePresetDark;
        quickNavigation_->appearance = MakeQuickNavigationAppearancePreset(preset);
        if (request.theme)
        {
            quickNavigation_->appearance = request.theme->appearance;
            snowdesktop::themes::ApplyQuickPanel(navigationSettings_, *request.theme);
        }
        if (request.contentOnly)
        {
            quickNavigation_->appearance.widgetAlpha = quickNavigation_->appearance.widgetBorderAlpha = quickNavigation_->appearance.gradientEndA = 0.f;
            quickNavigation_->appearance.widgetEdgeHighlightEnabled = quickNavigation_->appearance.glassEnabled = quickNavigation_->appearance.acrylicEnabled = false;
        }
        quickNavigation_->lightTheme = quickNavigation_->appearance.contentTheme == 1;
        quickNavigation_->glassTheme = quickNavigation_->appearance.glassEnabled;
        quickNavigation_->blurRadius = quickNavigation_->appearance.glassBlurRadius;
        generalSettings_.demoModeEnabled = false;
        iconBeautifySettings_.enabled = false;
        EnsureQuickNavTextFormats();
        virtualLeft_ = virtualTop_ = 0;
        virtualWidth_ = request.canvasWidth; virtualHeight_ = request.canvasHeight;
        layoutWorkArea_ = {0,0,virtualWidth_,virtualHeight_};
        gridPages_.clear(); GridPage page; page.bounds = page.workArea = layoutWorkArea_; gridPages_.push_back(page);
        quickNavigation_->openPoint = {virtualWidth_ / 2, virtualHeight_ / 2};
        const wchar_t* names[] = {L"SnowDesktop", L"Microsoft Edge", L"研究资料 Research library", L"文件管理器", L"Terminal", L"照片 Photos", // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            L"季度报告 — 长名称与多语言标签示例.pdf", L"音乐 Music", L"Recycle Bin", L"Visual Studio Code", L"设计草图 Design notes", L"Web documentation"}; // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
        const SHSTOCKICONID stocks[] = {SIID_APPLICATION, SIID_WORLD, SIID_FOLDER, SIID_DRIVEFIXED, SIID_APPLICATION, SIID_IMAGEFILES,
            SIID_DOCASSOC, SIID_AUDIOFILES, SIID_RECYCLER, SIID_APPLICATION, SIID_DOCNOASSOC, SIID_WORLD};
        items_.clear(); widgets_.clear(); dockEntries_.clear();
        for (size_t i = 0; i < std::size(names); ++i)
        {
            DesktopItem item; item.name = names[i]; item.parsingName = L"C:\\Preview\\" + std::wstring(names[i]) + (i == 6 ? L"" : L".lnk"); item.layoutKey = item.parsingName;
            SHSTOCKICONINFO stock{}; stock.cbSize = sizeof(stock);
            if (SUCCEEDED(SHGetStockIconInfo(stocks[i], SHGSI_ICON | SHGSI_LARGEICON, &stock)))
            {
                item.iconBitmap = CreateAlphaBitmapFromIcon(stock.hIcon, i % 3 == 0 ? 48 : 64, i % 3 == 1 ? 48 : 64, item.iconBitmapSize);
                DestroyIcon(stock.hIcon);
            }
            item.iconState = IconState::FullQuality;
            items_.push_back(std::move(item));
        }
        DesktopWidget apps; apps.type = DesktopWidgetType::Collection; apps.id = L"preview-apps"; apps.title = L"应用 Applications"; // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
        DesktopWidget files; files.type = DesktopWidgetType::Collection; files.id = L"preview-files"; files.title = L"资料 Library"; // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (i < 3) {DockEntry entry; entry.type = DockEntryType::DesktopItem; entry.reference = items_[i].layoutKey; dockEntries_.push_back(entry);}
            else (i < 6 ? apps.itemKeys : files.itemKeys).push_back(items_[i].layoutKey);
        }
        widgets_.push_back(std::move(apps)); widgets_.push_back(std::move(files));
        EnsureNavTabOrder(); RefreshDesktopItemIndexCache();
        quickNavigation_->appsIndexed = true;
        for (size_t i = 0; i < 4; ++i)
        {
            QuickNavigationAppEntry app; app.name = names[i]; app.parsingName = L"Applications\\" + app.name;
            app.iconBitmap = CopyBitmapToAlphaDib(items_[i].iconBitmap, app.iconBitmapSize);
            app.iconCacheIdentity = app.parsingName; quickNavigation_->appEntries.push_back(std::move(app));
        }
        const auto stage = request.backgroundImage.empty() ? widget_preview::GenerateWallpaper(request.canvasWidth, request.canvasHeight, false) :
            widget_preview::GenerateWallpaper(widget_preview::LoadWallpaperImage(request.backgroundImage), request.canvasWidth, request.canvasHeight);
        std::filesystem::create_directories(request.outputDirectory);
        std::array<ComPtr<ID2D1Bitmap1>, 2> genieSources;
        std::array<RECT, 2> genieSourceRects{};
        constexpr const char* scenarios[] = {"expanded-tile", "expanded-source", "expanded-initial", "collapsed-empty", "collapsed-composite",
            "expanded-mixed", "type-menu", "view-menu", "typed-app", "typed-empty", "typed-web", "typed-settings", "typed-run", "typed-calculator",
            "calculator-error", "everything-unavailable", "index-loading", "empty-results",
            "expanded-scrolled", "expanded-scrolled-tab-hover", "view-menu-scrolled", "collapsed-scrolled",
            "collapsed-prefix", "expanded-prefix", "engine-prefix", "expanded-calculator",
            "expanded-run", "expanded-web", "expanded-settings", "expanded-calculator-copy",
            "ime-calculator-preedit", "ime-run-preedit",
            "expanded-files-only", "expanded-apps-only"};
        for (const auto* scenario : scenarios)
        {
            if (request.theme && std::string_view(scenario) != "expanded-tile") continue;
            const std::string name(scenario);
            const bool scrolled = name.find("scrolled") != std::string::npos;
            if (!request.theme) navigationSettings_.layout.maximumHeight = scrolled && !name.starts_with("collapsed") ? 420 : NavigationSettings{}.layout.maximumHeight;
            quickNavigation_->collapsed = name.starts_with("collapsed") || name.starts_with("typed") || name == "calculator-error" || name == "everything-unavailable" || name == "index-loading" || name == "empty-results";
            quickNavigation_->searchType = QuickNavigationSearchType::All; quickNavigation_->searchEngine.clear();
            quickNavigation_->searchCompositionText.clear();
            navigationSettings_.desktopViewMode = name == "expanded-source" || (scrolled && !name.starts_with("collapsed")) ? QuickNavigationDesktopViewMode::Source : name == "expanded-initial" ? QuickNavigationDesktopViewMode::Initial : QuickNavigationDesktopViewMode::Tile;
            quickNavigation_->menu = name == "type-menu" ? QuickNavigationMenu::Types : name.starts_with("view-menu") ? QuickNavigationMenu::Views : QuickNavigationMenu::None;
            quickNavigation_->menuSelection = 1;
            quickNavigation_->searchText = quickNavigation_->effectiveSearchText = (name.starts_with("expanded-") && name != "expanded-mixed") || name == "collapsed-empty" || quickNavigation_->menu != QuickNavigationMenu::None ? L"" : L"e";
            quickNavigation_->settingsResults.clear(); quickNavigation_->everythingResults.clear(); quickNavigation_->appResultIndices.clear();
            quickNavigation_->scrollOffset = 0; quickNavigation_->appsExpanded = false; quickNavigation_->everythingHasMore = false;
            quickNavigation_->everythingSearchAvailable = name != "everything-unavailable"; quickNavigation_->everythingSearchPending = name == "index-loading";
            quickNavigation_->appsIndexed = name != "index-loading";
            if (name == "everything-unavailable" || name == "index-loading") quickNavigation_->effectiveSearchText = L"No matching file";
            if (name == "expanded-mixed" || name == "collapsed-composite" || name == "collapsed-scrolled") quickNavigation_->effectiveSearchText = L"SnowDesktop";
            if (name == "typed-app") quickNavigation_->searchType = QuickNavigationSearchType::App;
            if (name == "typed-empty") {quickNavigation_->searchType = QuickNavigationSearchType::Web; quickNavigation_->effectiveSearchText.clear();}
            if (name == "typed-web" || name == "expanded-web") {quickNavigation_->searchType = QuickNavigationSearchType::Web; quickNavigation_->effectiveSearchText = L"SnowDesktop 快捷导航";} // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            if (name == "typed-settings" || name == "expanded-settings")
            {
                quickNavigation_->searchType = QuickNavigationSearchType::Settings;
                quickNavigation_->effectiveSearchText = L"SnowDesktop";
                quickNavigation_->settingsResults.push_back({SettingsSearchEntryKind::StaticSetting, SettingsRoute::ForPage(SettingsPage::QuickNavigation,"quickNav.view"), "quickNav.view", _LW("quickNav.view"), _LW("quickNav.description"), _LW("quickNav.title")});
                quickNavigation_->settingsResults.push_back({SettingsSearchEntryKind::WidgetSetting, SettingsRoute::ForWidget(L"preview-widget","color"), "color", L"组件颜色 Widget color", L"Color and appearance", L"Calendar"}); // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            }
            if (name == "typed-run" || name == "expanded-run") {quickNavigation_->searchType = QuickNavigationSearchType::Run; quickNavigation_->effectiveSearchText = L"\"C:\\Program Files\\Example\\editor.exe\" --new-window";}
            if (name == "typed-calculator" || name == "calculator-error" || name == "expanded-calculator" || name == "expanded-calculator-copy") {quickNavigation_->searchType = QuickNavigationSearchType::Calculator; quickNavigation_->effectiveSearchText = name == "calculator-error" ? L"1 / 0" : L"(12.5 + 7.5) * 3 ^ 2 + 50%";}
            quickNavigation_->actionNotice = name == "expanded-calculator-copy" ? _LW("quickNav.copied") : L"";
            if (name == "empty-results") {quickNavigation_->searchType = QuickNavigationSearchType::App; quickNavigation_->effectiveSearchText = L"No matching application";}
            if (name == "collapsed-prefix" || name == "expanded-prefix") quickNavigation_->effectiveSearchText = L"ap";
            if (name == "engine-prefix") {quickNavigation_->collapsed = true; quickNavigation_->effectiveSearchText = L"goo";}
            if (name == "expanded-files-only" || name == "expanded-apps-only")
                quickNavigation_->effectiveSearchText = L"No matching desktop item";
            if (name.starts_with("ime-"))
            {
                quickNavigation_->collapsed = true;
                quickNavigation_->searchType = name == "ime-calculator-preedit" ? QuickNavigationSearchType::Calculator : QuickNavigationSearchType::Run;
                quickNavigation_->searchText.clear();
                quickNavigation_->searchCompositionText = quickNavigation_->effectiveSearchText = L"ce";
            }
            if (name == "expanded-mixed" || name == "collapsed-composite" || name == "collapsed-scrolled" || name == "typed-app" || name == "expanded-apps-only")
                for (size_t i = 0; i < (name == "typed-app" ? quickNavigation_->appEntries.size() : size_t{1}); ++i) quickNavigation_->appResultIndices.push_back(i);
            if (name == "expanded-mixed" || name == "collapsed-composite" || name == "collapsed-scrolled" || name == "expanded-files-only")
                for (int i = 0; i < 7; ++i) {QuickNavigationEverythingEntry file; file.name = (name == "expanded-mixed" || name == "collapsed-composite" ? L"SnowDesktop " : L"") + std::wstring(i % 2 ? L"Research notes — design review.pdf" : L"研究资料与长名称示例.png"); file.path = L"C:\\Preview\\Documents\\" + file.name; SHSTOCKICONINFO stock{}; stock.cbSize = sizeof(stock); if (SUCCEEDED(SHGetStockIconInfo(i % 2 ? SIID_DOCASSOC : SIID_IMAGEFILES,SHGSI_SYSICONINDEX,&stock))) file.systemIconIndex = stock.iSysImageIndex; quickNavigation_->everythingResults.push_back(std::move(file));} // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            quickNavigation_->fixedTop = false;
            if (name.starts_with("expanded-") && quickNavigation_->searchType != QuickNavigationSearchType::All)
            {
                // Reproduce locking a type with an empty query, then typing at
                // the same header anchor rather than only exporting a fresh state.
                const auto query = quickNavigation_->effectiveSearchText;
                quickNavigation_->effectiveSearchText.clear();
                quickNavigation_->anchorTop = GetQuickNavigationRect().top;
                quickNavigation_->fixedTop = true;
                quickNavigation_->effectiveSearchText = query;
            }
            quickNavigation_->rect = GetQuickNavigationRect(); quickNavigation_->hostRect = quickNavigation_->rect;
            if (UseQuickNavigationList() && !GetQuickNavigationEffectiveSearchText().empty())
            {
                const auto rows = BuildQuickNavigationListRows();
                const auto content = GetQuickNavigationContentRect(quickNavigation_->rect);
                if (!rows.empty() && content.bottom - content.top < QuickNavigationListRowHeight(rows.front()))
                    throw std::runtime_error("quick navigation first result is clipped");
            }
            quickNavigation_->listSelection = 1; quickNavigation_->keyboardTargetKind = QuickNavigationKeyboardTargetKind::Item; quickNavigation_->keyboardTargetIndex = 0;
            const RECT hover = UseQuickNavigationList() ? GetQuickNavigationListRowRect(2) : GetQuickNavigationItemRect(quickNavigation_->rect,1);
            quickNavigation_->lastMousePoint = {(hover.left + hover.right) / 2, (hover.top + hover.bottom) / 2};
            if (name == "expanded-files-only" || name == "expanded-apps-only")
            {
                // Real search, drawing and activation must agree after an empty
                // source disappears, with no phantom heading above the first row.
                const auto targets = GetQuickNavigationKeyboardTargets();
                const auto content = GetQuickNavigationContentRect(quickNavigation_->rect);
                if (!GetQuickNavigationEntries().empty() || targets.empty() ||
                    targets.front().rect.top != content.top + QuickNavScale(28) + QuickNavScale(8) + QuickNavScale(2))
                    throw std::runtime_error("empty search source reserves heading space");
                const auto& target = targets.front();
                const POINT point{(target.rect.left + target.rect.right) / 2,
                    (target.rect.top + target.rect.bottom) / 2};
                const QuickNavigationAppEntry* app = nullptr;
                QuickNavigationEverythingEntry file;
                if (name == "expanded-files-only" ? !TryGetQuickNavigationEverythingEntryAtPoint(point, file) :
                    !TryGetQuickNavigationAppEntryAtPoint(point, app))
                    throw std::runtime_error("search activation disagrees with first visible row");
                quickNavigation_->keyboardTargetKind = target.kind;
                quickNavigation_->keyboardTargetIndex = target.index;
                quickNavigation_->lastMousePoint = {LONG_MIN, LONG_MIN};
            }
            if (scrolled)
            {
                quickNavigation_->scrollOffset = std::min(QuickNavScale(60), GetQuickNavigationMaxScrollOffset(quickNavigation_->rect) / 2);
                quickNavigation_->listSelection = -1; ResetQuickNavigationKeyboardTarget();
                quickNavigation_->lastMousePoint = {LONG_MIN, LONG_MIN};
                if (name == "expanded-scrolled-tab-hover")
                {
                    const auto item = GetQuickNavigationItemRect(quickNavigation_->rect, 0);
                    const auto tabs = GetQuickNavigationTabsRect(quickNavigation_->rect);
                    quickNavigation_->lastMousePoint = {(item.left + item.right) / 2, tabs.bottom - QuickNavScale(2)};
                }
            }
            UpdateQuickNavTabWidths();
            ComPtr<ID2D1DeviceContext> context; require(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&context));
            const auto size = D2D1::SizeU(static_cast<UINT>(request.canvasWidth),static_cast<UINT>(request.canvasHeight));
            const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
            ComPtr<ID2D1Bitmap1> target; require(context->CreateBitmap(size,nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,format,96,96),&target));
            context->SetTarget(target.Get()); context->SetDpi(96,96); context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            context->BeginDraw(); context->Clear(D2D1::ColorF(0,0.f));
            if (!request.transparent && !request.contentOnly)
            {
                ComPtr<ID2D1Bitmap> backdrop;
                require(context->CreateBitmap(size,stage.pixels.data(),static_cast<UINT>(stage.width * 4),D2D1::BitmapProperties(format),&backdrop));
                context->DrawBitmap(backdrop.Get());
                if (quickNavigation_->glassTheme)
                    if (!widget_preview::DrawStage(context.Get(),quickNavigation_->rect, {quickNavigation_->lightTheme,true,quickNavigation_->blurRadius,static_cast<float>(QuickNavScale(navigationSettings_.layout.cornerRadius))},
                            {request.canvasWidth,request.canvasHeight,quickNavigation_->rect.left,quickNavigation_->rect.top}, &stage))
                        throw std::runtime_error("quick navigation material backdrop failed");
            }
            brushCache_.clear(); brushCacheContext_ = context.Get();
            DrawQuickNavigationSurface(context.Get());
            require(context->EndDraw()); context->SetTarget(nullptr); brushCache_.clear(); brushCacheContext_ = nullptr;
            if (name == "expanded-tile" || name == "collapsed-empty")
            {
                const std::size_t fixture = name == "expanded-tile" ? 0 : 1;
                const auto panelSize = D2D1::SizeU(
                    static_cast<UINT>(quickNavigation_->rect.right - quickNavigation_->rect.left),
                    static_cast<UINT>(quickNavigation_->rect.bottom - quickNavigation_->rect.top));
                require(context->CreateBitmap(panelSize, nullptr, 0,
                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96),
                    &genieSources[fixture]));
                genieSourceRects[fixture] = quickNavigation_->rect;
                context->SetTarget(genieSources[fixture].Get());
                context->SetTransform(D2D1::Matrix3x2F::Translation(
                    static_cast<float>(-quickNavigation_->rect.left),
                    static_cast<float>(-quickNavigation_->rect.top)));
                context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
                brushCacheContext_ = context.Get();
                DrawQuickNavigationSurface(context.Get());
                require(context->EndDraw());
                context->SetTarget(nullptr);
                context->SetTransform(D2D1::Matrix3x2F::Identity());
                brushCache_.clear(); brushCacheContext_ = nullptr;
            }
            ComPtr<ID2D1Bitmap1> readback;
            require(context->CreateBitmap(size,nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,format,96,96),&readback));
            require(readback->CopyFromBitmap(nullptr,target.Get(),nullptr));
            D2D1_MAPPED_RECT mapped{}; require(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped));
            std::vector<std::uint32_t> pixels(static_cast<size_t>(request.canvasWidth) * request.canvasHeight);
            for (int y = 0; y < request.canvasHeight; ++y)
                std::memcpy(pixels.data() + static_cast<size_t>(y) * request.canvasWidth,mapped.bits + static_cast<size_t>(y) * mapped.pitch,static_cast<size_t>(request.canvasWidth) * 4);
            readback->Unmap();
            const auto path = request.outputDirectory / ("quick-navigation-" + name + ".png");
            if (!preview_png::Save(path,request.canvasWidth,request.canvasHeight,pixels,result.error)) throw std::runtime_error("cannot write quick navigation preview");
            const auto bounds = quickNavigation_->rect;
            result.outputs.push_back({request.component,name,path,UseQuickNavigationList(),false,false,false,false,true,navigationSettings_.layout.cornerRadius,
                bounds.right-bounds.left,bounds.bottom-bounds.top,bounds.left,bounds.top});
        }
        // These diagnostic frames rasterize real panel content with the same
        // perspective matrices and destination clips as DComp. They exercise
        // internal alpha joins, not the live DWM backdrop or animation timing.
        namespace genie = dock_genie;
        namespace navigation = quick_navigation_animation_rules;
        struct GenieFixture
        {
            const char* name;
            std::size_t source;
            double collapsed;
            bool alphaProbe = false;
        };
        constexpr GenieFixture genieFixtures[] = {
            {"genie-expanded-pull", 0, 0.12}, {"genie-expanded-mid", 0, 0.33},
            {"genie-expanded-travel", 0, 0.55}, {"genie-expanded-late", 0, 0.81},
            {"genie-collapsed-mid", 1, 0.33}, {"genie-collapsed-late", 1, 0.81},
            {"genie-alpha-expanded", 0, 0.2096, true},
            {"genie-alpha-collapsed", 1, 0.2994, true},
            {"genie-pole-expanded", 0, 0.55, true},
            {"genie-pole-collapsed", 1, 0.55, true}};
        for (const auto& fixture : genieFixtures)
        {
            if (request.theme) break;
            ComPtr<ID2D1DeviceContext> context;
            require(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
            const auto size = D2D1::SizeU(static_cast<UINT>(request.canvasWidth),
                static_cast<UINT>(request.canvasHeight));
            const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED);
            ComPtr<ID2D1Bitmap1> target;
            require(context->CreateBitmap(size, nullptr, 0,
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96), &target));
            context->SetTarget(target.Get()); context->SetDpi(96, 96);
            context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
            if (!fixture.alphaProbe && !request.transparent && !request.contentOnly)
            {
                ComPtr<ID2D1Bitmap> backdrop;
                require(context->CreateBitmap(size, stage.pixels.data(),
                    static_cast<UINT>(stage.width * 4), D2D1::BitmapProperties(format), &backdrop));
                context->DrawBitmap(backdrop.Get());
            }
            const auto bounds = genieSourceRects[fixture.source];
            const double width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
            ComPtr<ID2D1Bitmap1> source = genieSources[fixture.source];
            if (fixture.alphaProbe)
            {
                // A constant 40% alpha rectangle exposes missing or doubled
                // coverage in readback without text, icons or backdrop noise.
                const std::vector<std::uint32_t> uniform(
                    static_cast<std::size_t>(width * height), 0x66284455);
                require(context->CreateBitmap(D2D1::SizeU(static_cast<UINT>(width),
                    static_cast<UINT>(height)), uniform.data(), static_cast<UINT>(width) * 4,
                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, format, 96, 96), &source));
            }
            const genie::Rect panel{static_cast<double>(bounds.left), static_cast<double>(bounds.top),
                static_cast<double>(bounds.right), static_cast<double>(bounds.bottom)};
            const double dockSize = QuickNavScale(48);
            const double dockLeft = (request.canvasWidth - dockSize) * 0.5;
            const double dockTop = request.canvasHeight - dockSize - QuickNavScale(12);
            const genie::Rect dock{dockLeft, dockTop, dockLeft + dockSize, dockTop + dockSize};
            for (std::size_t band = 0; band < genie::StripCount; ++band)
            {
                const auto clip = navigation::GenieRasterBandClip(panel, dock, genie::Edge::Bottom,
                    fixture.collapsed, width, height, band, 0, 0,
                    request.canvasWidth, request.canvasHeight);
                if (clip.bottom <= clip.top) continue;
                const auto projection = navigation::GenieProjection(panel, dock, genie::Edge::Bottom,
                    fixture.collapsed, width, height, band);
                const auto crop = navigation::GenieSourceBandClip(projection, clip,
                    genie::Edge::Bottom, width, height);
                const auto sourceRect = D2D1::RectF(static_cast<float>(crop.left),
                    static_cast<float>(crop.top), static_cast<float>(crop.right),
                    static_cast<float>(crop.bottom));
                const auto destinationRect = D2D1::RectF(sourceRect.left - projection.sourceX,
                    sourceRect.top - projection.sourceY, sourceRect.right - projection.sourceX,
                    sourceRect.bottom - projection.sourceY);
                const D2D1_MATRIX_4X4_F matrix{
                    projection.m11, projection.m12, 0.f, projection.m14,
                    projection.m21, projection.m22, 0.f, projection.m24,
                    0.f, 0.f, 1.f, 0.f,
                    projection.m41, projection.m42, 0.f, projection.m44};
                context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(clip.left),
                    static_cast<float>(clip.top), static_cast<float>(clip.right),
                    static_cast<float>(clip.bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
                context->DrawBitmap(source.Get(), &destinationRect,
                    1.f, D2D1_INTERPOLATION_MODE_LINEAR, &sourceRect, &matrix);
                context->PopAxisAlignedClip();
            }
            require(context->EndDraw()); context->SetTarget(nullptr);
            ComPtr<ID2D1Bitmap1> readback;
            require(context->CreateBitmap(size, nullptr, 0,
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ |
                    D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96, 96), &readback));
            require(readback->CopyFromBitmap(nullptr, target.Get(), nullptr));
            D2D1_MAPPED_RECT mapped{}; require(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped));
            std::vector<std::uint32_t> pixels(static_cast<std::size_t>(request.canvasWidth) * request.canvasHeight);
            for (int y = 0; y < request.canvasHeight; ++y)
                std::memcpy(pixels.data() + static_cast<std::size_t>(y) * request.canvasWidth,
                    mapped.bits + static_cast<std::size_t>(y) * mapped.pitch,
                    static_cast<std::size_t>(request.canvasWidth) * 4);
            readback->Unmap();
            const auto path = request.outputDirectory /
                (std::string("quick-navigation-") + fixture.name + ".png");
            if (!preview_png::Save(path, request.canvasWidth, request.canvasHeight, pixels, result.error))
                throw std::runtime_error("cannot write quick navigation Genie frame");
            result.outputs.push_back({request.component, fixture.name, path, fixture.source == 1,
                false, false, false, false, true, navigationSettings_.layout.cornerRadius,
                bounds.right - bounds.left, bounds.bottom - bounds.top, bounds.left, bounds.top});
        }
        result.ok = true; result.stage = "complete";
    }
    catch (const std::exception& error) {result.error = error.what();}
    quickNavigation_->open = false; quickNavigation_->preview = false;
    return result;
}
