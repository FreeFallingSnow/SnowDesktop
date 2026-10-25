#pragma once

namespace native_group_surface_cases
{
using namespace native_widget_pair_cases;

// U06/B09 literal boundaries. These rows are independent of ReorderItems and
// Apply; original order is A B C D, active child C. The last row cancels.
struct LabelMove
{
    std::vector<std::wstring> selected;
    const wchar_t* boundary;
    bool after, cancel;
    std::vector<std::wstring> expected;
    std::size_t insert;
};
inline const LabelMove labelMoves[]{
    {{L"B", L"D"}, L"A", false, false, {L"B", L"D", L"A", L"C"}, 0},
    {{L"B", L"D"}, L"D", true, false, {L"A", L"C", L"B", L"D"}, 4},
    {{L"B"}, L"C", false, false, {L"A", L"B", L"C", L"D"}, 2},
    {{L"C"}, L"B", false, false, {L"A", L"C", L"B", L"D"}, 1},
    {{L"B", L"D"}, L"C", false, false, {L"A", L"B", L"D", L"C"}, 2},
    {{L"B", L"D"}, L"A", false, true, {L"A", L"B", L"C", L"D"}, 0},
};
inline std::vector<DesktopWidget> SurfaceWidgets(Samples& samples, bool fileGroup, bool list, bool search,
    bool mappedInactive = true)
{
    std::vector<DesktopWidget> result;
    for (int index = 0; index < 4; ++index)
    {
        const auto id = std::wstring(1, static_cast<wchar_t>(L'A' + index));
        const auto type = !fileGroup ? collection : index % 2 && mappedInactive ? mapping : files;
        auto child = Component(id, type, 0, 0, index % 2 != 0);
        child.title = child.customTitle = L"Source " + id;
        samples.Add(child, search && index < 2 ? L"needle" : L"keep");
        result.push_back(std::move(child));
    }
    auto group = Group(L"surface-group", fileGroup ? files : collection, {L"A", L"B", L"C", L"D"}, 0, 0, false);
    group.listMode = list; group.showSearchBox = search; group.activeCategoryId = search ? L"A" : L"C";
    result.push_back(std::move(group));
    auto sentinel = Component(L"untouched", collection, 6, 4, true);
    samples.Add(sentinel, L"keep"); result.push_back(std::move(sentinel));
    return result;
}
inline void CheckSurface(DesktopApp& app, const std::vector<DesktopWidget>& original,
    const std::vector<std::wstring>& order)
{
    Require(Access::Widgets(app).size() == original.size(), "label/search operations preserve every native component identity");
    for (const auto& widget : original)
        if (widget.id == L"surface-group")
        {
            auto expected = widget; expected.childWidgetIds = order;
            Require(Configuration(native_widget_pair_cases::Widget(app, widget.id)) == Configuration(expected),
                "U06: label reorder changes only the specified order and preserves active child and group settings");
        }
        else CheckChild(app, widget);
    CheckRuntime(app);
}
inline std::set<std::wstring> VisiblePaths(DesktopApp& app)
{
    std::set<std::wstring> result;
    for (const auto& slot : Access::MemberContainer(app, L"surface-group")->GetSlots())
        if (auto* item = slot->GetItem())
            Require(result.insert(ToUpperInvariant(item->GetPath())).second,
                "native search cannot publish the same physical member twice");
    return result;
}
inline void RunLabels()
{
    Profile profile;
    std::size_t scenes = 0;
    for (const bool fileGroup : {false, true})
    for (const bool list : {false, true})
    for (const auto& move : labelMoves)
    {
        context = "A25 labels file-group=" + std::to_string(fileGroup) + " list=" + std::to_string(list) +
            " sequence=" + std::to_string(&move - labelMoves);
        Samples samples(profile, scenes++);
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, SurfaceWidgets(samples, fileGroup, list, false), samples.ordinary);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto original = Access::Widgets(app);
        Require(Access::Save(app), "save native label model before the gesture");
        const auto saved = Read(Access::LayoutPath(app));
        const auto captured = Access::BeginGroupLabels(app, L"surface-group", move.selected);
        Require(captured.entries.size() == move.selected.size(), "native label capture retains the complete supplied selection");
        for (std::size_t index = 0; index < move.selected.size(); ++index)
        {
            // Mapped labels use Widget metadata because they can also leave
            // the group as a component. The actual entrance still has to be
            // the real label wrapper; its metadata enum is not a UI contract.
            const auto* fileLabel = dynamic_cast<FileGroupEntryItem*>(captured.entries[index].item);
            const auto* collectionLabel = dynamic_cast<CollectionGroupEntryItem*>(captured.entries[index].item);
            Require(captured.entries[index].widgetId == move.selected[index] && captured.entries[index].sourceIndex == index &&
                (fileGroup ? fileLabel && fileLabel->GetChildWidgetId() == move.selected[index]
                    : collectionLabel && collectionLabel->GetCollectionId() == move.selected[index]),
                "native labels retain their actual original child identities, label wrappers and source order");
        }
        // The same horizontal insertion must survive title, member content and
        // bottom-edge hits. Hover is presentation only, including cancellation.
        for (int zone = 0; zone < 4; ++zone)
        {
            const auto projected = Access::GroupSurfaceLabelPreview(app, L"surface-group", move.boundary, move.after, zone);
            Require(!projected.Empty() && projected.insertIndex == move.insert,
                "own label drag uses tab insertion across the complete source component");
            Require(Access::GroupSurfaceLabelHint(app, L"surface-group").empty(),
                "same-group sorting has live position feedback without a floating text hint");
            const auto visual = Access::GroupSurfaceTabLayout(app, L"surface-group");
            std::vector<std::wstring> order, placeholders;
            LONG previousRight = LONG_MIN;
            for (const auto& tab : visual.tabs)
            {
                order.push_back(tab.id);
                if (tab.placeholder) placeholders.push_back(tab.id);
                Require(tab.bounds.left >= previousRight && tab.bounds.right > tab.bounds.left,
                    "live tab layout makes nonoverlapping space for every label");
                previousRight = tab.bounds.right;
            }
            // A cancelled gesture still previews the requested order.
            const auto expected = move.cancel ? std::vector<std::wstring>{L"B", L"D", L"A", L"C"} : move.expected;
            Require(visual.active && order == expected && placeholders == move.selected,
                "live preview shows the independently specified final tab order and full selection");
            CheckSurface(app, original, {L"A", L"B", L"C", L"D"});
            Require(Read(Access::LayoutPath(app)) == saved, "live tab preview never persists an intermediate order");
        }
        const auto preview = Access::GroupSurfaceLabelPreview(app, L"surface-group", move.boundary, move.after, 1);
        Require(!preview.Empty() && !preview.fileBacked && preview.action == DropAction::Move && preview.targetWidget &&
            preview.targetWidget->id == L"surface-group" && preview.insertIndex == move.insert &&
            preview.landings.size() == move.selected.size(),
            "native tab hit and preview publish the independently specified group insertion edge");
        if (move.cancel)
        {
            Access::EndActualGesture(app);
            Require(Read(Access::LayoutPath(app)) == saved, "cancelled label drag cannot save a new order");
            Require(!Access::GroupSurfaceTabLayout(app, L"surface-group").active,
                "cancelled gesture immediately retires the live insertion layout");
        }
        else Access::DropGroupSurfaceLabels(app, L"surface-group");
        CheckSurface(app, original, move.expected); samples.Check();
        Require(Access::Save(app), "save native label result through actual host storage");
        native_drag_async_reload_cases::Refresh(app);
        CheckSurface(app, original, move.expected);
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
        CheckSurface(app, original, move.expected); samples.Check();
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " native same-group label hit, reorder, cancellation and storage scenes passed\n";
}
inline void RunInsertionZones()
{
    Profile profile;
    std::size_t scenes = 0;
    for (const bool fileGroup : {false, true})
    for (const bool list : {false, true})
    for (int zone = 0; zone < 4; ++zone)
    {
        context = "label insertion file-group=" + std::to_string(fileGroup) + " list=" + std::to_string(list) +
            " zone=" + std::to_string(zone);
        Samples samples(profile, scenes++);
        auto widgets = SurfaceWidgets(samples, fileGroup, list, false);
        for (const auto* id : {L"E", L"F"})
        {
            auto child = Component(id, fileGroup ? (id[0] == L'E' ? mapping : files) : collection, 6, 0, false);
            samples.Add(child, L"incoming"); widgets.push_back(std::move(child));
        }
        widgets.push_back(Group(L"source-group", fileGroup ? files : collection, {L"E", L"F"}, 6, 0, false));
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, std::move(widgets), samples.ordinary);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto original = Access::Widgets(app);
        Require(Access::Save(app), "save cross-group baseline before hovering");
        const auto saved = Read(Access::LayoutPath(app));
        Access::BeginGroupLabels(app, L"source-group", {L"E"});
        auto* target = Access::MemberContainer(app, L"surface-group");
        const RECT a = Access::GroupSurfaceTabRect(app, L"surface-group", L"A");
        const RECT b = Access::GroupSurfaceTabRect(app, L"surface-group", L"B");
        const RECT frame = target->GetFrameRect();
        POINT point{b.left + 1, (b.top + b.bottom) / 2};
        if (zone == 0) point.x = (a.right + b.left) / 2; // Actual gap, not a label hit.
        if (zone == 1) point.y = b.top - target->Cu(3.0f);
        if (zone == 2) point.y = b.bottom + target->Cu(3.0f);
        if (zone == 3) point.y = frame.bottom - 1;
        const auto preview = Access::GroupSurfacePointPreview(app, L"surface-group", point);
        Require(!preview.Empty() && preview.insertIndex == (zone == 3 ? 4u : 1u),
            "another component retains body append but its tab gaps and nearby strip publish exact insertion");
        const auto visual = Access::GroupSurfaceTabLayout(app, L"surface-group");
        const std::vector<std::wstring> expected = zone == 3
            ? std::vector<std::wstring>{L"A", L"B", L"C", L"D", L"E"}
            : std::vector<std::wstring>{L"A", L"E", L"B", L"C", L"D"};
        Require(visual.active == (zone != 3),
            "cross-group body keeps membership-box feedback; only the expanded tab strip uses live insertion");
        Require(Access::GroupSurfaceLabelHint(app, L"surface-group") == (zone == 3
                ? _LW(fileGroup ? "widget.file_group.add_hint" : "widget.collection_group.add_hint") : L""),
            "only the yellow cross-group membership box publishes a floating text hint; tab insertion remains silent");
        if (visual.active)
        {
            std::vector<std::wstring> order;
            for (const auto& tab : visual.tabs) order.push_back(tab.id);
            Require(order == expected && visual.tabs[1].placeholder,
                "incoming label occupies the actual previewed gap and displaces later labels");
        }
        Require(Read(Access::LayoutPath(app)) == saved, "cross-group hover cannot persist membership or ordering");
        Access::LeaveGroupSurface(app);
        Require(!Access::GroupSurfaceTabLayout(app, L"surface-group").active,
            "leaving the insertion strip retires its displaced layout without a model mutation");
        Access::GroupSurfacePointPreview(app, L"surface-group", point);
        Access::DropGroupSurfaceLabels(app, L"surface-group");
        const auto check = [&] {
            for (auto widget : original)
            {
                if (widget.id == L"surface-group") widget.childWidgetIds = expected;
                if (widget.id == L"source-group") { widget.childWidgetIds = {L"F"}; widget.activeCategoryId = L"F"; }
                Require(Configuration(native_widget_pair_cases::Widget(app, widget.id)) == Configuration(widget),
                    "cross-group drop changes only expected membership/order and preserves component data and identity");
            }
            samples.Check(); CheckRuntime(app);
        };
        check(); Require(Access::Save(app), "save cross-group insertion through host storage");
        native_drag_async_reload_cases::Refresh(app); check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " native cross-group insertion zones, live layout and storage scenes passed\n";
}
inline void RunSearch()
{
    // Product preview help is an actual menu consumer: collection-group search
    // filters only the active collection; file-group search combines sources.
    Profile profile;
    std::size_t scenes = 0;
    for (const bool fileGroup : {false, true})
    for (const bool list : {false, true})
    {
        context = "A26 search file-group=" + std::to_string(fileGroup) + " list=" + std::to_string(list);
        Samples samples(profile, scenes++);
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, SurfaceWidgets(samples, fileGroup, list, true), samples.ordinary);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto original = Access::Widgets(app);
        Require(Access::Save(app), "save native search source before its cancelled gesture");
        const auto saved = Read(Access::LayoutPath(app));
        std::vector<fs::path> selected;
        std::set<std::wstring> expected;
        for (const auto& file : samples.all)
            if (file.path.filename().wstring().find(L"needle") != std::wstring::npos &&
                (fileGroup || file.path.filename().wstring().find(L"-A-") != std::wstring::npos))
            { selected.push_back(file.path); expected.insert(ToUpperInvariant(file.path.wstring())); }
        Require(expected.size() == (fileGroup ? 2u : 1u), "independent search fixture has its declared active/cross-source matches");
        Access::GroupSurfaceSearch(app, L"surface-group", L"needle");
        Require(VisiblePaths(app) == expected,
            "product search scope must match the declared active-collection or all-file-sources contract");
        const auto captured = Access::BeginGroupSurfaceSearch(app, L"surface-group", selected);
        std::set<std::wstring> capturedPaths;
        for (const auto& entry : captured.entries) capturedPaths.insert(ToUpperInvariant(entry.filePath));
        Require(captured.entries.size() == expected.size() && capturedPaths == expected,
            "native search selection captures every actual matching file under its real source identity");
        Access::EndActualGesture(app);
        Require(Read(Access::LayoutPath(app)) == saved, "cancelled search-result drag cannot persist membership or order");
        Access::GroupSurfaceSearch(app, L"surface-group", L"no-native-member-matches-this-query");
        Require(VisiblePaths(app).empty(), "a zero-match query must not expose hidden unrelated native members");
        Access::GroupSurfaceSearch(app, L"surface-group", L"");
        std::set<std::wstring> active;
        for (const auto& file : samples.all)
            if (file.path.filename().wstring().find(L"-A-") != std::wstring::npos) active.insert(ToUpperInvariant(file.path.wstring()));
        Require(VisiblePaths(app) == active, "clearing group search restores the same active source, not all source members");
        CheckSurface(app, original, {L"A", L"B", L"C", L"D"}); samples.Check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
        Require(VisiblePaths(app) == active, "actual disk reload after cancelled search retains active-source membership");
        CheckSurface(app, original, {L"A", L"B", L"C", L"D"}); samples.Check();
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " native group search scope, real capture, cancellation and storage scenes passed\n";
}
inline void RunDateHeaders()
{
    Profile profile;
    std::size_t scenes = 0;
    for (const auto type : {files, mapping})
    for (const bool list : {false, true})
    for (const bool multiple : {false, true})
    {
        context = "A27 date headers type=" + std::to_string(static_cast<int>(type)) + " list=" + std::to_string(list) +
            " multiple=" + std::to_string(multiple);
        const auto root = profile.root / (L"date-headers-" + std::to_wstring(scenes++));
        const auto source = type == mapping ? root / L"mapped" : snowdesktop::debug_profile::Current().configuration.desktop;
        fs::create_directories(source);
        std::vector<fs::path> members;
        for (const wchar_t name : std::wstring_view(L"ABCD"))
        {
            members.push_back(source / (std::wstring(1, name) + L".txt"));
            std::ofstream stream(members.back(), std::ios::binary); stream << static_cast<char>(name);
        }
        auto& app = profile.Model(false, root / L"layout");
        Access::SeedGroupedMembers(app, members, list, type);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        Access::SortGroupedMembers(app, native_member_order_cases::sort::kName, true);
        Access::GroupSurfaceDateHeaders(app, true);
        Require(Access::Save(app), "save date-grouped native baseline before the prohibited gesture");
        const auto original = Access::Widgets(app);
        std::vector<std::wstring> selected{L"B.txt"};
        if (multiple) selected.push_back(L"D.txt");
        const auto captured = Access::BeginGroupedMembers(app, selected);
        Require(captured.entries.size() == selected.size(), "date-header refusal must receive the complete actual selection");
        Require(Access::GroupedMemberPreview(app, true).Empty(), "U11: hosted date-grouped members must not advertise manual reorder");
        Require(Access::GroupSurfaceHint(app, true) == _LW("widget.desktop.sort_after_date"),
            "U11: hosted date-grouped refusal must explain that date headers need to be disabled");
        Access::DropGroupedMembers(app, true);
        const auto check = [&] {
            Require(Access::Widgets(app).size() == original.size(), "date-header refusal preserves all native components");
            for (const auto& widget : original) CheckChild(app, widget);
            Require(native_widget_pair_cases::Widget(app, L"ordered-group").dateHeaders && native_widget_pair_cases::Widget(app, L"ordered").contentSortColumn ==
                snowdesktop::list_detail_rules::Column::Name,
                "U11/D02: rejected hosted member drag preserves date headers and automatic sorting");
            for (std::size_t index = 0; index < members.size(); ++index)
                Require(Read(members[index]) == std::string(1, static_cast<char>(L'A' + index)),
                    "date-header refusal preserves every physical source and byte");
        };
        check();
        Require(Access::Save(app), "save rejected date-header state through actual host storage");
        native_drag_async_reload_cases::Refresh(app); check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Require(Access::NoIngressPopup(app), "group surface tests cannot create desktop presentation windows");
        Access::StopModel(app);
        if (type != mapping) for (const auto& member : members) fs::remove(member);
    }
    std::cout << scenes << " native file-group date-header rejection and storage scenes passed\n";
}
inline void RunExternalInactiveTab()
{
    Profile profile;
    std::size_t scenes = 0;
    for (const auto targetType : {collection, files, mapping})
    {
        context = "D07 actual external inactive tab type=" + std::to_string(static_cast<int>(targetType));
        Samples samples(profile, scenes++);
        auto widgets = SurfaceWidgets(samples, targetType != collection, false, false, targetType == mapping);
        samples.Add(widgets[1], L"second"); samples.Add(widgets[1], L"third");
        widgets[4].activeCategoryId = L"A";
        const std::vector<fs::path> sources{samples.root / L"External-One.txt", samples.root / L"External-Two.txt"};
        for (std::size_t i = 0; i < sources.size(); ++i)
        { std::ofstream stream(sources[i], std::ios::binary); stream << "external-inactive-" << i; }
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, std::move(widgets), samples.ordinary);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto original = Access::Widgets(app);
        const auto originalB = native_widget_pair_cases::Widget(app, L"B");
        Require(Access::Save(app), "save original external inactive-tab model before actual OLE input");
        const auto saved = Read(Access::LayoutPath(app));
        const RECT frame = Access::GroupSurfaceTabRect(app, L"surface-group", L"B");
        Require(!IsRectEmpty(&frame), "external D07 input requires an actual visible inactive native tab");
        const POINT point{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        auto source = ole_file_source_fixture::Make(sources, true);
        auto adapter = Access::ExternalAdapter(app);
        DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), MK_LBUTTON | MK_CONTROL, Access::ExternalPoint(point), &effect)),
            "external inactive input enters through the actual native OLE adapter");
        Access::FreshExternalDwell(app); effect = 7;
        Require(SUCCEEDED(adapter->DragOver(MK_LBUTTON | MK_CONTROL, Access::ExternalPoint(point), &effect)) && effect == DROPEFFECT_COPY &&
            Access::Target(app) == Access::MemberContainer(app, L"surface-group"),
            "external inactive hover routes to the requested real group and advertises explicit Copy");
        const auto binding = Access::GroupSurfaceExternalBinding(app, point);
        Require(binding.first == L"B" && binding.second == 3 && native_widget_pair_cases::Widget(app, L"surface-group").activeCategoryId == L"A" &&
            source->starts == 0 && source->ends == 0 && source->dataReads == 0 && Read(Access::LayoutPath(app)) == saved,
            "D07: external inactive preview binds the hit B and its real tail without early activation, extraction or save");
        Access::FreshExternalDwell(app); effect = 7;
        Require(SUCCEEDED(adapter->Drop(source.Get(), MK_CONTROL, Access::ExternalPoint(point), &effect)) && effect == DROPEFFECT_COPY && source->starts == 1,
            "external inactive Copy acknowledges the chosen effect and acquires one real asynchronous source lease");
        native_ole_file_cases::Wait([&] { return (targetType == mapping ? Access::FolderDropReadIdle(app) : Access::FileDropIdle(app)) && source->ends == 1; },
            "external inactive delivery waits for actual source/file completion and native refresh");
        Require(!source->wrongApartment && !source->fileReadBeforeStart && source->dataReads > 0 && SUCCEEDED(source->outcome) && source->endEffect == DROPEFFECT_COPY,
            "external inactive input extracts on the source STA after StartOperation and completes Copy exactly once");
        const auto destination = targetType == mapping ? fs::path(originalB.sourceFolderPath) : samples.desktop;
        std::vector<fs::path> outputs;
        for (const auto& path : sources) outputs.push_back(destination / path.filename());
        const auto check = [&] {
            Require(Access::Widgets(app).size() == original.size(), "external inactive acceptance preserves every original component");
            for (const auto& before : original)
                if (before.id == L"surface-group")
                {
                    auto expected = before; expected.activeCategoryId = L"B";
                    Require(Configuration(native_widget_pair_cases::Widget(app, before.id)) == Configuration(expected),
                        "D07: accepted external inactive-tab drop activates exactly the hit child and preserves all other group settings");
                }
                else if (before.id != L"B") CheckChild(app, before);
            Require(Configuration(native_widget_pair_cases::Widget(app, L"B"), true, false) == Configuration(originalB, true, false),
                "external inactive Copy preserves all target settings apart from accepted membership");
            std::vector<fs::path> expected;
            if (targetType == mapping) for (const auto& entry : originalB.folderEntries) expected.emplace_back(entry.fullPath);
            else for (const auto& key : originalB.itemKeys) expected.emplace_back(key);
            expected.insert(expected.end(), outputs.begin(), outputs.end());
            std::vector<fs::path> observed, displayed;
            const auto& current = native_widget_pair_cases::Widget(app, L"B");
            if (targetType == mapping) for (const auto& entry : current.folderEntries) observed.emplace_back(entry.fullPath);
            else for (const auto& key : current.itemKeys) observed.emplace_back(key);
            for (const auto& slot : Access::MemberContainer(app, L"surface-group")->GetSlots())
                if (const auto* item = slot->GetItem()) displayed.emplace_back(item->GetPath());
            const auto sameOrder = [&](const auto& actual) {
                return actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin(), expected.end(),
                    [](const auto& a, const auto& b) { return fs::equivalent(a, b); });
            };
            Require(sameOrder(observed) && sameOrder(displayed),
                "D07: actual external acceptance activates hit B and appends both copied files after all old members in source order");
            for (std::size_t i = 0; i < sources.size(); ++i)
                Require(Read(sources[i]) == "external-inactive-" + std::to_string(i) &&
                    Read(outputs[i]) == "external-inactive-" + std::to_string(i) && !fs::equivalent(sources[i], outputs[i]),
                    "explicit external Copy retains both source files and creates distinct real output identities with unchanged bytes");
            for (const auto& file : samples.all)
            {
                const auto now = Samples::Identity(file.path);
                Require(Read(file.path) == "original:" + WideToUtf8(file.path.filename().wstring()) && now.dwVolumeSerialNumber == file.identity.dwVolumeSerialNumber &&
                    now.nFileIndexHigh == file.identity.nFileIndexHigh && now.nFileIndexLow == file.identity.nFileIndexLow &&
                    CompareFileTime(&now.ftLastWriteTime, &file.identity.ftLastWriteTime) == 0,
                    "external inactive acceptance preserves every pre-existing real member's bytes, identity and time");
            }
            Require(Access::NoIngressPopup(app), "external inactive model tests cannot create desktop presentation windows");
        };
        check(); Require(Access::Save(app), "save external inactive acceptance and active-tab result");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); source.Reset();
        for (const auto& output : outputs) fs::remove(output);
        samples.Check(); samples.Cleanup();
    }
    std::cout << scenes << " actual external inactive-tab routing, Copy, activation, tail append and storage scenes passed\n";
}
inline void RunInactiveTab()
{
    // D07 is the user's independently supplied policy: receive in the tab
    // actually hit, activate it, append when there is no content insert mark.
    Profile profile;
    std::size_t scenes = 0;
    for (const auto targetType : {collection, files, mapping})
    for (const bool list : {false, true})
    for (const bool multiple : {false, true})
    {
        context = "D07 inactive tab type=" + std::to_string(static_cast<int>(targetType)) +
            " list=" + std::to_string(list) + " multiple=" + std::to_string(multiple);
        Samples samples(profile, scenes++);
        auto widgets = SurfaceWidgets(samples, targetType != collection, list, false, targetType == mapping);
        auto& destination = widgets[1];
        samples.Add(destination, L"second"); samples.Add(destination, L"third");
        widgets[4].activeCategoryId = L"A";
        auto incoming = Component(L"incoming", files, 10, 6, false);
        const auto firstIncoming = samples.all.size();
        samples.Add(incoming, L"one");
        if (multiple) samples.Add(incoming, L"two");
        std::vector<fs::path> selected;
        for (std::size_t index = firstIncoming; index < samples.all.size(); ++index) selected.push_back(samples.all[index].path);
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, std::move(widgets), samples.ordinary);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto original = Access::Widgets(app);
        const auto originalB = native_widget_pair_cases::Widget(app, L"B");
        const auto captured = Access::BeginGroupSurfaceIncoming(app, selected);
        Require(captured.entries.size() == selected.size(), "D07 receives the complete real desktop selection");
        const auto preview = Access::GroupSurfaceIncomingPreview(app);
        Require(!preview.Empty() && preview.targetWidget && preview.targetWidget->id == L"B" &&
            preview.landings.size() == selected.size() && preview.insertIndex == 3,
            "D07: inactive-tab preview must target the hit child and append after its three existing members");
        Require(native_widget_pair_cases::Widget(app, L"surface-group").activeCategoryId == L"A",
            "D07: observing the inactive tab before acceptance cannot activate it");
        Access::DropGroupSurfaceLabels(app, L"surface-group");
        native_ole_file_cases::Wait([&] { return (targetType == mapping ? Access::FolderDropReadIdle(app) : Access::FileDropIdle(app)); },
            "D07 native tab submission and real background file delivery must finish");
        const auto check = [&] {
            Require(Access::Widgets(app).size() == original.size(), "D07 preserves every original component identity");
            for (const auto& widget : original)
                if (widget.id == L"surface-group")
                {
                    auto expected = widget; expected.activeCategoryId = L"B";
                    Require(Configuration(native_widget_pair_cases::Widget(app, widget.id)) == Configuration(expected),
                        "D07: accepted inactive-tab drop activates exactly that tab without changing other group settings");
                }
                else if (widget.id != L"B") CheckChild(app, widget);
            Require(Configuration(native_widget_pair_cases::Widget(app, L"B"), true, false) == Configuration(originalB, true, false),
                "D07 preserves target settings apart from the explicitly accepted new members");
            std::vector<fs::path> expected;
            if (targetType == mapping)
            {
                for (const auto& entry : originalB.folderEntries) expected.emplace_back(entry.fullPath);
                for (const auto& path : selected) expected.push_back(fs::path(originalB.sourceFolderPath) / path.filename());
            }
            else
            {
                for (const auto& key : originalB.itemKeys) expected.emplace_back(key);
                expected.insert(expected.end(), selected.begin(), selected.end());
            }
            std::vector<fs::path> observed;
            const auto& currentB = native_widget_pair_cases::Widget(app, L"B");
            if (targetType == mapping) for (const auto& entry : currentB.folderEntries) observed.emplace_back(entry.fullPath);
            else for (const auto& key : currentB.itemKeys) observed.emplace_back(key);
            Require(observed.size() == expected.size() && std::equal(observed.begin(), observed.end(), expected.begin(), expected.end(),
                [](const auto& left, const auto& right) { return fs::equivalent(left, right); }),
                "D07: accepted files follow all existing target members in the captured order");
            std::vector<fs::path> displayed;
            for (const auto& slot : Access::MemberContainer(app, L"surface-group")->GetSlots())
                if (const auto* item = slot->GetItem()) displayed.emplace_back(item->GetPath());
            Require(displayed.size() == expected.size() && std::equal(displayed.begin(), displayed.end(), expected.begin(), expected.end(),
                [](const auto& left, const auto& right) { return fs::equivalent(left, right); }),
                "D07: the activated target must display its actual appended file order");
            for (std::size_t index = 0; index < samples.all.size(); ++index)
            {
                const auto& file = samples.all[index];
                const auto actual = targetType == mapping && index >= firstIncoming
                    ? fs::path(originalB.sourceFolderPath) / file.path.filename() : file.path;
                Require(Read(actual) == "original:" + WideToUtf8(file.path.filename().wstring()),
                    "D07 preserves every original or moved file's bytes");
                const auto identity = Samples::Identity(actual);
                Require(identity.dwVolumeSerialNumber == file.identity.dwVolumeSerialNumber &&
                    identity.nFileIndexHigh == file.identity.nFileIndexHigh && identity.nFileIndexLow == file.identity.nFileIndexLow &&
                    CompareFileTime(&identity.ftLastWriteTime, &file.identity.ftLastWriteTime) == 0,
                    "D07 moves retain physical file identity and modification time without producing copies");
                if (targetType == mapping && index >= firstIncoming)
                    Require(!fs::exists(file.path), "D07 mapped target Move removes only the accepted original desktop source");
            }
            Require(Access::NoIngressPopup(app), "D07 tab integration cannot create a desktop presentation window");
        };
        check();
        Require(Access::Save(app), "save D07's accepted target and active-tab result");
        native_drag_async_reload_cases::Refresh(app); check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " native inactive-tab preview, activation, tail append and storage scenes passed\n";
    RunExternalInactiveTab();
}
inline void Run(std::string_view mode)
{
    if (mode == "labels") { RunLabels(); RunInsertionZones(); }
    else if (mode == "search") RunSearch();
    else if (mode == "date") RunDateHeaders();
    else if (mode == "inactive") RunInactiveTab();
    else if (mode == "inactive-external") RunExternalInactiveTab();
    else throw std::runtime_error("unknown native group surface mode");
}
}
