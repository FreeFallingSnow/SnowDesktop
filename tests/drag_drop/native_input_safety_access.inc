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
        if (context && !context->popup && !context->dock && (message == WM_LBUTTONUP || message == WM_MBUTTONUP))
            return context->app->HandleMessage(window, message, wp, lp);
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
    static inline unsigned inputSafetyAttachCalls = 0;
    static inline bool inputSafetyAllowForeground = false;
    static inline decltype(&GetWindowThreadProcessId) inputSafetyOriginalThread = nullptr;
    static HWND WINAPI InputSafetyForeground() { return inputSafetyForeground; }
    static BOOL WINAPI InputSafetyDenyForeground(HWND window)
    { if (!inputSafetyAllowForeground) return FALSE; inputSafetyForeground = window; return TRUE; }
    static HWND WINAPI InputSafetyDenyFocus(HWND) { return nullptr; }
    static BOOL WINAPI InputSafetyAttach(DWORD, DWORD, BOOL) { ++inputSafetyAttachCalls; return FALSE; }
    static DWORD WINAPI InputSafetyThread(HWND window, LPDWORD process)
    {
        if (window == inputSafetyForeground)
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
    static inline std::mutex inputSafetyLogMutex;
    static inline std::condition_variable inputSafetyLogChanged;
    static inline std::vector<std::wstring> inputSafetyLogs;
    static void InputSafetyLog(const wchar_t* line)
    {
        std::lock_guard<std::mutex> lock(inputSafetyLogMutex);
        inputSafetyLogs.emplace_back(line);
        inputSafetyLogChanged.notify_all();
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
            app->LoadLayoutSlots();
            const auto index = app->FindWidgetIndexById(widget.id);
            check(index < app->widgets_.size() && app->widgets_[index].gridCell.pageId == page.id &&
                app->widgets_[index].gridCell.column == 3 && app->widgets_[index].gridCell.row == 2,
                "widget release cell survives the real layout reload");
            app->HideDragHintWindow();
            if (app->hintHwnd_) DestroyWindow(app->hintHwnd_);
            app->hintHwnd_ = nullptr;
            ReleaseCapture(); DestroyWindow(window); app->hwnd_ = nullptr;
        }
        if (!failures.empty()) throw std::runtime_error(std::to_string(failures.size()) + " widget release assertions failed");
    }

    static void RunInputSafety()
    {
        std::vector<std::string> failures;
        const auto check = [&](bool value, const char* message) {
            if (!value) { failures.emplace_back(message); std::cerr << "FAIL: " << message << '\n'; }
        };
        WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = InputSafetyWindowProc;
        cls.lpszClassName = L"SnowDesktop.NativeInputSafety";
        if (!RegisterClassW(&cls)) throw std::runtime_error("register isolated input windows");
        RunWidgetReleaseSafety(cls.lpszClassName);
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
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        }
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
        if (!app->InitGraphics()) throw std::runtime_error("initialize real graphics for popup recovery control");
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
            const bool focused = app->FocusKeyboardWindow(dockWindow, true, L"Input safety denied activation");
            check(!focused && inputSafetyAttachCalls == 0, "denied host activation must return without attaching a foreign input queue");
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
