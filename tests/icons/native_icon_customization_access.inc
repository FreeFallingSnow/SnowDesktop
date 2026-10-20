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
        app.LoadLayoutSlots();
        return app.beautifyFileThumbnails_;
    }
