    // Runs the production visibility/flush functions against real offscreen
    // Lua/D2D/DComp surfaces. Only clip setters are observed; they still
    // forward to the actual clip attached to the visual. No desktop is opened.
    class WidgetHandoffClip final : public IDCompositionRectangleClip
    {
        ULONG references_ = 1;
        ComPtr<IDCompositionRectangleClip> actual_;
    public:
        float right = 0;
        float bottom = 0;
        unsigned positiveWrites = 0;
        explicit WidgetHandoffClip(IDCompositionRectangleClip* actual) : actual_(actual) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
        {
            if (!value) return E_POINTER;
            *value = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IDCompositionClip) &&
                iid != __uuidof(IDCompositionRectangleClip)) return E_NOINTERFACE;
            *value = static_cast<IDCompositionRectangleClip*>(this);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG remaining = --references_;
            if (!remaining) delete this;
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE SetRight(float value) override
        {
            const HRESULT hr = actual_->SetRight(value);
            if (SUCCEEDED(hr)) { right = value; if (value > 0) ++positiveWrites; }
            return hr;
        }
        HRESULT STDMETHODCALLTYPE SetBottom(float value) override
        {
            const HRESULT hr = actual_->SetBottom(value);
            if (SUCCEEDED(hr)) { bottom = value; if (value > 0) ++positiveWrites; }
            return hr;
        }
        HRESULT STDMETHODCALLTYPE SetRight(IDCompositionAnimation* value) override { return actual_->SetRight(value); }
        HRESULT STDMETHODCALLTYPE SetBottom(IDCompositionAnimation* value) override { return actual_->SetBottom(value); }
#define SNOW_HANDOFF_CLIP_FORWARD(Name) \
        HRESULT STDMETHODCALLTYPE Name(float value) override { return actual_->Name(value); } \
        HRESULT STDMETHODCALLTYPE Name(IDCompositionAnimation* value) override { return actual_->Name(value); }
        SNOW_HANDOFF_CLIP_FORWARD(SetLeft)
        SNOW_HANDOFF_CLIP_FORWARD(SetTop)
        SNOW_HANDOFF_CLIP_FORWARD(SetTopLeftRadiusX)
        SNOW_HANDOFF_CLIP_FORWARD(SetTopLeftRadiusY)
        SNOW_HANDOFF_CLIP_FORWARD(SetTopRightRadiusX)
        SNOW_HANDOFF_CLIP_FORWARD(SetTopRightRadiusY)
        SNOW_HANDOFF_CLIP_FORWARD(SetBottomLeftRadiusX)
        SNOW_HANDOFF_CLIP_FORWARD(SetBottomLeftRadiusY)
        SNOW_HANDOFF_CLIP_FORWARD(SetBottomRightRadiusX)
        SNOW_HANDOFF_CLIP_FORWARD(SetBottomRightRadiusY)
#undef SNOW_HANDOFF_CLIP_FORWARD
    };

    static void CheckWidgetSurfaceHandoff(DesktopApp& app)
    {
        const auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &app.d3dDevice_, nullptr, nullptr)), "create offscreen widget WARP device");
        ComPtr<IDXGIDevice> dxgi;
        check(SUCCEEDED(app.d3dDevice_.As(&dxgi)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                IID_PPV_ARGS(&app.d2dFactory_))) &&
            SUCCEEDED(app.d2dFactory_->CreateDevice(dxgi.Get(), &app.d2dDevice_)) &&
            SUCCEEDED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                &app.d2dContext_)), "initialize actual widget D2D surfaces");
        check(SUCCEEDED(DCompositionCreateDevice2(app.d2dDevice_.Get(),
                IID_PPV_ARGS(&app.dcompDevice_))) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.dcompVisual_)) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.desktopWidgetCompositionLayer_)),
            "initialize actual widget composition tree");
        check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))),
            "initialize widget text rendering");
        app.personalizationSettings_.glassEnabled = false;
        app.personalizationSettings_.acrylicEnabled = false;
        app.lastMousePoint_ = {LONG_MIN, LONG_MIN};

        const auto package = std::filesystem::path(LayoutPath(app)).parent_path() / L"handoff-fixture";
        std::filesystem::create_directories(package);
        const auto write = [&](const wchar_t* file, const char* contents) {
            std::ofstream stream(package / file, std::ios::binary);
            stream << contents;
            stream.close();
            check(static_cast<bool>(stream), "write isolated Lua handoff package");
        };
        write(L"widget.json", R"json({"schemaVersion":2,"apiVersion":2,"dataVersion":1,
            "id":"8c29eb0e-71d4-4479-b217-5203cfa93912","slug":"handoff-fixture",
            "version":"1.0.0","entry":"main.lua","name":"Handoff fixture",
            "description":"Isolated resize surface handoff","author":"SnowDesktop","license":"MIT",
            "minHostVersion":"1.0.12.0","defaultSize":{"columns":2,"rows":2},
            "requiredFeatures":["view.tree.core"]})json");
        write(L"main.lua", R"lua(return widget.define({apiVersion=2,view=function()
            return view.text({key="body",text="Surface handoff",width="fill",height="fill"})
            end}))lua");
        const std::wstring id = L"handoff-lua";
        app.widgetEngine_ = std::make_unique<WidgetEngine>();
        check(app.widgetEngine_->InitPreview(app.d2dContext_.Get(), app.dwriteFactory_.Get()) &&
            app.widgetEngine_->EnsureWidgetDirectoryPreviewLoaded(id, package),
            "load the actual isolated Lua handoff widget");
        DesktopWidget widget;
        widget.id = id;
        widget.type = DesktopWidgetType::LuaScript;
        widget.showTitle = false;
        widget.bottomBarHover = true;
        widget.packageId = L"8c29eb0e-71d4-4479-b217-5203cfa93912";
        widget.gridCell = {app.gridPages_.front().id, 0, 0};
        widget.gridSpan = {2, 2};
        widget.bounds = GetGridRect(app.gridPages_, widget.gridCell, widget.gridSpan);
        app.widgets_.push_back(std::move(widget));
        app.items_oo_.push_back(std::make_unique<LuaScript>(&app.widgets_.back(), &app));

        auto& item = app.desktopWidgetCompositionItems_[id];
        ComPtr<IDCompositionRectangleClip> actualClip;
        check(SUCCEEDED(app.dcompDevice_->CreateVisual(&item.visual)) &&
            SUCCEEDED(app.dcompDevice_->CreateRectangleClip(&actualClip)) &&
            SUCCEEDED(item.visual->SetClip(actualClip.Get())),
            "attach a real clip to the offscreen widget");
        ComPtr<WidgetHandoffClip> clip;
        clip.Attach(new WidgetHandoffClip(actualClip.Get()));
        item.clip = clip;
        item.visible = true;
        app.pendingDesktopWidgetCompositions_.insert(id);
        const auto flush = [&] {
            check(app.FlushPendingDesktopWidgetComposition() &&
                !app.desktopWidgetCompositionFailurePending_ && item.surface &&
                app.pendingDesktopWidgetCompositions_.empty(),
                "complete the actual Lua surface draw without deferred or failed work");
            const RECT frame = app.GetStandaloneWidgetFrameRect(app.widgets_.front());
            const auto& runtime = app.widgetEngine_->GetWidgets().front();
            check(runtime.valid && runtime.viewTree.has_value() &&
                !app.widgetEngine_->GetWidgetRuntimeFailure(id) &&
                EqualRect(&item.bounds, &frame) &&
                EqualRect(&runtime.lastBounds, &frame) &&
                runtime.lastColumns == app.widgets_.front().gridSpan.columns &&
                runtime.lastRows == app.widgets_.front().gridSpan.rows &&
                clip->right == static_cast<float>(item.width) &&
                clip->bottom == static_cast<float>(item.height),
                "reveal only the surface that rendered the latest Lua span and frame");
        };
        flush();
        const auto armHover = [&] {
            app.desktopWidgetHoverTailToken_ = app.uiAnimationScheduler_.ScheduleOnce(
                60'000, [](snowdesktop::UiScheduleToken) {});
            check(app.desktopWidgetHoverTailToken_ != 0, "retain a controlled pending passive hover");
            return app.desktopWidgetHoverTailToken_;
        };
        const auto setSpan = [&](GridSpan span) {
            app.widgets_.front().gridSpan = span;
            app.widgets_.front().bounds = GetGridRect(app.gridPages_,
                app.widgets_.front().gridCell, span);
        };
        const auto show = [&] {
            app.SetDesktopWidgetCompositionVisible(id, true,
                app.GetStandaloneWidgetFrameRect(app.widgets_.front()));
        };
        const auto assertWaitingForFreshPixels = [&](unsigned writes) {
            check(clip->right == 0 && clip->bottom == 0 && clip->positiveWrites == writes,
                "a released resize must not reveal retained old-size pixels before EndDraw");
            check(app.desktopWidgetHoverTailToken_ == 0 &&
                app.pendingDesktopWidgetCompositions_.contains(id),
                "a resize handoff must bypass pending passive hover and queue the current surface");
        };

        for (const GridSpan span : {GridSpan{3, 2}, GridSpan{1, 1}, GridSpan{4, 3}})
        {
            app.SetDesktopWidgetCompositionVisible(id, false, item.bounds);
            check(clip->right == 0 && clip->bottom == 0, "resize preview hides its original surface");
            armHover();
            setSpan(span);
            const unsigned before = clip->positiveWrites;
            show();
            assertWaitingForFreshPixels(before);
            flush();
        }
        // Re-entering the same page must refresh hidden content too.
        app.SetDesktopWidgetCompositionVisible(id, false, item.bounds);
        armHover();
        unsigned before = clip->positiveWrites;
        show();
        assertWaitingForFreshPixels(before);
        flush();

        // Unchanged visible hover keeps coalescing; geometry changes interrupt it.
        const auto hover = armHover();
        before = clip->positiveWrites;
        show();
        check(app.desktopWidgetHoverTailToken_ == hover && clip->positiveWrites == before &&
            clip->right > 0 && clip->bottom > 0, "unchanged hover retains its current surface and batching");
        setSpan({2, 2});
        show();
        assertWaitingForFreshPixels(before);
        setSpan({3, 1});
        show();
        assertWaitingForFreshPixels(before);
        flush();

        // A temporarily unavailable renderer cannot expose retained stale pixels.
        setSpan({2, 1});
        show();
        ComPtr<IDCompositionDesktopDevice> device = std::move(app.dcompDevice_);
        check(!app.FlushPendingDesktopWidgetComposition() &&
            clip->right == 0 && clip->bottom == 0,
            "a failed refresh must leave the stale widget surface clipped");
        app.dcompDevice_ = std::move(device);
        app.desktopWidgetCompositionFailurePending_ = false;
        flush();

        app.ResetDesktopWidgetComposition();
        app.widgetEngine_.reset();
        app.uiAnimationScheduler_.Shutdown();
        app.dcompVisual_.Reset();
        app.dcompDevice_.Reset();
        app.d2dContext_.Reset();
        app.d2dDevice_.Reset();
        app.d2dFactory_.Reset();
        app.d3dDevice_.Reset();
        app.dwriteFactory_.Reset();
    }
#include "../drag_drop/native_widget_drag_hover_access.inc"
