// Included after the real host fixture and its access adapter.
#include "navigation/search_match.h"
#include "navigation/quick_navigation_search_async.h"
#include "shell/shell_extension_service.h"
#include "diagnostics/diagnostic_log_modules.h"
#include <condition_variable>
#include <mutex>
#include <utility>

namespace behavior
{
void Search()
{
    HostProfile profile;
    std::vector<fs::path> files;
    for (const auto* name : {L"z-editor", L"editor notes", L"editor", L"unrelated"})
    {
        files.push_back(profile.desktop / name);
        Write(files.back(), "search-entry");
    }
    Require(Access::QuickNavigationNames(*profile.app, files, L"editor") ==
        std::vector<std::wstring>{L"editor", L"editor notes", L"z-editor"},
        "production search model filters misses and orders exact, prefix and interior matches");
    Require(NameMatchesQuery(L"文件管理器", L"wjglq") && !NameMatchesQuery(L"editor", L"unrelated"),
        "name-matching contract accepts pinyin initials and rejects an unrelated query");
    struct Gate { std::mutex mutex; std::condition_variable ready; bool entered = false, release = false; };
    auto gate = std::make_shared<Gate>();
    snowdesktop::QuickNavigationEverythingSearchAsync search([gate](const std::wstring& query, DWORD) {
        if (query == L"old")
        {
            std::unique_lock lock(gate->mutex); gate->entered = true; gate->ready.notify_all();
            if (!gate->ready.wait_for(lock, std::chrono::seconds(5), [&] { return gate->release; }))
                return snowdesktop::QuickNavigationEverythingSearchResponse{{},ERROR_TIMEOUT};
        }
        EverythingSearchResult entry; entry.name = query; entry.path = L"C:\\isolated\\" + query;
        return snowdesktop::QuickNavigationEverythingSearchResponse{{entry},ERROR_SUCCESS};
    });
    Require(search.Submit(nullptr, 0, {1,L"old",20}), "submit first query");
    { std::unique_lock lock(gate->mutex); Require(gate->ready.wait_for(lock,std::chrono::seconds(3),[&] { return gate->entered; }), "provider entered"); }
    Require(search.Submit(nullptr,0,{2,L"discarded",20}) && search.Submit(nullptr,0,{3,L"latest",20}), "typing remains responsive during blocked query");
    { std::lock_guard lock(gate->mutex); gate->release = true; gate->ready.notify_all(); }
    std::optional<snowdesktop::QuickNavigationEverythingSearchResult> result;
    Wait([&] { auto next=search.TakeCompleted(); if(next) result=std::move(next); return result && result->generation == 3; },
        "latest search result generation=3", [&] {
            return result ? "published generation=" + std::to_string(result->generation) : "no published result";
        });
    Require(result->query == L"latest" && result->results.size() == 1 && result->results[0].name == L"latest", "only newest pending query reaches published results");
    search.Stop(); Require(!search.Submit(nullptr,0,{4,L"after-stop",20}), "disposed search rejects further work");
}
// Diagnostic drafts immediately select the diagnostic sink for inspection.
// The real host adapter mirror and JSON persistence change on commit.
// No Windows setting or hotkey changes.
void SettingsHostCommit()
{
    HostProfile profile;
    // This fixture does not run DesktopApp's destructor. Match its explicit
    // pre-CRT shutdown while COM and the main thread are still alive.
    struct MenuServiceLifetime
    {
        ~MenuServiceLifetime() { snowdesktop::shell_extensions::SharedMenuService().Shutdown(); }
    } menuServiceLifetime;
    using namespace snowdesktop;
    auto& controller = Access::HostSettings(*profile.app);
    const auto before = controller.Snapshot()->values.general;
    auto draft = before;
    draft.diagnosticLogModules ^= diagnostics::ModuleBit(DiagnosticLogModule::Settings);
    controller.UpdateGeneral(draft,SettingsUpdateMode::Draft);
    Require(controller.FlushPending().Succeeded() &&
        Access::AppliedLogModules(*profile.app) == before.diagnosticLogModules &&
        diagnostics::IsLogEnabled(DiagnosticLogModule::Settings) ==
            ((draft.diagnosticLogModules & diagnostics::ModuleBit(DiagnosticLogModule::Settings)) != 0),
        "diagnostic draft selects its sink immediately without committing the host mirror");
    controller.UpdateGeneral(draft,SettingsUpdateMode::Preview);
    Require(controller.FlushPending().Succeeded() &&
        Access::AppliedLogModules(*profile.app) == before.diagnosticLogModules,
        "appearance preview cannot commit the logging preference");
    controller.RequestCommit(SettingsDomain::General);
    Require(controller.FlushPending().Succeeded() &&
        Access::AppliedLogModules(*profile.app) == draft.diagnosticLogModules &&
        diagnostics::IsLogEnabled(DiagnosticLogModule::Settings) ==
            ((draft.diagnosticLogModules & diagnostics::ModuleBit(DiagnosticLogModule::Settings)) != 0),
        "real host commit reconciles its logging mirror with the selected diagnostic sink");
    SettingsValues disk;
    Require(CreateNativeSettingsStore()->Load(disk).Succeeded() &&
        disk.general.diagnosticLogModules == draft.diagnosticLogModules,
        "real host commit persists the applied preference");
}
void GroupTypeContract()
{
    HostProfile profile;
    const auto desktopFile = profile.desktop / L"A.needle.txt";
    const auto desktopFolder = profile.desktop / L"Z.needle";
    const auto mapped = profile.root / L"mapped";
    const auto mappedFile = mapped / L"A.needle.txt";
    const auto mappedFolder = mapped / L"Z.needle";
    Write(desktopFile,"desktop-source-bytes"); Write(mappedFile,"mapped-source-bytes");
    fs::create_directories(desktopFolder); fs::create_directories(mappedFolder);
    // Each source starts file-before-folder, so both must actually reorder.
    Access::SeedGroupTypeSources(*profile.app,{desktopFile,desktopFolder},mapped);
    const std::vector<fs::path> expected{desktopFolder,desktopFile,mappedFolder,mappedFile};
    const auto check = [&](const char* phase) {
        const auto ordered = Access::GroupTypeOrder(*profile.app);
        const bool correctOrder = ordered.size() == expected.size() &&
            std::equal(ordered.begin(),ordered.end(),expected.begin(),expected.end(),
                [](const auto& a,const auto& b) { return fs::equivalent(a,b); });
        const auto& widgets = Access::Widgets(*profile.app);
        const auto group = std::find_if(widgets.begin(),widgets.end(),[](const auto& value) { return value.id==L"type-group"; });
        if (!correctOrder)
        {
            const auto print = [&](const char* label, const auto& paths) {
                std::cerr << label;
                for (const auto& path : paths)
                    std::cerr << ' ' << path.lexically_relative(profile.root).generic_string();
                std::cerr << '\n';
            };
            std::cerr << "group ordering phase=" << phase << '\n';
            print("expected:", expected); print("observed:", ordered);
            if (group != widgets.end())
                std::cerr << "sort=" << static_cast<int>(group->contentSortColumn) <<
                    " ascending=" << group->contentSortAscending <<
                    " active=" << WideToUtf8(group->activeCategoryId) << '\n';
            std::cerr << Access::AsyncState(*profile.app) << '\n';
        }
        Require(correctOrder, std::string(phase) +
            ": user contract: retain source sections and place folders before files inside each section");
        Require(group!=widgets.end() && group->activeCategoryId==L"type-desktop" &&
            group->childWidgetIds==std::vector<std::wstring>{L"type-desktop",L"type-mapped"},
            "sorting preserves original source order and active source");
        Require(Read(desktopFile)=="desktop-source-bytes" && Read(mappedFile)=="mapped-source-bytes" &&
            fs::is_empty(desktopFolder) && fs::is_empty(mappedFolder),
            "sorting and reload cannot change either source's files");
    };
    (void)Access::GroupTypeOrder(*profile.app,true); check("after sort");
    Require(Access::Save(*profile.app),"persist accepted grouped Type sort");
    const auto refresh = [&](bool disk) {
        const auto delivered = Access::asyncReadMessages;
        Access::RequestAsync(*profile.app,disk);
        Wait([&] { return Access::BackgroundIdle(*profile.app) && Access::asyncReadMessages>delivered; },
            disk ? "disk reload applied and background idle" : "refresh applied and background idle",
            [&] { return Access::AsyncState(*profile.app); });
    };
    refresh(false); check("after refresh");
    Access::DiscardGroupTypeRuntimeState(*profile.app);
    refresh(true); check("after disk reload");
}
}
