    // Included in the existing native friend. Replace only enumeration and
    // wall-time inputs; lifecycle work, delivery, settings reads and saves run
    // through the actual DesktopApp services without Run() or a desktop window.
    static void SeedIconCustomization(DesktopApp& app, const std::wstring& target,
        const std::string& asset, std::int64_t since = 0)
    {
        if (!asset.empty()) app.iconOverrides_[target] = asset;
        snowdesktop::IconAppearanceRecord record;
        record.target = target; record.name = L"Retained native icon";
        record.settings = snowdesktop::icon_beautify::MakePreset(snowdesktop::IconBeautifyPreset::DefaultBeautify);
        app.iconAppearanceOverrides_[target] = record;
        if (since) app.missingIconTargets_[target] = since;
        app.dataCleanupHasCompleteModel_ = true;
        app.iconAppearanceReferencesDirty_ = false;
    }
    static void ProbeIconCustomizations(DesktopApp& app)
    { app.nextIconCustomizationProbe_ = 0; app.ScheduleIconCustomizationCleanup(); }
    static void DrainIconCustomizations(DesktopApp& app) { app.DrainBackgroundShellWork(); }
    static const auto& MissingIconTargets(const DesktopApp& app) { return app.missingIconTargets_; }
    static bool HasIconReplacement(const DesktopApp& app, const std::wstring& target)
    { return app.iconOverrides_.contains(target); }
    static bool HasIconAppearance(const DesktopApp& app, const std::wstring& target)
    { return app.iconAppearanceOverrides_.contains(target); }
    static auto ReadIconAppearances(DesktopApp& app) { return app.EditIconAppearance({}); }
    static auto ReadIconLibrary(DesktopApp& app) { return app.EditIconLibrary({}); }
    static bool ApplyRetainedIcon(DesktopApp& app, const std::wstring& target, const std::string& asset)
    { return app.SetIconOverride(target, asset); }
    static bool BeginRetainedIconAppearance(DesktopApp& app, const std::wstring& target)
    {
        snowdesktop::IconAppearanceRequest request; request.action = "begin"; request.target = target;
        return app.EditIconAppearance(request).succeeded;
    }
    static void ReloadIconCustomizationLayout(DesktopApp& app)
    { app.LoadLayoutSlots(); app.desktopItemsReady_ = true; }
    static D2D1_SIZE_U UploadIconCustomization(DesktopApp& app, ID2D1DeviceContext* context,
        HBITMAP source, bool enabled)
    {
        app.d2dContext_ = context;
        auto style = snowdesktop::icon_beautify::MakePreset(snowdesktop::IconBeautifyPreset::DefaultBeautify);
        style.enabled = enabled;
        const auto bitmap = app.CreateStyledD2DBitmap(source, style, true);
        if (!bitmap) throw std::runtime_error("actual icon upload must return an observable D2D bitmap");
        const auto size = bitmap->GetPixelSize();
        app.iconGlassBackdrop_.erase(bitmap.Get()); app.iconBitmapStyles_.erase(bitmap.Get());
        app.d2dContext_.Reset();
        return size;
    }
    static auto UploadFileThumbnail(DesktopApp& app, ID2D1DeviceContext* context,
        HBITMAP source, bool enabled, bool thumbnail, int mode = 0)
    {
        app.d2dContext_ = context;
        app.iconBeautifySettings_ = snowdesktop::icon_beautify::MakePreset(
            snowdesktop::IconBeautifyPreset::DefaultBeautify);
        const std::wstring identity = L"C:\\ThumbnailStyleProbe.png";
        const auto target = app.ResolveIconTarget(identity);
        if (mode == 2)
        {
            snowdesktop::IconAppearanceRecord own;
            own.settings = app.iconBeautifySettings_;
            own.settings.mode = mode;
            app.iconAppearanceOverrides_[target] = own;
        }
        else app.iconBeautifySettings_.mode = mode;
        app.beautifyFileThumbnails_ = enabled;
        const auto bitmap = app.GetTargetIconBitmap(context, source, identity, thumbnail);
        if (!bitmap) throw std::runtime_error("thumbnail appearance must reach the actual bitmap upload");
        const auto result = std::make_pair(bitmap->GetPixelSize(), app.iconBitmapStyles_.at(bitmap));
        app.ClearStyledIconCache();
        app.iconAppearanceOverrides_.erase(target);
        app.d2dContext_.Reset();
        return result;
    }
    static bool ReloadThumbnailPreference(DesktopApp& app, bool enabled)
    {
        app.SetIconBeautifySettings(snowdesktop::icon_beautify::MakePreset(
            snowdesktop::IconBeautifyPreset::DefaultBeautify), enabled);
        app.beautifyFileThumbnails_ = !enabled;
        ReloadIconCustomizationLayout(app);
        return app.beautifyFileThumbnails_;
    }

#include "native_icon_cold_pixels_access.inc"
#include "native_system_icon_refresh_access.inc"

    static void RecordIconResourceSample(const DesktopApp& app) { app.RecordIconTextResources(); }
    static void SeedIconResourceSample(DesktopApp& app, ID2D1DeviceContext* context)
    {
        app.items_.clear();
        snowdesktop::icon_image::Image image;
        image.width = 16; image.height = 8; image.pixels.assign(128, 0xffaabbcc);
        for (int i = 0; i < 2; ++i)
        {
            DesktopItem item;
            item.iconBitmap = snowdesktop::icon_image::CreateBitmap(image);
            if (!item.iconBitmap) throw std::runtime_error("create resource sample source");
            item.iconBitmapSize = i == 0 ? SIZE{16, 8} : SIZE{};
            app.items_.push_back(std::move(item));
        }
        auto prepared = std::make_shared<snowdesktop::icon_styled_pixels::Prepared>();
        prepared->source = app.items_.front().iconBitmap;
        prepared->buffer.pixels.reserve(256);
        prepared->buffer.pixels.assign(128, 0xffaabbcc);
        app.preparedIconPixels_.Put(std::move(prepared));
        ComPtr<ID2D1Bitmap1> bitmap;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(context->CreateBitmap(D2D1::SizeU(10, 5), nullptr, 0, &properties, &bitmap)))
            throw std::runtime_error("create resource sample D2D bitmap");
        app.d2dIconCache_[1] = bitmap;
        app.styledIconCache_[{1, 0, false, false}] = bitmap;
        app.itemTextShadowCache_[L"shadow"] = bitmap;
        app.iconReflectionCache_.Insert(1, bitmap);
        const auto request = app.dockFolderBitmapCache_.Read(L"sample-folder");
        if (!app.dockFolderBitmapCache_.Publish(L"sample-folder", request.ticket, bitmap))
            throw std::runtime_error("publish actual folder cache sample");
        ComPtr<IDWriteFactory> fonts;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(fonts.GetAddressOf()))))
            throw std::runtime_error("create resource sample fonts");
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"en-US", &format)) ||
            FAILED(fonts->CreateTextLayout(L"hi", 2, format.Get(), 100, 30, &layout)))
            throw std::runtime_error("create actual resource sample layout");
        snowdesktop::ItemTextLayoutCacheEntry entry;
        entry.layout = layout;
        app.itemTextLayoutCache_[L"abc"] = std::move(entry);
        app.componentListTextLayoutCache_[L"xy"] = layout;
        app.popupFanTextLayouts_[L"z"] = layout;
    }
    static bool IconResourceSampleUnchanged(const DesktopApp& app)
    {
        return app.items_.size() == 2 && app.items_.front().iconBitmap &&
            app.preparedIconPixels_.Size() == 1 && app.preparedIconPixels_.Bytes() == 512 &&
            app.d2dIconCache_.size() == 1 && app.styledIconCache_.size() == 1 &&
            app.itemTextShadowCache_.size() == 1 && app.iconReflectionCache_.Size() == 1 &&
            app.itemTextLayoutCache_.size() == 1 && app.componentListTextLayoutCache_.size() == 1 &&
            app.popupFanTextLayouts_.size() == 1;
    }
