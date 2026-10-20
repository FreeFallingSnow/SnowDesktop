#pragma once

#include "layout/folder_sort_rules.h"

namespace native_member_order_cases
{
using namespace native_ole_file_cases;
namespace sort = snowdesktop::folder_sort_rules;
using Column = snowdesktop::list_detail_rules::Column;

// D02 was supplied by the user before these cases were written. Each row is
// an independently specified order, never the output of a production comparer.
struct SortCase
{
    int field; bool ascending; Column column;
    const wchar_t *sorted, *singleFront, *singleEnd, *multiFront, *multiEnd;
};
inline constexpr SortCase sorts[]{
    {sort::kName, true, Column::Name, L"ABCD", L"BACD", L"ACDB", L"BDAC", L"ACBD"},
    {sort::kName, false, Column::Name, L"DCBA", L"CDBA", L"DBAC", L"CADB", L"DBCA"},
    {sort::kSize, true, Column::Size, L"ACDB", L"CADB", L"ADBC", L"CBAD", L"ADCB"},
    {sort::kSize, false, Column::Size, L"BDCA", L"DBCA", L"BCAD", L"DABC", L"BCDA"},
    {sort::kModified, true, Column::Modified, L"ACDB", L"CADB", L"ADBC", L"CBAD", L"ADCB"},
    {sort::kModified, false, Column::Modified, L"BDCA", L"DBCA", L"BCAD", L"DABC", L"BCDA"},
};
inline std::wstring Names(DesktopApp& app)
{
    std::wstring result;
    auto* container = Access::MemberContainer(app, L"ordered");
    for (const auto& slot : container->GetSlots())
    {
        const auto* item = slot->GetItem();
        if (!item) continue;
        const auto name = fs::path(item->GetPath()).filename().wstring();
        Require(name.size() == 5 && name.substr(1) == L".txt", "native member slots must expose actual file identities");
        result += name[0];
    }
    return result;
}
inline const DesktopWidget& Data(const DesktopApp& app)
{
    Require(Access::Widgets(app).size() == 1, "native member operations preserve the single container identity");
    return Access::Widgets(app).front();
}
inline void Check(DesktopApp& app, std::wstring_view expected, const SortCase& sorted, bool manual, bool list)
{
    const auto& widget = Data(app);
    Require(widget.id == L"ordered" && widget.listMode == list && !widget.autoCollect &&
        widget.gridCell.pageId == L"__page:1" && widget.gridSpan.columns == 4 && widget.gridSpan.rows == 3,
        "manual ordering preserves container identity, view mode, placement and unrelated collection policy");
    Require(widget.contentSortColumn == (manual ? Column::None : sorted.column) &&
        (widget.type != DesktopWidgetType::FolderMapping || widget.folderSortMode == (manual ? sort::kManual : sorted.field)),
        "D02: accepted manual member drag must leave automatic sorting; cancellation or rejection must preserve it");
    Require(Names(app) == expected, "D02: native displayed member order must equal the independently specified drag result");
}
inline void Run(std::string_view typeName)
{
    DesktopWidgetType type;
    if (typeName == "collection") type = DesktopWidgetType::Collection;
    else if (typeName == "files") type = DesktopWidgetType::FileCategories;
    else if (typeName == "mapping") type = DesktopWidgetType::FolderMapping;
    else throw std::runtime_error("unknown native member-order type");
    native_drag_commit_cases::Profile profile;
    std::size_t scenes = 0, accepted = 0, cancelled = 0, rejected = 0;
    for (const auto& scenario : sorts)
    for (const bool list : {false, true})
    for (const int sequence : {0, 1, 2, 3, 4, 5})
    {
        // Four accepted single/multiple front/end drags, cancellation, and
        // the existing date-header prohibition where applicable (U11).
        if (sequence == 5 && type == DesktopWidgetType::Collection) continue;
        native_drag_commit_cases::context = "member type=" + std::string(typeName) + " sort=" +
            std::to_string(scenario.field) + " ascending=" + std::to_string(scenario.ascending) +
            " list=" + std::to_string(list) + " sequence=" + std::to_string(sequence);
        const auto directory = profile.root / (L"members-" + std::to_wstring(scenes++));
        fs::create_directories(directory);
        const auto source = type == DesktopWidgetType::FolderMapping ? directory / L"mapped"
            : snowdesktop::debug_profile::Current().configuration.desktop;
        fs::create_directories(source);
        const std::array<std::string, 4> contents{"a", "bbbb", "cc", "ddd"};
        const std::array<ULONGLONG, 4> times{1, 4, 2, 3};
        std::vector<fs::path> files;
        for (std::size_t i = 0; i < 4; ++i)
        {
            files.push_back(source / (std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L".txt"));
            { std::ofstream stream(files.back(), std::ios::binary); stream << contents[i]; }
            const HANDLE file = CreateFileW(files.back().c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            Require(file != INVALID_HANDLE_VALUE, "open isolated member fixture to set known sort metadata");
            ULARGE_INTEGER stamp; stamp.QuadPart = 133000000000000000ULL + times[i] * 10000000ULL;
            const FILETIME value{stamp.LowPart, stamp.HighPart};
            const bool changed = SetFileTime(file, nullptr, nullptr, &value) != FALSE;
            CloseHandle(file);
            Require(changed, "stamp known independent native sort times");
        }
        auto& app = profile.Model(false, directory / L"layout");
        Access::SeedMemberModel(app, files, type, list);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        Access::SortMembers(app, scenario.field, scenario.ascending);
        Check(app, scenario.sorted, scenario, false, list);
        const auto before = Read(Access::LayoutPath(app));
        if (sequence == 5) Access::DateHeaders(app, true);
        const bool multi = sequence == 2 || sequence == 3;
        const bool atEnd = sequence == 1 || sequence == 3;
        std::vector<std::wstring> selected{std::wstring(1, scenario.sorted[1]) + L".txt"};
        if (multi) selected.push_back(std::wstring(1, scenario.sorted[3]) + L".txt");
        const auto captured = Access::BeginMembers(app, selected);
        Require(captured.entries.size() == selected.size(), "native member capture cannot prefilter the supplied selection");
        const auto preview = Access::MemberPreview(app, atEnd);
        if (sequence == 4)
        {
            Access::EndActualGesture(app); ++cancelled;
            Require(Read(Access::LayoutPath(app)) == before, "cancelled native manual drag cannot write a new layout");
        }
        else if (sequence == 5)
        {
            Require(preview.Empty(), "U11: date-grouped native members cannot advertise an accepted manual reorder");
            Access::DropMembers(app, false); ++rejected;
            Access::DateHeaders(app, false);
        }
        else
        {
            Require(!preview.Empty() && !preview.fileBacked && preview.action == DropAction::Move &&
                preview.insertIndex == (atEnd ? 4u : 0u), "native member release must use the intended real insertion boundary");
            Access::DropMembers(app, atEnd); ++accepted;
        }
        const wchar_t* expected = sequence >= 4 ? scenario.sorted : multi
            ? (atEnd ? scenario.multiEnd : scenario.multiFront) : (atEnd ? scenario.singleEnd : scenario.singleFront);
        Check(app, expected, scenario, sequence < 4, list);
        Require(Access::Save(app), "save actual native drag result through the host persistence boundary");
        native_drag_async_reload_cases::Refresh(app);
        Check(app, expected, scenario, sequence < 4, list);
        Access::Load(app);
        native_drag_async_reload_cases::Refresh(app, true);
        Check(app, expected, scenario, sequence < 4, list);
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            Require(GetFileAttributesExW(files[i].c_str(), GetFileExInfoStandard, &attributes) && Read(files[i]) == contents[i],
                "manual member ordering preserves every real file and its bytes");
            ULARGE_INTEGER stamp; stamp.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
            stamp.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
            Require(stamp.QuadPart == 133000000000000000ULL + times[i] * 10000000ULL,
                "manual ordering cannot rewrite source file modification times");
        }
        Require(Access::NoPresentationWindows(app), "native member tests cannot create desktop presentation windows");
        Access::StopModel(app);
        for (const auto& file : files) fs::remove(file);
    }
    std::cout << scenes << " actual native member-order scenes passed: " << typeName << "; " << accepted
        << " accepted, " << cancelled << " cancelled, " << rejected << " date-blocked\n";
}
}
