    struct InputSafetyWindowContext
    {
        DesktopApp* app = nullptr;
        DesktopApp::PersistentDockHost* dock = nullptr;
        bool popup = false;
    };
    static LRESULT CALLBACK InputSafetyWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* context = reinterpret_cast<InputSafetyWindowContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        const bool pointerMessage = message == WM_MOUSEMOVE || message == WM_LBUTTONUP ||
            message == WM_MBUTTONUP;
        if (context && pointerMessage)
        {
            if (context->popup) return context->app->HandleFloatingPopupMessage(window, message, wp, lp);
            if (context->dock) return context->app->HandleFloatingDockMessage(*context->dock, window, message, wp, lp);
            return context->app->HandleMessage(window, message, wp, lp);
        }
        if (context && message == WM_NULL)
            return context->app->HandleControlMessage(window, message, wp, lp);
        if (context && (message == WM_CANCELMODE || message == WM_CAPTURECHANGED || message == WM_NCHITTEST ||
            (context->popup && message == WM_TIMER)))
        {
            if (context->popup) return context->app->HandleFloatingPopupMessage(window, message, wp, lp);
            if (context->dock) return context->app->HandleFloatingDockMessage(*context->dock, window, message, wp, lp);
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    // Override only this test process's imported OS boundaries. The production
    // activation methods still execute; denied foreground requests must return
    // without joining a foreign thread's input queue.
    struct InputSafetyImport
    {
        ULONG_PTR* slot = nullptr;
        ULONG_PTR original = 0;
        InputSafetyImport(const char* name, ULONG_PTR replacement, bool optional = false)
        {
            const auto module = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
            const auto imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(module +
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
            for (auto entry = imports; entry->Name && !slot; ++entry)
            {
                if (!entry->OriginalFirstThunk) continue;
                auto names = reinterpret_cast<const IMAGE_THUNK_DATA*>(module + entry->OriginalFirstThunk);
                auto addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(module + entry->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++addresses)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                    const auto imported = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(module + names->u1.AddressOfData);
                    if (strcmp(reinterpret_cast<const char*>(imported->Name), name) == 0)
                    { slot = &addresses->u1.Function; break; }
                }
            }
            if (!slot && optional) return;
            if (!slot) throw std::runtime_error(std::string("missing input-safety OS import: ") + name);
            DWORD protection = 0;
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
                throw std::runtime_error("cannot protect test-process import slot");
            original = *slot; *slot = replacement;
            DWORD ignored = 0; VirtualProtect(slot, sizeof(*slot), protection, &ignored);
        }
        ~InputSafetyImport()
        {
            if (!slot) return;
            DWORD protection = 0;
            if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
            { *slot = original; DWORD ignored = 0; VirtualProtect(slot, sizeof(*slot), protection, &ignored); }
        }
        InputSafetyImport(const InputSafetyImport&) = delete;
        InputSafetyImport& operator=(const InputSafetyImport&) = delete;
    };
    static inline HWND inputSafetyForeground = nullptr;
    static inline HWND inputSafetyOwnedWindow = nullptr;
    static inline unsigned inputSafetyAttachCalls = 0;
    static inline unsigned inputSafetyLocalFocusCalls = 0;
    static inline bool inputSafetyAllowForeground = false;
    static inline decltype(&GetWindowThreadProcessId) inputSafetyOriginalThread = nullptr;
    static HWND WINAPI InputSafetyForeground() { return inputSafetyForeground; }
    static inline decltype(&InvalidateRect) inputSafetyOriginalInvalidate = nullptr;
    static inline HWND inputSafetyInvalidationWindow = nullptr;
    static inline unsigned inputSafetyInvalidations = 0;
    static BOOL WINAPI InputSafetyInvalidate(HWND window, const RECT* bounds, BOOL erase)
    {
        const BOOL accepted = inputSafetyOriginalInvalidate(window, bounds, erase);
        if (accepted && window == inputSafetyInvalidationWindow) ++inputSafetyInvalidations;
        return accepted;
    }
    static BOOL WINAPI InputSafetyDenyForeground(HWND window)
    { if (!inputSafetyAllowForeground) return FALSE; inputSafetyForeground = window; return TRUE; }
    static HWND WINAPI InputSafetyDenyFocus(HWND) { ++inputSafetyLocalFocusCalls; return nullptr; }
    static BOOL WINAPI InputSafetyAttach(DWORD, DWORD, BOOL) { ++inputSafetyAttachCalls; return FALSE; }
    static DWORD WINAPI InputSafetyThread(HWND window, LPDWORD process)
    {
        if (window == inputSafetyForeground && window != inputSafetyOwnedWindow)
        { if (process) *process = GetCurrentProcessId() + 1000; return GetCurrentThreadId() + 1000; }
        return inputSafetyOriginalThread(window, process);
    }
    static BOOL WINAPI InputSafetyGui(DWORD, PGUITHREADINFO info)
    { info->hwndActive = info->hwndFocus = inputSafetyForeground; return TRUE; }
    static BOOL WINAPI InputSafetyDenyRegion(HWND, HRGN, BOOL)
    { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    static inline int inputSafetyPhysicalButton = 0;
    static SHORT WINAPI InputSafetyAsyncKeyState(int key)
    { return key == inputSafetyPhysicalButton ? static_cast<SHORT>(0x8000) : 0; }
    static inline DWORD inputSafetyQueuedButtons = 0;
    static DWORD WINAPI InputSafetyQueueStatus(UINT)
    { return inputSafetyQueuedButtons; }
    static inline bool inputSafetySwappedButtons = false;
    static inline decltype(&GetSystemMetrics) inputSafetyOriginalMetrics = nullptr;
    static int WINAPI InputSafetyMetrics(int index)
    { return index == SM_SWAPBUTTON ? static_cast<int>(inputSafetySwappedButtons) : inputSafetyOriginalMetrics(index); }
    static inline std::mutex inputSafetyLogMutex;
    static inline std::condition_variable inputSafetyLogChanged;
    static inline std::vector<std::wstring> inputSafetyLogs;
    static void InputSafetyLog(const wchar_t* line)
    {
        std::lock_guard<std::mutex> lock(inputSafetyLogMutex);
        inputSafetyLogs.emplace_back(line);
        inputSafetyLogChanged.notify_all();
    }

    static void FinishInputSafetyLayoutSave(DesktopApp& app)
    {
        if (!app.layoutSavePending_) return;
        if (!app.layoutScrollSave_.Pending())
            throw std::runtime_error("release must schedule its pending layout save");
        // Supply only the timer deadline. The production timer, background
        // writer and completion must finish before a reload cancels the timer.
        DueSaveTimer(app);
        DispatchSaveTimer(app);
        FlushSave(app);
        if (app.layoutSavePending_)
            throw std::runtime_error("release layout save did not complete");
    }

    static void RunQueuedDragReleaseSafety(const wchar_t* windowClass)
    {
        InputSafetyImport buttonState("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&InputSafetyAsyncKeyState));
        InputSafetyImport queuedButtons("GetQueueStatus", reinterpret_cast<ULONG_PTR>(&InputSafetyQueueStatus));
        inputSafetyOriginalMetrics = &GetSystemMetrics;
        InputSafetyImport metrics("GetSystemMetrics", reinterpret_cast<ULONG_PTR>(&InputSafetyMetrics));
        snowdesktop::test::TemporaryDirectory temporary;
        const auto file = temporary.path / L"drag.txt";
        { std::ofstream stream(file); stream << "release backlog fixture"; }
        for (int surface = 0; surface < 3; ++surface)
        for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON})
        for (const bool swapped : {false, true})
        {
            inputSafetySwappedButtons = swapped;
            auto& app = Scene(false);
            ConfigureModel(app, temporary.path / (std::to_wstring(surface) + L"-" +
                std::to_wstring(button) + (swapped ? L"-swapped" : L"-ordinary")));
            PopulateModel(app, {file}, 0);
            InputSafetyWindowContext mainContext{&app}, popupContext{&app, nullptr, true};
            auto host = std::make_unique<DesktopApp::PersistentDockHost>();
            host->active = true;
            InputSafetyWindowContext dockContext{&app, host.get()};
            std::vector<HWND> windows;
            const auto create = [&](InputSafetyWindowContext& context) {
                HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass,
                    L"Queued drag release fixture", WS_POPUP, 0, 0, 400, 300,
                    nullptr, nullptr, GetModuleHandleW(nullptr), &context);
                if (!window) throw std::runtime_error("create queued drag release window");
                windows.push_back(window);
                return window;
            };
            struct Cleanup
            {
                DesktopApp& app; std::vector<HWND>& windows;
                ~Cleanup()
                {
                    app.EndDragSession();
                    app.HideDragHintWindow();
                    if (app.hintHwnd_) DestroyWindow(app.hintHwnd_);
                    app.hintHwnd_ = nullptr;
                    ReleaseCapture();
                    for (auto it = windows.rbegin(); it != windows.rend(); ++it) DestroyWindow(*it);
                    app.hwnd_ = app.controlHwnd_ = app.floatingPopupHwnd_ = nullptr;
                    for (const auto& dock : app.persistentDockHosts_) dock->hwnd = nullptr;
                }
            } cleanup{app, windows};
            app.hwnd_ = create(mainContext);
            app.controlHwnd_ = app.hwnd_;
            host->hwnd = create(dockContext);
            const HWND dockWindow = host->hwnd;
            app.persistentDockHosts_.push_back(std::move(host));
            app.floatingPopupHwnd_ = create(popupContext);
            const HWND inputWindow = surface == 0 ? app.hwnd_ : surface == 1 ? dockWindow : app.floatingPopupHwnd_;
            if (!app.SaveLayoutSlots(false)) throw std::runtime_error("save queued drag release layout");
            Begin(app, false, 1);
            app.mouseDown_ = true;
            app.lastMousePoint_ = app.mouseDownPoint_ = {50, 50};
            app.middleButtonWidgetMove_ = button == VK_MBUTTON;
            app.rightButtonItemDrag_ = button == VK_RBUTTON;
            SetCapture(inputWindow);
            if (GetCapture() != inputWindow) throw std::runtime_error("capture queued drag release window");
            const WPARAM held = button == VK_LBUTTON ? MK_LBUTTON : button == VK_RBUTTON ? MK_RBUTTON : MK_MBUTTON;
            inputSafetyPhysicalButton = button == VK_MBUTTON ? VK_MBUTTON :
                swapped ? (button == VK_LBUTTON ? VK_RBUTTON : VK_LBUTTON) : button;
            inputSafetyQueuedButtons = 0;
            // A physically held initiating button must keep ordinary drag
            // updates working, including swapped logical left/right buttons.
            if (!app.OnNativeMouseMoveAt(held, {150, 50}) || app.dragSession_.CurrentPoint().x != 150)
                throw std::runtime_error("held initiating button incorrectly suppresses ordinary native dragging");
            app.dragSession_.UpdatePoint({50, 50});
            app.lastMousePoint_ = {50, 50};
            inputSafetyPhysicalButton = 0;
            inputSafetyQueuedButtons = MAKELONG(0, QS_MOUSEBUTTON);
            // Physical release has happened, but its real queued message is
            // still behind historical moves. Exercise each production HWND
            // route; neither pointer/model feedback nor presentation may replay.
            for (int lap = 0; lap < 3; ++lap)
            for (const POINT point : {POINT{250, 50}, POINT{250, 250}, POINT{50, 250}, POINT{50, 50}})
                SendMessageW(inputWindow, WM_MOUSEMOVE, held, MAKELPARAM(point.x, point.y));
            const POINT current = app.dragSession_.CurrentPoint();
            if (current.x != 50 || current.y != 50 || app.lastMousePoint_.x != 50 || app.lastMousePoint_.y != 50 ||
                app.presentedDragFeedbackRevision_ != 0 || !app.dragSession_.IsActive() || GetCapture() != inputWindow)
                throw std::runtime_error("released drag replays queued movement/presentation or loses its pending real release");
            inputSafetyQueuedButtons = 0;
            if (button == VK_RBUTTON)
            {
                // Right-drag menu choice is covered separately. Keep this
                // test independent of menu UI, and exercise actual lost-up
                // recovery after the queue reports no pending release.
                if (!app.OnNativeMouseMoveAt(0, {350, 250}) || app.dragSession_.IsActive() || GetCapture() ||
                    app.items_.front().gridCell.column != 0 || app.items_.front().gridCell.row != 0)
                    throw std::runtime_error("lost-release recovery must cancel without dropping at a historical point");
            }
            else
            {
                SendMessageW(inputWindow, button == VK_MBUTTON ? WM_MBUTTONUP : WM_LBUTTONUP,
                    0, MAKELPARAM(150, 150));
                if (app.dragSession_.IsActive() || app.mouseDown_ || GetCapture() ||
                    app.items_.front().gridCell.column != 1 || app.items_.front().gridCell.row != 1)
                    throw std::runtime_error("real release did not commit its own point exactly once after the stale move backlog");
                FinishInputSafetyLayoutSave(app);
                app.LoadLayoutSlots();
                const auto stored = app.layoutRecords_.find(ToUpperInvariant(file.wstring()));
                if (stored == app.layoutRecords_.end() || stored->second.cell.column != 1 || stored->second.cell.row != 1)
                    throw std::runtime_error("real release position did not survive layout persistence");
            }
        }
    }

    static void RunWidgetReleaseSafety(const wchar_t* windowClass)
    {
        std::vector<std::string> failures;
        const auto check = [&](bool value, const char* message) {
            if (!value) { failures.emplace_back(message); std::cerr << "FAIL: " << message << '\n'; }
        };
        // Seed only the established drag and isolated layout. Completion goes
        // through the actual HWND dispatcher, release resample, displacement,
        // persistence and reload. Only physical/queued button state is supplied.
        InputSafetyImport buttonState("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&InputSafetyAsyncKeyState));
        InputSafetyImport queuedButtons("GetQueueStatus", reinterpret_cast<ULONG_PTR>(&InputSafetyQueueStatus));
        inputSafetyPhysicalButton = 0;
        inputSafetyQueuedButtons = 0;
        for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::LuaScript})
        for (const bool middle : {false, true})
        {
            auto* app = new DesktopApp;
            app->instance_ = GetModuleHandleW(nullptr);
            app->initializationExperimentDirectory_ = std::filesystem::path(GetDataDirectoryPath()) /
                (std::wstring(type == DesktopWidgetType::LuaScript ? L"lua-release" : L"native-release") +
                    (middle ? L"-middle" : L"-left"));
            std::filesystem::create_directories(app->initializationExperimentDirectory_);
            app->generalSettings_.dockEnabled = false;
            app->generalSettings_.statusBar.enabled = false;
            GridPage page;
            page.id = L"__page:1"; page.monitorId = L"release-monitor";
            page.bounds = page.workArea = page.visualWorkArea = RECT{0, 0, 1200, 800};
            page.columns = 12; page.rows = 8;
            page.cellWidth = page.cellHeight = 80;
            page.itemPitchWidth = page.itemPitchHeight = 100;
            page.gapX = page.gapY = 20; page.marginX = page.marginY = 10;
            app->gridPages_ = {page}; app->savedPageIds_ = {page.id};
            app->savedPageColumns_[page.id] = 12; app->savedPageRows_[page.id] = 8;
            DesktopWidget widget;
            widget.id = L"release-component"; widget.type = type;
            if (type == DesktopWidgetType::LuaScript) widget.packageId = L"test.release";
            widget.gridCell = {page.id, 0, 0}; widget.gridSpan = {3, 2}; widget.autoCollect = false;
            widget.bounds = {10, 10, 290, 190}; widget.selected = true;
            app->widgets_ = {widget}; app->desktopItemsReady_ = true;
            InputSafetyWindowContext context{app};
            HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                windowClass, L"Isolated widget release", WS_POPUP, 0, 0, 1200, 800,
                nullptr, nullptr, app->instance_, &context);
            if (!window) throw std::runtime_error("create widget release fixture");
            app->hwnd_ = window;
            app->controlHwnd_ = window;
            app->RebuildContainersAndItems();
            if (!app->SaveLayoutSlots(false)) throw std::runtime_error("save original release fixture");
            app->mouseDown_ = true; app->mouseDownWidgetIndex_ = 0;
            app->widgetAction_ = DesktopApp::WidgetAction::Move;
            app->middleButtonWidgetMove_ = middle;
            app->widgetDragOriginalCell_ = widget.gridCell;
            app->widgetDragOriginalSpan_ = widget.gridSpan;
            app->widgetPreviewCell_ = widget.gridCell; app->widgetPreviewSpan_ = widget.gridSpan;
            app->widgetDragAnchor_ = CaptureGridDragAnchor(widget.bounds, {50, 50});
            SetCapture(window);
            if (GetCapture() != window) throw std::runtime_error("capture widget release fixture");
            // The release is already dequeued, so neither a physical down bit
            // nor a pending button message exists. This is a normal release.
            SendMessageW(window, middle ? WM_MBUTTONUP : WM_LBUTTONUP, 0, MAKELPARAM(350, 250));
            const auto& moved = app->widgets_.front();
            check(moved.gridCell.pageId == page.id && moved.gridCell.column == 3 && moved.gridCell.row == 2,
                "normal widget release commits the final cell instead of cancelling the move");
            check(!app->mouseDown_ && app->widgetAction_ == DesktopApp::WidgetAction::None && !GetCapture(),
                "completed widget release retires gesture and capture");
            FinishInputSafetyLayoutSave(*app);
            app->LoadLayoutSlots();
            const auto index = app->FindWidgetIndexById(widget.id);
            check(index < app->widgets_.size() && app->widgets_[index].gridCell.pageId == page.id &&
                app->widgets_[index].gridCell.column == 3 && app->widgets_[index].gridCell.row == 2,
                "widget release cell survives the real layout reload");
            app->HideDragHintWindow();
            if (app->hintHwnd_) DestroyWindow(app->hintHwnd_);
            app->hintHwnd_ = nullptr;
            ReleaseCapture(); DestroyWindow(window); app->hwnd_ = app->controlHwnd_ = nullptr;
        }
        if (!failures.empty()) throw std::runtime_error(std::to_string(failures.size()) + " widget release assertions failed");
    }

    static void RunQueuedWidgetReleaseSafety(const wchar_t* windowClass)
    {
        std::vector<std::string> failures;
        InputSafetyImport buttonState("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&InputSafetyAsyncKeyState));
        InputSafetyImport queuedButtons("GetQueueStatus", reinterpret_cast<ULONG_PTR>(&InputSafetyQueueStatus));
        inputSafetyOriginalMetrics = &GetSystemMetrics;
        InputSafetyImport metrics("GetSystemMetrics", reinterpret_cast<ULONG_PTR>(&InputSafetyMetrics));
        snowdesktop::test::TemporaryDirectory temporary;
        for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::LuaScript})
        for (const bool swapped : {false, true})
        for (const auto action : {DesktopApp::WidgetAction::Move, DesktopApp::WidgetAction::PendingMove,
            DesktopApp::WidgetAction::Resize, DesktopApp::WidgetAction::PendingResize})
        for (const int button : {VK_LBUTTON, VK_MBUTTON, VK_RBUTTON})
        {
            const bool resize = action == DesktopApp::WidgetAction::Resize ||
                action == DesktopApp::WidgetAction::PendingResize;
            if (resize && button != VK_LBUTTON) continue;
            const bool pending = action == DesktopApp::WidgetAction::PendingMove ||
                action == DesktopApp::WidgetAction::PendingResize;
            const auto check = [&](bool value, const char* message) {
                if (value) return;
                failures.emplace_back(message);
                std::cerr << "FAIL: " << message << " type=" << static_cast<int>(type)
                    << " action=" << static_cast<int>(action) << " button=" << button
                    << " swapped=" << swapped << '\n';
            };
            // Seed only the established component gesture. Native messages,
            // final geometry, displacement and persistence use production code;
            // only physical button/queue state and the OS button map are supplied.
            auto* app = new DesktopApp;
            app->instance_ = GetModuleHandleW(nullptr);
            app->initializationExperimentDirectory_ = temporary.path /
                (std::to_wstring(static_cast<int>(type)) + L"-" + std::to_wstring(swapped) + L"-" +
                    std::to_wstring(static_cast<int>(action)) + L"-" + std::to_wstring(button));
            std::filesystem::create_directories(app->initializationExperimentDirectory_);
            app->generalSettings_.dockEnabled = false;
            app->generalSettings_.statusBar.enabled = false;
            GridPage page;
            page.id = L"__page:1"; page.monitorId = L"backlog-monitor";
            page.bounds = page.workArea = page.visualWorkArea = RECT{0, 0, 1200, 800};
            page.columns = 12; page.rows = 8;
            page.cellWidth = page.cellHeight = 80;
            page.itemPitchWidth = page.itemPitchHeight = 100;
            page.gapX = page.gapY = 20; page.marginX = page.marginY = 10;
            app->gridPages_ = {page}; app->savedPageIds_ = {page.id};
            app->savedPageColumns_[page.id] = page.columns;
            app->savedPageRows_[page.id] = page.rows;
            DesktopWidget widget;
            widget.id = L"backlog-component"; widget.type = type;
            if (type == DesktopWidgetType::LuaScript) widget.packageId = L"test.release.backlog";
            widget.gridCell = {page.id, 0, 0}; widget.gridSpan = {3, 2};
            widget.bounds = {10, 10, 290, 190}; widget.selected = true; widget.autoCollect = false;
            app->widgets_ = {widget}; app->desktopItemsReady_ = true;
            InputSafetyWindowContext context{app};
            const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                windowClass, L"Queued component release", WS_POPUP, 0, 0, 1200, 800,
                nullptr, nullptr, app->instance_, &context);
            if (!window) throw std::runtime_error("create queued component release window");
            app->hwnd_ = window;
            app->controlHwnd_ = window;
            struct Cleanup
            {
                DesktopApp& app; HWND window;
                ~Cleanup()
                {
                    app.DestroyDragHintWindow();
                    ReleaseCapture(); DestroyWindow(window); app.hwnd_ = app.controlHwnd_ = nullptr;
                }
            } cleanup{*app, window};
            app->RebuildContainersAndItems();
            if (!app->SaveLayoutSlots(false)) throw std::runtime_error("save queued component layout");
            // A data-only app has no tooltip window, so a hidden-window
            // assertion alone would be vacuous. One active resize uses real
            // WARP/DComp text feedback on this inactive private desktop.
            const bool observeTooltip = resize && !pending && !swapped &&
                type == DesktopWidgetType::Collection;
            if (observeTooltip)
            {
                ComPtr<IDXGIDevice> dxgi;
                if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                        &app->d3dDevice_, nullptr, nullptr)) ||
                    FAILED(app->d3dDevice_.As(&dxgi)) ||
                    FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                        IID_PPV_ARGS(&app->d2dFactory_))) ||
                    FAILED(app->d2dFactory_->CreateDevice(dxgi.Get(), &app->d2dDevice_)) ||
                    FAILED(DCompositionCreateDevice2(app->d2dDevice_.Get(),
                        IID_PPV_ARGS(&app->dcompDevice_))) ||
                    FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(app->dwriteFactory_.GetAddressOf()))))
                    throw std::runtime_error("initialize isolated drag hint graphics");
                ShowWindow(window, SW_SHOWNOACTIVATE);
            }
            app->mouseDown_ = true; app->mouseDownWidgetIndex_ = 0;
            app->widgetAction_ = action;
            app->middleButtonWidgetMove_ = button == VK_MBUTTON;
            app->rightButtonItemDrag_ = button == VK_RBUTTON;
            app->mouseDownPoint_ = app->lastMousePoint_ = {50, 50};
            app->widgetDragOriginalCell_ = widget.gridCell;
            app->widgetDragOriginalSpan_ = widget.gridSpan;
            app->widgetPreviewCell_ = widget.gridCell; app->widgetPreviewSpan_ = widget.gridSpan;
            app->widgetDragAnchor_ = CaptureGridDragAnchor(widget.bounds, {50, 50});
            SetCapture(window);
            if (GetCapture() != window) throw std::runtime_error("capture queued component release window");
            const WPARAM held = button == VK_LBUTTON ? MK_LBUTTON :
                button == VK_RBUTTON ? MK_RBUTTON : MK_MBUTTON;
            inputSafetySwappedButtons = swapped;
            inputSafetyPhysicalButton = button == VK_MBUTTON ? VK_MBUTTON :
                swapped ? (button == VK_LBUTTON ? VK_RBUTTON : VK_LBUTTON) : button;
            inputSafetyQueuedButtons = 0;
            check(app->OnNativeMouseMoveAt(held, {150, 150}) &&
                app->widgetAction_ == (resize ? DesktopApp::WidgetAction::Resize : DesktopApp::WidgetAction::Move) &&
                !app->dragSession_.IsActive(), "held component gesture must update without a DragSession");
            check(resize ? app->widgetPreviewSpan_.columns == 4 && app->widgetPreviewSpan_.rows == 3 :
                app->widgetPreviewCell_.column == 1 && app->widgetPreviewCell_.row == 1,
                "held component gesture must update its visible geometry");
            if (observeTooltip)
                check(app->dragTooltip_.Visible() && app->hintHwnd_ && IsWindowVisible(app->hintHwnd_),
                    "isolated active resize must display real drag feedback before release");
            if (pending) app->widgetAction_ = action;
            const auto beforeCell = app->widgetPreviewCell_;
            const auto beforeSpan = app->widgetPreviewSpan_;
            const auto beforeFeedback = app->presentedWidgetDragFeedback_;
            inputSafetyPhysicalButton = 0;
            inputSafetyQueuedButtons = MAKELONG(0, QS_MOUSEBUTTON);
            for (int lap = 0; lap < 3; ++lap)
            for (const POINT point : {POINT{450, 50}, POINT{450, 350}, POINT{50, 350}, POINT{50, 50}})
                SendMessageW(window, WM_MOUSEMOVE, held, MAKELPARAM(point.x, point.y));
            check(app->widgetAction_ == (pending ? action :
                    resize ? DesktopApp::WidgetAction::Resize : DesktopApp::WidgetAction::Move) &&
                app->lastMousePoint_.x == 150 && app->lastMousePoint_.y == 150 &&
                app->widgetPreviewCell_.pageId == beforeCell.pageId &&
                app->widgetPreviewCell_.column == beforeCell.column && app->widgetPreviewCell_.row == beforeCell.row &&
                app->widgetPreviewSpan_.columns == beforeSpan.columns && app->widgetPreviewSpan_.rows == beforeSpan.rows &&
                app->presentedWidgetDragFeedback_ == beforeFeedback && GetCapture() == window,
                "released component must not replay queued movement, present it or start a pending gesture");
            inputSafetyQueuedButtons = 0;
            if (button == VK_RBUTTON)
            {
                // Existing right-drag suites cover menu decisions. With no
                // real release left queued, this production move must cancel.
                SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(350, 250));
            }
            else
                SendMessageW(window, button == VK_MBUTTON ? WM_MBUTTONUP : WM_LBUTTONUP,
                    0, MAKELPARAM(350, 250));
            const bool commit = !pending && button != VK_RBUTTON;
            const int column = commit && !resize ? 3 : 0;
            const int row = commit && !resize ? 2 : 0;
            const int columns = commit && resize ? 6 : 3;
            const int rows = commit && resize ? 4 : 2;
            const auto matches = [&](const DesktopWidget& value) {
                return value.gridCell.pageId == page.id && value.gridCell.column == column &&
                    value.gridCell.row == row && value.gridSpan.columns == columns && value.gridSpan.rows == rows;
            };
            check(matches(app->widgets_.front()), "component release must commit its release geometry; pending/lost release must not commit");
            check(!app->mouseDown_ && app->widgetAction_ == DesktopApp::WidgetAction::None && !GetCapture(),
                "component release or cancellation must retire gesture and capture");
            check(!app->dragTooltip_.Visible() && (!app->hintHwnd_ || !IsWindowVisible(app->hintHwnd_)),
                "component release or cancellation must hide its drag hint");
            FinishInputSafetyLayoutSave(*app);
            app->LoadLayoutSlots();
            const auto index = app->FindWidgetIndexById(widget.id);
            check(index < app->widgets_.size() && matches(app->widgets_[index]),
                "component release geometry must survive the real layout reload");
        }
        if (!failures.empty()) throw std::runtime_error(std::to_string(failures.size()) + " component backlog/release assertions failed");
    }

    static void RunWidgetVolumeGestures()
    {
        snowdesktop::test::TemporaryDirectory temporary;
        {
            std::ofstream manifest(temporary.path / "widget.json");
            manifest << R"json({"schemaVersion":2,"apiVersion":2,"dataVersion":1,
                "id":"79413e45-4abc-4c08-afc0-93a9f23426fe","slug":"volume-gesture-fixture",
                "version":"1.0.0","entry":"main.lua","minHostVersion":"1.0.12.0",
                "name":"Volume gestures","description":"Isolated gesture and task replacement regression",
                "author":"SnowDesktop","license":"MIT","defaultSize":{"columns":2,"rows":1},
                "permissions":["audio.output.control"],"requiredFeatures":["draw.immediate","lifecycle.model",
                    "task.audio.output.coalescedVolume","task.audio.output.control"]})json";
            std::ofstream script(temporary.path / "main.lua");
            script << R"lua(
return widget.define({setup=function() return {clicks=0,moves=0,probes=0} end,render=function() end,
event=function(_,m,e)
    if e.kind=='pointer' and e.action=='click' then
        m.clicks=m.clicks+1
        if m.clicks==2 then
            local id,err=task.start('audio.output.setVolume',{volume=0.5,coalesce=true})
            assert(not id and err=='permissionDenied');return
        end
        m.last=assert(task.start('audio.output.setVolume',{volume=0.2}))
        local id,err=task.start('audio.output.setVolume',{volume=0.3})
        assert(not id and err=='task concurrency limit exceeded','legacy concurrency remains bounded')
        for i=1,40 do
            local nextId=assert(task.start('audio.output.setVolume',{volume=i/40,coalesce=true}))
            assert(nextId>m.last,'each accepted intent keeps a distinct task identity');m.last=nextId
        end
    elseif e.kind=='action' and e.id=='range.volume' then
        assert(e.trustedGesture and e.source=='pointer','a pressed host slider is a trusted gesture')
        m.moves=m.moves+1
        m.last=assert(task.start('audio.output.setVolume',{volume=e.controlValue,coalesce=true}))
    elseif e.kind=='pointer' and e.action=='pointerMove' and e.x==300 then
        local id,err=task.start('audio.output.setVolume',{volume=0.9,coalesce=true})
        assert(not id and err=='userGestureRequired','ordinary pointer movement stays untrusted')
        assert(task.cancel(m.last),'an untrusted replacement must preserve the existing task')
        m.probes=m.probes+1
        if m.probes==2 then assert(m.moves>=2,'pointer down and captured move reached the real change route') end
    end
end})
)lua";
        }
        WidgetEngine engine;
        if (!engine.InitPreview(nullptr, nullptr) ||
            !engine.EnsureWidgetDirectoryPreviewLoaded(L"volume-gesture", temporary.path))
            throw std::runtime_error("load isolated volume gesture fixture");
        auto& widget = const_cast<LuaWidget&>(engine.GetWidgets().front());
        using namespace snowdesktop::widget_runtime;
        InteractionRegion slider;
        slider.key="volume.range"; slider.shape={InteractionShapeType::Rect,0,0,100,24,0};
        slider.controlKind=InteractionControlKind::Slider;
        slider.minimum=0;slider.maximum=1;slider.step=0.01f;slider.controlValue=0.5f;
        slider.events.emplace("change", InteractionAction{"range.volume",{}});
        std::string error;
        widget.interactionRegions.BeginFrame();
        if (!widget.interactionRegions.Submit(std::move(slider),error)) throw std::runtime_error(error);
        (void)widget.interactionRegions.CommitFrame();
        engine.InvokeClick(L"volume-gesture",300,12);
        engine.InvokeMouseEvent(L"volume-gesture","onMouseMove",300,12);
        engine.InvokeMouseEvent(L"volume-gesture","onMouseDown",20,12,1);
        engine.InvokeMouseEvent(L"volume-gesture","onMouseMove",80,12,1);
        engine.InvokeMouseEvent(L"volume-gesture","onMouseUp",80,12,1);
        engine.InvokeMouseEvent(L"volume-gesture","onMouseMove",300,12);
        widget.permissions.clear();
        engine.InvokeClick(L"volume-gesture",300,12);
        for (const auto& failure : engine.GetWidgetErrors())
            throw std::runtime_error("volume gesture production route: "+failure.message);
    }

    static void RunWidgetDockEdgeRevealSafety()
    {
        // Execute the production sampler and visibility policy on the inactive
        // test desktop. Seed only an established gesture; GPU presentation is
        // unavailable here and guarded so this checks routing, not pixels.
        auto* app = new DesktopApp;
        app->generalSettings_.dockEnabled = true;
        app->generalSettings_.statusBar.enabled = false;
        const HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(info)};
        if (!monitor || !GetMonitorInfoW(monitor, &info))
            throw std::runtime_error("widget edge reveal requires monitor geometry");
        const RECT area = info.rcMonitor;
        DockContainer dock(app, &app->dockEntries_, area);
        auto ownedHost = std::make_unique<DesktopApp::PersistentDockHost>();
        ownedHost->active = true; ownedHost->container = &dock; ownedHost->monitor = monitor;
        ownedHost->compositionPaintInProgress = true;
        auto& host = *ownedHost;
        app->persistentDockHosts_.push_back(std::move(ownedHost));
        DesktopWidget widget; widget.id = L"edge-reveal-component";
        app->widgets_.push_back(widget);
        const auto check = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        for (const bool summonOnly : {true, false})
        for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::LuaScript})
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        {
            app->dockSettings_.showOnlyWhenSummoned = summonOnly;
            app->dockSettings_.position = position;
            app->widgets_.front().type = type;
            const LONG cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
            const bool horizontal = position == DockPosition::Bottom || position == DockPosition::Top;
            const POINT edge = position == DockPosition::Bottom ? POINT{cx, area.bottom - 1} :
                position == DockPosition::Top ? POINT{cx, area.top} :
                position == DockPosition::Left ? POINT{area.left, cy} : POINT{area.right - 1, cy};
            host.dockRect = horizontal ? RECT{cx - 100, edge.y - 30, cx + 100, edge.y + 30} :
                RECT{edge.x - 30, cy - 100, edge.x + 30, cy + 100};
            host.passivelyRevealed = false; host.passiveLeaveStartTick = 0;
            app->mouseDown_ = true; app->mouseDownWidgetIndex_ = 0;
            for (const auto action : {DesktopApp::WidgetAction::None, DesktopApp::WidgetAction::PendingMove,
                DesktopApp::WidgetAction::PendingResize, DesktopApp::WidgetAction::Resize})
            {
                app->widgetAction_ = action;
                check(!app->UpdatePassiveDragRevealHosts(edge) && !host.passivelyRevealed,
                    "a click, pending gesture or widget resize must not summon the Dock");
            }
            app->widgetAction_ = DesktopApp::WidgetAction::Move;
            app->mouseDownWidgetIndex_ = app->widgets_.size();
            check(!app->UpdatePassiveDragRevealHosts(edge) && !host.passivelyRevealed,
                "a stale widget move index must not summon the Dock");
            app->mouseDownWidgetIndex_ = 0;
            check(!app->UpdatePassiveDragRevealHosts({cx, cy}) && !host.passivelyRevealed,
                "widget movement away from the configured edge must not summon the Dock");
            check(app->UpdatePassiveDragRevealHosts(edge) && host.passivelyRevealed &&
                !host.promoted && app->ShouldShowPersistentDockHost(host) &&
                app->IsDockContainerInteractionVisible(&dock),
                "moving a native or Lua component to its Dock edge must reveal an interactive passive host");
            host.passiveLeaveStartTick = GetTickCount64() - 1000;
            app->UpdatePassiveDragRevealHosts(edge);
            check(host.passivelyRevealed && !host.passiveLeaveStartTick,
                "remaining at the edge while dragging a widget cancels pending Dock dismissal");
            app->CancelPointerPressWithoutCaptureRelease();
            host.passiveLeaveStartTick = GetTickCount64() - 1000;
            app->UpdatePassiveDragRevealHosts({cx, cy});
            check(!host.passivelyRevealed && !host.promoted &&
                app->ShouldShowPersistentDockHost(host) == !summonOnly,
                "cancelled widget movement leaves summon-only Dock hidden after the leave delay");
            check(!app->UpdatePassiveDragRevealHosts(edge) && !host.passivelyRevealed,
                "a cancelled widget move must not reveal the Dock again at the edge");
        }
        app->persistentDockHosts_.clear();
    }

    static inline const DesktopItem* inputSafetyMenuItem = nullptr;
    static inline RECT inputSafetyMenuBounds{};
    static inline bool inputSafetyMenuBoundsStable = false;
    static inline unsigned inputSafetyMenuBoundaryCalls = 0;
    static HMENU WINAPI InputSafetyNoMenuAllocation()
    {
        ++inputSafetyMenuBoundaryCalls;
        inputSafetyMenuBoundsStable = inputSafetyMenuBoundsStable && inputSafetyMenuItem &&
            EqualRect(&inputSafetyMenuItem->bounds, &inputSafetyMenuBounds);
        return nullptr;
    }
    static void RunDockMappingContextSafety(const wchar_t* windowClass)
    {
        // This runs on the inactive private desktop. Deny only native menu
        // allocation; the real right-release, selection and menu route runs.
        snowdesktop::test::TemporaryDirectory temporary;
        const auto folder = temporary.path / L"folder";
        const auto shortcut = temporary.path / L"folder.lnk";
        const auto file = temporary.path / L"ordinary.txt";
        const auto executable = temporary.path / L"application.exe";
        const auto application = temporary.path / L"application.lnk";
        std::filesystem::create_directory(folder);
        { std::ofstream stream(file); stream << "ordinary mapped icon"; }
        { std::ofstream stream(executable); stream << "never executed"; }
        ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persisted;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link->SetPath(folder.c_str())) || FAILED(link.As(&persisted)) ||
            FAILED(persisted->Save(shortcut.c_str(), TRUE)))
            throw std::runtime_error("create actual folder shortcut for mapped Dock context input");
        if (FAILED(link->SetPath(executable.c_str())) || FAILED(persisted->Save(application.c_str(), TRUE)))
            throw std::runtime_error("create application shortcut for mapped Dock context input");
        auto& app = Scene(false);
        ConfigureModel(app, temporary.path / L"layout");
        PopulateModel(app, {folder, shortcut, file, application}, 0);
        DesktopItem systemItem;
        systemItem.name = L"Mapped namespace icon";
        systemItem.desktopIconClsid = kDesktopIconClsidThisPC;
        systemItem.parsingName = L"::" + systemItem.desktopIconClsid;
        systemItem.layoutKey = ToUpperInvariant(systemItem.parsingName);
        systemItem.gridCell = {app.gridPages_.front().id, 4, 0};
        PIDLIST_ABSOLUTE systemPidl = nullptr;
        if (FAILED(SHParseDisplayName(systemItem.parsingName.c_str(), nullptr, &systemPidl, 0, nullptr)))
            throw std::runtime_error("create actual non-file namespace identity for mapped Dock context input");
        systemItem.absolutePidl.reset(systemPidl);
        systemItem.childPidl.reset(ILCloneFull(systemPidl));
        app.items_.push_back(std::move(systemItem));
        app.RefreshDesktopItemIndexCache();
        SeedDock(app);
        for (const auto& item : app.items_)
        {
            app.dockEntries_.push_back({DockEntryType::DesktopItem, item.layoutKey, true});
            (void)app.ResolveDockFolderTarget(app.dockEntries_.back(), nullptr, true);
        }
        app.LayoutItems();
        InputSafetyWindowContext context{&app};
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass,
            L"Mapped Dock context fixture", WS_POPUP, 0, 0, 1200, 800,
            nullptr, nullptr, GetModuleHandleW(nullptr), &context);
        if (!window) throw std::runtime_error("create private mapped Dock context owner");
        struct Cleanup
        {
            DesktopApp& app; HWND window;
            ~Cleanup()
            {
                app.hwnd_ = completionWindow;
                DestroyWindow(window);
                NativeDragIntegrationAccess::StopModel(app);
                inputSafetyMenuItem = nullptr;
            }
        } cleanup{app, window};
        app.hwnd_ = window;
        InputSafetyImport allocation("CreatePopupMenu", reinterpret_cast<ULONG_PTR>(&InputSafetyNoMenuAllocation));
        for (std::size_t index = 0; index < app.items_.size(); ++index)
        {
            auto* dock = Dock(app);
            const auto entryIndex = std::find_if(app.dockEntries_.begin(), app.dockEntries_.end(), [&](const auto& entry) {
                return PathsEqualInsensitive(entry.reference, app.items_[index].layoutKey);
            });
            if (entryIndex == app.dockEntries_.end()) throw std::runtime_error("mapped source must retain its own Dock reference");
            const auto dockIndex = static_cast<std::size_t>(std::distance(app.dockEntries_.begin(), entryIndex));
            const RECT slot = dock->GetSlots()[dockIndex]->GetBounds();
            const POINT point{(slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2};
            const RECT original = app.items_[index].bounds;
            if (IsRectEmpty(&original) || EqualRect(&original, &slot))
                throw std::runtime_error("mapped source requires its own visible desktop geometry");
            inputSafetyMenuItem = &app.items_[index]; inputSafetyMenuBounds = original;
            inputSafetyMenuBoundsStable = true;
            const unsigned before = inputSafetyMenuBoundaryCalls;
            app.OnRightButtonUp(MAKELPARAM(point.x, point.y));
            if (inputSafetyMenuBoundaryCalls == before || !inputSafetyMenuBoundsStable ||
                !EqualRect(&app.items_[index].bounds, &original) ||
                !app.dockEntries_[dockIndex].keepOnDesktop || app.items_[index].gridCell.pageId == kDockPageId)
                throw std::runtime_error("Dock mapping context input must preserve the original folder/shortcut desktop geometry before and after menu dispatch");
        }
        std::cout << "Mapped Dock folders, files, application shortcuts and non-file namespace icons preserve source geometry at menu boundary and return\n";
    }

    static void RunExternalWindowSelectionSafety(const wchar_t* windowClass)
    {
        // User contract: a click into an ordinary window clears the selection,
        // while transfers to our surfaces and live menu/drag owners retain it.
        // Use the real native dispatchers on this fixture's inactive desktop;
        // only the OS foreground sample is supplied by the test.
        auto& app = Scene(false);
        app.generalSettings_.dockEnabled = false;
        std::vector<HWND> windows;
        const auto create = [&] {
            const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                windowClass, L"External selection fixture", WS_POPUP,
                0, 0, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window) throw std::runtime_error("create isolated selection window");
            windows.push_back(window);
            return window;
        };
        struct Cleanup
        {
            DesktopApp& app; std::vector<HWND>& windows;
            ~Cleanup()
            {
                app.EndDragSession();
                for (const HWND window : windows)
                {
                    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DefWindowProcW));
                    DestroyWindow(window);
                }
                app.hwnd_ = app.inputHwnd_ = app.controlHwnd_ = app.floatingPopupHwnd_ = nullptr;
                app.floatingDockInputHwnd_ = app.renameInputWindow_ = nullptr;
                inputSafetyForeground = nullptr;
                inputSafetyInvalidationWindow = nullptr;
            }
        } cleanup{app, windows};
        app.hwnd_ = create();
        ShowWindow(app.hwnd_, SW_SHOWNOACTIVATE);
        app.inputHwnd_ = create();
        app.controlHwnd_ = create();
        app.floatingPopupHwnd_ = create();
        app.floatingDockInputHwnd_ = create();
        app.renameInputWindow_ = create();
        const HWND external = create(); // An ordinary window, even in our process.
        const auto dispatch = [&](HWND window, WNDPROC procedure) {
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procedure));
        };
        dispatch(app.inputHwnd_, &DesktopApp::InputWndProc);
        dispatch(app.controlHwnd_, &DesktopApp::ControlWndProc);
        app.dockEntries_.resize(1);
        app.dockUnpinnedRunningApps_.resize(1);
        app.widgets_.resize(1);
        app.widgets_[0].folderEntries.resize(1);
        app.dockFolderPopupWidget_.folderEntries.resize(1);
        const auto seed = [&] {
            for (auto& item : app.items_) item.selected = true;
            app.dockEntries_[0].selected = app.dockUnpinnedRunningApps_[0].selected = true;
            app.widgets_[0].selected = app.widgets_[0].folderEntries[0].selected = true;
            app.dockFolderPopupWidget_.folderEntries[0].selected = true;
            app.keyboardNavVisualFocus_ = app.keyboardNavInsideWidget_ = true;
            app.keyboardNavWidgetIndex_ = 0;
            ValidateRect(app.hwnd_, nullptr);
            inputSafetyInvalidations = 0;
        };
        const auto requireCleared = [&](const char* route) {
            if (std::any_of(app.items_.begin(), app.items_.end(), [](const auto& item) { return item.selected; }) ||
                app.dockEntries_[0].selected || app.dockUnpinnedRunningApps_[0].selected ||
                app.widgets_[0].selected || app.widgets_[0].folderEntries[0].selected ||
                app.dockFolderPopupWidget_.folderEntries[0].selected ||
                app.keyboardNavVisualFocus_ || app.keyboardNavInsideWidget_)
                throw std::runtime_error(std::string(route) + ": leaving desktop must clear every selected model and keyboard focus");
            if (inputSafetyInvalidations == 0)
                throw std::runtime_error(std::string(route) + ": external selection clear must request repaint of the old selected pixels");
        };
        // Foreground reconciliation can synchronously UpdateWindow and consume
        // the update region before dispatch returns. Observe accepted real OS
        // invalidations, retaining the production repaint and presentation path.
        inputSafetyOriginalInvalidate = &InvalidateRect;
        inputSafetyInvalidationWindow = app.hwnd_;
        InputSafetyImport invalidation("InvalidateRect", reinterpret_cast<ULONG_PTR>(&InputSafetyInvalidate));
        InputSafetyImport foreground("GetForegroundWindow", reinterpret_cast<ULONG_PTR>(&InputSafetyForeground));
        inputSafetyForeground = external;
        seed();
        SendMessageW(app.inputHwnd_, WM_KILLFOCUS, reinterpret_cast<WPARAM>(external), 0);
        requireCleared("input proxy focus loss");
        seed();
        app.HandleMessage(app.hwnd_, WM_KILLFOCUS, reinterpret_cast<WPARAM>(external), 0);
        requireCleared("desktop focus loss");
        seed();
        SendMessageW(app.inputHwnd_, WM_KILLFOCUS, 0, 0);
        requireCleared("focus loss without next HWND");
        seed();
        SendMessageW(app.controlHwnd_, kForegroundInteractionChangedMessage, 0, 0);
        requireCleared("global foreground notification");
        ValidateRect(app.hwnd_, nullptr);
        inputSafetyInvalidations = 0;
        SendMessageW(app.inputHwnd_, WM_KILLFOCUS, reinterpret_cast<WPARAM>(external), 0);
        if (inputSafetyInvalidations != 0)
            throw std::runtime_error("repeated external notifications must not repaint unchanged selection");
        for (const HWND owned : {app.hwnd_, app.inputHwnd_, app.floatingPopupHwnd_,
                app.floatingDockInputHwnd_, app.renameInputWindow_})
        {
            seed();
            inputSafetyForeground = owned;
            SendMessageW(app.inputHwnd_, WM_KILLFOCUS, reinterpret_cast<WPARAM>(owned), 0);
            SendMessageW(app.controlHwnd_, kForegroundInteractionChangedMessage, 0, 0);
            if (!app.items_[0].selected || !app.widgets_[0].folderEntries[0].selected)
                throw std::runtime_error("internal surface transfers must retain the selection");
        }
        inputSafetyForeground = external;
        for (int owner = 0; owner < 3; ++owner)
        {
            seed();
            if (owner == 0) app.shellPopupMenuLayerDepth_ = 1;
            if (owner == 1) Begin(app, false, 1);
            if (owner == 2) app.mouseDown_ = true;
            SendMessageW(app.inputHwnd_, WM_KILLFOCUS, reinterpret_cast<WPARAM>(external), 0);
            SendMessageW(app.controlHwnd_, kForegroundInteractionChangedMessage, 0, 0);
            if (!app.items_[0].selected)
                throw std::runtime_error("menu and drag ownership must preserve command source selection");
            app.shellPopupMenuLayerDepth_ = 0;
            app.mouseDown_ = false;
            app.EndDragSession();
        }
        std::cout << "External window selection: native focus and foreground routes, owned surfaces and menu/drag retention passed\n";
    }

    static void RunInputSafety()
    {
        std::vector<std::string> failures;
        const auto check = [&](bool value, const char* message) {
            if (!value) { failures.emplace_back(message); std::cerr << "FAIL: " << message << '\n'; }
        };
        std::cerr << "Input safety: volume gestures\n";
        RunWidgetVolumeGestures();
        WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = InputSafetyWindowProc;
        cls.lpszClassName = L"SnowDesktop.NativeInputSafety";
        if (!RegisterClassW(&cls)) throw std::runtime_error("register isolated input windows");
        RunExternalWindowSelectionSafety(cls.lpszClassName);
        std::cerr << "Input safety: queued item release\n";
        RunQueuedDragReleaseSafety(cls.lpszClassName);
        std::cerr << "Input safety: widget release\n";
        RunWidgetReleaseSafety(cls.lpszClassName);
        std::cerr << "Input safety: queued widget release\n";
        RunQueuedWidgetReleaseSafety(cls.lpszClassName);
        std::cerr << "Input safety: widget Dock edge reveal\n";
        RunWidgetDockEdgeRevealSafety();
        std::cerr << "Input safety: mapped Dock context\n";
        RunDockMappingContextSafety(cls.lpszClassName);
        std::cerr << "Input safety: remaining capture and activation scenes\n";
        auto* app = new DesktopApp; // Same process-lifetime model policy as the native integration fixture.
        app->instance_ = cls.hInstance;
        std::vector<HWND> windows;
        const auto create = [&](InputSafetyWindowContext* context, int width = 400, int height = 300) {
            HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                (context && context->popup ? WS_EX_NOREDIRECTIONBITMAP : 0),
                cls.lpszClassName, L"Input safety fixture", WS_POPUP, 0, 0, width, height,
                nullptr, nullptr, cls.hInstance, context);
            if (!window) throw std::runtime_error("create isolated input window");
            windows.push_back(window); return window;
        };
        InputSafetyWindowContext dockContext{app}, popupContext{app, nullptr, true};
        struct Cleanup
        {
            DesktopApp& app; std::vector<HWND>& windows;
            ~Cleanup()
            {
                app.StopFloatingPopupOutsideClickMonitor();
                app.luaWidgetPanelRequest_ = {};
                ReleaseCapture();
                for (auto it = windows.rbegin(); it != windows.rend(); ++it) DestroyWindow(*it);
                app.hwnd_ = app.floatingPopupHwnd_ = nullptr;
                for (const auto& host : app.persistentDockHosts_) host->hwnd = nullptr;
            }
        } cleanup{*app, windows};
        app->hwnd_ = create(nullptr, 6400, 2160);
        const HWND control = create(nullptr);
        auto host = std::make_unique<DesktopApp::PersistentDockHost>();
        host->active = true; dockContext.dock = host.get(); host->hwnd = create(&dockContext);
        const HWND dockWindow = host->hwnd;
        app->persistentDockHosts_.push_back(std::move(host));
        const HWND popupWindow = create(&popupContext);
        app->floatingPopupHwnd_ = popupWindow;
        {
            auto& dock = *app->persistentDockHosts_.front();
            const auto savedAppearance = app->floatingDockPersonalization_;
            app->floatingDockPersonalization_.widgetEdgeHighlightEnabled = true;
            app->floatingDockPersonalization_.widgetBorderWidth = 1.0f;
            app->floatingDockPersonalization_.widgetEdgeHighlightWidth = 1.0f;
            dock.promoted = true;
            dock.dockRect = dock.animationVisualRect = dock.visualPanelRect = {40, 40, 200, 100};
            dock.sourceRect = {0, 0, 400, 300};
            dock.windowRegionPending = true;
            HRGN observed = CreateRectRgn(0, 0, 0, 0);
            const bool applied = app->ApplyFloatingDockWindowRegion(dock);
            const int regionKind = observed ? GetWindowRgn(dockWindow, observed) : ERROR;
            const int halo = snowdesktop::flat_glass_rim::kPanelOverdraw;
            check(applied && regionKind != ERROR && PtInRegion(observed, 200 + halo - 1, 70) &&
                PtInRegion(observed, 40 - halo, 70) && !PtInRegion(observed, 200 + halo + 1, 70),
                "actual native Dock region preserves the renderer's full outer rim on both extended edges");
            if (observed) DeleteObject(observed);
            SetWindowRgn(dockWindow, nullptr, FALSE);
            dock.promoted = false;
            dock.dockRect = dock.animationVisualRect = dock.visualPanelRect = dock.sourceRect = {};
            app->floatingDockPersonalization_ = savedAppearance;
        }
        {
            // All HWNDs live on this fixture's inactive desktop. Keep a Dock
            // already raised while its foreground changes after the summon.
            auto& dock = *app->persistentDockHosts_.front();
            const HWND proxy = create(nullptr);
            app->floatingDockInputHwnd_ = proxy;
            app->SelectPersistentDockHost(&dock);
            dock.frameReady = dock.promoted = dock.promotionInputVisible = true;
            app->floatingDockVisible_ = true;
            ShowWindow(control, SW_SHOWNOACTIVATE);
            SetWindowPos(dockWindow, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            inputSafetyForeground = control;
            inputSafetyOwnedWindow = app->hwnd_;
            inputSafetyOriginalThread = &GetWindowThreadProcessId;
            InputSafetyImport foreground("GetForegroundWindow", reinterpret_cast<ULONG_PTR>(&InputSafetyForeground));
            InputSafetyImport thread("GetWindowThreadProcessId", reinterpret_cast<ULONG_PTR>(&InputSafetyThread));
            InputSafetyImport denyForeground("SetForegroundWindow", reinterpret_cast<ULONG_PTR>(&InputSafetyDenyForeground));
            InputSafetyImport denyActive("SetActiveWindow", reinterpret_cast<ULONG_PTR>(&InputSafetyDenyFocus));
            InputSafetyImport denyFocus("SetFocus", reinterpret_cast<ULONG_PTR>(&InputSafetyDenyFocus));
            InputSafetyImport buttonState("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&InputSafetyAsyncKeyState));
            inputSafetyPhysicalButton = 0;
            dock.promotionInputForeground = control;
            dock.promotionInputProcess = GetCurrentProcessId() + 1000;
            dock.promotionInputPrivilegeGap = true;
            app->RefreshFloatingDockPromotionInputs();
            check(dock.explicitDismissAvailable && inputSafetyForeground == control,
                "already-visible Dock gains its escape path when elevated foreground persists without a focus request");
            check(app->floatingDockPromotionInputToken_ != 0,
                "raised Dock maintains an independent foreground watch throughout its visible lifetime");
            app->floatingDockKeyboardSessionActive_ = true;
            dock.promotionInputFocusPendingUntil = GetTickCount64() + 10000;
            app->RefreshFloatingDockPromotionInputs();
            check(!dock.explicitDismissAvailable && dock.promotionInputFocusPendingUntil &&
                app->floatingDockKeyboardSessionActive_,
                "queued proxy activation retains its session without flashing the fallback before a result");
            inputSafetyForeground = app->hwnd_;
            app->RefreshFloatingDockPromotionInputs();
            check(!dock.explicitDismissAvailable && !dock.promotionInputFocusPendingUntil,
                "live foreground recheck hides the escape path after confirmed loss of higher-privilege foreground");

            inputSafetyForeground = control;
            dock.promotionInputForeground = control;
            dock.promotionInputProcess = GetCurrentProcessId() + 1000;
            dock.promotionInputPrivilegeGap = true;
            dock.promotionInputFocusPendingUntil = GetTickCount64() - 1;
            app->RefreshFloatingDockPromotionInputs();
            check(dock.explicitDismissAvailable && !dock.promotionInputFocusPendingUntil &&
                !app->floatingDockKeyboardSessionActive_ && inputSafetyForeground == control,
                "uncompleted activation publishes fallback only after bounded observation and retires the proxy session");

            inputSafetyForeground = control;
            app->dockWindowActivationObservations_[control].Begin(false, GetTickCount64(),
                GetCurrentProcessId() + 1000, GetCurrentThreadId() + 1000);
            app->floatingDockInputDeactivated_ = true;
            check(app->ObserveDockPromotionActivation(control) &&
                app->dockWindowActivationObservations_.empty() && !app->floatingDockInputDeactivated_,
                "restored app activation retires its retry before the raised Dock rechecks foreground and fallback");

            // The proxy was foreground only between timer samples. Repeated
            // native deactivation must dismiss even with identical baselines
            // and all global mouse buttons unreadable.
            const ULONGLONG savedMinimizeTick = app->dockSystemMinimizeStartedTick_.load();
            WNDCLASSW inputClass{};
            if (!GetClassInfoW(app->instance_, kInputWindowClassName, &inputClass))
            {
                inputClass.hInstance = app->instance_;
                inputClass.lpfnWndProc = &DesktopApp::InputWndProc;
                inputClass.lpszClassName = kInputWindowClassName;
                if (!RegisterClassW(&inputClass)) throw std::runtime_error("register private proxy fixture class");
            }
            for (int repeat = 0; repeat < 4; ++repeat)
            {
                check(app->EnsureFloatingDockInputWindow(), "each repeated Dock session creates an input proxy");
                const HWND sessionProxy = app->floatingDockInputHwnd_;
                dock.promoted = true;
                dock.desktopLayerForegroundWindow = control;
                app->floatingDockVisible_ = app->floatingDockKeyboardSessionActive_ = true;
                app->dockSystemMinimizeStartedTick_.store(GetTickCount64());
                app->HandleInputMessage(sessionProxy, WM_ACTIVATEAPP, FALSE, 0);
                app->UpdateFloatingDockEdgeSwipe();
                check(!dock.promoted && !app->floatingDockVisible_ && !app->floatingDockKeyboardSessionActive_ &&
                    !app->floatingDockInputDeactivated_ && inputSafetyForeground == control,
                    "native proxy loss repeatedly dismisses Dock without a poll observing the proxy or physical press");
                check(!app->floatingDockInputHwnd_ && !IsWindow(sessionProxy),
                    "every ended session destroys its native proxy instead of retaining hidden activation state");
            }
            app->dockSystemMinimizeStartedTick_.store(savedMinimizeTick);
            app->RefreshFloatingDockPromotionInputs();
            check(!app->floatingDockPromotionInputToken_, "closed Dock retires its foreground watch");
            app->floatingDockInputHwnd_ = nullptr;
            app->SelectPersistentDockHost(nullptr);
            dock.frameReady = false;
            inputSafetyOwnedWindow = inputSafetyForeground = nullptr;
            ShowWindow(control, SW_HIDE);
            SetWindowPos(dockWindow, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
        }
        {
            // A delivered button press is an independent escape path, even
            // when global button state is unavailable. Run actual host dispatch.
            auto& dock = *app->persistentDockHosts_.front();
            dock.promoted = dock.explicitDismissAvailable = true;
            dock.explicitDismissRect = {10, 10, 34, 34};
            app->floatingDockVisible_ = true;
            InputSafetyImport buttonState("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&InputSafetyAsyncKeyState));
            inputSafetyPhysicalButton = 0;
            app->HandleFloatingDockMessage(dock, dockWindow, WM_LBUTTONDOWN, 0, MAKELPARAM(20, 20));
            check(!dock.promoted && !app->floatingDockVisible_ && !dock.explicitDismissAvailable,
                "native explicit Dock dismiss works without a readable external press");
            dock.explicitDismissRect = {};
        }
        {
            // The original desktop press must close promotion even when the
            // global API reports no button. Only that OS query is replaced;
            // native dispatch and the real close/visibility path still run.
            InputSafetyImport buttonState("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&InputSafetyAsyncKeyState));
            inputSafetyPhysicalButton = 0;
            for (const UINT press : {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN})
            {
                app->persistentDockHosts_.front()->promoted = true;
                app->floatingDockVisible_ = true;
                app->HandleMessage(app->hwnd_, press, 0, MAKELPARAM(200, 200));
                check(!app->persistentDockHosts_.front()->promoted && !app->floatingDockVisible_,
                    "delivered native desktop presses dismiss Dock without global button state");
                app->CancelPointerPressWithoutCaptureRelease();
                ReleaseCapture();
            }
        }
        const auto capture = [](HWND window) {
            SetCapture(window);
            if (GetCapture() != window) throw std::runtime_error("input capture fixture setup failed");
        };
        capture(control); SendMessageW(control, WM_CANCELMODE, 0, 0);
        check(!GetCapture(), "default WM_CANCELMODE control must release capture");
        for (const auto window : {dockWindow, popupWindow})
        {
            app->mouseDown_ = true; capture(window); SendMessageW(window, WM_CANCELMODE, 0, 0);
            check(!GetCapture(), window == dockWindow ? "Dock cancellation must release its capture" :
                "popup cancellation must release its capture");
            check(!app->mouseDown_, "cancellation must clear the ordinary pointer press");
            ReleaseCapture();
            const HWND other = window == dockWindow ? popupWindow : dockWindow;
            app->mouseDown_ = true; capture(window); capture(other);
            SendMessageW(window, WM_CANCELMODE, 0, 0);
            check(GetCapture() == other && app->mouseDown_, "old host cancellation must preserve transferred owned capture and press");
            ReleaseCapture();
            app->mouseDown_ = true; capture(window); capture(control);
            check(GetCapture() == control && !app->mouseDown_, "capture loss must preserve the external owner and cancel the old press");
            ReleaseCapture();
        }
        {
            InputSafetyImport buttonState("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&InputSafetyAsyncKeyState));
            InputSafetyImport queuedButtons("GetQueueStatus", reinterpret_cast<ULONG_PTR>(&InputSafetyQueueStatus));
            inputSafetyQueuedButtons = 0;
            for (const HWND window : {app->hwnd_, dockWindow, popupWindow})
            {
                app->mouseDown_ = true;
                capture(window);
                // No button-up was delivered. A queued pressed move still
                // belongs to the gesture and must not be treated as loss.
                inputSafetyPhysicalButton = 0;
                app->OnMouseMoveAt(0, {0, 0});
                check(GetCapture() == window && app->mouseDown_,
                    "internal pointer resampling must not infer a lost button release");
                app->OnNativeMouseMoveAt(MK_LBUTTON, {0, 0});
                check(GetCapture() == window && app->mouseDown_,
                    "a queued button-down move preserves the current press");
                // Conversely, an old button-free message can precede a newer
                // physical press. Protect all buttons, including swapped and
                // side-button/chord input, without synthesizing system input.
                for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
                {
                    inputSafetyPhysicalButton = button;
                    app->OnNativeMouseMoveAt(0, {0, 0});
                    check(GetCapture() == window && app->mouseDown_,
                        "a physically held button protects a live gesture from stale movement");
                }
                inputSafetyPhysicalButton = 0;
                // HIGH word means still queued, even when its LOW change bits
                // were consumed by another query. Never preempt that release.
                for (const DWORD queued : {MAKELONG(0, QS_MOUSEBUTTON), MAKELONG(QS_MOUSEBUTTON, QS_MOUSEBUTTON)})
                {
                    inputSafetyQueuedButtons = queued;
                    app->OnNativeMouseMoveAt(0, {0, 0});
                    check(GetCapture() == window && app->mouseDown_,
                        "pending native button messages must be processed before stale-press cancellation");
                }
                // LOW-only changes describe history, not a queued release.
                inputSafetyQueuedButtons = MAKELONG(QS_MOUSEBUTTON, 0);
                app->OnNativeMouseMoveAt(0, {0, 0});
                check(GetCapture() == nullptr && !app->mouseDown_,
                    "button-free pointer return cancels a press whose release was lost");
                check(!app->rightButtonItemDrag_ && !app->middleButtonWidgetMove_,
                    "lost-release cleanup retires gesture button state");
                inputSafetyQueuedButtons = 0;
            }
            for (const bool middle : {false, true})
            {
                app->mouseDown_ = true;
                app->rightButtonItemDrag_ = !middle;
                app->middleButtonWidgetMove_ = middle;
                capture(dockWindow);
                app->OnNativeMouseMoveAt(0, {0, 0});
                check(!GetCapture() && !app->mouseDown_ &&
                    !app->rightButtonItemDrag_ && !app->middleButtonWidgetMove_,
                    "lost right/middle release is cancelled without committing a drop or click");
            }
            app->mouseDown_ = true;
            capture(control);
            app->OnNativeMouseMoveAt(0, {0, 0});
            check(GetCapture() == control, "button-free recovery preserves capture owned by another window");
            ReleaseCapture();
            app->mouseDown_ = false;
        }
        // Real background worker + real control dispatcher on the private
        // desktop. Withhold dispatch to produce a late asynchronous probe;
        // then resume it and observe recovery, without synthesizing input.
        std::cerr << "Input safety: asynchronous probe and recovery\n";
        {
            std::lock_guard<std::mutex> lock(inputSafetyLogMutex);
            inputSafetyLogs.clear();
        }
        const auto logged = [](const wchar_t* text) {
            return std::any_of(inputSafetyLogs.begin(), inputSafetyLogs.end(),
                [text](const std::wstring& line) { return line.find(text) != std::wstring::npos; });
        };
        {
            snowdesktop::InputDiagnostics diagnostics(popupWindow, {20, 60000, 80}, InputSafetyLog);
            check(!snowdesktop::InputDiagnostics::Acknowledge(control, WM_NULL, 0, 0),
                "unrelated WM_NULL must not acknowledge an input probe");
            MSG input{}; input.hwnd = popupWindow; input.message = WM_MOUSEMOVE;
            snowdesktop::InputDiagnostics::ObserveDequeued(input);
            input.message = WM_LBUTTONDOWN;
            snowdesktop::InputDiagnostics::ObserveDequeued(input);
            snowdesktop::InputDiagnostics::DeviceChange(DBT_DEVNODES_CHANGED);
            {
                std::unique_lock<std::mutex> lock(inputSafetyLogMutex);
                check(inputSafetyLogChanged.wait_for(lock, std::chrono::seconds(3),
                    [&] { return logged(L"reason=probe-late"); }),
                    "background diagnostics must record a late UI probe while dispatch is stopped");
                check(logged(L"pumpMoves=1 pumpButtons=1") && logged(L"deviceCount=1"),
                    "snapshot must retain delivered pointer counts and aggregated device changes");
            }
            const ULONGLONG deadline = GetTickCount64() + 3000;
            bool recovered = false;
            while (GetTickCount64() < deadline)
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
                {
                    std::lock_guard<std::mutex> lock(inputSafetyLogMutex);
                    recovered = logged(L"reason=probe-recovered");
                }
                if (recovered) break;
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            }
            check(recovered, "real control dispatcher must acknowledge and log probe recovery");
            std::lock_guard<std::mutex> lock(inputSafetyLogMutex);
            check(inputSafetyLogs.size() < 12, "diagnostics must keep late-probe logging bounded");
        }
        // A posted probe carries no object pointer and remains harmless after
        // monitor destruction. Drain it before the rest of the input fixture.
        std::cerr << "Input safety: drain retired diagnostic probes\n";
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        }
        std::cerr << "Input safety: unavailable modal first frame\n";
        app->luaWidgetPanelRequest_.widgetId = L"input-safety-dialog";
        app->luaWidgetPanelRequest_.surface = "dialog";
        app->luaWidgetPanelRequest_.modal = true;
        app->luaWidgetPanelAnimation_.ShowImmediately();
        app->UpdateFloatingPopupWindowBounds(true);
        check(!app->floatingPopupDcompSurface_, "first-frame failure fixture must have no completed surface");
        check(!IsWindowVisible(popupWindow), "failed modal first frame must not expose a full-desktop input window");
        for (unsigned retry = 0; retry < 8; ++retry)
            SendMessageW(popupWindow, WM_TIMER, 2, 0);
        check(app->floatingPopupPresentationRetryCount_ == 5 && !IsWindowVisible(popupWindow),
            "unavailable presentation must stop after five retries without reviving its input window");
        std::cerr << "Input safety: initialize popup recovery graphics\n";
        if (!app->InitGraphics()) throw std::runtime_error("initialize real graphics for popup recovery control");
        std::cerr << "Input safety: present recovered popup\n";
        app->UpdateFloatingPopupWindowBounds(true);
        check(app->floatingPopupDcompSurface_ && IsWindowVisible(popupWindow) && app->floatingPopupPresentationRetryCount_ == 0,
            "a real successful composition frame must restore the hidden popup and clear retry state");
        app->floatingPopupCompositionPaintInProgress_ = true;
        app->UpdateFloatingPopupWindowBounds(true);
        check(IsWindowVisible(popupWindow), "a deferred reentrant frame must not retire an existing usable input surface");
        app->floatingPopupCompositionPaintInProgress_ = false;
        {
            InputSafetyImport region("SetWindowRgn", reinterpret_cast<ULONG_PTR>(&InputSafetyDenyRegion));
            app->mouseDown_ = true;
            capture(popupWindow);
            app->floatingPopupWindowBounds_ = {};
            app->UpdateFloatingPopupWindowBounds(true);
            check(!IsWindowVisible(popupWindow) && IsRectEmpty(&app->floatingPopupWindowBounds_),
                "native clipping failure must hide input and invalidate its successful-geometry cache");
            check(!GetCapture() && !app->mouseDown_,
                "presentation failure releases hidden popup capture and cancels its production pointer state");
        }
        SendMessageW(popupWindow, WM_TIMER, 2, 0);
        check(IsWindowVisible(popupWindow) && !IsRectEmpty(&app->floatingPopupWindowBounds_),
            "identical geometry must retry clipping and recover through the production timer route");
        app->mouseDown_ = true;
        capture(popupWindow);
        app->ResetFloatingPopupCompositionResources();
        check(!IsWindowVisible(popupWindow), "discarding the last surface must immediately retire native popup input");
        check(!GetCapture() && !app->mouseDown_,
            "discarding the last popup surface also retires its capture and pointer state");
        app->StopFloatingPopupOutsideClickMonitor();
        app->luaWidgetPanelRequest_ = {};

        std::cerr << "Input safety: denied and permitted activation\n";
        inputSafetyForeground = control;
        inputSafetyOriginalThread = &GetWindowThreadProcessId;
        {
            InputSafetyImport foreground("GetForegroundWindow", reinterpret_cast<ULONG_PTR>(&InputSafetyForeground));
            InputSafetyImport denyForeground("SetForegroundWindow", reinterpret_cast<ULONG_PTR>(&InputSafetyDenyForeground));
            InputSafetyImport denyActive("SetActiveWindow", reinterpret_cast<ULONG_PTR>(&InputSafetyDenyFocus));
            InputSafetyImport denyFocus("SetFocus", reinterpret_cast<ULONG_PTR>(&InputSafetyDenyFocus));
            InputSafetyImport thread("GetWindowThreadProcessId", reinterpret_cast<ULONG_PTR>(&InputSafetyThread));
            InputSafetyImport gui("GetGUIThreadInfo", reinterpret_cast<ULONG_PTR>(&InputSafetyGui));
            InputSafetyImport attach("AttachThreadInput", reinterpret_cast<ULONG_PTR>(&InputSafetyAttach), true);
            inputSafetyAttachCalls = 0;
            inputSafetyLocalFocusCalls = 0;
            const bool focused = app->FocusKeyboardWindow(dockWindow, true, L"Input safety denied activation");
            check(!focused && inputSafetyAttachCalls == 0, "denied host activation must return without attaching a foreign input queue");
            check(inputSafetyLocalFocusCalls == 0,
                "denied foreground activation must not leave the proxy active or focused in the local input queue");
            (void)app->FocusKeyboardWindow(popupWindow, true, L"Unavailable popup activation");
            check(!IsWindowVisible(popupWindow), "activation must not revive a popup whose presentation is unavailable");
            inputSafetyAllowForeground = true;
            check(app->FocusKeyboardWindow(dockWindow, true, L"Input safety permitted activation") && inputSafetyAttachCalls == 0,
                "a permitted foreground request must retain the ordinary successful focus route");
            inputSafetyAllowForeground = false;
        }
        if (!failures.empty()) throw std::runtime_error(std::to_string(failures.size()) + " native input safety assertions failed");
        std::cout << "Native input safety: cancel, capture transfer, failed modal frame and denied activation passed\n";
    }
