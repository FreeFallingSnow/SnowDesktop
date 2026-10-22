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
    }
