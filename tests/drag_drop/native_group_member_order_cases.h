#pragma once

namespace native_group_member_order_cases
{
using namespace native_member_order_cases;

inline void Check(DesktopApp& app, const SortCase& sorted, std::wstring_view expected, bool manual, bool list)
{
    Require(Access::Widgets(app).size() == 3, "active-group ordering preserves all three native component identities");
    const auto* group = Access::MemberContainer(app, L"ordered-group")->GetWidgetData();
    const auto* active = Access::MemberContainer(app, L"ordered")->GetWidgetData();
    const auto* other = Access::MemberContainer(app, L"other")->GetWidgetData();
    Require(group && active && other && group->type == DesktopWidgetType::CollectionGroup &&
        group->childWidgetIds == std::vector<std::wstring>{L"ordered", L"other"} && group->activeCategoryId == L"ordered" &&
        group->gridCell.pageId == L"__page:1" && group->gridSpan.columns == 4 && group->gridSpan.rows == 3 && group->listMode == list &&
        other->itemKeys.empty() && other->customTitle == L"Unchanged child" && other->listMode == !list &&
        other->contentSortColumn == Column::Size && !other->contentSortAscending,
        "D02: active-content drag cannot change tab order, active identity, group placement or the unrelated child configuration");
    Require(active->contentSortColumn == (manual ? Column::None : sorted.column),
        "D02: native group-proxy manual drag must leave the active child's automatic sorting");
    std::wstring names;
    for (const auto& slot : Access::MemberContainer(app, L"ordered-group")->GetSlots())
        if (const auto* item = slot->GetItem())
        {
            const auto name = fs::path(item->GetPath()).filename().wstring();
            Require(name.size() == 5 && name.substr(1) == L".txt", "group content slots must expose the original real file identities");
            names += name[0];
        }
    Require(names == expected, "D02: active-group content must retain its independently specified manual order");
}
inline void Run()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    const std::array<std::string, 4> contents{"a", "bbbb", "cc", "ddd"};
    const std::array<ULONGLONG, 4> times{1, 4, 2, 3};
    std::vector<fs::path> files;
    for (std::size_t i = 0; i < 4; ++i)
    {
        files.push_back(desktop / (std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L".txt"));
        { std::ofstream stream(files.back(), std::ios::binary); stream << contents[i]; }
        const HANDLE file = CreateFileW(files.back().c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(file != INVALID_HANDLE_VALUE, "open isolated group member fixture to set its sort metadata");
        ULARGE_INTEGER stamp; stamp.QuadPart = 133000000000000000ULL + times[i] * 10000000ULL;
        const FILETIME value{stamp.LowPart, stamp.HighPart};
        const bool changed = SetFileTime(file, nullptr, nullptr, &value) != FALSE;
        CloseHandle(file);
        Require(changed, "set distinct real group member modification times");
    }
    std::size_t scenes = 0;
    for (const auto& sorted : sorts)
    for (const bool list : {false, true})
    for (const int sequence : {0, 1, 2, 3, 4})
    {
        const auto description = "active-group sort=" + std::to_string(sorted.field) + " ascending=" +
            std::to_string(sorted.ascending) + " list=" + std::to_string(list) + " sequence=" + std::to_string(sequence);
        native_drag_commit_cases::context = description;
        auto& app = profile.Model(false, profile.root / (L"group-order-" + std::to_wstring(scenes++)));
        Access::SeedGroupedMembers(app, files, list);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        Access::SortGroupedMembers(app, sorted.field, sorted.ascending);
        Check(app, sorted, sorted.sorted, false, list);
        const auto saved = Read(Access::LayoutPath(app));
        const bool multi = sequence == 2 || sequence == 3;
        const bool atEnd = sequence == 1 || sequence == 3;
        std::vector<std::wstring> selected{std::wstring(1, sorted.sorted[1]) + L".txt"};
        if (multi) selected.push_back(std::wstring(1, sorted.sorted[3]) + L".txt");
        const auto captured = Access::BeginGroupedMembers(app, selected);
        Require(captured.entries.size() == selected.size() && std::all_of(captured.entries.begin(), captured.entries.end(),
            [](const auto& entry) { return entry.kind == DropSourceKind::DesktopIcon && entry.widgetId == L"ordered"; }),
            "native group capture must retain every selected file under the stable active-child identity");
        const auto preview = Access::GroupedMemberPreview(app, atEnd);
        Require(!preview.Empty() && !preview.fileBacked && preview.targetWidget && preview.targetWidget->id == L"ordered" &&
            preview.insertIndex == (atEnd ? 4u : 0u), "native group preview must target its active child and the given real insertion boundary");
        if (sequence == 4)
        {
            Access::EndActualGesture(app);
            Require(Read(Access::LayoutPath(app)) == saved, "cancelling active-group drag cannot persist a new order");
        }
        else Access::DropGroupedMembers(app, atEnd);
        const auto* expected = sequence == 4 ? sorted.sorted : multi
            ? (atEnd ? sorted.multiEnd : sorted.multiFront) : (atEnd ? sorted.singleEnd : sorted.singleFront);
        native_drag_commit_cases::context = description + " phase=after-drop";
        Check(app, sorted, expected, sequence != 4, list);
        Require(Access::Save(app), "save active-group manual result through actual host storage");
        native_drag_async_reload_cases::Refresh(app);
        native_drag_commit_cases::context = description + " phase=after-refresh";
        Check(app, sorted, expected, sequence != 4, list);
        Access::Load(app);
        native_drag_async_reload_cases::Refresh(app, true);
        native_drag_commit_cases::context = description + " phase=after-reload";
        Check(app, sorted, expected, sequence != 4, list);
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            Require(GetFileAttributesExW(files[i].c_str(), GetFileExInfoStandard, &attributes) && Read(files[i]) == contents[i],
                "active-group ordering cannot change any real member file or bytes");
            ULARGE_INTEGER stamp; stamp.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
            stamp.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
            Require(stamp.QuadPart == 133000000000000000ULL + times[i] * 10000000ULL,
                "active-group ordering must retain every source modification time");
        }
        Require(Access::NoPresentationWindows(app), "native active-group tests cannot create desktop presentation windows");
        Access::StopModel(app);
    }
    std::cout << scenes << " actual native active-group manual order, cancellation, metadata and storage scenes passed\n";
}
}
