    static POINT TrackNativePopupRequest(DesktopApp& app, const std::wstring& id)
    {
        auto* collection = dynamic_cast<Collection*>(MemberContainer(app, id));
        if (!collection) throw std::runtime_error("popup request fixture requires the actual collection opener");
        const RECT frame = collection->GetAllButtonRect();
        if (IsRectEmpty(&frame)) throw std::runtime_error("popup request opener must be a real visible native surface");
        const POINT point{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        app.lastMousePoint_ = point; app.dragSession_.UpdatePoint(point);
        app.UpdateCollectionPopupDwell(point);
        return point;
    }
    static bool NativePopupRequestArmed(const DesktopApp& app)
    { return app.collectionPopupDwellTimerArmed_ && !app.popupDwellController_.IsIdle(); }
    static bool RequestNativePopup(DesktopApp& app, DWORD now,
        const DesktopApp::CollectionPopupRequestPresenter& presenter)
    { return app.TryOpenDwellCollectionPopup(now, presenter); }
    static void LeaveNativePopupRequest(DesktopApp& app)
    {
        const POINT point{1150, 750}; app.lastMousePoint_ = point;
        app.dragSession_.UpdatePoint(point); app.UpdateCollectionPopupDwell(point);
    }
    static void ReplaceNativePopupTarget(DesktopApp& app)
    {
        const auto index = app.FindWidgetIndexById(L"popup-A");
        if (index >= app.widgets_.size()) throw std::runtime_error("popup identity replacement requires the actual pending target");
        app.widgets_[index].id = L"replacement-popup-A";
        app.RebuildContainersAndItems();
    }
    static void ObserveNativePopupRequests(DesktopApp& app,
        const DesktopApp::CollectionPopupRequestPresenter& presenter)
    { app.collectionPopupRequestPresenter_ = presenter; }
    static void ClearNativePopupRequestObserver(DesktopApp& app) { app.collectionPopupRequestPresenter_ = {}; }
    static void CancelNativePopupRequests(DesktopApp& app) { app.CancelCollectionPopupDwell(); }
    static void CheckLuaPopupReuse(DesktopApp& app)
    {
        const auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        for (const auto* surface : {"panel", "dialog", "popover"})
        for (const int phase : {0, 1, 2})
        {
            LuaWidgetPanelRequest request;
            request.widgetId = L"lua-popup-owner";
            request.title = surface == std::string_view("popover") ? L"" : L"Playback settings";
            request.surface = surface;
            request.anchorKey = surface == std::string_view("popover") ? "audio.button" : "";
            request.desktopGesture = true;
            app.luaWidgetPanelRequest_ = request;
            app.luaWidgetPanelAnimation_.ShowImmediately();
            if (phase == 0)
            {
                app.luaWidgetPanelAnimation_.ResetHidden();
                app.luaWidgetPanelAnimation_.Open(1000);
            }
            if (phase == 2) app.luaWidgetPanelAnimation_.Close(1000);
            const auto visual = app.luaWidgetPanelAnimation_.GetVisual();
            const auto generation = app.floatingPopupMouseHookGeneration_;
            const bool closing = app.luaWidgetPanelAnimation_.IsClosing();
            request.width += 20; request.height += 20;
            app.OpenLuaWidgetPanel(request);
            check(app.floatingPopupMouseHookGeneration_ == generation &&
                !app.pendingLuaWidgetPanelOpen_ && app.luaWidgetPanelRequest_.width != request.width &&
                app.luaWidgetPanelAnimation_.IsClosing() == closing &&
                app.luaWidgetPanelAnimation_.GetVisual().progress == visual.progress,
                "same desktop panel/dialog/popover must preserve scene generation and animation in every phase");
            app.luaWidgetPanelFinalizing_ = true;
            app.OpenLuaWidgetPanel(request);
            check(!app.pendingLuaWidgetPanelOpen_, "reentrant duplicate must not queue another surface");
            auto different = request; different.title += L"Other";
            app.OpenLuaWidgetPanel(different);
            check(app.pendingLuaWidgetPanelOpen_ && app.pendingLuaWidgetPanelOpen_->title == different.title,
                "different popup title must still request replacement");
            app.pendingLuaWidgetPanelOpen_.reset();
            different = request; different.widgetId += L"Other";
            app.OpenLuaWidgetPanel(different);
            check(app.pendingLuaWidgetPanelOpen_ && app.pendingLuaWidgetPanelOpen_->widgetId == different.widgetId,
                "another instance must keep independent popup identity");
            app.pendingLuaWidgetPanelOpen_.reset();
            different = request; different.anchorKey += "Other";
            app.OpenLuaWidgetPanel(different);
            check(app.pendingLuaWidgetPanelOpen_.has_value(), "a different popover anchor remains a distinct popup");
            app.pendingLuaWidgetPanelOpen_.reset();
            different = request; different.desktopGesture = false;
            app.OpenLuaWidgetPanel(different);
            check(app.pendingLuaWidgetPanelOpen_.has_value(), "programmatic panel refresh retains its previous behavior");
            app.pendingLuaWidgetPanelOpen_.reset();
            app.luaWidgetPanelFinalizing_ = false;
        }
        // Execute the real pointer-down route against this process's isolated
        // model/window. No desktop host, live component or presentation opens.
        app.luaWidgetPanelRequest_ = {};
        app.luaWidgetPanelRequest_.widgetId = L"lua-popup-owner";
        app.luaWidgetPanelAnchorPoint_ = {700, 500};
        app.luaWidgetPanelAnimation_.ShowImmediately();
        app.layoutWorkArea_ = {0, 0, 1600, 1000};
        app.gridPages_.front().workArea = app.layoutWorkArea_;
        DesktopWidget owner;
        owner.id = L"lua-popup-owner";
        owner.type = DesktopWidgetType::LuaScript;
        owner.bounds = {50, 50, 250, 200};
        owner.gridCell.pageId = app.gridPages_.front().id;
        app.widgets_ = {owner};
        const POINT press{80, 80};
        const auto popup = app.GetLuaWidgetPanelRect();
        check(!PtInRect(&popup, press) && app.HitTestStandaloneWidget(0, press) == WidgetHit::Content,
            "fixture click must hit owning component content outside its popup");
        app.OnLeftButtonDown(MK_LBUTTON, MAKELPARAM(press.x, press.y));
        check(app.luaWidgetPanelAnimation_.IsInteractive() &&
            app.luaWidgetPanelRequest_.widgetId == owner.id,
            "owner opener press must not dismiss popup before its release action reuses it");
        ReleaseCapture();
        app.mouseDown_ = false;
        app.mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        app.luaWidgetPanelRequest_ = {};
        app.luaWidgetPanelAnimation_.ResetHidden();
        CheckLuaPopupScrollbar(app);
    }

    static void CheckPopupRenderStorage(DesktopApp& app, ID2D1Device* device,
        ID2D1DeviceContext* ctx, ID2D1Bitmap1* target, IDXGIDevice* dxgi)
    {
        const auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        DragRenderCache cache;
        unsigned draws = 0;
        const auto draw = [&](ID2D1DeviceContext* output) {
            ++draws;
            output->Clear(D2D1::ColorF(draws == 1 ? D2D1::ColorF::Red : D2D1::ColorF::Blue));
        };
        check(cache.Ensure(device, D2D1::SizeU(64, 64), 1, draw), "create real popup snapshot");
        check(cache.Ensure(device, D2D1::SizeU(64, 64), 1, draw) && draws == 1,
            "an unchanged snapshot skips its drawing callback");
        check(cache.Ensure(device, D2D1::SizeU(64, 64), 2, draw) && draws == 2 && cache.BitmapAllocationCount() == 1,
            "a new content revision redraws in the same GPU bitmap allocation");
        ctx->BeginDraw();
        ctx->Clear(D2D1::ColorF(D2D1::ColorF::Black, 0));
        check(cache.DrawAt(ctx, D2D1::Point2F(0, 0)), "updated snapshot draws on the actual target");
        check(SUCCEEDED(ctx->EndDraw()), "finish snapshot pixel rendering");
        ComPtr<ID2D1Bitmap1> readback;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        check(SUCCEEDED(ctx->CreateBitmap(D2D1::SizeU(64, 64), nullptr, 0, &properties, &readback)),
            "create isolated snapshot readback");
        const D2D1_RECT_U region{0, 0, 64, 64};
        check(SUCCEEDED(readback->CopyFromBitmap(nullptr, target, &region)), "copy updated snapshot pixels");
        D2D1_MAPPED_RECT mapped{};
        check(SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "map updated snapshot pixels");
        const bool blue = mapped.bits[0] == 255 && mapped.bits[1] == 0 && mapped.bits[2] == 0 && mapped.bits[3] == 255;
        readback->Unmap();
        check(blue, "same-allocation refresh publishes new blue pixels, not the old red snapshot");
        cache.Invalidate();
        check(!cache.DrawAt(ctx, D2D1::Point2F(0, 0)), "invalidating storage prevents stale snapshot draws");
        check(cache.Ensure(device, D2D1::SizeU(64, 64), 1, draw) && draws == 3 && cache.BitmapAllocationCount() == 1,
            "reopening reuses storage even when the content revision restarts");

        const auto savedDevice = app.dcompDevice_;
        const auto savedRoot = app.dcompVisual_;
        check(SUCCEEDED(DCompositionCreateDevice2(device, __uuidof(IDCompositionDesktopDevice),
            reinterpret_cast<void**>(app.dcompDevice_.ReleaseAndGetAddressOf()))), "create test-only native snapshot compositor");
        check(SUCCEEDED(app.dcompDevice_->CreateVisual(app.dcompVisual_.ReleaseAndGetAddressOf())),
            "create unattached test-only visual root");
        UiCompositionAnimationOverlay overlay;
        check(app.PrepareCompositionAnimationOverlay(overlay, cache, RECT{0, 0, 64, 64}, UiCompositionAnimationHost::Desktop),
            "prepare production native snapshot surface");
        const auto firstSurface = overlay.surface;
        app.ResetCompositionAnimationOverlay(overlay);
        check(app.PrepareCompositionAnimationOverlay(overlay, cache, RECT{20, 20, 84, 84}, UiCompositionAnimationHost::Desktop) &&
            overlay.surface.Get() == firstSurface.Get(), "reopen at another anchor reuses the same native surface");
        check(cache.Ensure(device, D2D1::SizeU(80, 64), 3, draw) && cache.BitmapAllocationCount() == 2,
            "a changed size creates exactly one new bitmap");
        check(app.PrepareCompositionAnimationOverlay(overlay, cache, RECT{0, 0, 80, 64}, UiCompositionAnimationHost::Desktop) &&
            overlay.surface.Get() != firstSurface.Get(), "a changed size replaces the native surface");
        snowdesktop::popup_animation_rules::State motion;
        motion.Configure(false, 1.0, true);
        motion.Open(1000);
        const auto trajectory = motion.RemainingTimeline();
        check(app.AnimateCompositionAnimationOverlay(overlay, 0, 1, POINT{40, 64}, 1, 1,
            static_cast<UINT>(trajectory.durationMilliseconds), 0, &trajectory) && app.FlushPendingCompositionCommit(),
            "DirectComposition accepts the production zero-to-one spring segments");
        app.dcompVisual_->RemoveVisual(overlay.visual.Get());
        // Exercise the real warmup against a test-owned, never-shown HWND.
        // It must initialize storage without publishing popup state or input.
        const HWND savedPopupWindow = app.floatingPopupHwnd_;
        const bool savedGlass = app.collectionPopupGlassTheme_;
        app.collectionPopupGlassTheme_ = false;
        app.floatingPopupHwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"STATIC", L"isolated-popup-warmup", WS_POPUP, 0, 0, 1, 1,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(app.floatingPopupHwnd_ != nullptr && app.WarmFloatingPopupResources(),
            "prepare the actual popup composition infrastructure in a hidden test window");
        const auto warmedTarget = app.floatingPopupDcompTarget_;
        const auto warmedSurface = app.floatingPopupDcompSurface_;
        check(app.WarmFloatingPopupResources() && app.floatingPopupDcompTarget_.Get() == warmedTarget.Get() &&
            app.floatingPopupDcompSurface_.Get() == warmedSurface.Get(),
            "repeated infrastructure warmup reuses its native target and storage");
        check(!IsWindowVisible(app.floatingPopupHwnd_) && !app.ShouldShowFloatingPopupWindow() &&
            IsRectEmpty(&app.floatingPopupWindowBounds_) && !app.floatingPopupMouseHookNotificationWindow_.load(),
            "warmup cannot reveal a popup, establish input bounds or install an outside-click hook");
        app.DestroyFloatingPopupWindow();
        app.floatingPopupHwnd_ = savedPopupWindow;
        app.collectionPopupGlassTheme_ = savedGlass;
        app.dcompVisual_ = savedRoot;
        app.dcompDevice_ = savedDevice;

        ComPtr<ID2D1Device> otherDevice;
        ComPtr<ID2D1Factory> baseFactory;
        device->GetFactory(&baseFactory);
        ComPtr<ID2D1Factory1> factory;
        check(SUCCEEDED(baseFactory.As(&factory)) && SUCCEEDED(factory->CreateDevice(dxgi, &otherDevice)),
            "create a replacement graphics resource domain");
        check(cache.Ensure(otherDevice.Get(), D2D1::SizeU(80, 64), 3, draw) && cache.BitmapAllocationCount() == 3,
            "a device switch cannot reuse an old snapshot or its context");
    }

    static void CheckFanRenderReuse(DesktopApp& app, ID2D1Device* device, ID2D1DeviceContext* ctx)
    {
        const auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        auto savedWidget = std::move(app.dockFolderPopupWidget_);
        const auto savedDevice = app.d2dDevice_;
        const auto savedContext = app.d2dContext_;
        const auto savedFormat = app.fanTitleTextFormat_;
        const auto savedAnchor = app.popupAnchorPoint_;
        const bool savedAnchored = app.popupAnchoredToDock_, savedOpen = app.dockFolderPopupOpen_;
        const bool savedHasAnchor = app.popupHasAnchor_;
        app.d2dDevice_ = device;
        app.d2dContext_ = ctx;
        app.dockFolderPopupWidget_ = {};
        auto& widget = app.dockFolderPopupWidget_;
        widget.type = DesktopWidgetType::FolderMapping;
        widget.fanPopup = true;
        widget.folderEntries.emplace_back();
        auto& entry = widget.folderEntries.back();
        entry.name = L"Cached fan icon";
        entry.fullPath = L"C:\\isolated-fan-fixture\\icon.png";
        std::vector<std::uint32_t> pixels(32 * 32, 0xff3050d0);
        entry.iconBitmap = CreateBitmap(32, 32, 1, 32, pixels.data());
        check(entry.iconBitmap != nullptr, "create the isolated fan source icon");
        app.dockFolderPopupOpen_ = app.popupAnchoredToDock_ = app.popupHasAnchor_ = true;
        app.popupAnchorPoint_ = {800, 900};
        app.popupAnimation_.ShowImmediately();
        check(SUCCEEDED(app.dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16, L"en-US",
            app.fanTitleTextFormat_.ReleaseAndGetAddressOf())), "create the fan fixture's actual text format");
        app.fanTitleTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        app.fanTitleTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto* initialIcon = app.GetTargetIconBitmap(ctx, entry.iconBitmap, entry.fullPath, false);
        check(initialIcon != nullptr, "upload the actual source icon through the production graphics context");
        const auto render = [&] {
            ctx->BeginDraw();
            ctx->Clear(D2D1::ColorF(D2D1::ColorF::Black, 0));
            app.DrawCollectionPopup(ctx, false);
            check(SUCCEEDED(ctx->EndDraw()), "render the real fan icon, shadow, and labels offscreen");
        };
        render();
        check(app.popupFanIconDrawings_.size() >= 2 && !app.popupFanTextLayouts_.empty(),
            "fan rendering materializes cached icon/action drawings and real text layouts");
        const auto recordings = app.popupFanIconDrawings_;
        const auto recorder = app.popupFanIconRecorder_;
        const auto layouts = app.popupFanTextLayouts_;
        entry.selected = true;
        render();
        check(app.popupFanIconRecorder_.Get() == recorder.Get() && recordings.size() == app.popupFanIconDrawings_.size(),
            "selection feedback reuses the recorder and static icon recordings");
        for (const auto& [key, recording] : recordings)
            check(app.popupFanIconDrawings_.at(key).commands.Get() == recording.commands.Get() &&
                app.popupFanIconDrawings_.at(key).shadow.Get() == recording.shadow.Get(),
                "stable fan frames reuse actual command-list and shadow-effect identities");
        for (const auto& [key, layout] : layouts)
            check(app.popupFanTextLayouts_.at(key).Get() == layout.Get(), "stable fan frames reuse actual DirectWrite layouts");
        std::fill(pixels.begin(), pixels.end(), 0xff30d050);
        const HBITMAP replacement = CreateBitmap(32, 32, 1, 32, pixels.data());
        check(replacement != nullptr, "create a late arriving replacement icon");
        DeleteObject(entry.iconBitmap);
        entry.iconBitmap = replacement;
        auto* replacementIcon = app.GetTargetIconBitmap(ctx, entry.iconBitmap, entry.fullPath, false);
        check(replacementIcon != nullptr && replacementIcon != initialIcon,
            "a late icon reaches a distinct real uploaded bitmap");
        render();
        check(app.popupFanIconDrawings_.size() > recordings.size(),
            "a late icon replacement cannot keep a cached old icon or placeholder");
        app.InvalidateCollectionPopupContent();
        check(app.popupFanIconDrawings_.empty(), "source changes invalidate fan recordings without retaining stale pixels");
        app.popupFanIconRecorder_.Reset();
        app.popupFanTextLayouts_.clear();
        app.popupFanCachedTextFormat_.Reset();
        app.ClearStyledIconCache();
        app.dockFolderPopupWidget_ = std::move(savedWidget);
        app.d2dDevice_ = savedDevice;
        app.d2dContext_ = savedContext;
        app.fanTitleTextFormat_ = savedFormat;
        app.dockFolderPopupOpen_ = savedOpen;
        app.popupAnchoredToDock_ = savedAnchored;
        app.popupHasAnchor_ = savedHasAnchor;
        app.popupAnchorPoint_ = savedAnchor;
        app.popupAnimation_.ResetHidden();
    }

    static void CheckLuaPopupScrollbar(DesktopApp& app)
    {
        const auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        // Render the actual host popup offscreen, with no live desktop services.
        ComPtr<ID3D11Device> d3d;
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr)), "create isolated scrollbar WARP device");
        ComPtr<IDXGIDevice> dxgi;
        check(SUCCEEDED(d3d.As(&dxgi)), "query scrollbar DXGI device");
        ComPtr<ID2D1Factory1> factory;
        check(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            IID_PPV_ARGS(&factory))), "create scrollbar D2D factory");
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> ctx;
        check(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx)),
            "create scrollbar offscreen context");
        check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))),
            "create scrollbar text factory");
        ComPtr<ID2D1Bitmap1> target;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        check(SUCCEEDED(ctx->CreateBitmap(D2D1::SizeU(1600, 1000), nullptr, 0,
            &props, &target)), "create scrollbar bitmap");
        ctx->SetTarget(target.Get());
        CheckPopupRenderStorage(app, device.Get(), ctx.Get(), target.Get(), dxgi.Get());
        CheckFanRenderReuse(app, device.Get(), ctx.Get());
        const auto package = std::filesystem::path(LayoutPath(app)).parent_path() / L"scrollbar-fixture";
        std::filesystem::create_directories(package);
        const auto write = [&](const wchar_t* file, const char* text) {
            const HANDLE handle = CreateFileW((package / file).c_str(), GENERIC_WRITE,
                0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            check(handle != INVALID_HANDLE_VALUE, "create isolated scrollbar package file");
            DWORD written = 0;
            const auto size = static_cast<DWORD>(std::strlen(text));
            const bool ok = WriteFile(handle, text, size, &written, nullptr) && written == size;
            CloseHandle(handle);
            check(ok, "write isolated scrollbar package file");
        };
        write(L"widget.json", R"json({"schemaVersion":2,"apiVersion":2,"dataVersion":1,
            "id":"a7f377db-f8b8-462e-a08b-ed64ea774afd","slug":"popup-scrollbar-fixture",
            "version":"1.0.0","entry":"main.lua","name":"Popup scrollbar fixture",
            "description":"Isolated host scrollbar routing","author":"SnowDesktop","license":"MIT",
            "minHostVersion":"1.0.12.0","defaultSize":{"columns":2,"rows":2},
            "requiredFeatures":["view.tree.core","view.scroll","view.flex.layout",
                "view.surface.panel","view.surface.dialog","view.surface.popover"]})json");
        write(L"main.lua", R"lua(local function body()
            return view.scroll({key="outer",width="fill",height="fill",children={
                view.column({key="body",height=1800,padding=12,children={
                    view.text({key="title",text="Output devices",height=40})}})}})
            end
            return widget.define({apiVersion=2,view=body,panel=body,dialog=body,popover=body}))lua");
        app.widgetEngine_ = std::make_unique<WidgetEngine>();
        check(app.widgetEngine_->InitPreview(ctx.Get(), app.dwriteFactory_.Get()),
            "initialize render-only scrollbar engine");
        if (!app.widgetEngine_->EnsureWidgetDirectoryPreviewLoaded(L"lua-popup-owner", package))
        {
            const auto failure = app.widgetEngine_->GetWidgetRuntimeFailure(L"lua-popup-owner");
            throw std::runtime_error("load scrollbar package: " +
                (failure ? failure->detail : "no runtime diagnostic"));
        }
        for (const auto* surface : {"panel", "dialog", "popover"})
        for (const bool header : {false, true})
        {
            app.luaWidgetPanelRequest_ = {};
            app.luaWidgetPanelRequest_.widgetId = L"lua-popup-owner";
            app.luaWidgetPanelRequest_.surface = surface;
            app.luaWidgetPanelRequest_.showHeader = header;
            app.luaWidgetPanelRequest_.width = 410;
            app.luaWidgetPanelRequest_.height = 420;
            app.luaWidgetPanelAnimation_.ShowImmediately();
            ctx->BeginDraw();
            ctx->Clear(D2D1::ColorF(0, 0));
            app.DrawLuaWidgetPanel(ctx.Get(), false);
            check(SUCCEEDED(ctx->EndDraw()), "paint actual popup including gutter track");
            const auto panel = app.GetLuaWidgetPanelRect();
            const auto content = app.GetLuaWidgetPanelContentRect();
            ComPtr<ID2D1Bitmap1> pixels;
            auto readProps = props;
            readProps.bitmapOptions = static_cast<D2D1_BITMAP_OPTIONS>(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
            check(SUCCEEDED(ctx->CreateBitmap(D2D1::SizeU(1600, 1000), nullptr, 0,
                &readProps, &pixels)) &&
                SUCCEEDED(pixels->CopyFromBitmap(nullptr, target.Get(), nullptr)),
                "read actual popup pixels");
            D2D1_MAPPED_RECT mapped{};
            check(SUCCEEDED(pixels->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "map popup pixels");
            const auto* row = mapped.bits + (content.top + 10) * mapped.pitch;
            const int trackBlue = row[(panel.right - 7) * 4];
            const int gutterBlue = row[(panel.right - 11) * 4];
            pixels->Unmap();
            check(std::abs(trackBlue - gutterBlue) > 12,
                "real D2D scrollbar must be visible in the outer gutter rather than clipped at content edge");
            const auto& widget = app.widgetEngine_->GetWidgets().front();
            const auto control = std::find_if(widget.hostControls.begin(), widget.hostControls.end(),
                [&](const auto& item) { return item.id == "outer" && item.surface == surface; });
            check(control != widget.hostControls.end() &&
                control->rect.right + content.left == panel.right - 4 &&
                control->viewportWidth == content.right - content.left,
                "popup track hit area reaches outer gutter while content width stays unchanged");
            const POINT press{panel.right - 7, content.top + 10};
            check(!PtInRect(&content, press), "test thumb press lies outside content");
            app.OnLeftButtonDown(MK_LBUTTON, MAKELPARAM(press.x, press.y));
            check(app.luaWidgetPanelMouseDown_ &&
                app.widgetEngine_->IsHostScrollbarDragging(L"lua-popup-owner", surface),
                "real popup pointer-down must acquire thumb from outer gutter");
            check(app.widgetEngine_->HandleHostInputPointerMove(L"lua-popup-owner",
                press.x - content.left, press.y - content.top + 80, surface) &&
                app.widgetEngine_->RuntimeGetScrollOffset(L"lua-popup-owner", "outer", surface) > 0,
                "dragging gutter thumb moves the real retained scroll offset");
            app.widgetEngine_->HandleHostInputPointerMove(L"lua-popup-owner",
                press.x - content.left, -10000, surface);
            app.widgetEngine_->HandleHostInputPointerUp(L"lua-popup-owner",
                press.x - content.left, -10000, surface);
            ReleaseCapture();
            app.luaWidgetPanelMouseDown_ = app.mouseDown_ = false;
            app.luaWidgetPanelCaptureHwnd_ = nullptr;
            POINT wheelPoint = press;
            ClientToScreen(app.hwnd_, &wheelPoint);
            app.OnMouseWheel(MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
                MAKELPARAM(wheelPoint.x, wheelPoint.y));
            check(app.widgetEngine_->RuntimeGetScrollOffset(L"lua-popup-owner", "outer", surface) > 0,
                "real host wheel routing scrolls from the gutter outside Lua content");
            app.OnMouseWheel(MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(wheelPoint.x, wheelPoint.y));
            app.widgetEngine_->CloseWidgetPanelSurface(L"lua-popup-owner", surface);
        }
        // Negative control: a regular view overflowing right/bottom must not
        // inherit the wider chrome clip merely because edge tracks can use it.
        app.widgetEngine_->UnloadWidget(L"lua-popup-owner");
        write(L"main.lua", R"lua(local function body()
            return view.stack({key="root",height="fill",children={
                view.box({key="overflow",width=900,height=1800,
                    style={background=0xFF0000}})}})
            end
            return widget.define({apiVersion=2,view=body,popover=body}))lua");
        check(app.widgetEngine_->EnsureWidgetDirectoryPreviewLoaded(L"lua-popup-owner", package),
            "load non-scrolling overflow counterexample");
        app.luaWidgetPanelRequest_.surface = "popover";
        app.luaWidgetPanelRequest_.showHeader = false;
        ctx->BeginDraw();
        ctx->Clear(D2D1::ColorF(0, 0));
        app.DrawLuaWidgetPanel(ctx.Get(), false);
        check(SUCCEEDED(ctx->EndDraw()), "paint non-scrolling overflow counterexample");
        ComPtr<ID2D1Bitmap1> overflowPixels;
        auto readProps = props;
        readProps.bitmapOptions = static_cast<D2D1_BITMAP_OPTIONS>(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
        check(SUCCEEDED(ctx->CreateBitmap(D2D1::SizeU(1600, 1000), nullptr, 0,
            &readProps, &overflowPixels)) &&
            SUCCEEDED(overflowPixels->CopyFromBitmap(nullptr, target.Get(), nullptr)),
            "read non-scrolling overflow pixels");
        D2D1_MAPPED_RECT overflowMap{};
        check(SUCCEEDED(overflowPixels->Map(D2D1_MAP_OPTIONS_READ, &overflowMap)), "map overflow pixels");
        const auto content = app.GetLuaWidgetPanelContentRect();
        const auto* row = overflowMap.bits + (content.top + 20) * overflowMap.pitch;
        const int bodyRed = row[(content.left + 20) * 4 + 2];
        const int gutterRed = row[(content.right + 4) * 4 + 2];
        const int bottomRed = overflowMap.bits[(content.bottom + 4) * overflowMap.pitch +
            (content.left + 20) * 4 + 2];
        overflowPixels->Unmap();
        check(bodyRed > 240 && gutterRed < 160 && bottomRed < 160,
            "ordinary content must remain clipped out of the popup scrollbar gutter");
        app.widgetEngine_.reset();
        app.luaWidgetPanelRequest_ = {};
        app.luaWidgetPanelAnimation_.ResetHidden();
        ctx->SetTarget(nullptr);
    }
