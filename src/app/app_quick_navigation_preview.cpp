#include "app.h"
#include "../preview_png_writer.h"
#include "../widget_preview_stage.h"
#include "../utils.h"
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
        constexpr const char* scenarios[] = {"expanded-tile", "expanded-source", "expanded-initial", "collapsed-empty", "collapsed-composite",
            "expanded-mixed", "type-menu", "view-menu", "typed-app", "typed-empty", "typed-web", "typed-settings", "typed-run", "typed-calculator",
            "calculator-error", "everything-unavailable", "index-loading", "empty-results",
            "expanded-scrolled", "expanded-scrolled-tab-hover", "collapsed-scrolled"};
        for (const auto* scenario : scenarios)
        {
            const std::string name(scenario);
            const bool scrolled = name.find("scrolled") != std::string::npos;
            navigationSettings_.layout.maximumHeight = scrolled && name.starts_with("expanded") ? 420 : NavigationSettings{}.layout.maximumHeight;
            quickNavigationCollapsed_ = name.starts_with("collapsed") || name.starts_with("typed") || name == "calculator-error" || name == "everything-unavailable" || name == "index-loading" || name == "empty-results";
            quickNavigationSearchType_ = QuickNavigationSearchType::All; quickNavigationSearchEngine_.clear();
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
            if (name == "typed-web") {quickNavigationSearchType_ = QuickNavigationSearchType::Web; quickNavigationEffectiveSearchText_ = L"SnowDesktop 快捷导航";} // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            if (name == "typed-settings")
            {
                quickNavigationSearchType_ = QuickNavigationSearchType::Settings;
                quickNavigationSettingsResults_.push_back({SettingsSearchEntryKind::StaticSetting, SettingsRoute::ForPage(SettingsPage::QuickNavigation,"quickNav.defaultCollapsed"), "quickNav.defaultCollapsed", _LW("quickNav.defaultCollapsed"), _LW("quickNav.description"), _LW("quickNav.title")});
                quickNavigationSettingsResults_.push_back({SettingsSearchEntryKind::WidgetSetting, SettingsRoute::ForWidget(L"preview-widget","color"), "color", L"组件颜色 Widget color", L"Color and appearance", L"Calendar"}); // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            }
            if (name == "typed-run") {quickNavigationSearchType_ = QuickNavigationSearchType::Run; quickNavigationEffectiveSearchText_ = L"\"C:\\Program Files\\Example\\editor.exe\" --new-window";}
            if (name == "typed-calculator" || name == "calculator-error") {quickNavigationSearchType_ = QuickNavigationSearchType::Calculator; quickNavigationEffectiveSearchText_ = name == "calculator-error" ? L"1 / 0" : L"(12.5 + 7.5) * 3 ^ 2 + 50%";}
            if (name == "empty-results") {quickNavigationSearchType_ = QuickNavigationSearchType::App; quickNavigationEffectiveSearchText_ = L"No matching application";}
            if (name == "expanded-mixed" || name == "collapsed-composite" || name == "collapsed-scrolled" || name == "typed-app")
                for (size_t i = 0; i < (name == "typed-app" ? quickNavigationAppEntries_.size() : size_t{1}); ++i) quickNavigationAppResultIndices_.push_back(i);
            if (name == "expanded-mixed" || name == "collapsed-composite" || name == "collapsed-scrolled")
                for (int i = 0; i < 7; ++i) {QuickNavigationEverythingEntry file; file.name = (name == "expanded-mixed" || name == "collapsed-composite" ? L"SnowDesktop " : L"") + std::wstring(i % 2 ? L"Research notes — design review.pdf" : L"研究资料与长名称示例.png"); file.path = L"C:\\Preview\\Documents\\" + file.name; SHSTOCKICONINFO stock{}; stock.cbSize = sizeof(stock); if (SUCCEEDED(SHGetStockIconInfo(i % 2 ? SIID_DOCASSOC : SIID_IMAGEFILES,SHGSI_SYSICONINDEX,&stock))) file.systemIconIndex = stock.iSysImageIndex; quickNavigationEverythingResults_.push_back(std::move(file));} // l10n-allow: fixed multilingual names for private offscreen visual fixtures, never runtime UI text
            quickNavigationFixedTop_ = false; quickNavigationRect_ = GetQuickNavigationRect(); quickNavigationHostRect_ = quickNavigationRect_;
            quickNavigationListSelection_ = 1; quickNavigationKeyboardTargetKind_ = QuickNavigationKeyboardTargetKind::Item; quickNavigationKeyboardTargetIndex_ = 0;
            const RECT hover = UseQuickNavigationList() ? GetQuickNavigationListRowRect(2) : GetQuickNavigationItemRect(quickNavigationRect_,1);
            quickNavigationLastMousePoint_ = {(hover.left + hover.right) / 2, (hover.top + hover.bottom) / 2};
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
        result.ok = true; result.stage = "complete";
    }
    catch (const std::exception& error) {result.error = error.what();}
    quickNavigationOpen_ = false; quickNavigationPreview_ = false;
    return result;
}
