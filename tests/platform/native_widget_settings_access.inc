    static snowdesktop::control::Value NativeSettings(DesktopApp& app, std::string_view method,
        const snowdesktop::control::Value& params)
    {
        using namespace snowdesktop::control_settings;
        return HandleNativeWidgetSettings(method, params, ParseTarget(params), {
            [&](std::string_view id) { return app.CaptureNativeWidgetSettings(Utf8ToWide(std::string(id))); },
            [&](const auto& state, const auto& patch) { return app.ApplyNativeWidgetSettings(state, patch); },
            [&](std::string_view id, const std::string& query) { return app.NativeWidgetSearchAction(Utf8ToWide(std::string(id)), Utf8ToWide(query)); }});
    }
    static auto NativeSettingsState(DesktopApp& app, const std::wstring& id) { return app.CaptureNativeWidgetSettings(id); }
    static void NativeSettingsTopBar(DesktopApp& app, bool enabled)
    { app.personalizationSettings_.scrollableTitleBarOnTop = enabled; }
    static void NativeSettingsScroll(DesktopApp& app, const std::wstring& id, int offset)
    { app.widgets_.at(app.FindWidgetIndexById(id)).scrollOffset = offset; }
    static auto NativeSettingsModelIdentity(const DesktopApp& app)
    {
        using namespace snowdesktop::control;
        auto widgets = Array(), items = Array(), pages = Array(), records = Array();
        for (const auto& widget : app.widgets_)
        {
            auto keys = Array(), children = Array(), categories = Array(), entries = Array();
            for (const auto& key : widget.itemKeys) keys.array.push_back(String(WideToUtf8(key)));
            for (const auto& id : widget.childWidgetIds) children.array.push_back(String(WideToUtf8(id)));
            for (const auto& category : widget.autoCollectCategoryIds) categories.array.push_back(String(WideToUtf8(category)));
            for (const auto& entry : widget.folderEntries) entries.array.push_back(String(WideToUtf8(entry.fullPath)));
            widgets.array.push_back(Object({{"id", String(WideToUtf8(widget.id))}, {"title", String(WideToUtf8(widget.title))},
                {"keys", std::move(keys)}, {"children", std::move(children)}, {"entries", std::move(entries)},
                {"sortColumn", Number(static_cast<int>(widget.contentSortColumn))}, {"sortAscending", Boolean(widget.contentSortAscending)},
                {"folderSortMode", Number(widget.folderSortMode)}, {"folderSortAscending", Boolean(widget.folderSortAscending)},
                {"autoCollect", Boolean(widget.autoCollect)}, {"categories", std::move(categories)},
                {"page", String(WideToUtf8(widget.gridCell.pageId))}, {"column", Number(widget.gridCell.column)}, {"row", Number(widget.gridCell.row)}}));
        }
        for (const auto& item : app.items_) items.array.push_back(Object({{"id", String(WideToUtf8(item.layoutKey))},
            {"page", String(WideToUtf8(item.gridCell.pageId))}, {"column", Number(item.gridCell.column)}, {"row", Number(item.gridCell.row)}, {"slot", Number(item.slot)}}));
        for (const auto& id : app.savedPageIds_) pages.array.push_back(String(WideToUtf8(id)));
        std::vector<std::wstring> recordIds;
        for (const auto& [id, record] : app.layoutRecords_) { (void)record; recordIds.push_back(id); }
        std::sort(recordIds.begin(), recordIds.end());
        for (const auto& id : recordIds)
        {
            const auto& record = app.layoutRecords_.at(id);
            records.array.push_back(Object({{"id", String(WideToUtf8(id))}, {"page", String(WideToUtf8(record.cell.pageId))},
                {"column", Number(record.cell.column)}, {"row", Number(record.cell.row)}, {"columns", Number(record.span.columns)},
                {"rows", Number(record.span.rows)}, {"slot", Number(record.legacySlot)}}));
        }
        return Serialize(Object({{"widgets", std::move(widgets)}, {"items", std::move(items)}, {"pages", std::move(pages)}, {"records", std::move(records)}}));
    }
