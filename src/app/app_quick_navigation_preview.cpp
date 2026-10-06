#include "app.h"
#include "../preview_png_writer.h"
#include "../widget_preview_stage.h"
#include "../utils.h"
#include "quick_navigation_genie_rules.h"
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
        quickNavigationPreview_ = true; quickNavigationOpen_ = true;
        navigationSettings_ = NavigationSettings{};
        quickNavDpiScale_ = static_cast<float>(request.dpi) / 96.f;
        const int preset = request.appearance == "light" ? kAppearancePresetLight : request.appearance == "acrylic-light" ? kAppearancePresetAcrylicLight :
            request.appearance == "acrylic-dark" ? kAppearancePresetAcrylicDark : kAppearancePresetDark;
        quickNavAppearance_ = MakeQuickNavigationAppearancePreset(preset);
        if (request.theme)
        {
            quickNavAppearance_ = request.theme->appearance;
            snowdesktop::themes::ApplyQuickPanel(navigationSettings_, *request.theme);
        }
        if (request.contentOnly)
        {
            quickNavAppearance_.widgetAlpha = quickNavAppearance_.widgetBorderAlpha = quickNavAppearance_.gradientEndA = 0.f;
            quickNavAppearance_.widgetEdgeHighlightEnabled = quickNavAppearance_.glassEnabled = quickNavAppearance_.acrylicEnabled = false;
        }
        quickNavLightTheme_ = quickNavAppearance_.contentTheme == 1;
        quickNavGlassTheme_ = quickNavAppearance_.glassEnabled;
        quickNavBlurRadius_ = quickNavAppearance_.glassBlurRadius;
        generalSettings_.demoModeEnabled = false;
        iconBeautifySettings_.enabled = false;
        EnsureQuickNavTextFormats();
        virtualLeft_ = virtualTop_ = 0;
        virtualWidth_ = request.canvasWidth; virtualHeight_ = request.canvasHeight;
        layoutWorkArea_ = {0,0,virtualWidth_,virtualHeight_};
        gridPages_.clear(); GridPage page; page.bounds = page.workArea = layoutWorkArea_; gridPages_.push_back(page);
        quickNavigationOpenPoint_ = {virtualWidth_ / 2, virtualHeight_ / 2};
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
        quickNavigationAppsIndexed_ = true;
        for (size_t i = 0; i < 4; ++i)
        {
            QuickNavigationAppEntry app; app.name = names[i]; app.parsingName = L"Applications\\" + app.name;
            app.iconBitmap = CopyBitmapToAlphaDib(items_[i].iconBitmap, app.iconBitmapSize);
            app.iconCacheIdentity = app.parsingName; quickNavigationAppEntries_.push_back(std::move(app));
        }
        const auto stage = request.backgroundImage.empty() ? widget_preview::GenerateWallpaper(request.canvasWidth, request.canvasHeight, false) :
            widget_preview::GenerateWallpaper(widget_preview::LoadWallpaperImage(request.backgroundImage), request.canvasWidth, request.canvasHeight);
        std::filesystem::create_directories(request.outputDirectory);
        std::array<ComPtr<ID2D1Bitmap1>, 2> genieSources;
        std::array<RECT, 2> genieSourceRects{};
        constexpr const char* scenarios[] = {"expanded-tile", "expanded-source", "expanded-initial", "collapsed-empty", "collapsed-composite",
            "expanded-mixed", "type-menu", "view-menu", "typed-app", "typed-empty", "typed-web", "typed-settings", "typed-run", "typed-calculator",
            "calculator-error", "everything-unavailable", "index-loading", "empty-results",
            "expanded-scrolled", "expanded-scrolled-tab-hover", "collapsed-scrolled",
            "collapsed-prefix", "expanded-prefix", "engine-prefix", "expanded-calculator",
            "expanded-run", "expanded-web", "expanded-settings", "expanded-calculator-copy",
            "ime-calculator-preedit", "ime-run-preedit",
            "expanded-files-only", "expanded-apps-only"};
        for (const auto* scenario : scenarios)
        {
            if (request.theme && std::string_view(scenario) != "expanded-tile") continue;
            const std::string name(scenario);
            const bool scrolled = name.find("scrolled") != std::string::npos;
            if (!request.theme) navigationSettings_.layout.maximumHeight = scrolled && name.starts_with("expanded") ? 420 : NavigationSettings{}.layout.maximumHeight;
            quickNavigationCollapsed_ = name.starts_with("collapsed") || name.starts_with("typed") || name == "calculator-error" || name == "everything-unavailable" || name == "index-loading" || name == "empty-results";
            quickNavigationSearchType_ = QuickNavigationSearchType::All; quickNavigationSearchEngine_.clear();
            quickNavigationSearchCompositionText_.clear();
            navigationSettings_.desktopViewMode = name == "expanded-source" || (scrolled && name.starts_with("expanded")) ? QuickNavigationDesktopViewMode::Source : name == "expanded-initial" ? QuickNavigationDesktopViewMode::Initial : QuickNavigationDesktopViewMode::Tile;
            quickNavigationMenu_ = name == "type-menu" ? QuickNavigationMenu::Types : name == "view-menu" ? QuickNavigationMenu::Views : QuickNavigationMenu::None;
            quickNavigationMenuSelection_ = 1;
            quickNavigationSearchText_ = quickNavigationEffectiveSearchText_ = (name.starts_with("expanded-") && name != "expanded-mixed") || name == "collapsed-empty" || quickNavigationMenu_ != QuickNavigationMenu::None ? L"" : L"e";
            quickNavigationSettingsResults_.clear(); quickNavigationEverythingResults_.clear(); quickNavigationAppResultIndices_.clear();
            quickNavigationScrollOffset_ = 0; quickNavigationAppsExpanded_ = false; quickNavigationEverythingHasMore_ = false;
            everythingSearchAvailable_ = name != "everything-unavailable"; quickNavigationEverythingSearchPending_ = name == "index-loading";
            quickNavigationAppsIndexed_ = name != "index-loading";
            if (name == "everything-unavailable" || name == "index-loading") quickNavigationEffectiveSearchText_ = L"No matching file";
            if (name == "expanded-mixed" || name == "collapsed-composite" || name == "collapsed-scrolled") quickNavigationEffectiveSearchText_ = L"SnowDesktop";
            if (name == "typed-app") quickNavigationSearchType_ = QuickNavigationSearchType::App;
            if (name == "typed-empty") {quickNavigationSearchType_ = QuickNavigationSearchType::Web; quickNavigationEffectiveSearchText_.clear();}
            if (name == "typed-web" || name == "expanded-web") {quickNavigationSearchType_ = QuickNavigationSearchType::Web; quickNavigationEffectiveSearchText_ = L"SnowDesktop 快捷导航";} // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            if (name == "typed-settings" || name == "expanded-settings")
            {
                quickNavigationSearchType_ = QuickNavigationSearchType::Settings;
                quickNavigationEffectiveSearchText_ = L"SnowDesktop";
                quickNavigationSettingsResults_.push_back({SettingsSearchEntryKind::StaticSetting, SettingsRoute::ForPage(SettingsPage::QuickNavigation,"quickNav.view"), "quickNav.view", _LW("quickNav.view"), _LW("quickNav.description"), _LW("quickNav.title")});
                quickNavigationSettingsResults_.push_back({SettingsSearchEntryKind::WidgetSetting, SettingsRoute::ForWidget(L"preview-widget","color"), "color", L"组件颜色 Widget color", L"Color and appearance", L"Calendar"}); // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            }
            if (name == "typed-run" || name == "expanded-run") {quickNavigationSearchType_ = QuickNavigationSearchType::Run; quickNavigationEffectiveSearchText_ = L"\"C:\\Program Files\\Example\\editor.exe\" --new-window";}
            if (name == "typed-calculator" || name == "calculator-error" || name == "expanded-calculator" || name == "expanded-calculator-copy") {quickNavigationSearchType_ = QuickNavigationSearchType::Calculator; quickNavigationEffectiveSearchText_ = name == "calculator-error" ? L"1 / 0" : L"(12.5 + 7.5) * 3 ^ 2 + 50%";}
            quickNavigationActionNotice_ = name == "expanded-calculator-copy" ? _LW("quickNav.copied") : L"";
            if (name == "empty-results") {quickNavigationSearchType_ = QuickNavigationSearchType::App; quickNavigationEffectiveSearchText_ = L"No matching application";}
            if (name == "collapsed-prefix" || name == "expanded-prefix") quickNavigationEffectiveSearchText_ = L"ap";
            if (name == "engine-prefix") {quickNavigationCollapsed_ = true; quickNavigationEffectiveSearchText_ = L"goo";}
            if (name == "expanded-files-only" || name == "expanded-apps-only")
                quickNavigationEffectiveSearchText_ = L"No matching desktop item";
            if (name.starts_with("ime-"))
            {
                quickNavigationCollapsed_ = true;
                quickNavigationSearchType_ = name == "ime-calculator-preedit" ? QuickNavigationSearchType::Calculator : QuickNavigationSearchType::Run;
                quickNavigationSearchText_.clear();
                quickNavigationSearchCompositionText_ = quickNavigationEffectiveSearchText_ = L"ce";
            }
            if (name == "expanded-mixed" || name == "collapsed-composite" || name == "collapsed-scrolled" || name == "typed-app" || name == "expanded-apps-only")
                for (size_t i = 0; i < (name == "typed-app" ? quickNavigationAppEntries_.size() : size_t{1}); ++i) quickNavigationAppResultIndices_.push_back(i);
            if (name == "expanded-mixed" || name == "collapsed-composite" || name == "collapsed-scrolled" || name == "expanded-files-only")
                for (int i = 0; i < 7; ++i) {QuickNavigationEverythingEntry file; file.name = (name == "expanded-mixed" || name == "collapsed-composite" ? L"SnowDesktop " : L"") + std::wstring(i % 2 ? L"Research notes — design review.pdf" : L"研究资料与长名称示例.png"); file.path = L"C:\\Preview\\Documents\\" + file.name; SHSTOCKICONINFO stock{}; stock.cbSize = sizeof(stock); if (SUCCEEDED(SHGetStockIconInfo(i % 2 ? SIID_DOCASSOC : SIID_IMAGEFILES,SHGSI_SYSICONINDEX,&stock))) file.systemIconIndex = stock.iSysImageIndex; quickNavigationEverythingResults_.push_back(std::move(file));} // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            quickNavigationFixedTop_ = false;
            if (name.starts_with("expanded-") && quickNavigationSearchType_ != QuickNavigationSearchType::All)
            {
                // Reproduce locking a type with an empty query, then typing at
                // the same header anchor rather than only exporting a fresh state.
                const auto query = quickNavigationEffectiveSearchText_;
                quickNavigationEffectiveSearchText_.clear();
                quickNavigationAnchorTop_ = GetQuickNavigationRect().top;
                quickNavigationFixedTop_ = true;
                quickNavigationEffectiveSearchText_ = query;
            }
            quickNavigationRect_ = GetQuickNavigationRect(); quickNavigationHostRect_ = quickNavigationRect_;
            if (UseQuickNavigationList() && !GetQuickNavigationEffectiveSearchText().empty())
            {
                const auto rows = BuildQuickNavigationListRows();
                const auto content = GetQuickNavigationContentRect(quickNavigationRect_);
                if (!rows.empty() && content.bottom - content.top < QuickNavigationListRowHeight(rows.front()))
                    throw std::runtime_error("quick navigation first result is clipped");
            }
            quickNavigationListSelection_ = 1; quickNavigationKeyboardTargetKind_ = QuickNavigationKeyboardTargetKind::Item; quickNavigationKeyboardTargetIndex_ = 0;
            const RECT hover = UseQuickNavigationList() ? GetQuickNavigationListRowRect(2) : GetQuickNavigationItemRect(quickNavigationRect_,1);
            quickNavigationLastMousePoint_ = {(hover.left + hover.right) / 2, (hover.top + hover.bottom) / 2};
            if (name == "expanded-files-only" || name == "expanded-apps-only")
            {
                // Real search, drawing and activation must agree after an empty
                // source disappears, with no phantom heading above the first row.
                const auto targets = GetQuickNavigationKeyboardTargets();
                const auto content = GetQuickNavigationContentRect(quickNavigationRect_);
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
                quickNavigationKeyboardTargetKind_ = target.kind;
                quickNavigationKeyboardTargetIndex_ = target.index;
                quickNavigationLastMousePoint_ = {LONG_MIN, LONG_MIN};
            }
            if (scrolled)
            {
                quickNavigationScrollOffset_ = std::min(QuickNavScale(60), GetQuickNavigationMaxScrollOffset(quickNavigationRect_) / 2);
                quickNavigationListSelection_ = -1; ResetQuickNavigationKeyboardTarget();
                quickNavigationLastMousePoint_ = {LONG_MIN, LONG_MIN};
                if (name == "expanded-scrolled-tab-hover")
                {
                    const auto item = GetQuickNavigationItemRect(quickNavigationRect_, 0);
                    const auto tabs = GetQuickNavigationTabsRect(quickNavigationRect_);
                    quickNavigationLastMousePoint_ = {(item.left + item.right) / 2, tabs.bottom - QuickNavScale(2)};
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
                if (quickNavGlassTheme_)
                    if (!widget_preview::DrawStage(context.Get(),quickNavigationRect_, {quickNavLightTheme_,true,quickNavBlurRadius_,static_cast<float>(QuickNavScale(navigationSettings_.layout.cornerRadius))},
                            {request.canvasWidth,request.canvasHeight,quickNavigationRect_.left,quickNavigationRect_.top}, &stage))
                        throw std::runtime_error("quick navigation material backdrop failed");
            }
            brushCache_.clear(); brushCacheContext_ = context.Get();
            DrawQuickNavigationSurface(context.Get());
            require(context->EndDraw()); context->SetTarget(nullptr); brushCache_.clear(); brushCacheContext_ = nullptr;
            if (name == "expanded-tile" || name == "collapsed-empty")
            {
                const std::size_t fixture = name == "expanded-tile" ? 0 : 1;
                const auto panelSize = D2D1::SizeU(
                    static_cast<UINT>(quickNavigationRect_.right - quickNavigationRect_.left),
                    static_cast<UINT>(quickNavigationRect_.bottom - quickNavigationRect_.top));
                require(context->CreateBitmap(panelSize, nullptr, 0,
                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96),
                    &genieSources[fixture]));
                genieSourceRects[fixture] = quickNavigationRect_;
                context->SetTarget(genieSources[fixture].Get());
                context->SetTransform(D2D1::Matrix3x2F::Translation(
                    static_cast<float>(-quickNavigationRect_.left),
                    static_cast<float>(-quickNavigationRect_.top)));
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
            const auto bounds = quickNavigationRect_;
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
    quickNavigationOpen_ = false; quickNavigationPreview_ = false;
    return result;
}
