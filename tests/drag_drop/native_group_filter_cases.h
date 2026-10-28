#pragma once

namespace native_group_filter_cases
{
using namespace native_drag_commit_cases;
namespace order = native_member_order_cases;
namespace pair = native_widget_pair_cases;
struct Surface { std::string_view name; DesktopWidgetType type; bool grouped; };
inline constexpr Surface surfaces[]{
    {"collection", DesktopWidgetType::Collection, false},
    {"files", DesktopWidgetType::FileCategories, false},
    {"mapping", DesktopWidgetType::FolderMapping, false},
    {"collection_group", DesktopWidgetType::Collection, true},
    {"file_group_files", DesktopWidgetType::FileCategories, true},
    {"file_group_mapping", DesktopWidgetType::FolderMapping, true},
};
inline const Surface& Find(std::string_view name)
{
    const auto found = std::find_if(std::begin(surfaces), std::end(surfaces), [&](const auto& value) { return value.name == name; });
    Require(found != std::end(surfaces), "native filter/type mode must name a declared surface");
    return *found;
}
inline std::wstring Displayed(DesktopApp& app, bool grouped)
{
    std::wstring result;
    for (const auto& slot : Access::MemberContainer(app, grouped ? L"ordered-group" : L"ordered")->GetSlots())
        if (const auto* item = slot->GetItem())
        {
            const auto name = fs::path(item->GetPath()).filename().wstring();
            Require(!name.empty(), "displayed native filter/type member must expose its original file name");
            result += name.front();
        }
    return result;
}
inline void CheckOrder(DesktopApp& app, const Surface& surface, std::wstring_view expected, bool manual,
    const std::vector<DesktopWidget>& original)
{
    Require(Access::Widgets(app).size() == original.size(), "filtered reorder preserves every actual native component");
    for (const auto& before : original)
    {
        const auto& after = pair::Widget(app, before.id);
        auto expectedConfig = before;
        if (before.id == L"ordered")
        {
            expectedConfig.itemKeys = after.itemKeys;
            if (manual)
            {
                expectedConfig.contentSortColumn = order::Column::None;
                if (surface.type == DesktopWidgetType::FolderMapping) expectedConfig.folderSortMode = order::sort::kManual;
            }
        }
        if (pair::Configuration(after) != pair::Configuration(expectedConfig))
            std::cerr << "filter config widget=" << WideToUtf8(before.id) << " expected=" << pair::Configuration(expectedConfig)
                << " actual=" << pair::Configuration(after) << '\n';
        Require(pair::Configuration(after) == pair::Configuration(expectedConfig),
            "D12/D02: only accepted member order and automatic-sort state may change; inactive child and group settings stay fixed");
    }
    std::wstring stored;
    for (const auto& key : pair::Widget(app, L"ordered").itemKeys)
    {
        const auto name = fs::path(key).filename().wstring();
        Require(!name.empty(), "stored native member must retain its real file identity"); stored += name.front();
    }
    Require(stored == expected, "D12: hidden nonmoving members keep their original relative order around the real insertion anchor");
}
inline void RunFiltered(std::string_view name)
{
    const auto& surface = Find(name);
    Require(surface.grouped || surface.type != DesktopWidgetType::Collection,
        "plain collection has no product search box; collection-group search is a distinct supported surface");
    Profile profile;
    std::size_t scenes = 0;
    for (const bool list : {false, true})
    for (const int sequence : {0, 1, 2, 3})
    {
        if (sequence == 3 && surface.type == DesktopWidgetType::Collection) continue;
        context = "D12 filtered surface=" + std::string(name) + " list=" + std::to_string(list) + " sequence=" + std::to_string(sequence);
        const auto root = profile.root / (L"filtered-" + std::to_wstring(scenes++));
        const auto source = surface.type == DesktopWidgetType::FolderMapping ? root / L"mapped"
            : snowdesktop::debug_profile::Current().configuration.desktop;
        fs::create_directories(source);
        std::vector<fs::path> members;
        for (int index = 0; index < 6; ++index)
        {
            members.push_back(source / (std::wstring(1, static_cast<wchar_t>(L'A' + index)) + (index % 2 ? L"-needle.txt" : L".txt")));
            std::ofstream stream(members.back(), std::ios::binary); stream << static_cast<char>('A' + index);
        }
        auto& app = profile.Model(false, root / L"layout");
        Access::SeedFilterSurface(app, members, surface.type, list, surface.grouped);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        if (surface.grouped) Access::SortGroupedMembers(app, order::sort::kName, true);
        else Access::SortMembers(app, order::sort::kName, true);
        if (sequence == 3) Access::FilterSurfaceDateHeaders(app, surface.grouped);
        Require(Access::Save(app), "save actual filtered source before its gesture");
        const auto original = Access::Widgets(app);
        const auto saved = Read(Access::LayoutPath(app));
        Access::FilterSurfaceQuery(app, surface.grouped, L"needle");
        auto visible = Displayed(app, surface.grouped);
        if (sequence == 3) std::sort(visible.begin(), visible.end());
        Require(visible == L"BDF", "D12 fixture must expose exactly the three real query matches");
        std::vector<std::wstring> selected{sequence == 1 ? L"D-needle.txt" : L"F-needle.txt"};
        if (sequence == 1) selected.push_back(L"F-needle.txt");
        const auto captured = surface.grouped ? Access::BeginGroupedMembers(app, selected) : Access::BeginMembers(app, selected);
        Require(captured.entries.size() == selected.size(), "filtered drag must capture the complete visible selection");
        const std::wstring anchor = sequence == 1 ? L"B-needle.txt" : L"D-needle.txt";
        const auto preview = Access::FilterSurfacePreview(app, surface.grouped, anchor);
        if (sequence == 2) Access::EndActualGesture(app);
        else if (sequence == 3)
        {
            Require(preview.Empty(), "D12: enabled date headers prohibit manual reorder even while search hides their visual rows");
            Access::DropFilterSurface(app, surface.grouped, anchor);
        }
        else
        {
            Require(!preview.Empty() && !preview.fileBacked && preview.targetWidget && preview.targetWidget->id == L"ordered" &&
                preview.insertIndex == (sequence == 1 ? 1u : 3u),
                "D12: filtered insertion mark must address the actual full-container member, not a filtered row number");
            Access::DropFilterSurface(app, surface.grouped, anchor);
        }
        if (sequence == 2) Require(Read(Access::LayoutPath(app)) == saved, "cancelled filtered drag cannot persist any order or sort change");
        Access::FilterSurfaceQuery(app, surface.grouped, L"");
        const auto expected = sequence >= 2 ? L"ABCDEF" : sequence == 1 ? L"ADFBCE" : L"ABCFDE";
        const auto check = [&] {
            CheckOrder(app, surface, expected, sequence < 2, original);
            auto displayed = Displayed(app, surface.grouped);
            // U11/D12 rejects this gesture. The precise date-group projection
            // order was not part of that decision; require the exact set and
            // unchanged stored order, without freezing a date comparator.
            if (sequence == 3) std::sort(displayed.begin(), displayed.end());
            Require(displayed == expected, "D12: clearing search retains all members and the specified accepted manual order");
            for (std::size_t index = 0; index < members.size(); ++index)
                Require(Read(members[index]) == std::string(1, static_cast<char>('A' + index)), "filtered reorder cannot alter or move the physical files");
            Require(Access::NoIngressPopup(app), "filtered native tests cannot create desktop presentation windows");
        };
        check(); Require(Access::Save(app), "save actual filtered manual or rejected result");
        native_drag_async_reload_cases::Refresh(app); check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app);
        if (surface.type != DesktopWidgetType::FolderMapping) for (const auto& member : members) fs::remove(member);
    }
    std::cout << scenes << " native filtered real-anchor, hidden-member, cancellation/date refusal and storage scenes passed: " << name << '\n';
}
inline void CreateTypeShortcut(const fs::path& path, const fs::path& target)
{
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persisted;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link.As(&persisted)) &&
        SUCCEEDED(persisted->Save(path.c_str(), TRUE)), "create a real folder shortcut for the Type sort contract");
}
inline void RunType(std::string_view name)
{
    const auto& surface = Find(name);
    Profile profile;
    std::size_t scenes = 0;
    for (const bool list : {false, true})
    for (const bool ascending : {true, false})
    {
        context = "D12 type surface=" + std::string(name) + " list=" + std::to_string(list) + " ascending=" + std::to_string(ascending);
        const auto root = profile.root / (L"type-" + std::to_wstring(scenes++));
        const auto source = surface.type == DesktopWidgetType::FolderMapping ? root / L"mapped"
            : snowdesktop::debug_profile::Current().configuration.desktop;
        fs::create_directories(source);
        std::vector<fs::path> members;
        for (int index = 0; index < 4; ++index)
        {
            members.push_back(source / (std::wstring(1, static_cast<wchar_t>(L'A' + index)) +
                (index % 2 ? L".snowdesktop_type_z" : L".snowdesktop_type_a")));
            std::ofstream stream(members.back(), std::ios::binary); stream << static_cast<char>('A' + index);
        }
        for (const auto* directory : {L"P", L"Q"})
        { members.push_back(source / directory); fs::create_directory(members.back()); }
        members.push_back(source / L"R.lnk"); CreateTypeShortcut(members[6], members[4]);
        members.push_back(source / L"S.lnk"); CreateTypeShortcut(members[7], members[5]);
        SHFILEINFOW typeA{}, typeZ{};
        Require(SHGetFileInfoW(members[0].c_str(), 0, &typeA, sizeof(typeA), SHGFI_TYPENAME) != 0 &&
            SHGetFileInfoW(members[1].c_str(), 0, &typeZ, sizeof(typeZ), SHGFI_TYPENAME) != 0 &&
            CompareStringOrdinal(typeA.szTypeName, -1, typeZ.szTypeName, -1, TRUE) == CSTR_LESS_THAN,
            "independent Shell fixture must expose distinct type-column names in the declared A/Z lexical order");
        auto& app = profile.Model(false, root / L"layout");
        Access::SeedFilterSurface(app, members, surface.type, list, surface.grouped);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        Access::ReadyGroupTypeMetadata(app);
        const auto before = Access::Widgets(app);
        if (surface.grouped) Access::SortGroupedMembers(app, order::sort::kType, ascending);
        else Access::SortMembers(app, order::sort::kType, ascending);
        const auto expected = ascending ? L"PQRSACBD" : L"DBCASRQP";
        const auto check = [&] {
            Require(Displayed(app, surface.grouped) == expected,
                "Type: folders precede folder shortcuts and other types; descending reverses categories and name ties");
            Require(pair::Widget(app, L"ordered").contentSortColumn == order::Column::Type &&
                pair::Widget(app, L"ordered").contentSortAscending == ascending,
                "actual native sort command must persist its chosen Type field and direction");
            for (const auto& widget : before)
                if (widget.id != L"ordered") pair::CheckChild(app, widget);
            for (std::size_t index = 0; index < 4; ++index)
                Require(Read(members[index]) == std::string(1, static_cast<char>('A' + index)), "explicit Type sort cannot alter physical member bytes");
            Require(fs::is_empty(members[4]) && fs::is_empty(members[5]) && Access::NoIngressPopup(app),
                "Type sort preserves both real directories and does not create desktop presentation windows");
            Require(fs::equivalent(native_ole_file_cases::LinkTarget(members[6]), members[4]) &&
                fs::equivalent(native_ole_file_cases::LinkTarget(members[7]), members[5]),
                "Type sorting preserves both real folder shortcut targets");
        };
        check(); Require(Access::Save(app), "save actual native Type sort");
        native_drag_async_reload_cases::Refresh(app); Access::ReadyGroupTypeMetadata(app); check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); Access::ReadyGroupTypeMetadata(app); check();
        const auto sorted = Access::Widgets(app);
        const std::vector<std::wstring> selected{L"C.snowdesktop_type_a"};
        const auto captured = surface.grouped ? Access::BeginGroupedMembers(app, selected) : Access::BeginMembers(app, selected);
        Require(captured.entries.size() == 1, "D02 Type drag captures the actual selected member without prefiltering");
        const auto preview = surface.grouped ? Access::GroupedMemberPreview(app, false) : Access::MemberPreview(app, false);
        Require(!preview.Empty() && !preview.fileBacked && preview.insertIndex == 0,
            "D02 Type manual drag must publish the given visible first-member insertion mark");
        if (surface.grouped) Access::DropGroupedMembers(app, false);
        else Access::DropMembers(app, false);
        const auto manualOrder = ascending ? L"CPQRSABD" : L"CDBASRQP";
        const auto checkManual = [&] {
            CheckOrder(app, surface, manualOrder, true, sorted);
            Require(Displayed(app, surface.grouped) == manualOrder,
                "D02 Type: accepted manual drag leaves automatic directory/type grouping and preserves the literal dragged order");
            for (std::size_t index = 0; index < 4; ++index)
                Require(Read(members[index]) == std::string(1, static_cast<char>('A' + index)), "Type-to-manual reorder preserves physical file bytes");
            Require(fs::is_empty(members[4]) && fs::is_empty(members[5]), "Type-to-manual reorder cannot move or fill the original directories");
        };
        checkManual(); Require(Access::Save(app), "save native manual result after Type ordering");
        native_drag_async_reload_cases::Refresh(app); Access::ReadyGroupTypeMetadata(app); checkManual();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); Access::ReadyGroupTypeMetadata(app); checkManual();
        if (surface.name == "collection")
        {
            const auto observed = Access::SortFreeType(app, ascending);
            Require(observed == (ascending ? L"TUPQRSACBD" : L"DBCASRQPUT"),
                "SortIcons must place system objects, folders, uncached real folder links and file types in full reversible grid order; observed=" +
                    WideToUtf8(observed));
        }
        Access::StopModel(app);
        if (surface.type != DesktopWidgetType::FolderMapping) for (const auto& member : members) fs::remove(member);
    }
    std::cout << scenes << " native Type folder/shortcut priority, full reversal and storage scenes passed: " << name << '\n';
}
inline void RunCategoryDwell()
{
    Profile profile;
    std::size_t scenes = 0;
    for (const auto type : {pair::files, pair::mapping})
    {
        context = "D12/S03 inner category target-source identity type=" + std::to_string(static_cast<int>(type));
        pair::Samples samples(profile, scenes++);
        std::vector<DesktopWidget> widgets;
        for (const auto* id : {L"A", L"B"})
        {
            auto child = pair::Component(id, type, 0, 0, false);
            samples.Add(child, L"keep"); widgets.push_back(std::move(child));
        }
        auto group = pair::Group(L"surface-group", pair::files, {L"A", L"B"}, 0, 0, false);
        group.gridSpan = {4, 4}; widgets.push_back(std::move(group));
        auto incoming = pair::Component(L"incoming", pair::files, 10, 6, false);
        samples.Add(incoming, L"keep");
        const std::vector<fs::path> selected{samples.all.back().path};
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, std::move(widgets), samples.ordinary);
        Access::ConfigureGroupCategoryContract(app);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto original = Access::Widgets(app);
        Require(Access::BeginGroupSurfaceIncoming(app, selected).entries.size() == 1,
            "inner-category dwell starts from an actual captured desktop file");
        const POINT point = Access::GroupCategoryContractPoint(app);
        const auto oldTick = Access::TrackGroupCategoryContract(app, point);
        Require(Access::GroupSurfaceDwellArmed(app) && !Access::ActivateGroupSurfaceDwell(app, oldTick),
            "a fresh actual inner category hit must arm and wait");
        Access::ReplaceGroupCategorySource(app, point);
        Require(!Access::ActivateGroupSurfaceDwell(app, oldTick + 10000) && !Access::GroupSurfaceDwellArmed(app) &&
            pair::Widget(app, L"A").activeCategoryId == L"all" && pair::Widget(app, L"B").activeCategoryId == L"all",
            "D12: replacing the hosted category target source cancels old dwell even when category ID and hit point are unchanged");
        const auto newTick = Access::TrackGroupCategoryContract(app, point);
        Require(!Access::ActivateGroupSurfaceDwell(app, newTick) && Access::ActivateGroupSurfaceDwell(app, newTick + 10000),
            "the replacement source can activate its own fresh real category dwell");
        Access::EndActualGesture(app);
        const auto check = [&] {
            for (auto expected : original)
            {
                if (expected.id == L"surface-group") expected.activeCategoryId = L"B";
                if (expected.id == L"B") expected.activeCategoryId = L"contract-text";
                pair::CheckChild(app, expected);
            }
            samples.Check(); Require(Access::NoIngressPopup(app), "category dwell cannot create a desktop presentation window");
        };
        check(); Require(Access::Save(app), "save accepted category on the actual replacement source");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " native inner category target-source identity and storage scenes passed\n";
}
inline void RunDwell()
{
    Profile profile;
    std::size_t scenes = 0;
    for (const bool fileGroup : {false, true})
    for (const bool list : {false, true})
    for (const int sequence : {0, 1, 2, 3, 4})
    {
        context = "D12/S03 dwell file-group=" + std::to_string(fileGroup) + " list=" + std::to_string(list) +
            " sequence=" + std::to_string(sequence);
        pair::Samples samples(profile, scenes++);
        auto widgets = native_group_surface_cases::SurfaceWidgets(samples, fileGroup, list, false);
        widgets[4].activeCategoryId = L"A";
        auto source = pair::Component(L"incoming", pair::files, 10, 6, false);
        samples.Add(source, L"keep");
        const std::vector<fs::path> selected{samples.all.back().path};
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, std::move(widgets), samples.ordinary);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto original = Access::Widgets(app);
        const auto captured = Access::BeginGroupSurfaceIncoming(app, selected);
        Require(captured.entries.size() == 1, "dwell starts with a real captured desktop file");
        const auto started = Access::TrackGroupSurfaceDwell(app, L"B");
        Require(Access::GroupSurfaceDwellArmed(app) && !Access::ActivateGroupSurfaceDwell(app, started) &&
            pair::Widget(app, L"surface-group").activeCategoryId == L"A",
            "S03: a newly entered tab must wait before automatic activation");
        std::wstring id = L"surface-group", expected = L"B";
        DWORD finalTick = started + 10000;
        if (sequence == 1) { Access::LeaveGroupSurfaceDwell(app); expected = L"A"; }
        else if (sequence == 2)
        {
            const auto changed = Access::TrackGroupSurfaceDwell(app, L"C");
            Require(!Access::ActivateGroupSurfaceDwell(app, changed), "S03: a newly targeted tab cannot reuse a preceding dwell immediately");
            finalTick = changed + 10000; expected = L"C";
        }
        else if (sequence == 3) { Access::EndActualGesture(app); expected = L"A"; }
        else if (sequence == 4) { Access::ReplaceGroupSurfaceIdentity(app); id = L"replacement-group"; expected = L"A"; }
        const bool switched = Access::ActivateGroupSurfaceDwell(app, finalTick);
        Require(switched == (sequence == 0 || sequence == 2) && !Access::GroupSurfaceDwellArmed(app),
            "D12: late dwell activates only the same live target; leaving, ending or replacing its identity cancels it");
        Require(pair::Widget(app, id).activeCategoryId == expected,
            "S03: automatic tab activation must correspond to the final live native tab identity");
        Access::EndActualGesture(app);
        const auto check = [&] {
            for (const auto& widget : original)
                if (widget.id == L"surface-group")
                {
                    auto config = widget; config.id = id; config.activeCategoryId = expected;
                    Require(pair::Configuration(pair::Widget(app, id)) == pair::Configuration(config),
                        "tab dwell changes only the intended active identity, keeping all other group settings and members");
                }
                else pair::CheckChild(app, widget);
            samples.Check(); Require(Access::NoIngressPopup(app), "native dwell must not create a desktop presentation window");
        };
        check(); Require(Access::Save(app), "save actual dwell or cancelled replacement state");
        native_drag_async_reload_cases::Refresh(app); check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " native group tab dwell, cancellation, retarget, identity and storage scenes passed\n";
    RunCategoryDwell();
}
inline void RunTypeSearch()
{
    // Cross-source equal display names are valid here: the desktop and mapped
    // directories contain separate real Twin files. Their original source
    // order must survive a Type/name tie; no duplicate identity is fabricated.
    Profile profile;
    std::size_t scenes = 0;
    for (const bool list : {false, true})
    for (const bool ascending : {true, false})
    {
        context = "D12 mixed search Type list=" + std::to_string(list) + " ascending=" + std::to_string(ascending);
        const auto root = profile.root / (L"search-type-" + std::to_wstring(scenes++));
        const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop, mapped = root / L"mapped";
        fs::create_directories(desktop); fs::create_directories(mapped);
        const auto a = desktop / L"A.needle.snowdesktop_type_a", c = desktop / L"C.needle.snowdesktop_type_a";
        const auto p = desktop / L"P.needle", ta = desktop / L"Twin.needle.snowdesktop_type_a";
        const auto b = mapped / L"B.needle.snowdesktop_type_z", d = mapped / L"D.needle.snowdesktop_type_z";
        const auto q = mapped / L"Q.needle", tb = mapped / L"Twin.needle.snowdesktop_type_a";
        const std::vector<fs::path> regular{a, c, ta, b, d, tb};
        for (std::size_t index = 0; index < regular.size(); ++index)
        { std::ofstream stream(regular[index], std::ios::binary); stream << "search-type-" << index; }
        fs::create_directory(p); fs::create_directory(q);
        const auto r = desktop / L"R.needle.lnk", s = mapped / L"S.needle.lnk";
        CreateTypeShortcut(r, p); CreateTypeShortcut(s, q);
        auto& app = profile.Model(false, root / L"layout");
        Access::SeedSearchTypeSurface(app, {a, c, p, ta, r}, mapped, list);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        Access::ReadyGroupTypeMetadata(app);
        const auto original = Access::Widgets(app);
        Access::FilterSurfaceQuery(app, true, L"needle");
        Access::SortGroupedMembers(app, order::sort::kType, ascending);
        const auto expected = ascending ? std::vector<fs::path>{p, q, r, s, a, c, ta, tb, b, d}
            : std::vector<fs::path>{d, b, ta, tb, c, a, s, r, q, p};
        const auto check = [&] {
            std::vector<fs::path> observed;
            for (const auto& slot : Access::MemberContainer(app, L"ordered-group")->GetSlots())
                if (const auto* item = slot->GetItem()) observed.emplace_back(item->GetPath());
            Require(observed.size() == expected.size() && std::equal(observed.begin(), observed.end(), expected.begin(), expected.end(),
                [](const auto& left, const auto& right) { return fs::equivalent(left, right); }),
                "mixed search Type sort orders folders, folder shortcuts and file types; reverses names and categories; keeps equal-key source order");
            for (const auto& widget : original)
                if (widget.id != L"ordered-group") pair::CheckChild(app, widget);
            const auto& group = pair::Widget(app, L"ordered-group");
            Require(group.activeCategoryId == L"ordered" && group.childWidgetIds == std::vector<std::wstring>{L"ordered", L"other"} &&
                group.contentSortColumn == order::Column::Type && group.contentSortAscending == ascending,
                "mixed search sort changes only group result sorting, preserving active source and source order");
            for (std::size_t index = 0; index < regular.size(); ++index)
                Require(Read(regular[index]) == "search-type-" + std::to_string(index), "mixed search sort preserves all six actual source file identities and bytes");
            Require(fs::is_empty(p) && fs::is_empty(q) && Access::NoIngressPopup(app), "mixed search sort keeps real directories and creates no presentation window");
            Require(fs::equivalent(native_ole_file_cases::LinkTarget(r), p) && fs::equivalent(native_ole_file_cases::LinkTarget(s), q),
                "mixed search Type sort preserves folder shortcut targets from both real sources");
        };
        check(); Require(Access::Save(app), "save actual mixed search Type settings");
        native_drag_async_reload_cases::Refresh(app);
        Access::ReadyGroupTypeMetadata(app);
        Access::FilterSurfaceQuery(app, true, L"needle"); check();
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
        Access::ReadyGroupTypeMetadata(app);
        Access::FilterSurfaceQuery(app, true, L"needle"); check();
        Access::StopModel(app); for (const auto& path : {a, c, r, p, ta}) fs::remove(path);
    }
    std::cout << scenes << " native mixed-source search Type and stable equal-name storage scenes passed\n";
}
}
