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
    static DropPreviewList GroupSurfaceLabelPreview(DesktopApp& app, const std::wstring& groupId,
        const std::wstring& boundaryId, bool after)
    {
        auto* view = MemberContainer(app, groupId);
        const RECT frame = GroupSurfaceTabRect(app, groupId, boundaryId);
        if (IsRectEmpty(&frame)) throw std::runtime_error("group surface boundary must be a visible native tab");
        const POINT point{after ? frame.right - 1 : frame.left + 1, (frame.top + frame.bottom) / 2};
        app.dragSession_.UpdatePoint(point);
        Slot* slot = nullptr;
        const auto region = view->HitTestDrag(point, slot);
        if (!slot || region != (after ? HitRegion::SortAfter : HitRegion::SortBefore))
            throw std::runtime_error("native group tab hit must publish the requested insertion edge");
        return app.BuildDropPreviewList(app.dragSession_.SourceList(), view, slot, region, 0, point);
    }
    static void DropGroupSurfaceLabels(DesktopApp& app, const std::wstring& groupId)
    {
        auto* target = MemberContainer(app, groupId);
        Slot* slot = nullptr;
        const auto region = target->HitTestDrag(app.dragSession_.CurrentPoint(), slot);
        if (!slot || (region != HitRegion::SortBefore && region != HitRegion::SortAfter))
            throw std::runtime_error("group label release requires its actual native tab boundary");
        auto* origin = app.dragSession_.Source();
        const auto items = app.dragSession_.Items();
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, slot, region, 0);
        app.SaveLayoutSlots(); app.ClearSelection(); app.EndDragSession();
        app.ApplyPageMapping(); app.LayoutItems();
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
