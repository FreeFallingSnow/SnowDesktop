#pragma once

#include "ole_file_source_fixture.h"

namespace native_ole_file_cases
{
namespace fs = std::filesystem;
using Access = NativeDragIntegrationAccess;
using native_drag_commit_cases::Require;
using native_drag_commit_cases::Read;
using native_drag_reload_cases::Find;

struct Gesture
{
    std::string_view name;
    DWORD keys;
    // Literal effects in the order NONE,COPY,MOVE,COPY|MOVE,LINK,... .
    // Expectations come from the named verb and source capability, not a
    // production key-conversion or effect-selection function.
    std::array<DWORD, 8> effects;
    DropAction action;
};
inline constexpr Gesture gestures[]{
    {"copy", MK_CONTROL, {0,1,0,1,0,1,0,1}, DropAction::Copy},
    {"move", MK_SHIFT, {0,0,2,2,0,0,2,2}, DropAction::Move},
    {"alt_link", MK_ALT, {0,0,0,0,4,4,4,4}, DropAction::Link},
    {"ctrl_shift_link", MK_CONTROL | MK_SHIFT, {0,0,0,0,4,4,4,4}, DropAction::Link},
};

template<class Predicate>
void Wait(Predicate predicate, const char* failure)
{
    const auto started = GetTickCount64();
    while (!predicate())
    {
        Require(GetTickCount64() - started < 12000, failure);
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        // The original STA also owns standard COM proxy-dispatch windows.
        // Pump this process's thread queue; no desktop host was created.
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Access::ThrowAsyncException();
    }
}

inline std::vector<fs::path> Outputs(const fs::path& desktop, const std::vector<fs::path>& existing)
{
    std::vector<fs::path> output;
    for (const auto& entry : fs::directory_iterator(desktop))
        if (std::none_of(existing.begin(), existing.end(), [&](const auto& path) { return fs::equivalent(path, entry.path()); }))
            output.push_back(entry.path());
    return output;
}

inline fs::path LinkTarget(const fs::path& path)
{
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> persist;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link.As(&persist)) && SUCCEEDED(persist->Load(path.c_str(), STGM_READ)),
        "native OLE shortcut output must be a readable persisted Shell link");
    std::array<wchar_t, 32768> target{};
    Require(SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)),
        "native OLE shortcut output must expose its actual target");
    return target.data();
}

inline void CheckOriginals(DesktopApp& app, const std::vector<fs::path>& existing)
{
    for (std::size_t i = 0; i < existing.size(); ++i)
    {
        const auto& item = Find(app, ToUpperInvariant(existing[i].wstring()));
        Require(item.gridCell.pageId == L"__page:1" && item.gridCell.column == static_cast<int>(i) * 2 &&
            item.gridCell.row == 0 && Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item) &&
            Read(existing[i]) == "existing-file-" + std::to_string(i),
            "native OLE drop cannot move or alter unrelated existing desktop files");
    }
}

struct EnteredDrag
{
    ComPtr<OleDragDropAdapter> adapter;
    DropPreviewList preview;
};

inline EnteredDrag Enter(DesktopApp& app, ole_file_source_fixture::Source& source,
    const Gesture& selected, DWORD allowed, std::size_t summaryCount, POINT point, bool releaseChanges)
{
    Require(Access::NoPresentationWindows(app),
        "native OLE entry starts without desktop render devices or presentation windows");
    EnteredDrag drag{Access::ExternalAdapter(app), {}};
    const auto selectedIndex = static_cast<std::size_t>(&selected - std::begin(gestures));
    const auto& initial = gestures[(selectedIndex + 1) % std::size(gestures)];
    const auto& different = gestures[(selectedIndex + 2) % std::size(gestures)];
    const auto stage = [&](const Gesture& current, POINT position, bool entering, int slot) {
        DWORD feedback = allowed;
        const auto screen = Access::ExternalPoint(position);
        const auto result = entering
            ? drag.adapter->DragEnter(&source, current.keys | MK_LBUTTON, screen, &feedback)
            : drag.adapter->DragOver(current.keys | MK_LBUTTON, screen, &feedback);
        Require(SUCCEEDED(result) && feedback == current.effects[allowed],
            "native OLE hover feedback must honor current modifiers and the original source effects");
        const auto& summary = Access::ExternalSummary(app);
        Require(Access::ExternalSession(app) && summary.fileCount == static_cast<int>(summaryCount) &&
            !summary.hasShortcut && !summary.foldersOnly &&
            Access::Target(app) == Access::Desktop(app) && Access::Region(app) == HitRegion::Empty &&
            Access::TargetSlot(app) && Access::TargetSlot(app)->GetIndex() == slot,
            "native OLE enter and over must derive the external summary and current physical target without fixture repair");
        if (feedback)
            Require(Access::CurrentAction(app) == current.action,
                "native OLE accepted hover action must follow the current explicit gesture");
        Require(Access::NoPresentationWindows(app),
            "native OLE entry fixture must remain on its owned message-only window without host feedback windows");
        Require(source.starts == 0 && source.ends == 0 && !source.wrongApartment &&
            (!source.asynchronous || (source.fileReads == 0 && !source.fileReadBeforeStart)),
            "native OLE enter and over cannot start a lease or render delayed source content");
    };
    stage(initial, {point.x + 100, point.y}, true, 6);
    if (!releaseChanges) stage(different, point, false, 5);
    stage(selected, point, false, 5);
    drag.preview = Access::ObserveOlePreview(app, point, static_cast<int>(selected.keys));
    if (releaseChanges) stage(different, point, false, 5);
    return drag;
}

inline void Run(std::string_view action, bool nativeEntry = false)
{
    const auto gesture = std::find_if(std::begin(gestures), std::end(gestures),
        [&](const auto& value) { return value.name == action; });
    Require(gesture != std::end(gestures), "native OLE test requires a named independent gesture");
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    std::size_t scenes = 0;
    std::uint64_t completions = 0;
    for (const bool secondary : {false, true})
        for (const bool asynchronous : {false, true})
            for (const std::size_t count : {1u, 2u})
                for (const bool releaseChanges : {false, true})
                for (const DWORD allowed : {0u,1u,2u,3u,4u,5u,6u,7u})
                {
                    if (!nativeEntry && releaseChanges) continue;
                    const auto expected = gesture->effects[allowed];
                    auto& context = native_drag_commit_cases::context;
                    context = "OLE " + std::string(action) + " allowed=" + std::to_string(allowed) +
                        " count=" + std::to_string(count) + " async=" + std::to_string(asynchronous) +
                        " secondary=" + std::to_string(secondary) + " entry=" + std::to_string(nativeEntry) +
                        " release-change=" + std::to_string(releaseChanges);
                    const auto directory = profile.root / (L"ole-" + std::to_wstring(scenes++));
                    fs::create_directories(directory / L"source");
                    std::vector<fs::path> files{directory / L"source/Payload.txt"};
                    { std::ofstream stream(files[0], std::ios::binary); stream << "payload-content"; }
                    if (count == 2)
                    {
                        files.push_back(directory / L"source/Bundle");
                        fs::create_directory(files[1]);
                        std::ofstream stream(files[1] / L"nested.txt", std::ios::binary); stream << "directory-content";
                    }
                    const std::vector<fs::path> existing{desktop / L"Existing-A.txt", desktop / L"Existing-B.txt"};
                    for (std::size_t i = 0; i < existing.size(); ++i)
                    { std::ofstream stream(existing[i], std::ios::binary); stream << "existing-file-" << i; }
                    auto& app = profile.Model(secondary, directory / L"layout");
                    Access::PopulateModel(app, existing, 0);
                    Access::PrepareReload(app);
                    Access::BindAsync(app);
                    Require(Access::Save(app), "save actual native desktop before an OLE file drop");
                    const auto saved = Read(Access::LayoutPath(app));
                    auto source = ole_file_source_fixture::Make(files, asynchronous);
                    const POINT point{secondary ? -650L : 150L, 150};
                    const std::size_t summaryCount = asynchronous ? 1 : count;
                    EnteredDrag entered;
                    DropPreviewList preview;
                    if (nativeEntry)
                    {
                        entered = Enter(app, *source.Get(), *gesture, allowed, summaryCount, point, releaseChanges);
                        preview = entered.preview;
                        Require(Outputs(desktop, existing).empty() && Read(Access::LayoutPath(app)) == saved &&
                            Access::Items(app).size() == 2,
                            "native OLE hover must preserve physical files and saved/native layout until release");
                    }
                    else
                    {
                        Access::BeginExternal(app, summaryCount, point);
                        preview = Access::ObserveModelPreview(app, point, static_cast<int>(gesture->keys));
                    }
                    Require(Access::Target(app) == Access::Desktop(app) && preview.landings.size() == summaryCount,
                        "native OLE fixture must observe the actual accepted desktop landing preview");
                    DWORD returned = allowed;
                    const auto result = nativeEntry
                        ? entered.adapter->Drop(source.Get(), gesture->keys, Access::ExternalPoint(point), &returned)
                        : Access::DropExternal(app, source.Get(), gesture->keys, point, returned);
                    Require(SUCCEEDED(result),
                        "actual native OLE adapter must return through the host drop handler");
                    if (expected)
                    {
                        Wait([&] { return Access::FileDropIdle(app) &&
                            (!asynchronous || source->ends == 1) && Access::Items(app).size() == 2 + count; },
                            "native OLE submission, file/read completions and actual placement refresh must finish");
                    }
                    const auto outputs = Outputs(desktop, existing);
                    Require(outputs.size() == (expected ? count : 0),
                        "native OLE drop must create exactly the independently allowed physical outputs");
                    if (!expected)
                    {
                        Require(returned == DROPEFFECT_NONE && source->starts == 0 && source->ends == 0 &&
                            Access::Items(app).size() == 2 && Read(Access::LayoutPath(app)) == saved,
                            "native OLE capability rejection must preserve files, native model and saved layout without starting a lease");
                    }
                    Require(!source->wrongApartment && !source->fileReadBeforeStart,
                        "native OLE source content must stay on its STA and be read only after an asynchronous lease starts");
                    if (expected && asynchronous)
                        Require(source->starts == 1 && source->ends == 1 && SUCCEEDED(source->outcome) &&
                            source->endEffect == (expected == DROPEFFECT_MOVE ? DROPEFFECT_NONE : expected),
                            "native OLE source lease must end once after actual commit with optimized-move ownership");
                    std::vector<std::pair<std::wstring, GridCell>> published;
                    std::unordered_set<std::wstring> occupied;
                    for (std::size_t i = 0; i < files.size(); ++i)
                    {
                        Require(fs::exists(files[i]) == (expected != DROPEFFECT_MOVE),
                            "native OLE move must remove the real source; copy, link and rejection must retain it");
                        if (expected != DROPEFFECT_MOVE)
                            Require(Read(i ? files[i] / L"nested.txt" : files[i]) == (i ? "directory-content" : "payload-content"),
                                "native OLE copy, link and rejection must preserve the original source bytes");
                        if (!expected) continue;
                        const auto output = std::find_if(outputs.begin(), outputs.end(), [&](const auto& path) {
                            return expected == DROPEFFECT_LINK ? fs::equivalent(LinkTarget(path), files[i])
                                : path.filename() == files[i].filename();
                        });
                        Require(output != outputs.end(), "native OLE physical output must correspond to each original selected identity");
                        if (expected == DROPEFFECT_LINK)
                            Require(_wcsicmp(output->extension().c_str(), L".lnk") == 0,
                                "native OLE shortcut output must retain the Shell link file type");
                        if (expected != DROPEFFECT_LINK)
                            Require(Read(i ? *output / L"nested.txt" : *output) == (i ? "directory-content" : "payload-content"),
                                "native OLE move and copy must preserve actual file and nested-directory bytes");
                        const auto& item = Find(app, ToUpperInvariant(output->wstring()));
                        const auto landing = std::find_if(preview.landings.begin(), preview.landings.end(),
                            [&](const auto& value) { return value.sourceIndex == i; });
                        if (i < summaryCount)
                            Require(landing != preview.landings.end() && item.gridCell.pageId == landing->cell.pageId &&
                                item.gridCell.column == landing->cell.column && item.gridCell.row == landing->cell.row,
                                "native OLE physical output must fulfill its published landing in the rebuilt desktop model");
                        Require(Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item),
                            "every native OLE physical output must receive a valid visible runtime frame");
                        Require(occupied.insert(item.gridCell.pageId + L":" + std::to_wstring(item.gridCell.column) +
                            L"," + std::to_wstring(item.gridCell.row)).second,
                            "native OLE multi-file outputs cannot overlap on the desktop");
                        // Delayed sources advertise one placeholder before
                        // GetData. Later outputs need legal distinct cells;
                        // their accepted final cells must survive disk reload.
                        published.emplace_back(item.layoutKey, item.gridCell);
                    }
                    CheckOriginals(app, existing);
                    if (expected)
                    {
                        Access::Load(app);
                        native_drag_async_reload_cases::Refresh(app, true);
                        CheckOriginals(app, existing);
                        for (const auto& [key, cell] : published)
                        {
                            const auto& item = Find(app, key);
                            Require(item.gridCell.pageId == cell.pageId && item.gridCell.column == cell.column &&
                                item.gridCell.row == cell.row && Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item),
                                "native OLE outputs must retain their published cells after actual asynchronous disk reload");
                        }
                    }
                    completions += Access::fileCompletionMessages;
                    Access::StopModel(app);
                    source.Reset();
                    for (const auto& output : outputs)
                    {
                        Require(fs::weakly_canonical(output.parent_path()) == fs::weakly_canonical(desktop),
                            "only outputs inside the isolated native desktop may be removed");
                        fs::remove_all(output);
                    }
                    for (const auto& file : existing) fs::remove(file);
                }
    std::cout << scenes << " actual native OLE " << action << " file/effect/placement scenes passed; "
        << completions << " file/read completions dispatched; native entry=" << nativeEntry << '\n';
}
}
