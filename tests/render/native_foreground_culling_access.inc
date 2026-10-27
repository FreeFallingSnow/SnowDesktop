    // Calls the actual shared foreground renderer. Only the DComp allocation
    // boundary is replaced: D2D rasterization, Dock containers, tooltip damage
    // and the renderer's BeginDraw/repair decisions execute production code.
    class ForegroundSurface final : public IDCompositionSurface
    {
        ULONG references_ = 1;
        ComPtr<ID2D1DeviceContext> context_;
        ComPtr<ID2D1Bitmap1> target_;
    public:
        std::vector<RECT> updates;
        explicit ForegroundSurface(ID2D1DeviceContext* context, ID2D1Bitmap1* target)
            : context_(context), target_(target) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
        {
            if (!value) return E_POINTER;
            *value = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IDCompositionSurface)) return E_NOINTERFACE;
            *value = static_cast<IDCompositionSurface*>(this); AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG remaining = --references_;
            if (!remaining) delete this;
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE BeginDraw(const RECT* update, REFIID iid, void** value, POINT* offset) override
        {
            if (!value || !offset) return E_POINTER;
            const auto size = target_->GetPixelSize();
            const RECT clip = update ? *update : RECT{0, 0, static_cast<LONG>(size.width), static_cast<LONG>(size.height)};
            updates.push_back(clip);
            *offset = {clip.left, clip.top};
            context_->SetTarget(target_.Get());
            context_->SetTransform(D2D1::Matrix3x2F::Identity());
            context_->BeginDraw();
            context_->PushAxisAlignedClip(DesktopApp::ToD2DRect(clip), D2D1_ANTIALIAS_MODE_ALIASED);
            return context_->QueryInterface(iid, value);
        }
        HRESULT STDMETHODCALLTYPE EndDraw() override
        {
            context_->PopAxisAlignedClip();
            const HRESULT hr = context_->EndDraw();
            context_->SetTarget(nullptr);
            return hr;
        }
        HRESULT STDMETHODCALLTYPE SuspendDraw() override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE ResumeDraw() override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Scroll(const RECT*, const RECT*, int, int) override { return E_NOTIMPL; }
    };

    class ForegroundItem final : public Item
    {
        Item* source_;
    public:
        unsigned draws = 0;
        explicit ForegroundItem(Item* source) : source_(source) {}
        Item* Original() const { return source_; }
        std::wstring GetTitle() const override { return source_->GetTitle(); }
        std::wstring GetPath() const override { return source_->GetPath(); }
        HBITMAP GetIconBitmap() const override { return nullptr; }
        RECT GetBounds() const override { return source_->GetBounds(); }
        void SetBounds(RECT value) override { source_->SetBounds(value); }
        bool IsSelected() const override { return source_->IsSelected(); }
        void SetSelected(bool value) override { source_->SetSelected(value); }
        Container* GetContainer() const override { return source_->GetContainer(); }
        ComPtr<IDataObject> CreateDataObject() override { return nullptr; }
        void Draw(ID2D1DeviceContext* context, RECT bounds, int) override
        {
            ++draws;
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(0x36b879), &brush)))
                context->FillRectangle(DesktopApp::ToD2DRect(bounds), brush.Get());
        }
    };

    static void CheckForegroundCulling(DesktopApp& app)
    {
        const auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &app.d3dDevice_, nullptr, nullptr)), "create foreground WARP device");
        ComPtr<IDXGIDevice> dxgi;
        check(SUCCEEDED(app.d3dDevice_.As(&dxgi)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&app.d2dFactory_))) &&
            SUCCEEDED(app.d2dFactory_->CreateDevice(dxgi.Get(), &app.d2dDevice_)) &&
            SUCCEEDED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &app.d2dContext_)) &&
            SUCCEEDED(DCompositionCreateDevice2(app.d2dDevice_.Get(), IID_PPV_ARGS(&app.dcompDevice_))) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.dcompVisual_)) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.desktopForegroundCompositionVisual_)) &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))),
            "initialize real foreground D2D, text and composition resources");

        // A hidden fixture window supplies client geometry only. Run() and
        // all desktop presentation/input windows remain uninitialized.
        const HWND savedWindow = app.hwnd_;
        const HWND geometryWindow = CreateWindowExW(0, L"STATIC", L"Foreground geometry",
            WS_POPUP, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(geometryWindow != nullptr, "create hidden foreground geometry fixture");
        struct RestoreWindow
        {
            DesktopApp& app; HWND previous; HWND fixture;
            ~RestoreWindow() { app.hwnd_ = previous; DestroyWindow(fixture); }
        } restore{app, savedWindow, geometryWindow};
        app.hwnd_ = geometryWindow;
        check(!IsWindowVisible(geometryWindow), "foreground fixture never presents a desktop window");
        app.gridPages_.front().bounds = {0, 0, 800, 600};
        app.gridPages_.front().workArea = app.gridPages_.front().visualWorkArea = {0, 0, 800, 600};
        app.personalizationSettings_ = PersonalizationSettings::DarkPreset();
        app.personalizationSettings_.glassEnabled = false;
        app.personalizationSettings_.acrylicEnabled = false;
        app.generalSettings_.dockEnabled = true;
        app.dockSettings_.followComponentAppearance = true;
        app.dockSettings_.position = DockPosition::Bottom;
        app.dockSettings_.hoverEffect = 0;
        app.dockSettings_.showQuickSearchButton = false;
        app.dockSettings_.showWindowsButton = false;
        app.lastMousePoint_ = {LONG_MIN, LONG_MIN};
        app.items_oo_.clear(); app.items_.clear(); app.containers_.clear();
        for (unsigned i = 0; i < 2; ++i)
        {
            DesktopItem item; item.name = i ? L"Right foreground label" : L"Left foreground label";
            item.layoutKey = item.parsingName = item.name;
            const std::vector<unsigned> iconPixels(256 * 256, 0xff36b879);
            item.iconBitmap = CreateBitmap(256, 256, 1, 32, iconPixels.data());
            check(item.iconBitmap != nullptr, "supply deterministic tooltip icon pixels");
            item.iconBitmapSize = {256, 256}; item.iconState = IconState::FullQuality;
            app.items_.push_back(std::move(item));
        }
        // Real DockEntryItem title/icon lookup uses this production index;
        // populating the vector alone leaves both providers unresolved.
        app.RefreshDesktopItemIndexCache();
        app.dockEntries_.clear();
        for (const auto& item : app.items_)
        {
            DockEntry entry; entry.type = DockEntryType::DesktopItem; entry.reference = item.layoutKey;
            app.dockEntries_.push_back(std::move(entry));
        }
        auto left = std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{20, 400, 340, 550});
        auto right = std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{460, 400, 780, 550});
        DockContainer* docks[]{left.get(), right.get()};
        app.containers_.push_back(std::move(left)); app.containers_.push_back(std::move(right));
        std::vector<std::unique_ptr<ForegroundItem>> items;
        for (auto* dock : docks)
            for (const auto& slot : dock->GetSlots())
                if (slot->GetItem())
                {
                    check(!slot->GetItem()->GetTitle().empty() &&
                        slot->GetItem()->GetIconBitmap() != nullptr,
                        "real Dock providers resolve populated titles and icons through the desktop index");
                    auto marker = std::make_unique<ForegroundItem>(slot->GetItem());
                    slot->SetItem(marker.get()); items.push_back(std::move(marker));
                }
        check(items.size() == 4, "two production Docks expose four actual slot drawing providers");
        const auto resetCounts = [&] { for (auto& item : items) item->draws = 0; };
        const auto totalDraws = [&] { unsigned total = 0; for (const auto& item : items) total += item->draws; return total; };

        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        const auto readProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        ComPtr<ID2D1Bitmap1> target, readback;
        check(SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(800, 600), nullptr, 0, properties, &target)) &&
            SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(800, 600), nullptr, 0, readProperties, &readback)),
            "allocate retained foreground and CPU readback");
        auto* surface = new ForegroundSurface(app.d2dContext_.Get(), target.Get());
        app.desktopForegroundCompositionSurface_.Attach(surface);
        app.desktopForegroundCompositionWidth_ = 800; app.desktopForegroundCompositionHeight_ = 600;
        const auto pixels = [&] {
            D2D1_MAPPED_RECT mapped{};
            check(SUCCEEDED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr)) &&
                SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "map foreground pixels");
            std::vector<unsigned char> result(800 * 600 * 4);
            for (unsigned y = 0; y < 600; ++y)
                std::memcpy(result.data() + y * 800 * 4, mapped.bits + y * mapped.pitch, 800 * 4);
            readback->Unmap(); return result;
        };
        const auto render = [&](const RECT* update, bool hidden = false) {
            check(app.RenderDesktopForegroundComposition(update, hidden), "actual shared foreground render succeeds");
        };
        render(nullptr); const auto reference = pixels();
        check(totalDraws() == 4, "full reference executes every Dock content provider");
        resetCounts();
        const RECT leftUpdate{0, 380, 360, 580};
        render(&leftUpdate);
        check(totalDraws() == 2, "local foreground update omits every unrelated Dock Item::Draw");
        check(pixels() == reference, "retained local foreground pixels match complete reference byte for byte");
        resetCounts();
        const RECT unrelated{370, 100, 410, 140};
        render(&unrelated);
        check(totalDraws() == 0 && pixels() == reference,
            "unrelated partial update issues no Dock pixel drawing and retains identical content");

        // Execute the original failure path in this same renderer: a clipped
        // draw with no supplied culling region still traverses all four items.
        POINT offset{}; ID2D1DeviceContext* raw = nullptr;
        check(SUCCEEDED(surface->BeginDraw(&unrelated, __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(&raw), &offset)), "start original foreground draw path");
        ComPtr<ID2D1DeviceContext> context; context.Attach(raw);
        app.DrawDesktopForeground(context.Get(), false);
        check(SUCCEEDED(surface->EndDraw()) && totalDraws() == 4,
            "negative control reaches original unconditional Dock drawing despite the same update clip");

        // A tooltip newly exposed by popup dismissal must be discovered even
        // when this frame requested pixels nowhere near that Dock.
        resetCounts();
        for (auto* dock : docks)
            for (const auto& slot : dock->GetSlots())
                if (auto* marker = dynamic_cast<ForegroundItem*>(slot->GetItem()))
                    slot->SetItem(marker->Original());
        render(nullptr);
        const RECT anchor = docks[1]->GetSlots().front()->GetBounds();
        app.lastMousePoint_ = {(anchor.left + anchor.right) / 2, (anchor.top + anchor.bottom) / 2};
        // Semantic titles remain active with magnification disabled. Keep
        // geometry stationary while testing discovery and damage repair.
        surface->updates.clear();
        render(&unrelated);
        std::cout << "R06 new tooltip repair passes=" << surface->updates.size() << '\n';
        check(surface->updates.size() == 2,
            "new tooltip outside requested damage triggers production discovery and repair before return");
        const auto withTooltip = pixels();
        render(nullptr);
        check(withTooltip == pixels(), "tooltip repair pixels match the complete foreground reference");
        app.lastMousePoint_ = {LONG_MIN, LONG_MIN};
        render(&unrelated);
        const auto withoutTooltip = pixels();
        render(nullptr);
        check(withoutTooltip == pixels(), "old tooltip text is erased during the same local transaction");
        check(withoutTooltip != withTooltip, "tooltip checks observe real text pixel changes");
        const auto checkTiles = [&] {
            render(nullptr);
            const auto complete = pixels();
            for (LONG y = 0; y < 600; y += 100)
                for (LONG x = 0; x < 800; x += 100)
                {
                    const RECT tile{x, y, x + 100, y + 100};
                    render(&tile);
                    check(pixels() == complete,
                        "every retained tile matches the full reference, including stroke and hint boundaries");
                }
        };
        app.marqueeActive_ = true;
        app.marqueeWidgetIndex_ = static_cast<size_t>(-1);
        app.marqueeRect_ = {250, 200, 550, 350};
        checkTiles();
        app.marqueeActive_ = false;
        snowdesktop::LargeIconConfig config;
        config.columns = 2; config.rows = 1; config.shape = 2;
        app.largeIconGesture_ = DesktopApp::LargeIconGesture{
            app.items_.front().layoutKey, config,
            GridCell{app.gridPages_.front().id, 1, 1}, true, false, false};
        app.largeIconGesture_->valid = true;
        checkTiles();
        app.largeIconGesture_.reset();
        render(nullptr, true);
        const auto hiddenPixels = pixels();
        check(std::all_of(hiddenPixels.begin(), hiddenPixels.end(), [](unsigned char pixel) { return pixel == 0; }),
            "hidden unretained Docks keep the established visibility contract");
        // Exercise replacement with a real DComp allocation as well. An
        // unrelated local request must initialize both Docks on the new blank
        // surface; retaining the fake surface here would miss this regression.
        app.desktopForegroundCompositionSurface_.Reset();
        app.desktopForegroundCompositionWidth_ = app.desktopForegroundCompositionHeight_ = 0;
        items.clear();
        for (auto* dock : docks)
            for (const auto& slot : dock->GetSlots())
                if (slot->GetItem())
                {
                    auto marker = std::make_unique<ForegroundItem>(slot->GetItem());
                    slot->SetItem(marker.get()); items.push_back(std::move(marker));
                }
        render(&unrelated);
        check(totalDraws() == 4, "a newly allocated real DComp surface initializes the full foreground for a local request");
        app.containers_.clear();
        app.ResetDesktopForegroundComposition();
        app.CancelUiAnimationFrame();
        app.uiAnimationScheduler_.Shutdown();
        app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
        app.iconGlassBackdrop_.clear();
        app.iconBitmapStyles_.clear();
        app.d2dIconCache_.clear();
        app.itemTextEffectContext_.Reset();
        app.dcompVisual_.Reset();
        app.dcompDevice_.Reset();
        app.d2dContext_.Reset();
        app.d2dDevice_.Reset();
        app.d2dFactory_.Reset();
        app.d3dDevice_.Reset();
        app.dwriteFactory_.Reset();
    }
