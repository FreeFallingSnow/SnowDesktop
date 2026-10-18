#pragma once

#include "data/widget_layout_codec.h"
#include <set>
#include <sstream>

namespace native_widget_pair_cases
{
using namespace native_drag_commit_cases;
using Type = DesktopWidgetType;
inline constexpr Type collection = Type::Collection, files = Type::FileCategories, mapping = Type::FolderMapping;

inline const DesktopWidget* FindWidget(const DesktopApp& app, std::wstring_view id)
{
    const auto& widgets = Access::Widgets(app);
    const auto found = std::find_if(widgets.begin(), widgets.end(), [&](const auto& widget) { return widget.id == id; });
    return found == widgets.end() ? nullptr : &*found;
}
inline const DesktopWidget& Widget(const DesktopApp& app, std::wstring_view id)
{
    const auto* widget = FindWidget(app, id);
    Require(widget != nullptr, "component identity must remain in the actual host model");
    return *widget;
}
inline std::string Configuration(DesktopWidget widget, bool placement = true, bool members = true)
{
    if (!placement) { widget.gridCell = {}; widget.gridSpan = {}; }
    if (!members) widget.itemKeys.clear();
    std::ostringstream stream;
    snowdesktop::widget_layout_codec::Write(stream, widget, JsonEscapeUtf8, WideToUtf8);
    return stream.str();
}
inline std::set<std::wstring> Keys(const DesktopWidget& widget)
{
    std::set<std::wstring> result;
    for (const auto& key : widget.itemKeys) result.insert(ToUpperInvariant(key));
    return result;
}
inline std::vector<std::wstring> OrderedKeys(const DesktopWidget& widget)
{
    std::vector<std::wstring> result;
    for (const auto& key : widget.itemKeys) result.push_back(ToUpperInvariant(key));
    return result;
}
inline std::vector<std::wstring> Owners(const DesktopApp& app, const std::wstring& child)
{
    std::vector<std::wstring> result;
    for (const auto& widget : Access::Widgets(app))
        for (const auto& id : widget.childWidgetIds) if (id == child) result.push_back(widget.id);
    return result;
}
inline DesktopWidget Component(const std::wstring& id, Type type, int column, int row, bool alternate)
{
    DesktopWidget widget;
    widget.id = id; widget.type = type; widget.title = widget.customTitle = L"Original " + id;
    widget.gridCell = {L"__page:1", column, row}; widget.gridSpan = {4, 3};
    widget.autoCollect = false; widget.activeCategoryId = L"all";
    widget.listMode = alternate; widget.showTitle = true; widget.bottomBarHover = !alternate;
    widget.showSearchBox = alternate; widget.keepWhenDesktopHidden = alternate;
    widget.contentSortColumn = snowdesktop::list_detail_rules::Column::None;
    widget.contentSortAscending = !alternate;
    return widget;
}
inline DesktopWidget Group(const std::wstring& id, Type childType, std::vector<std::wstring> children,
    int column, int row, bool automatic, bool icon = false)
{
    auto group = Component(id, childType == collection ? Type::CollectionGroup : Type::FileGroup, column, row, false);
    group.childWidgetIds = std::move(children);
    group.activeCategoryId = group.childWidgetIds.empty() ? L"" : group.childWidgetIds.front();
    group.dissolveWhenSingle = automatic; group.singleCellIconMode = icon;
    if (icon) group.gridSpan = {1, 1};
    return group;
}

struct Samples
{
    fs::path root, desktop;
    std::vector<fs::path> ordinary, all;
    explicit Samples(const Profile& profile, std::size_t scene)
    {
        root = profile.root / (L"component-" + std::to_wstring(scene));
        desktop = snowdesktop::debug_profile::Current().configuration.desktop;
        fs::create_directories(root); fs::create_directories(desktop);
    }
    void Add(DesktopWidget& widget, std::wstring_view suffix)
    {
        const auto name = root.filename().wstring() + L"-" + widget.id + L"-" + std::wstring(suffix) + L".txt";
        fs::path file;
        if (widget.type == mapping)
        {
            file = root / widget.id / name;
            fs::create_directories(file.parent_path());
            widget.sourceFolderPath = file.parent_path().wstring();
        }
        else { file = desktop / name; ordinary.push_back(file); widget.itemKeys.push_back(ToUpperInvariant(file.wstring())); }
        { std::ofstream stream(file, std::ios::binary); stream << "original:" << WideToUtf8(name); }
        Require(fs::is_regular_file(file), "create isolated real component member");
        all.push_back(file);
        if (widget.type == mapping)
        {
            auto snapshot = snowdesktop::shell_refresh::ReadFolder(widget.sourceFolderPath, false);
            Require(snapshot.complete, "read actual mapped component fixture");
            widget.folderEntries = std::move(snapshot.entries);
        }
    }
    void Check() const
    {
        for (const auto& file : all)
            Require(Read(file) == "original:" + WideToUtf8(file.filename().wstring()),
                "component grouping/merging/transferring cannot move, copy, remove or alter any physical member");
        std::size_t count = 0;
        for (const auto& entry : fs::directory_iterator(desktop))
        {
            Require(std::find(ordinary.begin(), ordinary.end(), entry.path()) != ordinary.end(),
                "component-only operation cannot create an extra desktop file or shortcut");
            ++count;
        }
        Require(count == ordinary.size(), "component-only operation preserves the exact physical desktop file set");
    }
    void Cleanup() const { for (const auto& file : ordinary) fs::remove(file); }
};
inline void CheckChild(const DesktopApp& app, const DesktopWidget& original)
{
    const auto& child = Widget(app, original.id);
    Require(Configuration(child, false, false) == Configuration(original, false, false) &&
        OrderedKeys(child) == OrderedKeys(original),
        "group ownership changes must preserve each child's full settings, stable identity and ordered members");
    if (child.type == mapping)
    {
        std::set<std::wstring> expected, observed;
        for (const auto& entry : original.folderEntries) expected.insert(ToUpperInvariant(entry.fullPath));
        for (const auto& entry : child.folderEntries) observed.insert(ToUpperInvariant(entry.fullPath));
        Require(observed == expected && !observed.empty(), "mapped child must still expose the original real directory contents");
    }
}
inline void CheckRuntime(DesktopApp& app)
{
    for (const auto& widget : Access::Widgets(app))
    {
        if (!Owners(app, widget.id).empty()) continue;
        const bool dock = std::any_of(Access::DockEntries(app).begin(), Access::DockEntries(app).end(),
            [&](const auto& entry) { return entry.reference == widget.id; });
        if (dock)
            Require(std::any_of(Access::Dock(app)->GetSlots().begin(), Access::Dock(app)->GetSlots().end(),
                [&](const auto& slot) { const auto* item = dynamic_cast<DockEntryItem*>(slot->GetItem());
                    return item && item->GetReference() == widget.id; }), "Dock ownership must publish an accessible actual entrance");
        else
        {
            const auto* view = Access::MemberContainer(app, widget.id);
            const auto frame = view->GetBounds();
            Require(frame.right > frame.left && frame.bottom > frame.top,
                "standalone result must publish a nonempty native runtime frame");
        }
    }
    Require(Access::NoDockWindows(app) && Access::NoIngressPopup(app), "component integration cannot create a desktop or popup presentation window");
}

// A21: literal supported pair domain, supplied before calling production.
// Ctrl creates groups; Shift merges only the two same-type logical containers.
struct PairCase { Type source, target; bool merge; };
inline constexpr PairCase pairs[]{
    {collection, collection, true}, {files, files, true},
    {collection, collection, false}, {files, files, false},
    {files, mapping, false}, {mapping, files, false}, {mapping, mapping, false},
};
inline void RunPairs()
{
    Profile profile;
    std::size_t scenes = 0;
    for (const auto& pair : pairs)
    for (const int sourceForm : {0, 1, 2, 3}) // body boundary / real icon / real Dock / real group label
    for (const int targetForm : {0, 1, 2}) // body / icon / Dock, explicit commit-boundary target
    for (const bool reverse : {false, true})
    {
        context = "A21 pair=" + std::to_string(static_cast<int>(pair.source)) + ":" + std::to_string(static_cast<int>(pair.target)) +
            " merge=" + std::to_string(pair.merge) + " source=" + std::to_string(sourceForm) +
            " target=" + std::to_string(targetForm) + " reverse=" + std::to_string(reverse);
        Samples samples(profile, scenes++);
        auto source = Component(L"source", pair.source, 0, 0, true);
        auto target = Component(L"target", pair.target, 6, 0, false);
        auto sentinel = Component(L"unrelated", collection, 0, 4, true);
        samples.Add(source, L"one"); samples.Add(target, L"one"); samples.Add(sentinel, L"one");
        if (pair.merge)
        {
            samples.Add(target, L"shared");
            source.itemKeys.push_back(target.itemKeys.back());
        }
        if (sourceForm == 1) { source.singleCellIconMode = true; source.gridSpan = {1, 1}; }
        if (targetForm == 1) { target.singleCellIconMode = true; target.gridSpan = {1, 1}; }
        std::vector<DesktopWidget> widgets = reverse ? std::vector<DesktopWidget>{target, source, sentinel}
            : std::vector<DesktopWidget>{source, target, sentinel};
        if (sourceForm == 3)
        {
            auto passive = Component(L"passive", pair.source, 0, 0, false);
            samples.Add(passive, L"one"); widgets.push_back(passive);
            widgets.push_back(Group(L"origin-group", pair.source, {L"source", L"passive"}, 0, 0, false));
        }
        std::vector<std::wstring> dock;
        if (sourceForm == 2) dock.push_back(L"source");
        if (targetForm == 2) dock.push_back(L"target");
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, std::move(widgets), samples.ordinary, dock);
        Access::PrepareReload(app); Access::BindAsync(app);
        const auto originalSource = Widget(app, L"source"), originalTarget = Widget(app, L"target");
        const auto originalSentinel = Configuration(Widget(app, L"unrelated"));
        std::set<std::wstring> merged = Keys(originalTarget);
        const auto sourceKeys = Keys(originalSource); merged.insert(sourceKeys.begin(), sourceKeys.end());
        if (pair.merge)
            Require(Keys(originalSource).contains(ToUpperInvariant(target.itemKeys.back())) &&
                Keys(originalTarget).contains(ToUpperInvariant(target.itemKeys.back())),
                "D05 deduplication fixture must actually offer the shared member from both components");
        if (sourceForm != 0)
        {
            const auto captured = Access::BeginWidgetEntrance(app, L"source", sourceForm == 3 ? L"origin-group" : L"", sourceForm == 2);
            Require(captured.entries.size() == 1, "native pair must commit the complete captured single entrance");
        }
        Require(Access::CommitWidgetPair(app, L"source", L"target", pair.merge, sourceForm != 0),
            "D05/B08: specified native component pair action must commit");
        Access::FinishGroups(app);
        const auto check = [&] {
            Require(Configuration(Widget(app, L"unrelated")) == originalSentinel,
                "pair commit must preserve unrelated component settings, membership and placement");
            if (pair.merge)
            {
                Require(!FindWidget(app, L"source") && Configuration(Widget(app, L"target"), true, false) == Configuration(originalTarget, true, false),
                    "D05: remove only the source component and preserve every persisted target setting");
                const auto& current = Widget(app, L"target");
                Require(Keys(current) == merged && current.itemKeys.size() == merged.size(),
                    "D05: merge the original union exactly once per member, without losing target or source members");
                Require(Owners(app, L"source").empty(), "D05: removed source cannot remain as a dangling child");
            }
            else
            {
                CheckChild(app, originalSource); CheckChild(app, originalTarget);
                const auto sourceOwners = Owners(app, L"source"), targetOwners = Owners(app, L"target");
                Require(sourceOwners.size() == 1 && sourceOwners == targetOwners && sourceOwners.front() != L"origin-group",
                    "Ctrl grouping must give both original components exactly one common new owner");
                const auto& group = Widget(app, sourceOwners.front());
                Require(group.type == (pair.source == collection ? Type::CollectionGroup : Type::FileGroup) &&
                    group.dissolveWhenSingle && group.childWidgetIds.size() == 2,
                    "Ctrl grouping must create the specified group type with both original identities");
            }
            if (sourceForm == 3)
                Require(Widget(app, L"origin-group").childWidgetIds == std::vector<std::wstring>{L"passive"},
                    "D05: extracting a label must detach exactly that source and retain the manual origin group");
            samples.Check(); CheckRuntime(app);
        };
        check(); Require(Access::Save(app), "save actual pair commit through host storage");
        native_drag_async_reload_cases::Refresh(app); check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " actual native pair commit, configuration, ownership and storage scenes passed\n";
}

inline void RunLifecycle()
{
    Profile profile;
    std::size_t scenes = 0;
    for (const auto childType : {collection, files, mapping})
    for (const bool automatic : {false, true})
    for (const std::size_t remaining : {0u, 1u, 2u})
    for (const bool targetIcon : {false, true})
    {
        context = "A22 child=" + std::to_string(static_cast<int>(childType)) + " automatic=" + std::to_string(automatic) +
            " remaining=" + std::to_string(remaining) + " targetIcon=" + std::to_string(targetIcon);
        Samples samples(profile, scenes++);
        const std::size_t count = remaining == 2 ? 3u : 2u, movingCount = remaining == 0 ? 2u : 1u;
        std::vector<DesktopWidget> children;
        std::vector<std::wstring> sourceIds, moving, kept;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto id = L"source-" + std::to_wstring(i);
            auto child = Component(id, childType, 0, 0, i % 2 == 0);
            samples.Add(child, L"one"); children.push_back(child); sourceIds.push_back(id);
            (i < movingCount ? moving : kept).push_back(id);
        }
        std::vector<std::wstring> targetIds{L"target-0", L"target-1"};
        for (const auto& id : targetIds)
        {
            auto child = Component(id, childType, 6, 0, true);
            samples.Add(child, L"one"); children.push_back(child);
        }
        auto widgets = children;
        widgets.push_back(Group(L"origin-group", childType, sourceIds, 0, 0, automatic));
        widgets.push_back(Group(L"target-group", childType, targetIds, 6, 0, false, targetIcon));
        auto unrelated = Component(L"unrelated", collection, 0, 4, true);
        samples.Add(unrelated, L"one"); widgets.push_back(unrelated);
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, std::move(widgets), samples.ordinary);
        Access::PrepareReload(app); Access::BindAsync(app);
        // Capture baseline after native normalization, before the action.
        for (auto& child : children) child = Widget(app, child.id);
        const auto targetSettings = Configuration(Widget(app, L"target-group"));
        const auto unrelatedSettings = Configuration(Widget(app, L"unrelated"));
        const auto captured = Access::BeginGroupLabels(app, L"origin-group", moving);
        Require(captured.entries.size() == moving.size() && captured.hasOriginWidget && captured.originWidgetId == L"origin-group",
            "A22: native label capture must retain the whole selection and exact source group");
        Access::DropGroupLabels(app, L"target-group");
        Access::FinishGroups(app);
        Require(FindWidget(app, L"origin-group") != nullptr,
            "D06: source wrapper must survive until the active captured drag context has ended");
        Access::EndActualGesture(app); Access::FinishGroups(app);
        const auto check = [&] {
            const auto* origin = FindWidget(app, L"origin-group");
            Require(static_cast<bool>(origin) == (!automatic || remaining > 1),
                "D06: drag-created zero/one-child groups disappear; manual groups and larger groups remain");
            if (origin) Require(origin->childWidgetIds == kept,
                "D06: retained source group must preserve exactly the nonmoving children and their relative order");
            const auto& target = Widget(app, L"target-group");
            auto targetConfiguration = target;
            targetConfiguration.childWidgetIds = targetIds;
            Require(Configuration(targetConfiguration) == targetSettings,
                "D06: transfer cannot alter target group settings, placement or active child");
            Require(target.childWidgetIds.size() == targetIds.size() + moving.size() &&
                std::equal(targetIds.begin(), targetIds.end(), target.childWidgetIds.begin()),
                "D06: transfer must retain the original target children in their relative order");
            for (const auto& child : children)
            {
                CheckChild(app, child);
                const bool transferred = std::find(moving.begin(), moving.end(), child.id) != moving.end();
                const bool originallyTarget = std::find(targetIds.begin(), targetIds.end(), child.id) != targetIds.end();
                const auto owners = Owners(app, child.id);
                const std::vector<std::wstring> expected = transferred || originallyTarget ? std::vector<std::wstring>{L"target-group"}
                    : origin ? std::vector<std::wstring>{L"origin-group"} : std::vector<std::wstring>{};
                Require(owners == expected, "D06: transferred, retained and dissolved children must each have exactly their specified owner");
            }
            Require(Configuration(Widget(app, L"unrelated")) == unrelatedSettings,
                "D06: lifecycle transition cannot change unrelated component data or placement");
            samples.Check(); CheckRuntime(app);
        };
        check(); Require(Access::Save(app), "save native label transfer and finished lifecycle");
        native_drag_async_reload_cases::Refresh(app); check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); Access::FinishGroups(app); check();
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " actual native group-label transfer, deferred lifecycle and storage scenes passed\n";
}
}
