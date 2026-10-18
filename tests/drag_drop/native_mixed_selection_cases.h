#pragma once

#include "data/widget_layout_codec.h"
#include <sstream>

namespace native_mixed_selection_cases
{
using namespace native_ole_file_cases;

struct Components
{
    std::string configuration;
    std::vector<std::pair<std::wstring, std::vector<fs::path>>> members;
};
inline Components OriginalComponents(const DesktopApp& app)
{
    Components result;
    std::ostringstream stream;
    std::vector<const DesktopWidget*> originals;
    for (const auto& widget : Access::Widgets(app))
        if (widget.id != L"mixed-target") originals.push_back(&widget);
    std::sort(originals.begin(), originals.end(), [](const auto* left, const auto* right) { return left->id < right->id; });
    for (const auto* widget : originals)
    {
        std::vector<fs::path> members;
        for (const auto& key : widget->itemKeys) members.emplace_back(key);
        result.members.emplace_back(widget->id, std::move(members));
        auto configuration = *widget;
        // Persisted path spelling can be normalized on reload. Membership is
        // ordered physical identity, checked below with the real filesystem;
        // every other persisted configuration field remains a strict comparison.
        configuration.itemKeys.clear();
        snowdesktop::widget_layout_codec::Write(stream, configuration, JsonEscapeUtf8, WideToUtf8);
    }
    result.configuration = stream.str();
    return result;
}
inline void CheckComponents(const DesktopApp& app, const Components& original)
{
    const auto current = OriginalComponents(app);
    Require(current.configuration == original.configuration && current.members.size() == original.members.size(),
        "D01: an unsupported component, its children, members, placement and settings must remain unchanged");
    for (std::size_t i = 0; i < original.members.size(); ++i)
    {
        const auto& [id, members] = original.members[i];
        const auto& [currentId, currentMembers] = current.members[i];
        Require(currentId == id && currentMembers.size() == members.size(),
            "D01: unsupported components must retain every original member under its stable component identity");
        for (std::size_t j = 0; j < members.size(); ++j)
        {
            std::error_code error;
            const bool same = fs::equivalent(members[j], currentMembers[j], error);
            Require(!error && same,
                "D01: unsupported component membership must preserve the ordered physical file identities");
        }
    }
}
inline void Check(DesktopApp& app, const std::vector<fs::path>& files, std::size_t count,
    const Components& original, DesktopWidgetType targetType)
{
    CheckComponents(app, original);
    const auto* target = Access::MemberContainer(app, L"mixed-target")->GetWidgetData();
    Require(target && target->type == targetType && target->itemKeys.size() == count,
        "D01: the actual native target must receive every eligible selected file despite unsupported mixed members");
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        const auto key = ToUpperInvariant(files[i].wstring());
        const auto occurrences = std::count(target->itemKeys.begin(), target->itemKeys.end(), key);
        const auto& item = Find(app, key);
        Require(occurrences == (i < count ? 1 : 0) && Read(files[i]) == "mixed-file-" + std::to_string(i),
            "D01: logical file transfer preserves bytes and assigns only the supported selected identities once");
        if (i < count)
            Require(!Access::HasVisibleIcon(app, item) && item.gridCell.pageId == target->gridCell.pageId &&
                item.gridCell.column == target->gridCell.column && item.gridCell.row == target->gridCell.row,
                "D01: accepted files must belong to the intended container without a free-desktop duplicate");
        else
            Require(Access::HasVisibleIcon(app, item) && item.gridCell.pageId == L"__page:1" &&
                item.gridCell.column == static_cast<int>(i) * 2 && item.gridCell.row == 0,
                "D01: an unselected ordinary file must retain its original visible desktop cell");
    }
    std::unordered_set<std::wstring> visible;
    for (const auto& slot : Access::MemberContainer(app, L"mixed-target")->GetSlots())
        if (const auto* item = slot->GetItem()) visible.insert(ToUpperInvariant(item->GetPath()));
    Require(visible.size() == count && std::all_of(target->itemKeys.begin(), target->itemKeys.end(),
        [&](const auto& key) { return visible.contains(ToUpperInvariant(key)); }),
        "D01: eligible identities must be accessible through the real target member slots");
    Require(Access::NoPresentationWindows(app), "native mixed-selection tests cannot open desktop presentation windows");
}
inline void CheckFileAction(DesktopApp& app, const std::vector<fs::path>& files, std::size_t count,
    const std::vector<fs::path>& outputs, const Components& original, DesktopWidgetType targetType, bool link)
{
    CheckComponents(app, original);
    const auto* target = Access::MemberContainer(app, L"mixed-target")->GetWidgetData();
    Require(outputs.size() == count && target && target->type == targetType && target->itemKeys.size() == count,
        "D03: only eligible ordinary files may create physical outputs and target members");
    std::unordered_set<std::wstring> published;
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        const auto& source = Find(app, ToUpperInvariant(files[i].wstring()));
        Require(Read(files[i]) == "mixed-file-" + std::to_string(i) && Access::HasVisibleIcon(app, source) &&
            Access::HasMatchingRuntimeSlot(app, source) && source.gridCell.pageId == L"__page:1" &&
            source.gridCell.column == static_cast<int>(i) * 2 && source.gridCell.row == 0,
            "D03: Ctrl Copy and Alt Link must preserve every original file, bytes and visible desktop cell");
        if (i >= count) continue;
        const auto output = std::find_if(outputs.begin(), outputs.end(), [&](const auto& path) {
            if (link)
            {
                Require(_wcsicmp(path.extension().c_str(), L".lnk") == 0,
                    "D03: Alt Link must create persisted Shell shortcut files, not ordinary copies");
                return fs::equivalent(LinkTarget(path), files[i]);
            }
            return Read(path) == "mixed-file-" + std::to_string(i);
        });
        Require(output != outputs.end() && !fs::equivalent(*output, files[i]),
            "D03: every selected ordinary file needs its own actual copy or correctly targeted shortcut");
        const auto& item = Find(app, ToUpperInvariant(output->wstring()));
        Require(published.insert(ToUpperInvariant(item.parsingName)).second &&
            std::count(target->itemKeys.begin(), target->itemKeys.end(), item.layoutKey) == 1 &&
            !Access::HasVisibleIcon(app, item) && item.gridCell.pageId == target->gridCell.pageId &&
            item.gridCell.column == target->gridCell.column && item.gridCell.row == target->gridCell.row,
            "D03: actual outputs must belong once to the intended target without consuming originals or duplicating free-desktop icons");
    }
    std::unordered_set<std::wstring> accessible;
    for (const auto& slot : Access::MemberContainer(app, L"mixed-target")->GetSlots())
        if (const auto* item = slot->GetItem()) accessible.insert(ToUpperInvariant(item->GetPath()));
    Require(accessible == published && Access::NoPresentationWindows(app),
        "D03: every published identity must be accessible through actual target slots without desktop presentation windows");
}
inline void Run(std::string_view typeName, std::string_view verb = "move")
{
    DesktopWidgetType targetType;
    if (typeName == "collection") targetType = DesktopWidgetType::Collection;
    else if (typeName == "files") targetType = DesktopWidgetType::FileCategories;
    else throw std::runtime_error("unknown native mixed-selection target");
    Require(verb == "move" || verb == "copy" || verb == "link", "native mixed selection requires a declared verb");
    const bool physical = verb != "move";
    const int mods = verb == "copy" ? MK_CONTROL : verb == "link" ? MK_ALT : 0;
    const auto promised = verb == "copy" ? DropAction::Copy : verb == "link" ? DropAction::Link : DropAction::Move;
    native_drag_commit_cases::Profile profile;
    std::size_t scenes = 0;
    for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories,
        DesktopWidgetType::FolderMapping, DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (const std::size_t count : {0u, 1u, 2u})
    for (const bool reverse : {false, true})
    {
        const auto description = "mixed target=" + std::string(typeName) + " verb=" + std::string(verb) + " component=" +
            std::to_string(static_cast<int>(type)) + " files=" + std::to_string(count) + " reverse=" + std::to_string(reverse);
        native_drag_commit_cases::context = description;
        const auto directory = profile.root / (L"mixed-" + std::to_wstring(scenes++));
        fs::create_directories(directory / L"component");
        const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
        const std::vector<fs::path> files{desktop / L"A.txt", desktop / L"B.txt"};
        std::vector<fs::path> inside;
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            { std::ofstream stream(files[i], std::ios::binary); stream << "mixed-file-" << i; }
            inside.push_back((type == DesktopWidgetType::FolderMapping ? directory / L"component" : desktop) /
                (L"Inside-" + std::to_wstring(i) + L".txt"));
            { std::ofstream stream(inside.back(), std::ios::binary); stream << "inside-" << i; }
        }
        auto& app = profile.Model(false, directory / L"layout");
        Access::SeedMixedModel(app, files, inside, type, targetType);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        Require(Access::Save(app), "save original native mixed-selection model");
        const auto original = OriginalComponents(app);
        const auto captured = Access::BeginMixed(app, files, count, reverse, mods);
        Require(captured.hasWidgets && captured.HasDesktopWidgetIcons() && captured.entries.size() == count + 1,
            "D01: the production source capture must retain the complete mixed selection before filtering");
        const auto preview = Access::MixedPreview(app, mods);
        Require(preview.landings.size() == count && (count == 0 || preview.action == promised),
            "D01: native feedback must accept eligible members without claiming unsupported members");
        for (const auto& landing : preview.landings)
        {
            const auto source = std::find_if(captured.entries.begin(), captured.entries.end(),
                [&](const auto& entry) { return entry.sourceIndex == landing.sourceIndex; });
            Require(source != captured.entries.end() && source->kind == DropSourceKind::DesktopIcon &&
                !source->desktopKey.empty() && landing.widgetId == L"mixed-target",
                "D01: eligible landing indices must retain their full-selection identities and the intended destination");
        }
        auto baseline = files;
        if (type != DesktopWidgetType::FolderMapping) baseline.insert(baseline.end(), inside.begin(), inside.end());
        Access::DropMixed(app, mods);
        if (physical) Wait([&] { return Access::FileDropIdle(app); }, "D03: actual file workers and native refresh must become idle");
        const auto outputs = Outputs(desktop, baseline);
        const auto check = [&] {
            if (physical) CheckFileAction(app, files, count, outputs, original, targetType, verb == "link");
            else Check(app, files, count, original, targetType);
        };
        native_drag_commit_cases::context = description + " phase=after-drop";
        check();
        Require(Access::Save(app), "save actual partial mixed-selection result through native host storage");
        native_drag_async_reload_cases::Refresh(app);
        native_drag_commit_cases::context = description + " phase=after-refresh";
        check();
        Access::Load(app);
        native_drag_async_reload_cases::Refresh(app, true);
        native_drag_commit_cases::context = description + " phase=after-reload";
        check();
        for (std::size_t i = 0; i < inside.size(); ++i)
            Require(Read(inside[i]) == "inside-" + std::to_string(i),
                "D01: ignoring an unsupported component cannot alter any of its physical member files");
        Require(Outputs(desktop, baseline).size() == (physical ? count : 0),
            "D01/D03: mixed-selection processing cannot create unpromised physical outputs");
        Access::StopModel(app);
        for (const auto& output : outputs)
        {
            Require(fs::weakly_canonical(output.parent_path()) == fs::weakly_canonical(desktop),
                "only isolated native desktop mixed-action outputs may be removed");
            fs::remove(output);
        }
        for (const auto& file : files) fs::remove(file);
        for (const auto& file : inside) fs::remove(file);
    }
    std::cout << scenes << " actual native complete-selection/subset/model/storage scenes passed: " << typeName << ' ' << verb << '\n';
}
}
