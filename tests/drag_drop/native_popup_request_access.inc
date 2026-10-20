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
