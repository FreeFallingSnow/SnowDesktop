#pragma once

#include "placement_cases.h"
#include "widgets/folder_member_order.h"
#include "widgets/widget_pair_drop.h"

namespace member_order_cases
{
using namespace drop_placement_cases;

inline void Run(const std::filesystem::path& root)
{
    const auto directory = root / L"member-order";
    std::filesystem::create_directory(directory);
    const std::array<std::wstring, 4> names{L"A.txt", L"B.txt", L"C.txt", L"D.txt"};
    const std::array<std::string, 4> contents{"a", "bbbb", "cc", "ddd"};
    const std::array<ULONGLONG, 4> seconds{1, 4, 2, 3};
    std::array<std::filesystem::path, 4> paths;
    for (size_t index = 0; index < names.size(); ++index)
    {
        paths[index] = directory / names[index];
        std::ofstream(paths[index]) << contents[index];
        const HANDLE file = CreateFileW(paths[index].c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(file != INVALID_HANDLE_VALUE, "open only the isolated fixture to set its sort metadata");
        ULARGE_INTEGER stamp; stamp.QuadPart = 133000000000000000ULL + seconds[index] * 10000000ULL;
        const FILETIME time{stamp.LowPart, stamp.HighPart};
        const bool stamped = SetFileTime(file, nullptr, nullptr, &time) != FALSE;
        CloseHandle(file);
        Require(stamped, "fixture metadata has distinct known modification times");
    }
    auto enumerate = [&] {
        std::vector<FolderEntry> entries;
        for (const auto& file : std::filesystem::directory_iterator(directory))
        {
            if (file.path().extension() != L".txt") continue;
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            Require(GetFileAttributesExW(file.path().c_str(), GetFileExInfoStandard, &attributes),
                "read actual filesystem metadata for sorting");
            FolderEntry entry;
            entry.name = file.path().filename().wstring(); entry.fullPath = file.path().wstring();
            entry.fileSize = (static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
            entry.lastWriteTime = attributes.ftLastWriteTime;
            entries.push_back(std::move(entry));
        }
        return entries;
    };
    auto namesOf = [](const auto& entries) {
        std::vector<std::wstring> result;
        for (const auto& entry : entries) result.push_back(entry.name);
        return result;
    };
    auto unchangedFiles = [&] {
        for (size_t index = 0; index < paths.size(); ++index)
        {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            Require(GetFileAttributesExW(paths[index].c_str(), GetFileExInfoStandard, &attributes),
                "logical reorder preserves every original path");
            ULARGE_INTEGER stamp; stamp.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
            stamp.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
            Require(Bytes(paths[index]) == contents[index] &&
                stamp.QuadPart == 133000000000000000ULL + seconds[index] * 10000000ULL,
                "member and sort operations preserve original file bytes and modification times");
        }
    };
    DesktopWidget widget; widget.id = L"mapped"; widget.type = DesktopWidgetType::FolderMapping;
    widget.sourceFolderPath = directory.wstring(); widget.gridCell = {L"screen-2", 3, 2};
    widget.folderSortMode = folder_sort_rules::kManual;
    widget.folderEntries = enumerate();
    std::sort(widget.folderEntries.begin(), widget.folderEntries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    Require(folder_member_order::Reorder(widget, {1, 3}, 0) &&
        namesOf(widget.folderEntries) == std::vector<std::wstring>{L"B.txt", L"D.txt", L"A.txt", L"C.txt"},
        "A04: dragging B and D before A yields B,D,A,C without changing their captured order");
    const auto saved = RoundTrip(root / L"member-order-layout.json", {widget});
    DesktopWidget reloaded; reloaded.type = DesktopWidgetType::FolderMapping;
    widget_layout_codec::RestoreMembers(saved.widgets[0], reloaded, Decode, Key);
    widget_layout_codec::RestoreSort(saved.widgets[0], reloaded);
    reloaded.folderEntries = enumerate();
    std::reverse(reloaded.folderEntries.begin(), reloaded.folderEntries.end());
    folder_member_order::Refresh(reloaded, Key);
    Require(namesOf(reloaded.folderEntries) == std::vector<std::wstring>{L"B.txt", L"D.txt", L"A.txt", L"C.txt"},
        "manual reorder survives real storage, reload and a differently ordered fresh enumeration");
    const auto before = widget.itemKeys;
    widget.dateHeaders = true;
    Require(!folder_member_order::Reorder(widget, {0}, 4) && widget.itemKeys == before,
        "date-grouped mapped folders reject manual member reorder without changing persisted order");
    widget.dateHeaders = false; widget.folderSortMode = folder_sort_rules::kName;
    Require(!folder_member_order::Reorder(widget, {}, 0) && widget.itemKeys == before &&
        widget.folderSortMode == folder_sort_rules::kName, "empty drag does not clear sorting or change member order");
    unchangedFiles();

    struct SortCase { int mode; bool ascending; std::array<const wchar_t*, 4> expected; };
    const SortCase sorts[]{
        {folder_sort_rules::kName, true, {L"A.txt", L"B.txt", L"C.txt", L"D.txt"}},
        {folder_sort_rules::kName, false, {L"D.txt", L"C.txt", L"B.txt", L"A.txt"}},
        {folder_sort_rules::kSize, true, {L"A.txt", L"C.txt", L"D.txt", L"B.txt"}},
        {folder_sort_rules::kSize, false, {L"B.txt", L"D.txt", L"C.txt", L"A.txt"}},
        {folder_sort_rules::kModified, true, {L"A.txt", L"C.txt", L"D.txt", L"B.txt"}},
        {folder_sort_rules::kModified, false, {L"B.txt", L"D.txt", L"C.txt", L"A.txt"}},
    };
    for (const auto& scenario : sorts)
    {
        widget.folderSortMode = scenario.mode; widget.folderSortAscending = scenario.ascending;
        widget.folderEntries = enumerate();
        folder_member_order::Refresh(widget, Key);
        const std::vector<std::wstring> expected(scenario.expected.begin(), scenario.expected.end());
        Require(namesOf(widget.folderEntries) == expected,
            "name, actual size and actual modified-time sorts follow the explicit direction");
        const auto document = RoundTrip(root / L"sort-layout.json", {widget});
        DesktopWidget restored; restored.type = DesktopWidgetType::FolderMapping;
        widget_layout_codec::RestoreMembers(document.widgets[0], restored, Decode, Key);
        widget_layout_codec::RestoreSort(document.widgets[0], restored);
        restored.folderEntries = enumerate();
        folder_member_order::Refresh(restored, Key);
        Require(namesOf(restored.folderEntries) == expected,
            "sorting field and direction survive save, reload and actual metadata refresh");
        unchangedFiles();
    }
    struct PairCase { DesktopWidgetType source, target; widget_pair_drop::Action action; DesktopWidgetType group; };
    const PairCase pairs[]{
        {DesktopWidgetType::Collection, DesktopWidgetType::Collection,
            widget_pair_drop::Action::CreateCollectionGroup, DesktopWidgetType::CollectionGroup},
        {DesktopWidgetType::FileCategories, DesktopWidgetType::FileCategories,
            widget_pair_drop::Action::CreateFileGroup, DesktopWidgetType::FileGroup},
        {DesktopWidgetType::FolderMapping, DesktopWidgetType::FolderMapping,
            widget_pair_drop::Action::CreateFileGroup, DesktopWidgetType::FileGroup},
        {DesktopWidgetType::FileCategories, DesktopWidgetType::FolderMapping,
            widget_pair_drop::Action::CreateFileGroup, DesktopWidgetType::FileGroup},
        {DesktopWidgetType::FolderMapping, DesktopWidgetType::FileCategories,
            widget_pair_drop::Action::CreateFileGroup, DesktopWidgetType::FileGroup},
    };
    size_t pairIndex = 0;
    for (const auto& scenario : pairs)
    {
        const auto pairRoot = root / (L"pair-" + std::to_wstring(pairIndex++));
        const auto firstFolder = pairRoot / L"first", secondFolder = pairRoot / L"second";
        std::filesystem::create_directories(firstFolder);
        std::filesystem::create_directory(secondFolder);
        const auto firstFile = firstFolder / L"x.txt", secondFile = secondFolder / L"y.txt";
        std::ofstream(firstFile) << "first component";
        std::ofstream(secondFile) << "second component";
        std::vector<DesktopWidget> widgets(2);
        widgets[0].id = L"C1"; widgets[0].type = scenario.source; widgets[0].gridCell = {L"left-screen", 1, 2};
        widgets[1].id = L"C2"; widgets[1].type = scenario.target; widgets[1].gridCell = {L"right-screen", 3, 4};
        widgets[0].itemKeys = {firstFile.wstring()};
        widgets[1].itemKeys = {secondFile.wstring()};
        widgets[0].privacyMode = true; widgets[1].listMode = true;
        if (scenario.source == DesktopWidgetType::FolderMapping) widgets[0].sourceFolderPath = firstFolder.wstring();
        if (scenario.target == DesktopWidgetType::FolderMapping) widgets[1].sourceFolderPath = secondFolder.wstring();
        const auto sourceBefore = widgets[0], targetBefore = widgets[1];
        DesktopWidget group; group.id = L"new-group"; group.gridCell = widgets[1].gridCell;
        std::vector<DockEntry> dock;
        const auto action = widget_pair_drop::ResolveAction(widget_pair_drop::GetOptions(
            scenario.source, scenario.target), true, false, false);
        Require(action == scenario.action && widget_pair_drop::Apply(widgets, dock, 0, 1, action, group, Key),
            "Ctrl pairing executes the production component model transaction for the declared type pair");
        Require(widgets.size() == 3 && widgets[0].id == L"C1" && widgets[1].id == L"C2" &&
            widgets[0].itemKeys == sourceBefore.itemKeys && widgets[1].itemKeys == targetBefore.itemKeys &&
            widgets[0].privacyMode == sourceBefore.privacyMode && widgets[1].listMode == targetBefore.listMode &&
            widgets[0].sourceFolderPath == sourceBefore.sourceFolderPath &&
            widgets[1].sourceFolderPath == targetBefore.sourceFolderPath,
            "A05: grouping retains original child identities, members and independent configuration");
        const auto& children = widgets[2].childWidgetIds;
        Require(widgets[2].type == scenario.group && children.size() == 2 &&
            std::find(children.begin(), children.end(), L"C1") != children.end() &&
            std::find(children.begin(), children.end(), L"C2") != children.end(),
            "the new group owns exactly the two original components, without prescribing an undefined tab order");
        CheckRoundTrip(root / L"group-layout.json", widgets);
        Require(Bytes(firstFile) == "first component" && Bytes(secondFile) == "second component" &&
            std::distance(std::filesystem::directory_iterator(firstFolder), std::filesystem::directory_iterator{}) == 1 &&
            std::distance(std::filesystem::directory_iterator(secondFolder), std::filesystem::directory_iterator{}) == 1,
            "grouping components never transfers, copies, links or deletes either child's physical file");
        unchangedFiles();
    }
    std::cout << "member reorder, metadata sort, grouping and storage cases passed\n";
}
}
