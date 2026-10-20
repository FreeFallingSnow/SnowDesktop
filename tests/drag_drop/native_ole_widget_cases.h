#pragma once

#include "data/widget_layout_codec.h"
#include <sstream>

namespace native_ole_widget_cases
{
using namespace native_ole_file_cases;
struct Target
{
    std::string_view name;
    DesktopWidgetType root, member;
    bool grouped, mapped;
};
inline constexpr Target targets[]{
    {"collection", DesktopWidgetType::Collection, DesktopWidgetType::Collection, false, false},
    {"files", DesktopWidgetType::FileCategories, DesktopWidgetType::FileCategories, false, false},
    {"mapping", DesktopWidgetType::FolderMapping, DesktopWidgetType::FolderMapping, false, true},
    {"collection_group", DesktopWidgetType::CollectionGroup, DesktopWidgetType::Collection, true, false},
    {"file_group_files", DesktopWidgetType::FileGroup, DesktopWidgetType::FileCategories, true, false},
    {"file_group_mapping", DesktopWidgetType::FileGroup, DesktopWidgetType::FolderMapping, true, true},
};
inline const DesktopWidget& Widget(const DesktopApp& app, std::wstring_view id)
{
    const auto& all = Access::Widgets(app);
    const auto found = std::find_if(all.begin(), all.end(), [&](const auto& value) { return value.id == id; });
    Require(found != all.end(), "A18: expected stable native target identity must exist");
    return *found;
}
inline std::wstring MemberId(const Target& target) { return target.grouped ? L"ingress-active" : L"ingress-root"; }
inline std::string Configuration(const DesktopApp& app, const Target& target)
{
    std::vector<const DesktopWidget*> ordered;
    for (const auto& widget : Access::Widgets(app)) ordered.push_back(&widget);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) { return left->id < right->id; });
    std::ostringstream stream;
    for (const auto* widget : ordered)
    {
        auto saved = *widget;
        // Only destination membership/order may grow. Every other persisted
        // field, including inactive child membership, remains a strict check.
        if (saved.id == MemberId(target)) saved.itemKeys.clear();
        snowdesktop::widget_layout_codec::Write(stream, saved, JsonEscapeUtf8, WideToUtf8);
    }
    return stream.str();
}
inline void Check(DesktopApp& app, const Target& target, bool icon, const fs::path& original,
    const std::vector<fs::path>& outputs, const std::string& configuration)
{
    Require(Configuration(app, target) == configuration && Read(original) == "existing-target",
        "A18: root/active/inactive settings, identities and original member bytes must remain unchanged");
    const auto& root = Widget(app, L"ingress-root");
    Require(root.type == target.root && root.singleCellIconMode == icon && root.gridCell.pageId == L"__page:1" &&
        root.gridCell.column == 0 && root.gridCell.row == 0 && root.gridSpan.columns == (icon ? 1 : 4) &&
        root.gridSpan.rows == (icon ? 1 : 3) && !IsRectEmpty(&root.bounds) &&
        Access::MemberContainer(app, L"ingress-root")->IsSingleCellIconMode() == icon,
        "A18: actual root representation and original page/placement must survive the ingress");
    const auto& member = Widget(app, MemberId(target));
    std::vector<fs::path> current;
    if (target.mapped)
        for (const auto& entry : member.folderEntries) current.emplace_back(entry.fullPath);
    else
        for (const auto& key : member.itemKeys) current.emplace_back(key);
    auto expected = outputs; expected.push_back(original);
    Require(current.size() == expected.size(), "A18: active native source must contain exactly the old and accepted physical identities");
    for (const auto& path : expected)
    {
        Require(std::count_if(current.begin(), current.end(), [&](const auto& value) { return fs::equivalent(value, path); }) == 1,
            "A18: every accepted output must belong once to the intended active source");
        if (!target.mapped)
        {
            const auto& item = Find(app, ToUpperInvariant(path.wstring()));
            Require(!Access::HasVisibleIcon(app, item) && item.gridCell.pageId == root.gridCell.pageId &&
                item.gridCell.column == root.gridCell.column && item.gridCell.row == root.gridCell.row,
                "A18: logical target members must resolve to the intended root without free-desktop duplicates");
        }
    }
    if (!icon)
    {
        // The expanded input explicitly releases on empty space after the
        // only old member. Preserve its position, then append the captured
        // file/directory batch in source order, irrespective of name/type.
        auto ordered = outputs;
        ordered.insert(ordered.begin(), original);
        Require(std::equal(current.begin(), current.end(), ordered.begin(), ordered.end(),
            [](const auto& left, const auto& right) { return fs::equivalent(left, right); }),
            "A18: empty-tail ingress must preserve the old member and captured source order");
        std::vector<fs::path> accessible;
        for (const auto& slot : Access::MemberContainer(app, L"ingress-root")->GetSlots())
            if (const auto* item = slot->GetItem(); item && !item->GetPath().empty()) accessible.emplace_back(item->GetPath());
        Require(accessible.size() == expected.size() && std::all_of(expected.begin(), expected.end(), [&](const auto& path) {
            return std::count_if(accessible.begin(), accessible.end(), [&](const auto& value) { return fs::equivalent(value, path); }) == 1; }),
            "A18: expanded target runtime slots must expose the actual accepted member identities");
        Require(std::equal(accessible.begin(), accessible.end(), ordered.begin(), ordered.end(),
            [](const auto& left, const auto& right) { return fs::equivalent(left, right); }),
            "A18: expanded runtime slots must display the independently specified empty-tail batch order");
    }
    Require(Access::NoIngressPopup(app), "A18: native ingress must stay on the owned message-only window without a popup");
}
inline void Run(std::string_view targetName, std::string_view verb)
{
    const auto selectedTarget = std::find_if(std::begin(targets), std::end(targets),
        [&](const auto& value) { return value.name == targetName; });
    Require(selectedTarget != std::end(targets), "A18: an independently declared destination is required");
    const auto& target = *selectedTarget;
    const bool right = verb.starts_with("right_");
    const Gesture* gesture = nullptr;
    const native_right_drag_cases::Decision* decision = nullptr;
    if (right)
    {
        const auto name = verb.substr(6);
        const auto found = std::find_if(std::begin(native_right_drag_cases::decisions), std::end(native_right_drag_cases::decisions),
            [&](const auto& value) { return value.name == name; });
        Require(found != std::end(native_right_drag_cases::decisions), "A18: declared right-menu decision required");
        decision = &*found;
    }
    else
    {
        const auto found = std::find_if(std::begin(gestures), std::end(gestures), [&](const auto& value) { return value.name == verb; });
        Require(found != std::end(gestures), "A18: declared left-drag gesture required");
        gesture = &*found;
    }
    const DWORD chosen = right ? decision->effect : gesture->name == "move" ? 2u : gesture->name == "copy" ? 1u : 4u;
    const DWORD modifiers = right ? decision->modifiers : gesture->keys;
    const std::array<DWORD, 4> capabilities = chosen ? std::array<DWORD, 4>{7, chosen, 0, 7u & ~chosen}
        : std::array<DWORD, 4>{7, 0, 1, 4};
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    std::size_t scenes = 0;
    for (const bool icon : {false, true})
    for (const bool asynchronous : {false, true})
    for (const std::size_t count : {1u, 2u})
    for (const bool rebuild : {false, true})
    for (const DWORD allowed : capabilities)
    {
        if (!right && rebuild) continue;
        native_drag_commit_cases::context = "A18 target=" + std::string(targetName) + " verb=" + std::string(verb) +
            " icon=" + std::to_string(icon) + " async=" + std::to_string(asynchronous) + " count=" +
            std::to_string(count) + " allowed=" + std::to_string(allowed) + " rebuild=" + std::to_string(rebuild);
        const auto directory = profile.root / (L"widget-ole-" + std::to_wstring(scenes++));
        fs::create_directories(directory / L"source"); fs::create_directories(directory / L"mapped");
        const auto original = (target.mapped ? directory / L"mapped" : desktop) / L"Existing-Target.txt";
        { std::ofstream stream(original, std::ios::binary); stream << "existing-target"; }
        std::vector<fs::path> files{directory / L"source/Alpha-Payload.txt"};
        { std::ofstream stream(files[0], std::ios::binary); stream << "ingress-file"; }
        if (count == 2)
        {
            files.push_back(directory / L"source/Bundle"); fs::create_directory(files.back());
            std::ofstream stream(files.back() / L"nested.txt", std::ios::binary); stream << "ingress-directory";
        }
        auto& app = profile.Model(false, directory / L"layout");
        Access::SeedExternalStorage(app, target.root, target.member, icon, original);
        Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app), "A18: save real native destination baseline");
        const auto saved = Read(Access::LayoutPath(app));
        const auto configuration = Configuration(app, target);
        const auto destination = target.mapped ? original.parent_path() : desktop;
        Check(app, target, icon, original, {}, configuration);
        auto source = ole_file_source_fixture::Make(files, asynchronous);
        int menus = 0;
        Access::RightMenuBridge bridge(app, [&](HMENU menu, POINT, HWND owner) {
            Require(decision != nullptr, "A18: left-button ingress must not invoke a right-drag menu");
            ++menus;
            Require(owner == Access::completionWindow && Access::RightMenuRetained(app) && source->starts == 0 &&
                source->ends == 0 && (!asynchronous || source->dataReads == 0) &&
                Read(Access::LayoutPath(app)) == saved && Outputs(destination, {original}).empty(),
                "A18: actual menu decision must retain target context without early lease, file or layout submission");
            native_right_drag_cases::Menu(menu, allowed);
            if (rebuild) Access::RebuildDuringMenu(app);
            Require(Access::RightMenuRetained(app) && Access::NoIngressPopup(app),
                "A18: real menu model rebuild must retain the pending decision without a popup");
            return decision->command;
        });
        auto adapter = right ? ComPtr<OleDragDropAdapter>{} : Access::ExternalAdapter(app);
        if (right) adapter.Attach(new OleDragDropAdapter(&bridge));
        const POINT point = icon ? POINT{50, 50} : POINT{350, 250};
        const DWORD keys = right ? MK_RBUTTON : MK_LBUTTON | modifiers;
        DWORD feedback = allowed;
        Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), keys, Access::ExternalPoint(point), &feedback)),
            "A18: actual adapter Enter must reach native destination routing");
        Access::FreshExternalDwell(app); feedback = allowed;
        Require(SUCCEEDED(adapter->DragOver(keys, Access::ExternalPoint(point), &feedback)),
            "A18: actual adapter Over must reach native destination routing");
        const DWORD expected = right ? chosen & allowed : gesture->effects[allowed];
        if (!right) Require(feedback == expected, "A18: native target feedback must acknowledge only the independently selected supported verb");
        else Require((feedback & ~allowed) == 0, "A18: right-drag feedback cannot advertise an unoffered source effect");
        if (expected)
            Require(Access::Target(app) == Access::MemberContainer(app, L"ingress-root"),
                "A18: real OLE hit routing must select the requested native root, not free desktop or another source");
        Require(source->starts == 0 && source->ends == 0 && (!asynchronous || source->dataReads == 0) &&
            Configuration(app, target) == configuration && Read(Access::LayoutPath(app)) == saved,
            "A18: hover cannot render async source files, acquire a lease, mutate configuration or save layout");
        Access::FreshExternalDwell(app);
        DWORD returned = allowed;
        Require(SUCCEEDED(adapter->Drop(source.Get(), right ? MK_CONTROL : modifiers, Access::ExternalPoint(point), &returned)),
            "A18: actual adapter Drop must return through native host execution");
        if (right) Require(bridge.dropKeys == (MK_RBUTTON | MK_CONTROL) && menus == (allowed ? 1 : 0),
            "A18: initiating right-button ownership and the actual menu choice must reach the host once");
        if (expected && asynchronous)
            Require(returned == expected && source->starts == 1,
                "A18: accepted asynchronous native target must acknowledge the chosen effect and one source lease before returning");
        else if (expected && chosen != 2)
            Require(returned == expected, "A18: synchronous Copy/Link must acknowledge the actual chosen verb");
        if (!expected)
            Require(returned == 0 && source->starts == 0 && source->ends == 0 && Read(Access::LayoutPath(app)) == saved,
                "A18: unsupported, cancelled or dismissed ingress must not lease, submit or alter saved layout");
        Wait([&] { return (target.mapped ? Access::FolderDropReadIdle(app) : Access::FileDropIdle(app)) &&
                (!expected || !asynchronous || source->ends == 1); },
            "A18: native source/read/file completion and refresh must reach idle");
        const auto outputs = Outputs(destination, {original});
        Require(outputs.size() == (expected ? count : 0), "A18: actual destination must receive exactly the selected file outputs");
        std::vector<fs::path> orderedOutputs;
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            Require(fs::exists(files[i]) == (expected != 2), "A18: only selected Move may remove original source files");
            if (expected != 2) Require(Read(i ? files[i] / L"nested.txt" : files[i]) == (i ? "ingress-directory" : "ingress-file"),
                "A18: Copy/Link/cancellation must preserve source file and nested-directory bytes");
            if (!expected) continue;
            const auto output = std::find_if(outputs.begin(), outputs.end(), [&](const auto& path) {
                if (expected == 4)
                {
                    Require(_wcsicmp(path.extension().c_str(), L".lnk") == 0, "A18: Link must produce actual persisted Shell shortcuts");
                    return fs::equivalent(LinkTarget(path), files[i]);
                }
                return path.filename() == files[i].filename();
            });
            Require(output != outputs.end(), "A18: actual output must correspond to the selected physical source identity");
            orderedOutputs.push_back(*output);
            if (expected != 4) Require(Read(i ? *output / L"nested.txt" : *output) == (i ? "ingress-directory" : "ingress-file"),
                "A18: actual Move/Copy output must preserve file and nested-directory content");
        }
        Require(!source->wrongApartment && !source->fileReadBeforeStart,
            "A18: actual asynchronous source extraction must retain its STA and lease-before-read ordering");
        if (expected && asynchronous) Require(source->ends == 1 && SUCCEEDED(source->outcome) &&
            source->endEffect == (expected == 2 ? 0u : expected), "A18: exactly one successful source lease must report correct optimized-move ownership");
        if (target.mapped) Require(Outputs(desktop, {}).empty(), "A18: mapped ingress cannot accidentally materialize files on free desktop");
        Check(app, target, icon, original, orderedOutputs, configuration);
        if (expected)
        {
            Require(Access::Save(app), "A18: save accepted actual native target result");
            Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
            Check(app, target, icon, original, orderedOutputs, configuration);
        }
        Access::StopModel(app); source.Reset();
        for (const auto& output : outputs)
        {
            Require(fs::weakly_canonical(output.parent_path()) == fs::weakly_canonical(destination),
                "only isolated native ingress destination outputs may be removed");
            fs::remove_all(output);
        }
        fs::remove(original);
    }
    std::cout << scenes << " actual native external-widget file/menu/model/storage scenes passed: " << targetName << ' ' << verb << '\n';
}
}
