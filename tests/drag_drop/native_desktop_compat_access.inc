#include "../icons/native_large_icon_completion_access.inc"

    static void SeedCompatAppearance(DesktopApp& app, const std::filesystem::path& file,
        int destination, const snowdesktop::LargeIconConfig& config)
    {
        PopulateModel(app, {file}, 0);
        auto& page = app.gridPages_.front(); page.columns = 12; page.rows = 8;
        page.bounds = page.workArea = page.visualWorkArea = RECT{0, 0, 1200, 800};
        app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        if (destination == 2) SeedDock(app);
        else
        {
            DesktopWidget widget;
            widget.id = L"compat-target"; widget.title = widget.customTitle = L"Appearance owner";
            widget.type = destination ? DesktopWidgetType::FileCategories : DesktopWidgetType::Collection;
            widget.gridCell = {page.id, 4, 0}; widget.gridSpan = {3, 3};
            widget.autoCollect = false; app.widgets_.push_back(std::move(widget));
        }
        if (!SaveGeneralSettings(GetGeneralSettingsPath().c_str(), app.generalSettings_))
            throw std::runtime_error("D11 requires a saved normal settings baseline before large-icon upgrade");
        app.items_.front().largeIcon = config;
        app.LayoutItems();
    }
    static POINT CompatAppearancePoint(DesktopApp& app, int destination)
    {
        if (destination == 2) return DockPoint(app);
        const auto body = MemberContainer(app, L"compat-target")->GetBodyRect();
        return {(body.left + body.right) / 2, (body.top + body.bottom) / 2};
    }
    static void BeginCompatAppearanceReturn(DesktopApp& app, const std::wstring& key, int destination, POINT point)
    {
        if (destination == 2) { BeginDockFiles(app, {key}, point, 0); return; }
        for (auto& item : app.items_) item.selected = PathsEqualInsensitive(item.layoutKey, key);
        auto* origin = MemberContainer(app, L"compat-target");
        auto items = origin->GetSelectedItems();
        if (items.size() != 1) throw std::runtime_error("D11 requires the same original member on return");
        const auto frame = items.front()->GetBounds();
        const POINT start{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        app.dragGroupOriginX_ = frame.left; app.dragGroupOriginY_ = frame.top; app.mouseDownPoint_ = start;
        auto captured = app.BuildDragSourceList(items, origin);
        app.dragSession_.Begin(origin, std::move(items), std::move(captured), start, point);
    }
    static void SubmitCompatAppearance(DesktopApp& app)
    {
        auto* target = Target(app); auto* slot = TargetSlot(app); auto* origin = app.dragSession_.Source();
        const auto region = Region(app); const auto items = app.dragSession_.Items();
        if (!target || region == HitRegion::None || region == HitRegion::Blocked || region == HitRegion::Handoff)
            throw std::runtime_error("D11 requires an accepted native storage target");
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, slot, region, 0);
        app.EndDragSession(); app.ClearSelection();
        app.ApplyPageMapping(); app.LayoutItems();
    }
    static POINT CompatFilePoint(DesktopApp& app, const std::wstring& key)
    {
        app.ClearSelection();
        const auto index = app.FindItemIndexByKey(key);
        if (index >= app.items_.size()) throw std::runtime_error("D09 requires its real desktop folder shortcut");
        const auto frame = app.items_[index].bounds;
        return {(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
    }
