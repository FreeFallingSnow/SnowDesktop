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

    static void ResetIconPixelProbe(DesktopApp& app, ID2D1DeviceContext* context,
        const snowdesktop::IconBeautifySettings& style, const std::filesystem::path& path)
    {
        app.ClearStyledIconCache();
        const auto release = [&](auto& item) {
            if (!item.iconBitmap) return;
            app.EraseD2DIconCacheForBitmap(item.iconBitmap);
            DeleteObject(std::exchange(item.iconBitmap, nullptr));
        };
        for (auto& item : app.items_) release(item);
        for (auto& widget : app.widgets_) for (auto& entry : widget.folderEntries) release(entry);
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear(); app.widgets_.clear();
        DesktopItem item;
        item.layoutKey = path.wstring(); item.parsingName = path.wstring();
        app.items_.push_back(std::move(item));
        DesktopWidget widget;
        widget.id = L"cold-pixel-folder"; widget.sourceFolderPath = path.parent_path().wstring();
        FolderEntry entry; entry.fullPath = path.wstring();
        widget.folderEntries.push_back(std::move(entry)); app.widgets_.push_back(std::move(widget));
        app.d2dContext_ = context; app.iconBeautifySettings_ = style; app.beautifyFileThumbnails_ = true;
    }
    static void QueueIconPixelProbe(DesktopApp& app, const std::filesystem::path& path)
    {
        IconLoadTask task;
        task.serial = app.iconLoadSerial_; task.layoutKey = app.items_.front().layoutKey;
        task.parsingName = path.wstring(); task.phase = IconLoadPhase::Phase2; task.requestedSize = 64;
        task.requestKey = L"native-cold-pixel-worker" + snowdesktop::shell_icon_request::Stamp(app.items_.front());
        app.QueueIconTask(std::move(task));
    }
    static void DrainIconPixelProbe(DesktopApp& app) { app.iconWork_.Drain(); }
    static void StopIconPixelProbe(DesktopApp& app) { app.iconWork_.Stop(); }
    static size_t PreparedIconPixelCount(const DesktopApp& app) { return app.preparedIconPixels_.Size(); }
    static size_t StyledIconPixelCount(const DesktopApp& app) { return app.styledIconCache_.size(); }
    static HBITMAP IconPixelSource(const DesktopApp& app, bool folder = false)
    { return folder ? app.widgets_.front().folderEntries.front().iconBitmap : app.items_.front().iconBitmap; }
    static void ClearIconPixelCache(DesktopApp& app) { app.ClearStyledIconCache(); }
    static void SetIconPixelContext(DesktopApp& app, ID2D1DeviceContext* context) { app.d2dContext_ = context; }
    static void SetIconPixelStyle(DesktopApp& app, const snowdesktop::IconBeautifySettings& style)
    { app.iconBeautifySettings_ = style; }
    static ID2D1Bitmap* RequestCustomIconPixelProbe(DesktopApp& app, ID2D1DeviceContext* context,
        const std::string& asset, std::wstring_view identity)
    { return app.GetDockCustomIcon(context, asset, 64, identity); }
    static void DrainCustomIconPixelProbe(DesktopApp& app) { app.DrainIconLibrary(); }
    static bool DeliverIconPixelProbe(DesktopApp& app, HBITMAP source,
        std::shared_ptr<const snowdesktop::icon_styled_pixels::Prepared> prepared,
        IconLoadPhase phase = IconLoadPhase::Phase2, bool stale = false, bool folder = false)
    {
        auto result = std::make_unique<IconLoadResult>();
        result->serial = app.iconLoadSerial_ + (stale ? 1 : 0);
        result->bitmap = source; result->preparedPixels = std::move(prepared); result->phase = phase;
        BITMAP info{}; GetObjectW(source, sizeof(info), &info); result->bitmapSize = {info.bmWidth, info.bmHeight};
        result->isDesktopItem = !folder;
        if (folder)
        {
            result->widgetId = app.widgets_.front().id;
            result->folderPath = app.widgets_.front().folderEntries.front().fullPath;
            result->requestKey = L"native-cold-folder" +
                snowdesktop::shell_icon_request::Stamp(app.widgets_.front().folderEntries.front());
        }
        else
        {
            result->layoutKey = app.items_.front().layoutKey;
            result->requestKey = L"native-cold-desktop" + snowdesktop::shell_icon_request::Stamp(app.items_.front());
        }
        return app.OnIconLoaded(0, reinterpret_cast<LPARAM>(result.release()));
    }
    struct IconPixelSnapshot
    {
        snowdesktop::icon_bitmap_pixels::Buffer buffer;
        snowdesktop::IconBeautifySettings style;
        bool glass = false;
    };
    static IconPixelSnapshot ReadIconPixelProbe(DesktopApp& app, ID2D1DeviceContext* context,
        HBITMAP source, const snowdesktop::IconBeautifySettings& style,
        bool managedAlpha = false, bool nativeControlSource = false)
    {
        app.d2dContext_ = context;
        return ReadIconPixelBitmap(app, context,
            app.GetStyledD2DBitmap(context, source, style, managedAlpha, nativeControlSource));
    }
    static IconPixelSnapshot ReadIconPixelBitmap(DesktopApp& app, ID2D1DeviceContext* context, ID2D1Bitmap* bitmap)
    {
        const auto require = [](HRESULT hr) {
            if (FAILED(hr)) throw std::runtime_error("read back actual styled icon pixels");
        };
        if (!bitmap) throw std::runtime_error("styled icon probe requires actual host upload");
        const auto size = bitmap->GetPixelSize();
        auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ, bitmap->GetPixelFormat());
        ComPtr<ID2D1Bitmap1> readable;
        require(context->CreateBitmap(size, nullptr, 0, &properties, &readable));
        require(readable->CopyFromBitmap(nullptr, bitmap, nullptr));
        IconPixelSnapshot result;
        result.buffer.width = static_cast<int>(size.width); result.buffer.height = static_cast<int>(size.height);
        result.buffer.pixels.resize(static_cast<size_t>(size.width) * size.height);
        D2D1_MAPPED_RECT mapped{}; require(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        for (UINT32 row = 0; row < size.height; ++row)
            std::memcpy(result.buffer.pixels.data() + static_cast<size_t>(row) * size.width,
                mapped.bits + static_cast<size_t>(row) * mapped.pitch, static_cast<size_t>(size.width) * sizeof(std::uint32_t));
        require(readable->Unmap());
        result.style = app.iconBitmapStyles_.at(bitmap); result.glass = app.iconGlassBackdrop_.at(bitmap);
        return result;
    }
