    // Only OS hit testing / invalidation are isolated. Input resolution,
    // widget DComp BeginDraw/EndDraw and commit execute their production paths.
    struct WidgetDragObservation
    {
        std::vector<RECT> dirty;
        std::vector<std::wstring> drawn;
        unsigned full = 0, commits = 0, sync = 0;
    };
    static inline WidgetDragObservation* widgetDragObservation = nullptr;
    static BOOL WINAPI WidgetDragInvalidate(HWND, const RECT* dirty, BOOL)
    {
        if (dirty) widgetDragObservation->dirty.push_back(*dirty);
        else { ++widgetDragObservation->full; widgetDragObservation->dirty.push_back({0, 0, 800, 600}); }
        return TRUE;
    }
    static BOOL WINAPI WidgetDragUpdate(HWND) { ++widgetDragObservation->sync; return TRUE; }
    static BOOL WINAPI WidgetDragClient(HWND, RECT* rect) { *rect = {0, 0, 800, 600}; return TRUE; }
    static BOOL WINAPI WidgetDragNoScreen(HWND, POINT*) { return FALSE; }
    static HWND WINAPI WidgetDragNoWindow(POINT) { return nullptr; }
    static SHORT WINAPI WidgetDragNoKey(int) { return 0; }

    static std::vector<std::uint32_t> WidgetDragRead(ID2D1DeviceContext* context, ID2D1Bitmap1* source)
    {
        ComPtr<ID2D1Bitmap1> readable;
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(context->CreateBitmap(source->GetPixelSize(), nullptr, 0, &properties, &readable)) ||
            FAILED(readable->CopyFromBitmap(nullptr, source, nullptr)))
            throw std::runtime_error("read production drag raster");
        D2D1_MAPPED_RECT map{};
        if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &map))) throw std::runtime_error("map drag raster");
        const auto size = source->GetPixelSize();
        std::vector<std::uint32_t> pixels(static_cast<size_t>(size.width) * size.height);
        for (UINT y = 0; y < size.height; ++y)
            std::memcpy(pixels.data() + static_cast<size_t>(y) * size.width,
                map.bits + static_cast<size_t>(y) * map.pitch, size.width * sizeof(std::uint32_t));
        readable->Unmap();
        return pixels;
    }
    static ComPtr<ID2D1Bitmap1> WidgetDragBitmap(DesktopApp& app)
    {
        ComPtr<ID2D1Bitmap1> bitmap;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(app.d2dContext_->CreateBitmap(D2D1::SizeU(800, 600), nullptr, 0, &properties, &bitmap)))
            throw std::runtime_error("allocate isolated drag raster");
        return bitmap;
    }
    // Independent complete raster reference: the actual icon/chrome renderers,
    // without consulting the invalidation computation or its owner decisions.
    static std::vector<std::uint32_t> WidgetDragRaster(DesktopApp& app,
        ID2D1Bitmap1* bitmap, int widget, const RECT* dirty = nullptr)
    {
        auto* context = app.d2dContext_.Get();
        const bool previousPaint = app.compositionPaintInProgress_;
        auto pending = app.pendingDesktopWidgetCompositions_;
        app.compositionPaintInProgress_ = true;
        context->SetTarget(bitmap);
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->BeginDraw();
        if (dirty) context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(dirty->left),
            static_cast<float>(dirty->top), static_cast<float>(dirty->right), static_cast<float>(dirty->bottom)),
            D2D1_ANTIALIAS_MODE_ALIASED);
        context->Clear(D2D1::ColorF(0, 0.f));
        if (widget < 0) app.DrawStaticBackground(context, dirty, false);
        else
        {
            auto* data = &app.widgets_.at(static_cast<size_t>(widget));
            bool found = false;
            for (const auto& container : app.containers_)
                if (auto* native = dynamic_cast<WidgetContainer*>(container.get()); native && native->GetWidgetData() == data)
                { native->DrawChrome(context, app.lastMousePoint_); found = true; break; }
            if (!found)
                for (const auto& item : app.items_oo_)
                    if (auto* native = dynamic_cast<Widget*>(item.get()); native && native->GetWidgetData() == data)
                    {
                        if (auto* lua = dynamic_cast<LuaScript*>(native))
                            lua->DrawCompositionSurface(context, data->bounds, data->selected ? 2 : 0, true);
                        else native->Draw(context, data->bounds, data->selected ? 2 : 0);
                        found = true; break;
                    }
            if (!found) throw std::runtime_error("missing actual drag widget renderer");
        }
        if (dirty) context->PopAxisAlignedClip();
        const HRESULT hr = context->EndDraw();
        context->SetTarget(nullptr);
        app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
        app.pendingDesktopWidgetCompositions_ = std::move(pending);
        app.compositionPaintInProgress_ = previousPaint;
        if (FAILED(hr)) throw std::runtime_error("complete actual drag raster");
        return WidgetDragRead(context, bitmap);
    }

    static void RunWidgetDragHover(DesktopApp& app, const std::filesystem::path& root, bool restoreLegacyInvalidation)
    {
        const auto require = [](bool ok, const char* message) { if (!ok) throw std::runtime_error(message); };
        app.hwnd_ = completionWindow;
        app.initializationExperimentDirectory_ = root;
        app.generalSettings_.dockEnabled = false;
        app.generalSettings_.statusBar.enabled = false;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &app.d3dDevice_, nullptr, nullptr)), "create isolated drag WARP device");
        ComPtr<IDXGIDevice> dxgi;
        require(SUCCEEDED(app.d3dDevice_.As(&dxgi)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&app.d2dFactory_))) &&
            SUCCEEDED(app.d2dFactory_->CreateDevice(dxgi.Get(), &app.d2dDevice_)) &&
            SUCCEEDED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &app.d2dContext_)) &&
            SUCCEEDED(DCompositionCreateDevice2(app.d2dDevice_.Get(), IID_PPV_ARGS(&app.dcompDevice_))) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.dcompVisual_)), "create offscreen drag composition tree");
        require(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))), "create drag text renderer");
        app.d2dContext_->SetDpi(96, 96);
        app.RecreateItemTextFormat(); app.RecreateComponentListTextFormat();
        require(app.uiAnimationScheduler_.Initialize(), "initialize actual drag presentation scheduler");
        app.customDesktopVisible_ = true;
        app.personalizationSettings_.glassEnabled = app.personalizationSettings_.acrylicEnabled = false;
        app.gridPages_.front().bounds = app.gridPages_.front().workArea = app.gridPages_.front().visualWorkArea = RECT{0, 0, 800, 600};
        app.gridPages_.front().columns = 8; app.gridPages_.front().rows = 6;
        app.items_.at(1).selected = false;
        app.items_.at(1).bounds = {150, 80, 230, 160};
        app.items_.at(1).name = L"Icon under widget edge with a long title";
        app.widgets_.reserve(4);
        const RECT frames[] = {{180, 100, 400, 330}, {360, 220, 580, 450}, {480, 70, 700, 200}, {40, 360, 260, 570}};
        for (size_t i = 0; i < 4; ++i)
        {
            DesktopWidget data;
            data.id = L"drag-hover-" + std::to_wstring(i);
            data.type = i == 2 ? DesktopWidgetType::LuaScript : DesktopWidgetType::Guide;
            data.title = L"Drag hover reference title";
            data.bounds = frames[i]; data.gridCell = {app.gridPages_.front().id, 2, 1};
            data.gridSpan = {3, 3}; data.bottomBarHover = true;
            app.widgets_.push_back(std::move(data));
            auto widget = CreateWidget(&app.widgets_.back(), &app);
            if (auto* container = dynamic_cast<WidgetContainer*>(widget.get()))
            { app.containers_.push_back(std::unique_ptr<Container>(container)); widget.release(); }
            else app.items_oo_.push_back(std::move(widget));
        }
        const auto package = root / L"drag-hover-fixture";
        std::filesystem::create_directories(package);
        { std::ofstream out(package / L"widget.json"); out << R"json({"schemaVersion":2,"apiVersion":2,"dataVersion":1,"id":"b82b1ef2-f861-4f86-a5a3-d9d8e774db91","slug":"drag-hover-fixture","version":"1.0.0","entry":"main.lua","name":"Drag hover","description":"Offscreen reference","author":"SnowDesktop","license":"MIT","minHostVersion":"1.0.12.0","defaultSize":{"columns":3,"rows":3},"requiredFeatures":["view.tree.core"]})json"; }
        { std::ofstream out(package / L"main.lua"); out << R"lua(return widget.define({apiVersion=2,view=function() return view.text({key="body",text="Lua drag hover reference",width="fill",height="fill"}) end}))lua"; }
        app.widgetEngine_ = std::make_unique<WidgetEngine>();
        require(app.widgetEngine_->InitPreview(app.d2dContext_.Get(), app.dwriteFactory_.Get()) &&
            app.widgetEngine_->EnsureWidgetDirectoryPreviewLoaded(app.widgets_[2].id, package), "load actual Lua drag fixture");
        app.widgets_[2].packageId = L"b82b1ef2-f861-4f86-a5a3-d9d8e774db91";
        Begin(app, false, 1); app.dragSession_.SetVisualVisible(false);
        app.lastMousePoint_ = {100, 120};
        app.desktopBackdropFullCollectionPending_ = false;

        WidgetDragObservation observation{}; widgetDragObservation = &observation;
        struct ResetObservation { ~ResetObservation() { NativeDragIntegrationAccess::widgetDragObservation = nullptr; } } resetObservation;
        InputSafetyImport invalidation("InvalidateRect", reinterpret_cast<ULONG_PTR>(&WidgetDragInvalidate));
        InputSafetyImport update("UpdateWindow", reinterpret_cast<ULONG_PTR>(&WidgetDragUpdate));
        InputSafetyImport client("GetClientRect", reinterpret_cast<ULONG_PTR>(&WidgetDragClient));
        InputSafetyImport screen("ClientToScreen", reinterpret_cast<ULONG_PTR>(&WidgetDragNoScreen));
        InputSafetyImport windows("WindowFromPoint", reinterpret_cast<ULONG_PTR>(&WidgetDragNoWindow));
        InputSafetyImport keys("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&WidgetDragNoKey));
        auto retainedRoot = WidgetDragBitmap(app), complete = WidgetDragBitmap(app);
        (void)WidgetDragRaster(app, retainedRoot.Get(), -1);
        std::vector<std::vector<std::uint32_t>> retained;
        for (size_t i = 0; i < app.widgets_.size(); ++i)
        {
            require(app.QueueDesktopWidgetComposition(app.widgets_[i].id), "initialize actual child drag surface");
            retained.push_back(WidgetDragRaster(app, complete.Get(), static_cast<int>(i)));
        }
        observation.full = observation.sync = 0;
        app.dragSession_.SetVisualVisible(true);
        app.PresentPointerInteractionFrame(true);
        app.dragSession_.SetVisualVisible(false);
        require(observation.full > 0 && observation.sync > 0,
            "first feedback retains full migration and synchronous root presentation");
        bool detectsMissingOldWidget = false, detectsMissingIconDamage = false;
        unsigned savedUnrelatedDraws = 0, synchronousFrames = 0, legacyUnrelatedDraws = 0;
        auto* guide = dynamic_cast<WidgetContainer*>(app.containers_.at(1).get());
        require(guide != nullptr, "resolve the actual native guide hit owner");
        RECT button{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
        for (LONG y = frames[0].top; y < frames[0].bottom; ++y)
            for (LONG x = frames[0].left; x < frames[0].right; ++x)
                if (guide->HitTestWidget({x, y}) == WidgetHit::GuideAddWidgetBtn)
                { button.left = std::min(button.left, x); button.top = std::min(button.top, y);
                  button.right = std::max(button.right, x + 1); button.bottom = std::max(button.bottom, y + 1); }
        require(!IsRectEmpty(&button), "matrix includes a real native button rather than a guessed coordinate");
        const POINT buttonPoint{(button.left + button.right) / 2, (button.top + button.bottom) / 2};
        const POINT points[] = {{160, 120}, {190, 120}, {210, 120}, {220, 150}, buttonPoint,
            {380, 260}, {420, 280}, {510, 120}, {520, 130}, {710, 120}, {160, 120}};
        for (const POINT point : points)
        {
            const POINT old = app.lastMousePoint_;
            observation.dirty.clear(); observation.drawn.clear(); observation.full = observation.commits = observation.sync = 0;
            app.OnMouseMoveAt(MK_LBUTTON, point);
            if (restoreLegacyInvalidation &&
                (app.IsPointOverWidgetChrome(old) || app.IsPointOverWidgetChrome(point)))
            {
                std::cout << "Negative control: execute original widget-crossing full root invalidation after actual OnMouseMoveAt\n";
                InvalidateRect(app.hwnd_, nullptr, FALSE);
            }
            require(app.dragSession_.IsActive() && app.dragSession_.TargetContainer() == app.GetDesktopGrid() &&
                app.dragSession_.CurrentPoint().x == point.x && app.dragSession_.CurrentPoint().y == point.y,
                "actual input route preserves active drag, point and desktop target");
            require(observation.full == 0, "ordinary widget crossing must not invalidate the complete desktop");
            const auto inputDraws = observation.drawn;
            if (!inputDraws.empty())
            {
                require(observation.commits == 1 && app.pendingDesktopWidgetCompositions_.empty() &&
                    !app.desktopWidgetHoverTailToken_, "old/new child draws commit together before pressed move returns without timer delay");
                ++synchronousFrames;
            }
            app.dragSession_.SetVisualVisible(true);
            app.PresentPointerInteractionFrame(true);
            app.dragSession_.SetVisualVisible(false);
            require(observation.full == 0 && !app.desktopWidgetCompositionFailurePending_,
                "subsequent desktop target feedback preserves the immediate foreground route");
            require(!app.desktopBackdropFullCollectionPending_, "local pointer work preserves backdrop owner collection scope");
            // Repaint exactly the production root damage, retaining all other pixels.
            const auto priorRoot = WidgetDragRead(app.d2dContext_.Get(), retainedRoot.Get());
            for (const auto& dirty : observation.dirty) (void)WidgetDragRaster(app, retainedRoot.Get(), -1, &dirty);
            const auto expectedRoot = WidgetDragRaster(app, complete.Get(), -1);
            require(WidgetDragRead(app.d2dContext_.Get(), retainedRoot.Get()) == expectedRoot,
                "retained icon/background pixels equal a complete production reference including occlusion and title");
            if (old.x == 160 && point.x == 190 && old.y == 120 && point.y == 120 &&
                !observation.dirty.empty() && priorRoot != expectedRoot) detectsMissingIconDamage = true;
            for (size_t i = 0; i < app.widgets_.size(); ++i)
            {
                const auto expected = WidgetDragRaster(app, complete.Get(), static_cast<int>(i));
                const auto& id = app.widgets_[i].id;
                const unsigned count = static_cast<unsigned>(std::count(inputDraws.begin(), inputDraws.end(), id));
                require(count <= 1, "same owner must not be drawn twice in one pointer transaction");
                if (count)
                {
                    const auto& surface = app.desktopWidgetCompositionItems_.at(id);
                    require(surface.surface && surface.visible && EqualRect(&surface.bounds, &frames[i]) &&
                        !app.desktopWidgetCompositionFailurePending_, "observed actual DComp draw completed with unchanged geometry");
                    if (PtInRect(&frames[i], old) && !PtInRect(&frames[i], point) && retained[i] != expected)
                        detectsMissingOldWidget = true;
                    retained[i] = expected;
                }
                require(retained[i] == expected, "retained widget pixels equal complete chrome/content reference after old/new hover changes");
                if (i == 3) { require(count == 0, "unrelated component never redraws during this gesture"); ++savedUnrelatedDraws; }
            }
            // Execute the original failure's full background/owner route as a
            // reference control, rather than asserting a source string.
            if (app.IsPointOverWidgetChrome(old) || app.IsPointOverWidgetChrome(point))
            {
                const auto legacyStart = observation.drawn.size();
                const unsigned previousFull = observation.full;
                InvalidateRect(app.hwnd_, nullptr, FALSE);
                require(observation.full == previousFull + 1, "old widget-crossing branch requests actual full root invalidation");
                app.compositionPaintInProgress_ = true;
                (void)WidgetDragRaster(app, complete.Get(), -1);
                app.compositionPaintInProgress_ = false;
                for (const auto& widget : app.widgets_) require(app.QueueDesktopWidgetComposition(widget.id), "legacy full route draws every child");
                require(std::find(observation.drawn.begin() + static_cast<std::ptrdiff_t>(legacyStart), observation.drawn.end(),
                    app.widgets_[3].id) != observation.drawn.end(), "full reference actually executes the unrelated owner's surface draw");
                ++legacyUnrelatedDraws;
            }
        }
        require(detectsMissingOldWidget && detectsMissingIconDamage,
            "negative controls actually distinguish stale old hover and omitted icon occlusion damage");
        require(savedUnrelatedDraws == std::size(points) && synchronousFrames > 0,
            "real owner counts decrease while relevant input remains synchronous");
        require(app.items_[0].selected && !app.items_[1].selected && app.dragSession_.Items().size() == 1 &&
            app.dragSession_.IsMoveAction() && app.widgets_.size() == 4,
            "pointer refresh preserves source selection, move action and all model owners");
        app.desktopBackdropFullCollectionPending_ = false;
        observation.full = 0; app.presentedDragFeedbackRevision_ = app.dragSession_.PresentationRevision();
        auto unavailable = std::move(app.dcompDevice_);
        app.OnMouseMoveAt(MK_LBUTTON, {190, 120});
        require(observation.full > 0, "missing child composition device preserves full failure invalidation");
        app.dcompDevice_ = std::move(unavailable);
        app.ResetDesktopWidgetComposition(); app.widgetEngine_.reset();
        app.CancelUiAnimationFrame(); app.uiAnimationScheduler_.Shutdown();
        app.d2dContext_->SetTarget(nullptr);
        app.EndDragSession(); app.dcompVisual_.Reset(); app.dcompDevice_.Reset();
        std::cout << "Widget drag: " << std::size(points) << " complete-vs-retained pixel frames; unrelated child draws local=0 legacy="
            << legacyUnrelatedDraws << "; synchronous child frames=" << synchronousFrames
            << "; hit/state, first migration, fallback and stale-pixel controls passed\n";
    }
