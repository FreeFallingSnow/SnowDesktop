#pragma once

namespace native_widget_settings_cases
{
using namespace snowdesktop::control;
using namespace snowdesktop::control_settings;
using Access = NativeDragIntegrationAccess;
using native_drag_commit_cases::Require;
using native_drag_commit_cases::Read;

inline Value Strings(std::initializer_list<std::string> names)
{
    auto value = Array();
    for (const auto& name : names) value.array.push_back(String(name));
    return value;
}
inline Value TargetValue(const std::string& id) { return TargetJson({"native-widget", id}); }
inline Value Get(DesktopApp& app, const std::string& id, bool catalog = false)
{ return Access::NativeSettings(app, catalog ? "settings.catalog" : "settings.get", Object({{"target", TargetValue(id)}})); }
inline Value Update(DesktopApp& app, const std::string& id, Value changes)
{
    return Access::NativeSettings(app, "settings.update", Object({{"target", TargetValue(id)}, {"ifRevision", Get(app, id).object.at("revision")}, {"changes", std::move(changes)}}));
}
inline Value Action(DesktopApp& app, const std::string& id, const std::string& name, Value arguments)
{
    return Access::NativeSettings(app, "settings.action", Object({{"target", TargetValue(id)}, {"ifRevision", Get(app, id).object.at("revision")},
        {"action", String(name)}, {"arguments", std::move(arguments)}}));
}
template<class Function> void Reject(std::string_view code, Function function)
{
    try { function(); Require(false, "native settings operation should reject this input"); }
    catch (const Error& error) { Require(error.code == code, "native settings rejection preserves its declared error code: " + error.code); }
}
inline const DesktopWidget& Widget(const DesktopApp& app, const std::wstring& id)
{
    const auto& widgets = Access::Widgets(app);
    const auto found = std::find_if(widgets.begin(), widgets.end(), [&](const auto& widget) { return widget.id == id; });
    Require(found != widgets.end(), "actual native setting owner still exists");
    return *found;
}
inline DesktopWidget Storage(std::wstring id, DesktopWidgetType type, int column)
{
    DesktopWidget widget;
    widget.id = std::move(id); widget.type = type; widget.title = L"Initial native title";
    widget.gridCell = {L"__page:1", column, 0}; widget.gridSpan = {3, 2};
    return widget;
}
inline void Run()
{
    namespace fs = std::filesystem;
    native_drag_commit_cases::Profile profile;
    const auto directory = profile.root / L"native-settings-files";
    fs::create_directories(directory);
    const std::vector<fs::path> files{directory / L"Project notes.txt", directory / L"Budget.csv", directory / L"Zebra.txt"};
    for (const auto& path : files) { std::ofstream stream(path); stream << "Real native settings fixture\n"; }
    POINT cursorBefore{}; Require(GetCursorPos(&cursorBefore) != FALSE, "capture current system cursor without moving it");
    const auto foregroundBefore = GetForegroundWindow();

    auto& collectionApp = profile.Model(false, profile.root / L"native-collection-settings");
    auto collection = Storage(L"collection", DesktopWidgetType::Collection, 0);
    collection.scrollContainerMode = true;
    collection.itemKeys = {ToUpperInvariant(files[2].wstring()), ToUpperInvariant(files[0].wstring())};
    Access::SeedWidgetTransactions(collectionApp, {collection}, files);
    Access::NativeSettingsTopBar(collectionApp, true);
    Require(Access::Save(collectionApp), "persist the actual native baseline before direct settings edits");
    const auto catalog = Get(collectionApp, "collection", true);
    Require(catalog.object.at("fields").object.at("title").object.at("type").string == "string" &&
        catalog.object.at("fields").object.at("listColumns").object.at("type").string == "integer" &&
        !catalog.object.at("fields").object.at("autoCollect").object.at("available").boolean,
        "native catalog exposes typed applicable settings rather than pretending every field applies to every type");
    const auto oldRevision = Get(collectionApp, "collection").object.at("revision");
    Update(collectionApp, "collection", Object({{"title", String("Creative Tools")}, {"privacyMode", Boolean(true)},
        {"showTitle", Boolean(true)}, {"bottomBarHover", Boolean(false)}, {"keepWhenDesktopHidden", Boolean(true)}, {"showOnHoverOnly", Boolean(true)}}));
    Require(Widget(collectionApp, L"collection").title == L"Creative Tools" && Widget(collectionApp, L"collection").customTitle == L"Creative Tools" &&
        Widget(collectionApp, L"collection").privacyMode && Widget(collectionApp, L"collection").showTitle,
        "one direct patch updates actual native owner settings through the shared transaction");
    Reject("stale_settings", [&] { Access::NativeSettings(collectionApp, "settings.update", Object({{"target", TargetValue("collection")},
        {"ifRevision", oldRevision}, {"changes", Object({{"title", String("Stale")}})}})); });
    const auto beforeInvalid = Read(Access::LayoutPath(collectionApp));
    Reject("invalid_params", [&] { Update(collectionApp, "collection", Object({{"title", String("Partial mutation")}, {"listColumns", Number(5)}})); });
    Require(Widget(collectionApp, L"collection").title == L"Creative Tools" && Read(Access::LayoutPath(collectionApp)) == beforeInvalid,
        "full validation precedes every model and persistence mutation");
    Reject("unsupported_setting", [&] { Update(collectionApp, "collection", Object({{"sourceFolderPath", String("C:\\different")}})); });
    Update(collectionApp, "collection", Object({{"listMode", Boolean(true)}, {"listColumns", Number(3)}}));
    Update(collectionApp, "collection", Object({{"detailShowModified", Boolean(true)}, {"detailShowType", Boolean(true)}}));
    Require(Widget(collectionApp, L"collection").listColumns == 1 && Widget(collectionApp, L"collection").detailShowModified &&
        Widget(collectionApp, L"collection").showDetails, "native metadata columns use the real single-row list rules");
    Reject("invalid_params", [&] { Update(collectionApp, "collection", Object({{"detailModifiedPosition", Number(0.8)}, {"detailTypePosition", Number(0.4)}})); });
    Access::NativeSettingsScroll(collectionApp, L"collection", 19);
    Update(collectionApp, "collection", Object({{"titleBarCollapsed", Boolean(true)}}));
    Require(Widget(collectionApp, L"collection").titleBarCollapsed && Widget(collectionApp, L"collection").scrollOffset == 19,
        "shared collapse transaction preserves native scroll position and grid occupancy");
    Access::NativeSettings(collectionApp, "settings.reset", Object({{"target", TargetValue("collection")},
        {"ifRevision", Get(collectionApp, "collection").object.at("revision")}, {"keys", Strings({"title"})}}));
    Require(Widget(collectionApp, L"collection").customTitle.empty() && !Widget(collectionApp, L"collection").title.empty() &&
        Widget(collectionApp, L"collection").title == Utf8ToWide(Access::NativeSettingsState(collectionApp, L"collection").defaults.title),
        "title reset restores the native default and never writes an empty visible label");
    Update(collectionApp, "collection", Object({{"titleBarExpandOnHover", Boolean(true)}, {"titleBarCollapsed", Boolean(false)}}));
    Update(collectionApp, "collection", Object({{"scrollContainerMode", Boolean(false)}}));
    Update(collectionApp, "collection", Object({{"largeFolderTitleless", Boolean(true)}}));
    Require(!Widget(collectionApp, L"collection").scrollContainerMode && Widget(collectionApp, L"collection").largeFolderTitleless,
        "native collection display modes retain their original type and size applicability");
    auto& iconApp = profile.Model(false, profile.root / L"native-single-cell-settings");
    auto icon = Storage(L"single-cell", DesktopWidgetType::Collection, 0); icon.gridSpan = {1, 1};
    Access::SeedWidgetTransactions(iconApp, {icon}, files);
    Require(Access::Save(iconApp), "persist the actual single-cell native entrance");
    Update(iconApp, "single-cell", Object({{"singleCellIconMode", Boolean(true)}}));
    Require(Widget(iconApp, L"single-cell").singleCellIconMode, "icon presentation is editable on a real standalone one-cell native entrance");
    Reject("unsupported_setting", [&] { Update(iconApp, "single-cell", Object({{"scrollContainerMode", Boolean(true)}})); });

    auto& groupedApp = profile.Model(false, profile.root / L"native-collection-group-settings");
    auto first = Storage(L"first", DesktopWidgetType::Collection, 0);
    first.itemKeys = {ToUpperInvariant(files[0].wstring())};
    auto second = Storage(L"second", DesktopWidgetType::Collection, 3);
    second.itemKeys = {ToUpperInvariant(files[1].wstring())};
    auto group = Storage(L"group", DesktopWidgetType::CollectionGroup, 0);
    group.childWidgetIds = {first.id, second.id}; group.activeCategoryId = first.id; group.showSearchBox = true;
    Access::SeedWidgetTransactions(groupedApp, {first, second, group}, files);
    Require(Access::Save(groupedApp), "save collection-group ownership baseline");
    const auto grouped = Get(groupedApp, "group");
    Require(grouped.object.at("contentTargetId").string == "first" && grouped.object.at("activeChildId").string == "first",
        "native group catalog resolves the same active child as its real content view");
    Update(groupedApp, "group", Object({{"listMode", Boolean(true)}}));
    Require(Widget(groupedApp, L"first").listMode && !Widget(groupedApp, L"second").listMode && !Widget(groupedApp, L"group").listMode,
        "group content settings belong to its active child rather than an unused owner mirror");
    Reject("invalid_params", [&] { Update(groupedApp, "group", Object({{"activeCategoryId", String("second")}, {"listMode", Boolean(true)}})); });
    const auto firstRevision = Get(groupedApp, "group").object.at("revision");
    Update(groupedApp, "group", Object({{"activeCategoryId", String("second")}}));
    Reject("stale_settings", [&] { Access::NativeSettings(groupedApp, "settings.update", Object({{"target", TargetValue("group")},
        {"ifRevision", firstRevision}, {"changes", Object({{"listMode", Boolean(true)}})}})); });
    Update(groupedApp, "group", Object({{"listMode", Boolean(true)}}));
    Require(Widget(groupedApp, L"second").listMode, "a new revision directs a group edit to the newly selected child");
    Action(groupedApp, "group", "reorder-tabs", Object({{"order", Strings({"second", "first"})}}));
    Require(Widget(groupedApp, L"group").childWidgetIds == std::vector<std::wstring>{L"second", L"first"},
        "explicit tab reorder changes only the order of existing membership");
    Reject("invalid_params", [&] { Action(groupedApp, "group", "reorder-tabs", Object({{"order", Strings({"first", "foreign"})}})); });

    auto& fileApp = profile.Model(false, profile.root / L"native-file-group-settings");
    auto desktopFiles = Storage(L"desktop-files", DesktopWidgetType::FileCategories, 0);
    desktopFiles.itemKeys = {ToUpperInvariant(files[0].wstring())};
    auto oldCollector = Storage(L"previous-collector", DesktopWidgetType::FileCategories, 6);
    oldCollector.autoCollect = true; oldCollector.itemKeys = {ToUpperInvariant(files[1].wstring())};
    auto mapping = Storage(L"mapping", DesktopWidgetType::FolderMapping, 3);
    mapping.sourceFolderPath = directory.wstring();
    for (auto it = files.rbegin(); it != files.rend(); ++it)
    {
        FolderEntry entry; entry.name = it->filename().wstring(); entry.fullPath = it->wstring();
        mapping.folderEntries.push_back(std::move(entry));
    }
    auto fileGroup = Storage(L"file-group", DesktopWidgetType::FileGroup, 0);
    fileGroup.childWidgetIds = {desktopFiles.id, mapping.id}; fileGroup.activeCategoryId = desktopFiles.id; fileGroup.showSearchBox = true;
    Access::SeedWidgetTransactions(fileApp, {desktopFiles, mapping, oldCollector, fileGroup}, files);
    Require(Access::Save(fileApp), "save native file group and auto-collection ownership baseline");
    const auto beforeFailure = Access::NativeSettingsModelIdentity(fileApp);
    const auto savedBeforeFailure = Read(Access::LayoutPath(fileApp));
    struct LockedLayout
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~LockedLayout() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } locked{CreateFileW(Access::LayoutPath(fileApp).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Require(locked.handle != INVALID_HANDLE_VALUE, "hold a real sharing violation against native candidate persistence");
    Reject("settings_failed", [&] { Update(fileApp, "file-group", Object({{"title", String("Uncommitted")}, {"autoCollect", Boolean(true)},
        {"autoCollectCategoryIds", Array()}, {"sortColumn", String("name")}, {"sortAscending", Boolean(false)}})); });
    Require(Access::NativeSettingsModelIdentity(fileApp) == beforeFailure && Read(Access::LayoutPath(fileApp)) == savedBeforeFailure,
        "failed candidate save rolls back owner, child order, competing collector membership, item placements, pages and records");
    CloseHandle(locked.handle); locked.handle = INVALID_HANDLE_VALUE;
    Update(fileApp, "file-group", Object({{"autoCollect", Boolean(true)}, {"autoCollectCategoryIds", Array()}}));
    Require(Widget(fileApp, L"desktop-files").autoCollect && !Widget(fileApp, L"previous-collector").autoCollect,
        "direct automatic collection uses the original exclusive-category ownership rules");
    Reject("confirmation_required", [&] { Action(fileApp, "file-group", "collect-now", Object()); });
    Action(fileApp, "file-group", "collect-now", Object({{"confirmed", Boolean(true)}}));
    const auto searched = Action(fileApp, "file-group", "set-search", Object({{"query", String("Project")}}));
    Require(searched.object.at("transient").boolean && searched.object.at("contentTargetId").string == "file-group" &&
        searched.object.at("sortTargetId").string == "file-group", "live group search resolves aggregate display and sort ownership without persisting query text");
    Update(fileApp, "file-group", Object({{"sortColumn", String("name")}}));
    Require(Widget(fileApp, L"file-group").contentSortColumn == snowdesktop::list_detail_rules::Column::Name &&
        Get(fileApp, "file-group").object.at("searchText").string == "Project", "sorting the aggregate view preserves its actual live search context");
    Action(fileApp, "file-group", "set-search", Object({{"query", String("")}}));
    Update(fileApp, "file-group", Object({{"activeCategoryId", String("mapping")}}));
    Require(Get(fileApp, "file-group").object.at("sourceFolderPath").string == WideToUtf8(directory.wstring()),
        "mapping source path remains a real read-only source rather than a replacement directory command");
    const auto mappingBeforeFailure = Access::NativeSettingsModelIdentity(fileApp);
    const auto mappingDiskBeforeFailure = Read(Access::LayoutPath(fileApp));
    locked.handle = CreateFileW(Access::LayoutPath(fileApp).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(locked.handle != INVALID_HANDLE_VALUE, "hold a real mapping-order candidate sharing violation");
    Reject("settings_failed", [&] { Update(fileApp, "file-group", Object({{"sortColumn", String("name")}, {"sortAscending", Boolean(false)}})); });
    Require(Access::NativeSettingsModelIdentity(fileApp) == mappingBeforeFailure && Read(Access::LayoutPath(fileApp)) == mappingDiskBeforeFailure,
        "failed mapped-folder sorting restores actual folder-entry order, saved order keys and all layout records");
    CloseHandle(locked.handle); locked.handle = INVALID_HANDLE_VALUE;
    Update(fileApp, "file-group", Object({{"sortColumn", String("name")}, {"sortAscending", Boolean(true)}, {"showFileCategories", Boolean(true)}, {"dateHeaders", Boolean(true)}}));
    Require(Widget(fileApp, L"mapping").folderSortMode == 0 && Widget(fileApp, L"mapping").showFileCategories && Widget(fileApp, L"mapping").dateHeaders,
        "file-group settings target the real mapped source and its durable folder order");
    Access::NativeSettings(fileApp, "settings.reset", Object({{"target", TargetValue("mapping")},
        {"ifRevision", Get(fileApp, "mapping").object.at("revision")}, {"keys", Strings({"title"})}}));
    Require(Widget(fileApp, L"mapping").title == directory.filename().wstring(), "mapped folder title reset derives the real source folder name");
    POINT cursorAfter{}; Require(GetCursorPos(&cursorAfter) != FALSE && cursorBefore.x == cursorAfter.x && cursorBefore.y == cursorAfter.y &&
        foregroundBefore == GetForegroundWindow(), "semantic native settings never move the OS cursor or steal global foreground");
    std::cout << "Native settings: five storage types, typed validation, group ownership, persistence rollback, actual collection/sort/search, cursor and foreground preservation passed\n";
}
}
