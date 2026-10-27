    // Included inside NativeDragIntegrationAccess. Models and input points are
    // fixtures; native virtual hit/preview/drop, storage and refresh stay real.
    static RECT GroupSurfaceTabRect(DesktopApp& app, const std::wstring& groupId,
        const std::wstring& childId)
    {
        auto* view = MemberContainer(app, groupId);
        if (auto* group = dynamic_cast<CollectionGroup*>(view)) return group->GetTabRectById(childId);
        if (auto* group = dynamic_cast<FileGroup*>(view)) return group->GetSourceTabRectById(childId);
        throw std::runtime_error("group surface fixture requires a native group");
    }
    static auto GroupSurfaceTabLayout(DesktopApp& app, const std::wstring& groupId)
    {
        auto* view = MemberContainer(app, groupId);
        if (auto* group = dynamic_cast<CollectionGroup*>(view)) return group->GetTabDragPreviewLayout();
        if (auto* group = dynamic_cast<FileGroup*>(view)) return group->GetTabDragPreviewLayout();
        throw std::runtime_error("tab preview requires a native group");
    }
    static void BeginResponsiveGroupLabels(DesktopApp& app, const std::vector<std::wstring>& selected,
        const std::wstring& pressedId)
    {
        auto source = BeginGroupLabels(app, L"surface-group", selected);
        auto items = app.dragSession_.Items();
        const auto pressed = std::find_if(source.entries.begin(), source.entries.end(),
            [&](const auto& entry) { return entry.widgetId == pressedId; });
        if (pressed == source.entries.end()) throw std::runtime_error("pressed label must belong to the captured selection");
        const size_t primary = static_cast<size_t>(std::distance(source.entries.begin(), pressed));
        const RECT tab = GroupSurfaceTabRect(app, L"surface-group", pressedId);
        if (IsRectEmpty(&tab)) throw std::runtime_error("responsive sort starts on an actual visible source label");
        const POINT down{(tab.left + tab.right) / 2, (tab.top + tab.bottom) / 2};
        app.dragSession_.Begin(MemberContainer(app, L"surface-group"), std::move(items), std::move(source), down, {-100, -100});
        app.dragSession_.SetVisualItemBounds({}, primary);
    }
    static std::wstring GroupSurfaceLabelHint(DesktopApp& app, const std::wstring& groupId)
    {
        return MemberContainer(app, groupId)->GetDragHint(app.dragSession_.TargetSlot(),
            app.dragSession_.TargetRegion(), app.dragSession_.Items(), app.dragSession_.Source(), 0);
    }
    static DropPreviewList GroupSurfacePointPreview(DesktopApp& app, const std::wstring& groupId, POINT point)
    {
        auto* view = MemberContainer(app, groupId);
        app.ResolveCurrentDragTargetAt(point);
        if (app.dragSession_.TargetContainer() != view)
            throw std::runtime_error("actual resolver must retain the requested native group");
        return app.BuildDropPreviewList(app.dragSession_.SourceList(), view, app.dragSession_.TargetSlot(),
            app.dragSession_.TargetRegion(), 0, point);
    }
    static DropPreviewList GroupSurfaceLabelPreview(DesktopApp& app, const std::wstring& groupId,
        const std::wstring& boundaryId, bool after, int zone = 0)
    {
        auto* view = MemberContainer(app, groupId);
        const RECT frame = GroupSurfaceTabRect(app, groupId, boundaryId);
        if (IsRectEmpty(&frame)) throw std::runtime_error("group surface boundary must be a visible native tab");
        POINT point{after ? frame.right - 1 : frame.left + 1, (frame.top + frame.bottom) / 2};
        const RECT component = view->GetFrameRect();
        if (zone == 1) point.y = component.bottom - 1;
        if (zone == 2) point.y = component.top + 1;
        if (zone == 3) point.y = (component.top + component.bottom) / 2;
        const auto preview = GroupSurfacePointPreview(app, groupId, point);
        Slot* slot = app.dragSession_.TargetSlot();
        const auto region = app.dragSession_.TargetRegion();
        if (!slot || region != (after ? HitRegion::SortAfter : HitRegion::SortBefore))
            throw std::runtime_error("native group tab hit must publish the requested insertion edge");
        return preview;
    }
    static void DropGroupSurfaceLabels(DesktopApp& app, const std::wstring& groupId)
    {
        auto* target = MemberContainer(app, groupId);
        Slot* slot = nullptr;
        const auto region = target->HitTestDrag(app.dragSession_.CurrentPoint(), slot);
        if ((!slot || (region != HitRegion::SortBefore && region != HitRegion::SortAfter)) && region != HitRegion::Empty)
            throw std::runtime_error("group label release requires its actual native tab boundary");
        auto* origin = app.dragSession_.Source();
        const auto items = app.dragSession_.Items();
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, slot, region, 0);
        app.SaveLayoutSlots(); app.ClearSelection(); app.EndDragSession();
        app.ApplyPageMapping(); app.LayoutItems();
    }
    static void LeaveGroupSurface(DesktopApp& app)
    {
        app.ResolveCurrentDragTargetAt({-100, -100});
    }
    static bool GroupSurfaceGhostWanted(DesktopApp& app)
    {
        return snowdesktop::group_tab_drag_preview::ShouldShowFloatingPreview(app.dragSession_);
    }
    static inline bool groupSurfaceGhostHidden = false;
    static BOOL WINAPI GroupSurfaceGhostVisible(HWND window) { return window == completionWindow; }
    static BOOL WINAPI GroupSurfaceGhostShow(HWND window, int command)
    {
        if (window == completionWindow && command == SW_HIDE) groupSurfaceGhostHidden = true;
        return TRUE;
    }
    static bool GroupSurfaceRetiresActualGhost(DesktopApp& app)
    {
        bool validBounds = false;
        for (const auto* item : app.dragSession_.Items())
            if (item) { const RECT bounds = item->GetBounds(); validBounds = validBounds || !IsRectEmpty(&bounds); }
        if (!validBounds) throw std::runtime_error("ghost retirement requires real label geometry, not an empty visual");
        // Observe the real Sync -> Hide path using only the fixture's message
        // window. Imported ShowWindow is captured; no desktop window is shown.
        const HWND saved = app.dragPreviewHwnd_;
        auto indices = app.dragPreviewItemIndices_;
        app.dragPreviewHwnd_ = completionWindow;
        app.dragPreviewItemIndices_ = {static_cast<size_t>(-1)};
        groupSurfaceGhostHidden = false;
        InputSafetyImport visible("IsWindowVisible", reinterpret_cast<ULONG_PTR>(&GroupSurfaceGhostVisible));
        InputSafetyImport show("ShowWindow", reinterpret_cast<ULONG_PTR>(&GroupSurfaceGhostShow));
        app.SyncDragPreviewWindow();
        const bool retiredBeforeGeometry = groupSurfaceGhostHidden &&
            app.dragPreviewItemIndices_ == std::vector<size_t>{static_cast<size_t>(-1)};
        app.dragPreviewHwnd_ = saved; app.dragPreviewItemIndices_ = std::move(indices);
        return retiredBeforeGeometry;
    }
    static void GroupSurfaceTransportVisual(DesktopApp& app, bool visible)
    {
        app.dragSession_.SetVisualVisible(visible);
    }
    static void BeginGroupComponentMove(DesktopApp& app, const std::wstring& id)
    {
        const auto index = app.FindWidgetIndexById(id);
        if (index >= app.widgets_.size()) throw std::runtime_error("component move requires a real desktop source");
        app.widgetAction_ = DesktopApp::WidgetAction::Move;
        app.mouseDownWidgetIndex_ = index; app.mouseDown_ = true; app.mouseDownHit_ = nullptr;
        app.rightButtonItemDrag_ = app.middleButtonWidgetMove_ = false;
        app.pendingWidgetButtonAction_ = WidgetHit::None;
        app.widgetDragOriginalCell_ = app.widgetPreviewCell_ = app.widgets_[index].gridCell;
        app.widgetDragOriginalSpan_ = app.widgetPreviewSpan_ = app.widgets_[index].gridSpan;
        app.widgetCollectionGroupTargetIndex_ = app.widgetCollectionGroupInsertIndex_ = static_cast<size_t>(-1);
        const RECT frame = MemberContainer(app, id)->GetFrameRect();
        app.mouseDownPoint_ = app.lastMousePoint_ = {(frame.left + frame.right) / 2, frame.top + 2};
    }
    static size_t GroupComponentMovePoint(DesktopApp& app, POINT point)
    {
        const HWND saved = app.hwnd_; app.hwnd_ = completionWindow;
        app.OnMouseMoveAt(MK_LBUTTON, point);
        app.hwnd_ = saved;
        const auto target = app.widgetCollectionGroupTargetIndex_;
        if (target >= app.widgets_.size() || app.widgets_[target].id != L"surface-group")
            throw std::runtime_error("component move must resolve the real destination group");
        return app.widgetCollectionGroupInsertIndex_;
    }
    static void EndGroupComponentMove(DesktopApp& app, POINT point, bool cancel)
    {
        const HWND saved = app.hwnd_; app.hwnd_ = completionWindow;
        if (cancel) app.OnKeyDown(VK_ESCAPE);
        else app.OnLeftButtonUpAt(0, point, false);
        app.hwnd_ = saved;
    }
    static void GroupSurfaceDateHeaders(DesktopApp& app, bool enabled)
    {
        const auto index = app.FindWidgetIndexById(L"ordered-group");
        if (index >= app.widgets_.size()) throw std::runtime_error("date header fixture requires its group model");
        app.widgets_[index].dateHeaders = enabled;
        auto* view = MemberContainer(app, L"ordered-group");
        if (auto* group = dynamic_cast<FileGroup*>(view)) group->InvalidateHostedView();
        else view->InvalidateSlots();
    }
    static std::wstring GroupSurfaceHint(DesktopApp& app, bool atEnd)
    {
        auto* view = MemberContainer(app, L"ordered-group");
        const auto& slots = view->GetSlots();
        if (slots.empty()) throw std::runtime_error("date header hint requires native visible members");
        return view->GetDragHint(atEnd ? nullptr : slots.front().get(),
            atEnd ? HitRegion::SortAfter : HitRegion::SortBefore, app.dragSession_.Items(), app.dragSession_.Source(), 0);
    }
    static void GroupSurfaceSearch(DesktopApp& app, const std::wstring& id, const std::wstring& query)
    {
        auto* view = dynamic_cast<ScrollingItemWidget*>(MemberContainer(app, id));
        if (!view) throw std::runtime_error("group search fixture requires a real scrolling group view");
        view->SetSearchText(query);
    }
    static DragSourceList BeginGroupSurfaceSearch(DesktopApp& app, const std::wstring& groupId,
        const std::vector<std::filesystem::path>& selected)
    {
        for (auto& widget : app.widgets_)
        {
            widget.selected = false;
            for (auto& entry : widget.folderEntries)
                entry.selected = std::any_of(selected.begin(), selected.end(),
                    [&](const auto& path) { return PathsEqualInsensitive(entry.fullPath, path.wstring()); });
        }
        for (auto& item : app.items_)
            item.selected = std::any_of(selected.begin(), selected.end(),
                [&](const auto& path) { return PathsEqualInsensitive(item.parsingName, path.wstring()); });
        auto* origin = MemberContainer(app, groupId);
        auto items = origin->GetSelectedItems();
        auto captured = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), captured, {50, 50}, {50, 50});
        return captured;
    }
    static DragSourceList BeginGroupSurfaceIncoming(DesktopApp& app,
        const std::vector<std::filesystem::path>& selected)
    {
        for (auto& widget : app.widgets_) widget.selected = false;
        for (auto& item : app.items_)
            item.selected = std::any_of(selected.begin(), selected.end(),
                [&](const auto& path) { return PathsEqualInsensitive(item.parsingName, path.wstring()); });
        auto* origin = Desktop(app);
        auto items = origin->GetSelectedItems();
        auto captured = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), captured, {1050, 650}, {1050, 650});
        return captured;
    }
    static DropPreviewList GroupSurfaceIncomingPreview(DesktopApp& app)
    {
        auto* view = MemberContainer(app, L"surface-group");
        const RECT frame = GroupSurfaceTabRect(app, L"surface-group", L"B");
        if (IsRectEmpty(&frame)) throw std::runtime_error("D07 requires an actual visible inactive tab");
        const POINT point{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        app.dragSession_.UpdatePoint(point);
        Slot* slot = nullptr;
        const auto region = view->HitTestDrag(point, slot);
        if (!slot || (region != HitRegion::SortBefore && region != HitRegion::SortAfter))
            throw std::runtime_error("D07 requires the native tab surface, not a synthetic child target");
        return app.BuildDropPreviewList(app.dragSession_.SourceList(), view, slot, region, 0, point);
    }
    static std::pair<std::wstring, size_t> GroupSurfaceExternalBinding(DesktopApp& app, POINT point)
    {
        const auto destination = app.CaptureExternalSlotDestination(point, MK_CONTROL);
        if (!destination) throw std::runtime_error("external inactive tab must capture its actual native destination");
        return {destination->widgetId, destination->preview.insertIndex};
    }
