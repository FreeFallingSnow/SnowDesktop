#include "app/app.h"
#include "app_control_runtime.h"
#include "platform/control/control_elements.h"
#include "layout/widget_displacement.h"
#include "platform/control/control_protocol.h"
#include "data/data_paths.h"
#include "ui/menu/modern_menu.h"
#include "demo/builtin_scenes.h"
#include "desktop/desktop_source.h"

namespace
{
using namespace snowdesktop::control;
Value Rect(const RECT& rect)
{
    return Object({{"left", Number(rect.left)}, {"top", Number(rect.top)},
        {"right", Number(rect.right)}, {"bottom", Number(rect.bottom)}});
}
Value CaptureValue(const snowdesktop::screen_capture::State& state)
{
    return Object({{"id", String(state.id)}, {"status", String(state.status)}, {"error", String(state.error)},
        {"path", String(WideToUtf8(state.path.wstring()))}, {"monitorId", String(WideToUtf8(state.monitor.id))},
        {"kind", String(state.kind)}, {"source", String(state.source)},
        {"directory", String(WideToUtf8(state.directory.wstring()))},
        {"manifestPath", String(WideToUtf8(state.manifestPath.wstring()))},
        {"frameMapPath", String(WideToUtf8(state.frameMapPath.wstring()))},
        {"fps", Number(state.fps)}, {"durationMs", Number(state.durationMs)},
        {"frames", String(std::to_string(state.frames))}, {"capturedFrames", String(std::to_string(state.capturedFrames))},
        {"duplicatedFrames", String(std::to_string(state.duplicatedFrames))},
        {"discardedFrames", String(std::to_string(state.discardedFrames))},
        {"firstFrameTime100ns", String(std::to_string(state.firstFrameTime100ns))},
        {"lastFrameTime100ns", String(std::to_string(state.lastFrameTime100ns))},
        {"captureStartedUnixMs", Number(static_cast<double>(state.captureStartedUnixMs))},
        {"elapsed100ns", String(std::to_string(state.elapsed100ns))},
        {"encodedDuration100ns", String(std::to_string(state.encodedDuration100ns))},
        {"monitorBounds", Rect(state.monitor.bounds)}, {"width", Number(state.width)}, {"height", Number(state.height)},
        {"includeSystemCursor", Boolean(state.systemCursor)}, {"colorSpace", String("sdr-bgra8")},
        {"startedUnixMs", Number(static_cast<double>(state.startedUnixMs))},
        {"finishedUnixMs", Number(static_cast<double>(state.finishedUnixMs))},
        {"frameTime100ns", String(std::to_string(state.frameTime100ns))}, {"processId", Number(GetCurrentProcessId())}});
}
Value SettingsAutomationValue(const snowdesktop::settings_automation::State& state)
{
    return Object({{"id", String(state.id)}, {"status", String(state.status)}, {"kind", String(state.kind)},
        {"error", String(state.error)}, {"elementId", String(state.elementId)}, {"action", String(state.action)},
        {"elementCount", Number(static_cast<double>(state.elementCount))}, {"truncated", Boolean(state.truncated)},
        {"startedUnixMs", Number(static_cast<double>(state.startedUnixMs))},
        {"finishedUnixMs", Number(static_cast<double>(state.finishedUnixMs))}});
}
Value DemoValue(const snowdesktop::demo::ExportState& state)
{
    return Object({{"id", String(state.id)}, {"status", String(state.status)}, {"error", String(state.error)},
        {"directory", String(WideToUtf8(state.directory.wstring()))}, {"frames", Number(state.frames)},
        {"completedFrames", Number(state.completedFrames)}, {"sourceExeSha256", String(state.sourceExeSha256)},
        {"startedUnixMs", Number(static_cast<double>(state.startedUnixMs))},
        {"finishedUnixMs", Number(static_cast<double>(state.finishedUnixMs))}, {"manifest", state.manifest}});
}
Value WidgetValue(const DesktopWidget& widget, bool grouped)
{
    const char* type = "unknown";
    switch (widget.type)
    {
    case DesktopWidgetType::Collection: type = "collection"; break;
    case DesktopWidgetType::CollectionGroup: type = "collection-group"; break;
    case DesktopWidgetType::FileCategories: type = "file-categories"; break;
    case DesktopWidgetType::FolderMapping: type = "folder-mapping"; break;
    case DesktopWidgetType::FileGroup: type = "file-group"; break;
    case DesktopWidgetType::LuaScript: type = "lua"; break;
    }
    return Object({{"id", String(WideToUtf8(widget.id))}, {"type", String(type)},
        {"title", String(WideToUtf8(widget.title))}, {"pageId", String(WideToUtf8(widget.gridCell.pageId))},
        {"column", Number(widget.gridCell.column)}, {"row", Number(widget.gridCell.row)},
        {"columns", Number(widget.gridSpan.columns)}, {"rows", Number(widget.gridSpan.rows)},
        {"grouped", Boolean(grouped)}, {"bounds", Rect(widget.bounds)}});
}
std::string Revision(const Value& value)
{
    // A content identity, not a monotonic counter. Includes native items so
    // manual file rearrangement also invalidates a pending placement request.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : Serialize(value)) { hash ^= byte; hash *= 1099511628211ULL; }
    return std::to_string(hash);
}
}

void DesktopApp::InitializeApplicationControl()
{
    applicationControlRuntime_ = std::make_unique<snowdesktop::app_control::Runtime>();
    if (!applicationControl_.Start([this](std::string_view method, const snowdesktop::control::Value& params) {
        return HandleApplicationControl(method, params);
    }))
    {
        const auto message = L"Application control IPC unavailable, error=" + std::to_wstring(applicationControl_.LastError());
        WriteDiagnosticLogEntry(message.c_str());
    }
}

snowdesktop::control::Value DesktopApp::ApplicationControlLayout() const
{
    using namespace snowdesktop::control;
    Value pages = Array(), widgets = Array(), items = Array();
    for (const auto& page : gridPages_)
        pages.array.push_back(Object({{"id", String(WideToUtf8(page.id))},
            {"monitorId", String(WideToUtf8(page.monitorId))}, {"bounds", Rect(page.bounds)},
            {"columns", Number(page.columns)}, {"rows", Number(page.rows)},
            {"dpiX", Number(page.dpiX)}, {"dpiY", Number(page.dpiY)}}));
    for (const auto& widget : widgets_) widgets.array.push_back(WidgetValue(widget, IsGroupedWidget(widget)));
    Value identity = Object({{"pages", pages}, {"widgets", widgets}});
    for (const auto& item : items_)
        items.array.push_back(Object({{"key", String(WideToUtf8(item.layoutKey))},
            {"pageId", String(WideToUtf8(item.gridCell.pageId))}, {"column", Number(item.gridCell.column)},
            {"row", Number(item.gridCell.row)}, {"columns", Number(item.gridSpan.columns)}, {"rows", Number(item.gridSpan.rows)}}));
    identity.object.emplace("items", std::move(items));
    return Object({{"layoutRevision", String(Revision(identity))}, {"pages", std::move(pages)}, {"widgets", std::move(widgets)}});
}

snowdesktop::control::Value DesktopApp::HandleApplicationControl(
    std::string_view method, const snowdesktop::control::Value& params)
{
    using namespace snowdesktop::control;
    if (!startupInitializationComplete_ || exitRequested_)
        throw Error("not_ready", "Application is starting or shutting down");
    if (method == "input.begin" || method == "input.pointer" || method == "input.keyboard" || method == "input.state" || method == "input.end")
        return HandlePrivatePointer(method, params);
    if (method == "settings.catalog" || method == "settings.get" || method == "settings.update" || method == "settings.reset" || method == "settings.action")
        return HandleControlSettings(method, params);
    if (method == "preview.describe") { Fields(params, {}); return snowdesktop::demo::Describe(); }
    if (method == "preview.render")
    {
        Fields(params, {"scene"}); const auto* value = params.Find("scene");
        if (!value) throw Error("invalid_params", "scene is required", -32602);
        const auto scene = snowdesktop::demo::ParseScene(*value);
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768) throw Error("demo_host_unavailable", "Cannot identify the current render host");
        std::string error;
        if (!applicationControlRuntime_->demo.Start(scene, executable,
            std::filesystem::path(GetDataDirectoryPath()) / L"Automation" / L"Demos", error))
            throw Error(error, "Scene export could not start");
        return DemoValue(applicationControlRuntime_->demo.Current());
    }
    if (method == "preview.state" || method == "preview.cancel")
    {
        Fields(params, {"id"}); const auto state = applicationControlRuntime_->demo.Current();
        if (Text(params, "id") != state.id) throw Error("demo_not_found", "This export is no longer the current task");
        if (method == "preview.cancel" && state.status == "running") applicationControlRuntime_->demo.Cancel();
        return DemoValue(applicationControlRuntime_->demo.Current());
    }
    if (method == "ui.settingsRefresh")
    {
        Fields(params, {}); std::string error;
        if (!applicationControlRuntime_->settings.Refresh(settingsWindow_ ? settingsWindow_->Window() : nullptr, error))
            throw Error(error, "Settings tree refresh could not start");
        return SettingsAutomationValue(applicationControlRuntime_->settings.Current());
    }
    if (method == "ui.settingsState")
    {
        Fields(params, {"id"}); const auto state = applicationControlRuntime_->settings.Current();
        if (Text(params, "id") != state.id) throw Error("settings_task_not_found", "This settings task is no longer current");
        return SettingsAutomationValue(state);
    }
    if (method == "capture.monitors")
    {
        Fields(params, {}); auto monitors = Array();
        for (const auto& monitor : snowdesktop::screen_capture::Monitors())
            monitors.array.push_back(Object({{"id", String(WideToUtf8(monitor.id))},
                {"bounds", Rect(monitor.bounds)}, {"primary", Boolean(monitor.primary)}}));
        return Object({{"coordinateSpace", String("physical-screen-pixels")}, {"monitors", std::move(monitors)}});
    }
    if (method == "capture.screenshot")
    {
        Fields(params, {"monitorId", "includeSystemCursor"});
        auto monitor = Text(params, "monitorId", false); if (monitor.empty()) monitor = "primary";
        const auto* cursor = params.Find("includeSystemCursor");
        if (cursor && !cursor->IsBoolean()) throw Error("invalid_params", "includeSystemCursor must be boolean", -32602);
        std::string error;
        if (!applicationControlRuntime_->capture.Screenshot(Utf8ToWide(monitor),
            std::filesystem::path(GetDataDirectoryPath()) / L"Automation" / L"Captures", cursor && cursor->boolean, error))
            throw Error(error.empty() ? "capture_unavailable" : error, "Screenshot request could not start");
        return CaptureValue(applicationControlRuntime_->capture.Current());
    }
    if (method == "capture.record")
    {
        Fields(params, {"monitorId", "includeSystemCursor", "durationMs", "fps"});
        auto monitor = Text(params, "monitorId", false); if (monitor.empty()) monitor = "primary";
        const auto* cursor = params.Find("includeSystemCursor");
        if (cursor && !cursor->IsBoolean()) throw Error("invalid_params", "includeSystemCursor must be boolean", -32602);
        const unsigned duration = static_cast<unsigned>(Integer(params, "durationMs", 100, 120000));
        const unsigned fps = static_cast<unsigned>(Integer(params, "fps", 1, 60));
        std::string error;
        if (!applicationControlRuntime_->capture.Record(Utf8ToWide(monitor),
            std::filesystem::path(GetDataDirectoryPath()) / L"Automation" / L"Captures", cursor && cursor->boolean,
            duration, fps, error)) throw Error(error.empty() ? "capture_unavailable" : error, "Recording request could not start");
        return CaptureValue(applicationControlRuntime_->capture.Current());
    }
    if (method == "capture.state" || method == "capture.cancel")
    {
        Fields(params, {"id"}); const auto state = applicationControlRuntime_->capture.Current();
        if (Text(params, "id") != state.id) throw Error("capture_not_found", "This capture is no longer the current task");
        if (method == "capture.cancel" && state.status == "running") applicationControlRuntime_->capture.Cancel();
        return CaptureValue(applicationControlRuntime_->capture.Current());
    }
    if (method == "ui.snapshot") { Fields(params, {}); return ApplicationControlElements().ToJson(); }
    if (method == "ui.dismissMenu")
    {
        Fields(params, {});
        if (applicationControlRuntime_->privatePointer.Current().pendingEvents)
            throw Error("busy", "Wait for private pointer events before dismissing its menu");
        snowdesktop::modern_menu::DismissActive();
        return Object({{"status", String("accepted")}});
    }
    if (method == "app.getState")
    {
        Fields(params, {});
        const auto* popup = GetOpenPopupWidget();
        const auto popupId = dockFolderPopupOpen_ && !dockFolderPopupMappingWidgetId_.empty()
            ? WideToUtf8(dockFolderPopupMappingWidgetId_) : (popup ? WideToUtf8(popup->id) : "");
        return Object({{"processId", Number(GetCurrentProcessId())},
            {"runtimeMode", String("complete-desktop-host")},
            {"dataDirectory", String(WideToUtf8(GetDataDirectoryPath()))},
            {"desktopDirectory", String(WideToUtf8(snowdesktop::desktop_source::Directory()))},
            {"debugProfile", Boolean(snowdesktop::debug_profile::Enabled())},
            {"demoModeEnabled", Boolean(generalSettings_.demoModeEnabled)},
            {"locale", String(Locale::Instance().GetEffectiveLanguage())},
            {"desktopEnabled", Boolean(generalSettings_.softwareDesktopEnabled)},
            {"desktopVisible", Boolean(customDesktopVisible_)},
            {"presentationReady", Boolean(!desktopStartupPresentationPending_)},
            {"navigationOpen", Boolean(quickNavigationOpen_)},
            {"menuOpen", Boolean(snowdesktop::modern_menu::IsActive())},
            {"floatingDockVisible", Boolean(floatingDockVisible_)},
            {"popupWidgetId", String(popupId)},
            {"popupAnimating", Boolean(popupAnimation_.IsAnimating())},
            {"navigationAnimating", Boolean(quickNavigationAnimation_.IsAnimating())},
            {"layoutRevision", ApplicationControlLayout().object.at("layoutRevision")}});
    }
    if (method == "desktop.getLayout" || method == "widgets.list")
    {
        Fields(params, {});
        return ApplicationControlLayout();
    }
    const auto widgetIndex = [&] {
        const auto index = FindWidgetIndexById(Utf8ToWide(Text(params, "id")));
        if (index >= widgets_.size()) throw Error("not_found", "Widget does not exist");
        return index;
    };
    if (method == "widgets.get")
    {
        Fields(params, {"id"});
        const auto index = widgetIndex();
        return Object({{"widget", WidgetValue(widgets_[index], IsGroupedWidget(widgets_[index]))},
            {"layoutRevision", ApplicationControlLayout().object.at("layoutRevision")}});
    }
    // Do not enter UI actions from a nested modal loop or interrupt a live drag.
    if (applicationControlRuntime_->privatePointer.Current().enabled || mouseDown_ || dragSession_.IsActive() || widgetAction_ != WidgetAction::None || HasActiveContextMenuSession())
        throw Error("busy", "A pointer operation or context menu is active");
    if (desktopStartupPresentationPending_)
        throw Error("not_ready", "The first desktop frame is not ready");
    if (method == "widgets.place")
    {
        Fields(params, {"id", "pageId", "column", "row", "columns", "rows", "ifLayoutRevision"});
        if (!generalSettings_.softwareDesktopEnabled) throw Error("disabled", "Software desktop is disabled");
        if (GetOpenPopupWidget() || quickNavigationOpen_)
            throw Error("busy", "Close popups and Quick Navigation before changing the layout");
        if (Text(params, "ifLayoutRevision") != ApplicationControlLayout().object.at("layoutRevision").string)
            throw Error("stale_layout", "Query the current layout before applying this placement");
        const auto index = widgetIndex();
        if (IsGroupedWidget(widgets_[index])) throw Error("invalid_target", "Move the parent group instead");
        GridCell cell{Utf8ToWide(Text(params, "pageId")), Integer(params, "column", 0, 65535), Integer(params, "row", 0, 65535)};
        GridSpan span{Integer(params, "columns", 1, 65535), Integer(params, "rows", 1, 65535)};
        const auto* page = snowdesktop::widget_displacement::FindPage(gridPages_, cell.pageId);
        if (!page || cell.column >= page->columns || cell.row >= page->rows ||
            span.columns > page->columns - cell.column || span.rows > page->rows - cell.row)
            throw Error("invalid_placement", "Placement must fit an existing page");
        const auto clamped = snowdesktop::widget_displacement::ClampSpan(widgets_[index], span,
            page->columns - cell.column, page->rows - cell.row);
        if (clamped.columns != span.columns || clamped.rows != span.rows)
            throw Error("invalid_placement", "Widget does not support the requested span");
        const bool moving = widgets_[index].gridSpan.columns == span.columns && widgets_[index].gridSpan.rows == span.rows;
        PlaceWidgetWithDisplacement(index, cell, span, moving);
        const auto& placed = widgets_[index];
        if (placed.gridCell.pageId != cell.pageId || placed.gridCell.column != cell.column || placed.gridCell.row != cell.row ||
            placed.gridSpan.columns != span.columns || placed.gridSpan.rows != span.rows)
            throw Error("placement_rejected", "Existing layout rules rejected the placement");
        InvalidateDragStaticScene();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return Object({{"status", String("applied")}, {"persistence", String("queued")}, {"layout", ApplicationControlLayout()}});
    }
    if (method == "widgets.open")
    {
        Fields(params, {"id"});
        if (!generalSettings_.softwareDesktopEnabled || !customDesktopVisible_)
            throw Error("disabled", "Desktop is not visible");
        const auto index = widgetIndex();
        const auto& widget = widgets_[index];
        if (widget.type == DesktopWidgetType::LuaScript || IsGroupedWidget(widget))
            throw Error("invalid_target", "Use a standalone native storage widget");
        if (popupAnimation_.IsAnimating()) throw Error("busy", "Popup animation is active");
        const auto* current = GetOpenPopupWidget();
        const auto currentId = dockFolderPopupOpen_ && !dockFolderPopupMappingWidgetId_.empty()
            ? dockFolderPopupMappingWidgetId_ : (current ? current->id : std::wstring{});
        if (!current || currentId != widget.id)
        {
            if (current) throw Error("busy", "Close the current popup before opening another");
            OpenCollectionPopupAt(index, {widget.bounds.left + (widget.bounds.right - widget.bounds.left) / 2,
                widget.bounds.top + (widget.bounds.bottom - widget.bounds.top) / 2});
        }
        return Object({{"status", String("accepted")}});
    }
    if (method == "widgets.close") { Fields(params, {}); if (GetOpenPopupWidget()) CloseCollectionPopup(); }
    else if (method == "navigation.open")
    {
        Fields(params, {});
        if (!quickNavigationOpen_) OpenQuickNavigation();
        if (!quickNavigationOpen_) throw Error("unavailable", "Quick Navigation could not be opened");
    }
    else if (method == "navigation.close") { Fields(params, {}); if (quickNavigationOpen_) CloseQuickNavigation(); }
    else if (method == "dock.show")
    {
        Fields(params, {});
        if (!generalSettings_.dockEnabled) throw Error("disabled", "Dock is disabled");
        ShowFloatingDock();
    }
    else if (method == "dock.hide") { Fields(params, {}); CloseAllFloatingDocks(); }
    else if (method == "settings.open")
    {
        Fields(params, {"page", "widgetId"});
        const auto widgetId = Text(params, "widgetId", false);
        if (!widgetId.empty())
        {
            if (params.Find("page")) throw Error("invalid_params", "Use either page or widgetId", -32602);
            if (FindWidgetIndexById(Utf8ToWide(widgetId)) >= widgets_.size()) throw Error("not_found", "Widget does not exist");
            ShowSettingsWindow(snowdesktop::SettingsRoute::ForWidget(Utf8ToWide(widgetId)));
        }
        else
        {
            using Page = snowdesktop::SettingsPage;
            constexpr Page pages[]{Page::General, Page::Desktop, Page::Widgets, Page::BackupAndData, Page::About,
                Page::Dock, Page::Taskbar, Page::QuickNavigation, Page::ThemeManager, Page::StatusBar,
                Page::AppearanceWidgets, Page::AppearanceDesktopIcons, Page::AppearanceIconBeautification,
                Page::WidgetBehavior, Page::AnimationPerformance};
            const auto key = Text(params, "page", false);
            Page selected = Page::General;
            bool found = key.empty();
            for (const auto page : pages) if (SettingsPageKey(page) == key) { selected = page; found = true; }
            if (!found) throw Error("invalid_params", "Unsupported settings page", -32602);
            ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(selected));
        }
    }
    else throw Error("method_not_found", "Unknown method", -32601);
    return Object({{"status", String("accepted")}});
}
