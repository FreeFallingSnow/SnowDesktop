    // R04 keeps the real request service, host completion dispatcher, Win32
    // update region, cache invalidation and DrawStaticBackground path.
    static void ConfigureLargeCompletion(DesktopApp& app, HWND window,
        const std::shared_ptr<std::atomic<unsigned>>& notifications)
    {
        app.hwnd_ = app.controlHwnd_ = window;
        app.lastMousePoint_ = {500, 300};
        app.largeIconAssets_ = std::make_unique<snowdesktop::LargeIconAssets>(
            GetDataSubdirectoryPath(L"large-icons"), [notifications] { ++*notifications; },
            std::filesystem::path(GetDataDirectoryPath()) / L"r04-steam");
        app.gridPages_.front().bounds = app.gridPages_.front().workArea =
            app.gridPages_.front().visualWorkArea = RECT{0, 0, 512, 320};
        for (size_t i = 0; i < app.items_.size(); ++i)
        {
            auto& item = app.items_[i];
            item.bounds = {20 + static_cast<LONG>(i) * 240, 20,
                140 + static_cast<LONG>(i) * 240, 140};
            item.selected = false;
            item.largeIcon = snowdesktop::LargeIconConfig{};
            item.largeIcon->backgroundStyle = kAppearancePresetCustom;
            item.largeIcon->material = 0;
            item.largeIcon->foregroundContent = 1;
            item.largeIcon->foregroundImage = "r04-red.png";
            item.largeIcon->localOnly = true;
            app.largeIconRuntime_[item.layoutKey].visible = true;
        }
        app.desktopBackdropFullCollectionPending_ = false;
    }
    static void QueueLargeCompletion(DesktopApp& app, size_t index, bool refresh = false, int variant = 0)
    {
        auto& state = app.largeIconRuntime_.at(app.items_.at(index).layoutKey);
        (variant ? state.previewSignatures.at(static_cast<size_t>(variant - 1)) : state.signature).clear();
        app.RequestLargeIconAsset(index, refresh, {}, variant);
    }
    static void DrainLargeCompletion(DesktopApp& app)
    { app.HandleControlMessage(app.controlHwnd_, kLargeIconAssetsReadyMessage, 0, 0); }
    static auto ReadLargeCompletionSettings(DesktopApp& app)
    {
        snowdesktop::LargeIconSettingsRequest request;
        request.key = app.items_.front().layoutKey; request.action = "read";
        return app.EditLargeIcon(std::move(request));
    }
    static const auto& LargeCompletionState(const DesktopApp& app, size_t index = 0)
    { return app.largeIconRuntime_.at(app.items_.at(index).layoutKey); }
    static auto& LargeCompletionConfig(DesktopApp& app, size_t index = 0)
    { return *app.items_.at(index).largeIcon; }
    static void ExpireLargeCompletion(DesktopApp& app, int variant = 0)
    {
        auto& state = app.largeIconRuntime_.at(app.items_.front().layoutKey);
        ++(variant ? state.previewGenerations.at(static_cast<size_t>(variant - 1)) : state.generation);
    }
    static auto LargeCompletionSceneRevision(const DesktopApp& app)
    { return app.dragSession_.StaticSceneRevision(); }
    static bool LargeCompletionFullCollection(const DesktopApp& app)
    { return app.desktopBackdropFullCollectionPending_; }
    static void ResetLargeCompletionDamage(DesktopApp& app)
    {
        ValidateRect(app.hwnd_, nullptr);
        app.desktopBackdropFullCollectionPending_ = false;
    }
    static void HideLargeCompletion(DesktopApp& app, bool hidden)
    { app.desktopIconsHidden_ = hidden; }
    static void SelectLargeCompletion(DesktopApp& app, bool selected)
    { app.items_.front().selected = selected; }
    static void ReplayLegacyLargeCompletionTail(DesktopApp& app)
    {
        // Exact unconditional submission boundary from the pre-R04 function,
        // executed after a real preview delivery as the negative control.
        app.SynchronizeLargeIconAssets();
        app.InvalidateDragStaticScene();
        if (app.hwnd_ && IsWindow(app.hwnd_)) InvalidateRect(app.hwnd_, nullptr, FALSE);
    }
    static void DrawLargeCompletion(DesktopApp& app, ID2D1DeviceContext* context, const RECT* dirty)
    {
        app.d2dContext_ = context;
        if (!app.dwriteFactory_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))))
            throw std::runtime_error("create offscreen large-icon fonts");
        app.DrawStaticBackground(context, dirty, app.desktopIconsHidden_);
    }
    static void StopLargeCompletion(DesktopApp& app)
    {
        app.largeIconAssets_->Stop();
        KillTimer(app.controlHwnd_, kLargeIconRetryTimerId);
        app.largeIconRuntime_.clear();
        app.d2dIconCache_.clear();
        app.d2dContext_.Reset(); app.dwriteFactory_.Reset();
        app.controlHwnd_ = nullptr;
        app.hwnd_ = completionWindow;
    }
