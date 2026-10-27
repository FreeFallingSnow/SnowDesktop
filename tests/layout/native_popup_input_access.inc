    class PopupInputSurface final : public IDCompositionSurface
    {
        ULONG references_ = 1;
        ComPtr<IDCompositionSurface> actual_;
        ComPtr<ID2D1DeviceContext> context_;
        POINT offset_{};
        UINT width_, height_;
    public:
        unsigned begins = 0, ends = 0;
        HRESULT capture = E_PENDING;
        std::vector<unsigned char> pixels;
        PopupInputSurface(IDCompositionSurface* actual, UINT width, UINT height)
            : actual_(actual), width_(width), height_(height) {}
        IDCompositionSurface* Actual() const { return actual_.Get(); }
        UINT Width() const { return width_; }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IDCompositionSurface))
                return E_NOINTERFACE;
            *result = static_cast<IDCompositionSurface*>(this);
            AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG remaining = --references_;
            if (!remaining) delete this;
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE BeginDraw(const RECT* rect, REFIID iid, void** result, POINT* offset) override
        {
            const HRESULT hr = actual_->BeginDraw(rect, iid, result, offset);
            if (SUCCEEDED(hr))
            {
                ++begins;
                if (iid == __uuidof(ID2D1DeviceContext))
                    context_ = static_cast<ID2D1DeviceContext*>(*result);
                offset_ = *offset;
            }
            return hr;
        }
        static HRESULT Read(ID2D1DeviceContext* context, ID2D1Bitmap1* bitmap,
            UINT width, UINT height, POINT offset, std::vector<unsigned char>& output)
        {
            if (!context || !bitmap || offset.x < 0 || offset.y < 0) return E_INVALIDARG;
            const auto properties = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ComPtr<ID2D1Bitmap1> readback;
            HRESULT hr = context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &properties, &readback);
            const D2D1_RECT_U rect{static_cast<UINT>(offset.x), static_cast<UINT>(offset.y),
                static_cast<UINT>(offset.x) + width, static_cast<UINT>(offset.y) + height};
            if (SUCCEEDED(hr)) hr = readback->CopyFromBitmap(nullptr, bitmap, &rect);
            D2D1_MAPPED_RECT mapped{};
            if (SUCCEEDED(hr)) hr = readback->Map(D2D1_MAP_OPTIONS_READ, &mapped);
            if (SUCCEEDED(hr))
            {
                output.resize(static_cast<size_t>(width) * height * 4);
                for (UINT row = 0; row < height; ++row)
                    std::memcpy(output.data() + static_cast<size_t>(row) * width * 4,
                        mapped.bits + static_cast<size_t>(row) * mapped.pitch, static_cast<size_t>(width) * 4);
                readback->Unmap();
            }
            return hr;
        }
        HRESULT STDMETHODCALLTYPE EndDraw() override
        {
            ++ends;
            ComPtr<ID2D1Image> target;
            ComPtr<ID2D1Bitmap1> bitmap;
            capture = context_ ? context_->Flush() : E_UNEXPECTED;
            if (SUCCEEDED(capture)) { context_->GetTarget(&target); capture = target.As(&bitmap); }
            if (SUCCEEDED(capture)) capture = Read(context_.Get(), bitmap.Get(), width_, height_, offset_, pixels);
            bitmap.Reset(); target.Reset();
            context_.Reset();
            return actual_->EndDraw();
        }
        HRESULT STDMETHODCALLTYPE SuspendDraw() override { return actual_->SuspendDraw(); }
        HRESULT STDMETHODCALLTYPE ResumeDraw() override { return actual_->ResumeDraw(); }
        HRESULT STDMETHODCALLTYPE Scroll(const RECT* rect, const RECT* clip, int x, int y) override
        { return actual_->Scroll(rect, clip, x, y); }
    };

    static void InitializePopupInputRendering(DesktopApp& app)
    {
        const auto check = [](bool ok, const char* text) { if (!ok) throw std::runtime_error(text); };
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &app.d3dDevice_, nullptr, nullptr)), "initialize isolated popup WARP device");
        ComPtr<IDXGIDevice> dxgi;
        check(SUCCEEDED(app.d3dDevice_.As(&dxgi)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&app.d2dFactory_))) &&
            SUCCEEDED(app.d2dFactory_->CreateDevice(dxgi.Get(), &app.d2dDevice_)) &&
            SUCCEEDED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &app.d2dContext_)) &&
            SUCCEEDED(DCompositionCreateDevice2(app.d2dDevice_.Get(), IID_PPV_ARGS(&app.dcompDevice_))) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.dcompVisual_)) &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))), "initialize actual popup D2D/DWrite/DComp");
        check(app.uiAnimationScheduler_.Initialize(), "initialize isolated popup scheduler");
        app.gridPages_.front().bounds = app.gridPages_.front().workArea =
            app.gridPages_.front().visualWorkArea = app.layoutWorkArea_ = {0, 0, 1200, 900};
        app.gridPages_.front().columns = 12; app.gridPages_.front().rows = 9;
        app.savedPageColumns_[app.gridPages_.front().id] = 12;
        app.savedPageRows_[app.gridPages_.front().id] = 9;
        app.virtualWidth_ = 1200; app.virtualHeight_ = 900;
        app.desktopItemsReady_ = true;
        app.personalizationSettings_.glassEnabled = app.personalizationSettings_.acrylicEnabled = false;
        app.collectionPopupGlassTheme_ = false;
        // Use the hook's existing API seam: its worker lifecycle is real, but
        // the isolated fixture never installs a global input hook.
        app.floatingPopupMouseHook_.~LowLevelMouseHook();
        ::new (static_cast<void*>(&app.floatingPopupMouseHook_)) snowdesktop::LowLevelMouseHook(
            snowdesktop::LowLevelMouseHook::Api{PopupInputInstallHook, PopupInputUninstallHook});
        app.customDesktopVisible_ = true;
        app.lastMousePoint_ = {LONG_MIN, LONG_MIN};
        app.categorySettings_.rules = {{L"a", L"Alpha", L".TXT", true}, {L"b", L"Beta", L".PNG", true}};
        app.floatingPopupHwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"STATIC", L"R02 isolated popup", WS_POPUP, -32000, -32000, 1200, 900,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(app.floatingPopupHwnd_ != nullptr && !IsWindowVisible(app.floatingPopupHwnd_),
            "create a never-shown test-owned popup window");
    }

    static HHOOK WINAPI PopupInputInstallHook(int kind, HOOKPROC callback, HINSTANCE, DWORD)
    {
        return kind == WH_MOUSE_LL && callback == &DesktopApp::FloatingPopupMouseHookProc
            ? reinterpret_cast<HHOOK>(static_cast<ULONG_PTR>(1)) : nullptr;
    }
    static BOOL WINAPI PopupInputUninstallHook(HHOOK hook)
    { return hook == reinterpret_cast<HHOOK>(static_cast<ULONG_PTR>(1)); }
    static void StopPopupInputMonitor(DesktopApp& app) { app.StopFloatingPopupOutsideClickMonitor(); }

    static ScrollingItemWidget& SeedPopupInput(DesktopApp& app, bool grouped, bool singleCell, bool dockCopy,
        bool fileCategories)
    {
        app.containers_.clear(); app.items_oo_.clear(); app.items_.clear(); app.widgets_.clear();
        DesktopWidget child;
        child.id = L"r02-source";
        child.type = fileCategories ? DesktopWidgetType::FileCategories : DesktopWidgetType::FolderMapping;
        child.gridCell = {app.gridPages_.front().id, 0, 0};
        child.gridSpan = singleCell ? GridSpan{1, 1} : GridSpan{6, 5};
        child.bounds = singleCell ? RECT{10, 10, 90, 90} : RECT{10, 10, 590, 490};
        child.showSearchBox = child.showFileCategories = true;
        // Model an already active Alpha tab. Production pointer-down leaves
        // the pressed category active throughout its drag and cancellation.
        child.activeCategoryId = L"a"; child.listMode = true; child.listColumns = 4;
        for (const auto* name : {L"one.txt", L"two.png"})
        {
            FolderEntry entry; entry.name = name;
            entry.fullPath = L"C:\\R02-Isolated-Source\\" + entry.name;
            if (fileCategories)
            {
                DesktopItem item; item.name = entry.name;
                item.layoutKey = item.parsingName = entry.fullPath;
                child.itemKeys.push_back(item.layoutKey);
                app.items_.push_back(std::move(item));
            }
            else child.folderEntries.push_back(std::move(entry));
        }
        app.RefreshDesktopItemIndexCache();
        app.widgets_.reserve(2);
        app.widgets_.push_back(std::move(child));
        if (grouped)
        {
            DesktopWidget group = app.widgets_.front();
            group.id = L"r02-group"; group.type = DesktopWidgetType::FileGroup;
            group.folderEntries.clear(); group.itemKeys.clear(); group.childWidgetIds = {L"r02-source"};
            group.activeCategoryId = L"r02-source";
            app.widgets_.push_back(std::move(group)); app.widgets_.front().bounds = {};
            app.containers_.push_back(std::make_unique<FileGroup>(&app.widgets_.back(), &app));
        }
        else if (fileCategories) app.containers_.push_back(std::make_unique<FileCategories>(&app.widgets_.front(), &app));
        else app.containers_.push_back(std::make_unique<FolderMapping>(&app.widgets_.front(), &app));
        app.popupWidgetIndex_ = app.widgets_.size() - 1;
        app.popupAnchoredToDock_ = !singleCell && !grouped;
        app.popupHasAnchor_ = true; app.popupAnchorPoint_ = {400, 400};
        app.popupPageId_ = app.gridPages_.front().id;
        app.popupAnimation_.ShowImmediately();
        // OpenCollectionPopupAt delegates every standalone FolderMapping to
        // OpenDockFolderPopupAt. Even a desktop mapping popup renders/edits
        // this temporary model; a FileGroup instead hosts the original child.
        if (dockCopy || (!grouped && !fileCategories))
        {
            app.dockFolderPopupWidget_ = app.widgets_.front();
            app.dockFolderPopupWidget_.id = kDockFolderPopupWidgetId;
            app.dockFolderPopupWidget_.gridSpan = {1, 1};
            app.dockFolderPopupWidget_.gridCell = {kDockPageId, 0, 0};
            app.dockFolderPopupWidget_.bounds = {};
            app.dockFolderPopupOpen_ = app.dockFolderPopupAvailable_ = true;
            app.dockFolderPopupKnownItemCount_ = app.dockFolderPopupWidget_.folderEntries.size();
            app.dockFolderPopupMappingWidgetId_ = dockCopy ? L"" : L"r02-source";
            app.popupAnchoredToDock_ = dockCopy; app.popupWidgetIndex_ = static_cast<size_t>(-1);
            app.dockFolderPopupContainer_ = std::make_unique<FolderMapping>(&app.dockFolderPopupWidget_, &app);
        }
        auto* view = app.GetCategorizedPopupView();
        if (!view) throw std::runtime_error("seed an actual categorized native popup view");
        app.popupRect_ = app.GetCollectionPopupRect(*app.GetOpenPopupWidget());
        const RECT frame = app.GetCategorizedPopupFrame(app.popupRect_);
        view->SetPopupFrame(&frame);
        return *view;
    }

    static ComPtr<PopupInputSurface> ObservePopupInputSurface(
        DesktopApp& app, PopupInputSurface* previous = nullptr)
    {
        // Complete any preceding model/typing layout before measuring pointer
        // work. The actual host remains hidden; its compact client is real.
        const RECT bounds = app.CalculateFloatingPopupWindowBounds();
        if (IsRectEmpty(&bounds)) throw std::runtime_error("calculate compact categorized popup bounds");
        app.floatingPopupWindowBounds_ = bounds;
        app.floatingPopupCollectionRegion_ = app.GetCollectionPopupRect(*app.GetOpenPopupWidget());
        app.floatingPopupLuaPanelRegion_ = {}; app.floatingPopupModalRegion_ = false;
        if (!SetWindowPos(app.floatingPopupHwnd_, nullptr, -32000, -32000,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER))
            throw std::runtime_error("size the hidden compact popup client");
        if (FAILED(app.CreateOrResizeFloatingPopupCompositionSurface()))
            throw std::runtime_error("initialize the real never-shown popup surface");
        ComPtr<PopupInputSurface> observed;
        // Unwrap a previous observer when the client dimensions did not change.
        if (previous && app.floatingPopupDcompSurface_.Get() == previous)
        {
            ComPtr<IDCompositionSurface> actual = previous->Actual();
            app.floatingPopupDcompSurface_ = std::move(actual);
        }
        observed.Attach(new PopupInputSurface(app.floatingPopupDcompSurface_.Get(),
            app.floatingPopupCompWidth_, app.floatingPopupCompHeight_));
        app.floatingPopupDcompSurface_ = observed;
        return observed;
    }
    static HWND PopupInputWindow(const DesktopApp& app) { return app.floatingPopupHwnd_; }
    static HWND PopupInputDesktop(const DesktopApp& app) { return app.hwnd_; }
    static void PaintPopupInput(DesktopApp& app)
    {
        // Hit queries project the popup frame in the fixture. Restore the
        // native source before entering real production drawing/scopes.
        if (auto* view = app.GetCategorizedPopupView()) view->SetPopupFrame(nullptr);
        app.PaintFloatingPopupWindow(app.floatingPopupHwnd_);
        if (!app.FlushPendingCompositionCommit()) throw std::runtime_error("flush the actual popup composition commit");
        if (auto* view = app.GetCategorizedPopupView()) SyncPopupInputFrame(app, *view);
    }
    static void MovePopupInput(DesktopApp& app, POINT point, bool floating)
    {
        if (auto* view = app.GetCategorizedPopupView()) view->SetPopupFrame(nullptr);
        app.handlingFloatingPopupInput_ = floating;
        bool previewSynced = false;
        const bool updated = app.OnNativeMouseMoveAt(MK_LBUTTON, point, &previewSynced);
        app.handlingFloatingPopupInput_ = false;
        if (!updated) throw std::runtime_error("categorized pointer move reaches the native production updater");
        if (floating) app.PresentFloatingPopupPointerFrame(previewSynced);
        else app.PresentPointerInteractionFrame(previewSynced);
        if (auto* view = app.GetCategorizedPopupView()) SyncPopupInputFrame(app, *view);
    }
    static void OriginalPopupInput(DesktopApp& app, POINT point, bool floating)
    {
        // Frozen before/ branch, including the original fault. The reference
        // executes actual input mutation but restores its old presentation route.
        auto* view = app.GetCategorizedPopupView();
        const RECT popup = app.GetCollectionPopupRect(*app.GetOpenPopupWidget());
        const RECT frame = app.GetCategorizedPopupFrame(popup);
        const bool hosted = view->IsHosted(), popupHosted = view->IsPopupHosted();
        const RECT previous = view->GetLayoutFrameRect();
        view->SetPopupFrame(&frame);
        if (view->HasCategoryTabPress()) view->UpdateCategoryTabDrag(point);
        else view->UpdateSearchPointerSelection(point);
        InvalidateRect(app.hwnd_, nullptr, FALSE);
        if (popupHosted) view->SetPopupFrame(&previous);
        else { view->SetPopupFrame(nullptr); if (hosted) view->SetHostedFrame(&previous); }
        if (floating) app.UpdateFloatingPopupWindowBounds(false);
        app.InvalidateFloatingPopupWindow(true);
    }
    static void CancelPopupInput(DesktopApp& app) { app.CancelPointerPressWithoutCaptureRelease(); }
    static void SetPopupInputFullCollection(DesktopApp& app, bool pending)
    { app.desktopBackdropFullCollectionPending_ = pending; }
    static void ForgetPopupInputHostBounds(DesktopApp& app) { app.floatingPopupWindowBounds_ = {}; }
    static bool FinalizePopupInput(DesktopApp& app)
    {
        app.desktopBackdropFullCollectionPending_ = false;
        app.FinalizeCloseCollectionPopup();
        return app.GetOpenPopupWidget() == nullptr && app.desktopBackdropFullCollectionPending_;
    }
    static void SyncPopupInputFrame(DesktopApp& app, ScrollingItemWidget& view)
    {
        const RECT popup = app.GetCollectionPopupRect(*app.GetOpenPopupWidget());
        const RECT frame = app.GetCategorizedPopupFrame(popup);
        view.SetPopupFrame(&frame);
    }
    static const DesktopWidget& PopupInputSource(const DesktopApp& app) { return app.widgets_.front(); }
    static bool PopupInputFailed(const DesktopApp& app) { return app.desktopWidgetCompositionFailurePending_; }

    static std::vector<unsigned char> FullPopupInputPixels(DesktopApp& app)
    {
        ComPtr<ID2D1DeviceContext> context;
        if (FAILED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)))
            throw std::runtime_error("create the independent complete popup reference context");
        context->SetDpi(96, 96); context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        ComPtr<ID2D1Bitmap1> target;
        const UINT width = app.floatingPopupCompWidth_, height = app.floatingPopupCompHeight_;
        if (FAILED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &properties, &target)))
            throw std::runtime_error("allocate the complete reference image");
        context->SetTarget(target.Get()); context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0, 0, 0));
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            -static_cast<float>(app.floatingPopupWindowBounds_.left),
            -static_cast<float>(app.floatingPopupWindowBounds_.top)));
        app.brushCache_.clear(); app.brushCacheContext_ = context.Get();
        if (auto* view = app.GetCategorizedPopupView()) view->SetPopupFrame(nullptr);
        app.renderingFloatingPopup_ = true;
        app.DrawDynamicOverlays(context.Get(), false);
        app.renderingFloatingPopup_ = false;
        if (auto* view = app.GetCategorizedPopupView()) SyncPopupInputFrame(app, *view);
        app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
        if (FAILED(context->EndDraw())) throw std::runtime_error("complete the independent full popup reference");
        std::vector<unsigned char> pixels;
        if (FAILED(PopupInputSurface::Read(context.Get(), target.Get(), width, height, POINT{}, pixels)))
            throw std::runtime_error("read complete popup reference pixels");
        return pixels;
    }
    static void ResetPopupInput(DesktopApp& app)
    {
        app.popupWidgetIndex_ = static_cast<size_t>(-1); app.dockFolderPopupOpen_ = false;
        app.CancelUiAnimationFrame(); app.uiAnimationScheduler_.Shutdown();
        app.DestroyFloatingPopupWindow();
        app.pendingDesktopWidgetCompositions_.clear(); app.desktopWidgetCompositionItems_.clear();
        app.desktopWidgetCompositionLayer_.Reset(); app.dcompVisual_.Reset(); app.dcompDevice_.Reset();
        app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
        app.d2dContext_.Reset(); app.d2dDevice_.Reset(); app.d3dDevice_.Reset();
    }
