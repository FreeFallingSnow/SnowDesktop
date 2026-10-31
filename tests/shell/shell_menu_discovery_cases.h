#pragma once
#include "shell/shell_extension_application.h"

namespace menu_discovery_tests
{
namespace ext = snowdesktop::shell_extensions;

// Exercise the real scheduler, registry association and snapshot cache. Only
// the external Shell boundary supplies deterministic menu content.
void CheckMetadataIsolation()
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"selection.snowmetadata";
    std::ofstream(file) << "private";
    ext::Request request; request.paths = {file.wstring()};
    ext::Registration known; known.id = "reg:known"; known.contexts = 1;
    known.types = {L".snowmetadata"}; known.verbs = {"known"};
    known.revision = 1;
    auto handler = known;
    handler.id = "clsid:unrelated"; handler.kind = ext::RegistrationKind::Handler;
    handler.sources = {L"*\\shellex\\ContextMenuHandlers\\Unrelated"};
    handler.verbs = {"{11111111-1111-1111-1111-111111111111}"};
    auto package = handler;
    package.id = "package:known"; package.kind = ext::RegistrationKind::Packaged;
    ext::Catalogue catalogue; catalogue.revision = 1; catalogue.rows = {known, handler, package};
    std::atomic_uint metadata = 0, complete = 0, sources = 0;
    std::atomic_bool wrongMode = false;
    ext::Entry action; action.provider = "verb:known"; action.key = "known";
    action.label = L"Known command"; action.token = 9;
    ext::Entry child; child.provider = "verb:child"; child.key = "child";
    child.label = L"Actual child"; child.token = 10; action.children = {child};
    const auto cachePath = temp.path / L"cache";
    ext::MenuService service(cachePath, [&](const ext::Request& target) {
        if (!target.sourceClsid.empty()) ++sources;
        else if (target.rootMetadataOnly) ++metadata;
        else
        {
            ++complete;
            if (target.paths != request.paths) wrongMode = true;
        }
        auto root = action;
        if (target.rootMetadataOnly) root.children.clear();
        return ext::QueryWork{[root] { return ext::Reply{true, {root}, {}}; }, {}};
    }, [catalogue] { return catalogue; });
    service.Inspect(request);
    PumpUntil([&] { auto view = service.Inspect(request); return !view.scanning && !view.menu.pending; },
        "automatic discovery and the explicit selected menu complete independently");
    auto metadataRequest = request; metadataRequest.rootMetadataOnly = true;
    const auto selected = service.View(metadataRequest);
    Expect(metadata == 5 && complete == 0 && !wrongMode,
        "SETTINGS_MUST_READ_ROOTS: all baseline contexts and the selected settings target use metadata only");
    Expect(sources == 0,
        "PROVEN_REGISTRATION_MUST_NOT_PROBE: a known registration with absent application metadata never queries unrelated handlers or packages");
    Expect(selected.snapshot && selected.snapshot->entries.size() == 1 &&
        selected.snapshot->entries.front().children.empty() && !service.View(request).snapshot,
        "settings never prepare a complete child tree for their selected target");
    metadataRequest.visibleMenuProviders = std::vector<std::string>{"*"};
    Expect(!ext::CanPreviewPopupRoots(metadataRequest, 0),
        "metadata roots never publish user-menu preparation headers");
    metadataRequest.visibleMenuProviders.reset();
    ext::MenuSnapshotCache cache(cachePath);
    const auto full = cache.Capture(request), roots = cache.Capture(metadataRequest);
    Expect(full && roots && full.identity != roots.identity,
        "root metadata and complete menu snapshots have different durable cache identities");
    const auto encoded = snowdesktop::settings_ipc::Pack(metadataRequest);
    Expect(snowdesktop::settings_ipc::Unpack<ext::Request>(encoded).rootMetadataOnly,
        "the isolated helper retains the metadata query mode");
    service.QueryFullMenu(request);
    PumpUntil([&] { const auto view = service.View(request); return view.snapshot && !view.pending; },
        "an actual popup query prepares the complete menu independently of settings");
    Expect(complete == 1 && service.View(request).snapshot->entries.front().children.size() == 1,
        "an actual menu gets complete children instead of a metadata cache hit");
    service.Inspect(metadataRequest, true);
    PumpUntil([&] { return !service.Inspect(request).scanning; },
        "explicit settings refresh remains a metadata read");
    Expect(complete == 1 && service.View(request).snapshot->entries.front().children.size() == 1 &&
        service.View(metadataRequest).snapshot->entries.front().children.empty(),
        "settings refresh cannot overwrite or requery the complete popup tree");
}

void CheckUnrelatedRegistrationDoesNotRetry(bool ambiguous)
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"selection.snowmetadata";
    std::ofstream(file) << "private";
    ext::Request request; request.paths = {file.wstring()};
    ext::Registration handler; handler.id = "clsid:first"; handler.kind = ext::RegistrationKind::Handler;
    handler.contexts = 1; handler.types = {L".snowmetadata"}; handler.revision = 1;
    handler.sources = {L"*\\shellex\\ContextMenuHandlers\\First"};
    handler.verbs = {"{11111111-1111-1111-1111-111111111111}"};
    handler.application = {"first-app", L"First"};
    auto other = handler; other.id = "clsid:second";
    other.sources = {L"*\\shellex\\ContextMenuHandlers\\Second"};
    other.verbs = {"{22222222-2222-2222-2222-222222222222}"};
    other.application = {"second-app", L"Second"};
    ext::Registration known; known.id = "reg:known"; known.contexts = 1;
    known.types = {L".snowmetadata"}; known.verbs = {"known"}; known.revision = 1;
    ext::Catalogue catalogue; catalogue.revision = 1; catalogue.rows = {handler, known};
    if (ambiguous) catalogue.rows.push_back(other);
    std::atomic_uint probes = 0;
    std::atomic_bool extra = false;
    ext::Entry action; action.provider = "verb:runtime"; action.key = "runtime"; action.label = L"Runtime";
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request& target) {
        ext::Reply reply; reply.ok = true;
        if (!target.sourceClsid.empty())
        {
            ++probes;
            if (ambiguous) reply.entries = {action};
        }
        else if (target.paths == request.paths)
        {
            reply.entries = {action};
            if (extra)
            {
                auto item = action; item.provider = "verb:known"; item.key = "known"; item.label = L"Known";
                reply.entries.push_back(item);
            }
        }
        return ext::QueryWork{[reply] { return reply; }, {}};
    }, [catalogue] { return catalogue; });
    service.Query(request, ext::QueryPriority::Inspect);
    PumpUntil([&] { return !service.Inspect(request).scanning; }, "explicit complete source proof settles");
    Expect(probes == (ambiguous ? 2u : 1u),
        "all possible sources are checked before accepting an initially unknown owner");
    if (ambiguous)
    {
        const auto view = service.Inspect(request);
        const auto item = std::find_if(view.catalogue.rows.begin(), view.catalogue.rows.end(),
            [&](const auto& row) { return row.id == action.provider; });
        Expect(item != view.catalogue.rows.end() && item->application.id.empty(),
            "conflicting canonical source claims remain ambiguous after metadata optimization");
    }
    else
    {
        extra = true;
        service.Query(request, ext::QueryPriority::Inspect, true);
        PumpUntil([&] { const auto view = service.Inspect(request); const auto menu = service.View(request);
            return !view.scanning && menu.snapshot && menu.snapshot->entries.size() == 2; },
            "a new proven registration joins the refreshed actual menu");
        Expect(probes == 1,
            "UNRELATED_ROOT_MUST_NOT_REPROBE: a new proven root cannot retry old unknown-action attribution");
    }
}
void CheckManagementNeverExpands()
{
    TemporaryDirectory temp;
    ext::Registration handler; handler.id = "clsid:legacy"; handler.kind = ext::RegistrationKind::Handler;
    handler.contexts = 1; handler.types = {L".png", L".docx"}; handler.revision = 1;
    handler.sources = {L"*\\shellex\\ContextMenuHandlers\\Legacy"};
    handler.verbs = {"{11111111-1111-1111-1111-111111111111}"};
    handler.application = {"legacy-app", L"Legacy application"};
    ext::Catalogue catalogue; catalogue.revision = 1; catalogue.rows = {handler};
    ext::Entry action; action.provider = "menu:legacy:14695981039346656037"; action.label = L"Legacy";
    action.width = action.height = 1; action.pixels = {10, 20, 30, 255};
    ext::Entry child; child.key = "actual-child"; child.label = L"Actual child"; child.token = 7;
    action.children = {child};
    std::atomic_uint metadata = 0, complete = 0, sources = 0;
    std::atomic_bool partialProof = false;
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request& target) {
        ext::Reply reply; reply.ok = true;
        if (!target.sourceClsid.empty())
        {
            ++sources; if (target.rootMetadataOnly) partialProof = true; reply.entries = {action};
        }
        else if (!target.paths.empty() &&
            (std::filesystem::path(target.paths.front()).extension() == L".png" ||
             std::filesystem::path(target.paths.front()).extension() == L".docx"))
        {
            auto root = action;
            if (target.rootMetadataOnly) { ++metadata; root.native = true; root.children.clear(); }
            else ++complete;
            reply.entries = {root};
        }
        return ext::QueryWork{[reply] { return reply; }, {}};
    }, [catalogue] { return catalogue; });
    ext::Request installed; installed.catalogueOnly = true;
    service.Inspect(installed);
    PumpUntil([&] { return !service.Inspect(installed).scanning; }, "installed metadata completes without synthetic file samples");
    const auto result = service.Inspect(installed);
    Expect(metadata == 0 && complete == 0 && sources == 0 && !partialProof,
        "MANAGEMENT_MUST_NOT_SAMPLE: registry file types cannot create synthetic menu queries");
    Expect(result.registrations.rows.size() == 1 && result.catalogue.rows.empty() &&
        !std::filesystem::exists(temp.path / L"cache" / L"discovery-samples-v1"),
        "installed extensions remain separate from commands that have never been observed");
}

// Hold the external metadata boundary. A real user menu must still dispatch
// and publish while all three management queries remain unfinished.
void CheckManagementBudgetIsolation()
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"actual.snowmetadata"; std::ofstream(file) << "private";
    ext::Request request; request.paths = {file.wstring()};
    std::atomic_uint metadata = 0, complete = 0;
    std::atomic_bool release = false;
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request& target) {
        const bool roots = target.rootMetadataOnly;
        if (roots) ++metadata; else ++complete;
        return ext::QueryWork{[&, roots]() -> std::optional<ext::Reply> {
            if (roots && !release) return {};
            return ext::Reply{true, {}, {}};
        }, {}};
    }, [] { ext::Catalogue catalogue; catalogue.revision = 1; return catalogue; });
    service.Inspect();
    PumpUntil([&] { return metadata == 3; }, "three bounded management root queries start concurrently");
    service.QueryFullMenu(request);
    PumpUntil([&] { const auto view = service.View(request); return view.snapshot && !view.pending; },
        "an actual menu completes while the metadata budget is fully held");
    Expect(metadata == 3 && complete == 1,
        "MANAGEMENT_MUST_NOT_STARVE_POPUP: metadata stays bounded and cannot consume actual-menu capacity");
    release = true;
    PumpUntil([&] { return !service.Inspect().scanning; }, "remaining management roots finish after release");
}

void CheckManagementRegistrySwitch()
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"actual.snowmetadata"; std::ofstream(file) << "private";
    ext::Request target; target.paths = {file.wstring()};
    std::atomic_uint metadata = 0, complete = 0;
    std::atomic_bool release = false;
    ext::Catalogue inventory; inventory.revision = 1;
    ext::Registration registration; registration.id = "reg:*\\shell\\example";
    registration.contexts = 1; registration.types = {L"*"}; inventory.rows = {registration};
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request& request) {
        const bool roots = request.rootMetadataOnly;
        if (roots) ++metadata; else ++complete;
        return ext::QueryWork{[&, roots]() -> std::optional<ext::Reply> {
            if (roots && !release) return {};
            ext::Entry entry; entry.provider = "verb:actual"; entry.key = "actual"; entry.label = L"Actual";
            return ext::Reply{true, {entry}, {}};
        }, {}};
    }, [&] { return inventory; });
    service.Inspect(target);
    PumpUntil([&] { return metadata == 3; }, "command inspection starts only its bounded metadata reads");
    ext::Request installed; installed.catalogueOnly = true;
    service.Inspect(installed);
    service.QueryFullMenu(target);
    PumpUntil([&] { return service.View(target).snapshot.has_value(); },
        "returning to software inspection keeps ordinary actual menu reads available");
    release = true;
    PumpUntil([&] { return !service.Inspect(installed).scanning; }, "registry-only inspection finishes independently");
    const auto view = service.Inspect(installed);
    Expect(metadata == 3 && complete == 1 && view.catalogue.rows.empty() && view.registrations.rows.size() == 1,
        "software view retires queued command discovery and never includes observed commands");
}

// Real registry/publication/visibility/execution paths; only the external Shell
// boundary returns deterministic vendor commands. No user data is modified.
void CheckExtensionAndCommandControls()
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"real.snowmetadata"; std::ofstream(file) << "private";
    ext::Request target; target.paths = {file.wstring()};
    ext::Registration extension; extension.id = "clsid:{11111111-1111-1111-1111-111111111111}";
    extension.kind = ext::RegistrationKind::Handler; extension.contexts = 1; extension.types = {L"*"};
    extension.sources = {L"*\\shellex\\ContextMenuHandlers\\Example"};
    extension.verbs = {"{11111111-1111-1111-1111-111111111111}"};
    extension.display.label = L"Example extension"; extension.application = {"example-app", L"Example application"};
    extension.revision = extension.menuRevision = 17;
    ext::Catalogue inventory; inventory.revision = 7; inventory.rows = {extension};
    ext::Entry first; first.provider = "verb:first"; first.key = "first"; first.label = L"First"; first.token = 21;
    auto second = first; second.provider = "verb:second"; second.key = "second"; second.label = L"Second"; second.token = 22;
    ext::Entry child; child.provider = "verb:child"; child.key = "child"; child.label = L"Prepared child"; child.token = 23;
    second.children = {child};
    std::atomic_uint queries = 0, sources = 0, aggregates = 0, invoked = 0;
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request& request) {
        ++queries;
        if (request.rootMetadataOnly) return ext::QueryWork{[] { return ext::Reply{true, {}, {}}; }, {}};
        if (request.sourceClsid.empty()) ++aggregates; else ++sources;
        return ext::QueryWork{[=] { return ext::Reply{true, {first, second}, {}}; },
            [&](UINT token, POINT) { Expect(token == child.token, "group-enabled invocation resolves the fresh aggregate leaf token"); ++invoked; }};
    }, [=] { return inventory; }, {}, {}, [=](const auto&) { return inventory; });
    ext::Preferences prefs; service.Configure(prefs);
    auto installed = target; installed.catalogueOnly = true;
    service.Inspect(installed);
    PumpUntil([&] { return !service.Inspect(installed).scanning; }, "registry-only settings settle without vendor activation");
    auto view = service.Inspect(installed);
    Expect(queries == 0 && view.registrations.rows.size() == 1 && view.catalogue.rows.empty(),
        "EXTENSION_MANAGEMENT_ZERO_SHELL: installed sources never masquerade as actual commands");
    auto groups = ext::ExtensionManagementRows(view.registrations, ext::Category::Objects);
    Expect(groups.size() == 1 && groups.front().members.front().id == extension.id,
        "an unobserved registered extension has an actionable whole-extension switch");
    ext::SetManagementCommon(prefs, groups.front(), ext::Category::Objects, true);
    service.Configure(prefs); service.Prewarm(target);
    PumpUntil([&] {
        const auto menu = service.MenuDisplay(target, prefs);
        return menu.sourcesOnly && menu.sources.size() == 1 && service.View(menu.sources.front()).snapshot.has_value();
    }, "an extension opt-in immediately prefetches its real selection and children without format discovery");
    Expect(sources == 1 && aggregates == 0, "a direct extension opt-in needs no command-attribution aggregate");
    const auto plan = service.MenuDisplay(target, prefs);
    const auto prepared = service.MenuDisplay(plan.sources.front(), prefs);
    Expect(prepared.snapshot && prepared.snapshot->entries.size() == 2 && prepared.snapshot->entries.back().children.size() == 1,
        "a whole extension exposes every returned root with complete immediate children");
    view = service.Inspect(target);
    auto commands = ext::ManagementRows(view.catalogue, ext::Category::Objects);
    Expect(commands.size() == 2 && std::all_of(commands.begin(), commands.end(), [&](const auto& row) {
        return row.members.size() == 1 && row.members.front().id != extension.id &&
            ext::ManagementCommon(prefs, row, ext::Category::Objects) == true;
    }), "each source root remains independently manageable and inherits the whole-extension setting");
    const auto command = std::find_if(commands.begin(), commands.end(), [&](const auto& row) { return row.display.provider == first.provider; });
    Expect(command != commands.end(), "the first live command has its own settings identity");
    ext::SetManagementCommon(prefs, *command, ext::Category::Objects, false); service.Configure(prefs);
    auto filtered = service.MenuDisplay(plan.sources.front(), prefs);
    Expect(filtered.snapshot && filtered.snapshot->entries.size() == 1 && filtered.snapshot->entries.front().provider == second.provider,
        "a single-command hide overrides an enabled extension without hiding its sibling");
    std::atomic_bool completed = false; bool success = false;
    const auto visibleReference = ext::AppendReference(ext::AppendReference({}, prepared.snapshot->entries.back()),
        prepared.snapshot->entries.back().children.front());
    service.Execute(target, visibleReference, {}, [&](bool result) { success = result; completed = true; });
    PumpUntil([&] { return completed.load(); }, "a command shown by its extension setting finishes the fresh execution path");
    Expect(success && invoked == 1 && aggregates == 1, "extension display authority survives a separately resolved real invocation");
    completed = false; success = true;
    service.Execute(target, ext::AppendReference({}, prepared.snapshot->entries.front()), {}, [&](bool result) { success = result; completed = true; });
    PumpUntil([&] { return completed.load(); }, "a command-level hide is checked again during execution");
    Expect(!success && invoked == 1, "an enabled extension cannot execute a command explicitly hidden within it");
    groups = ext::ExtensionManagementRows(service.Inspect(installed).registrations, ext::Category::Objects);
    Expect(groups.size() == 1 && ext::ManagementCommon(prefs, groups.front(), ext::Category::Objects) == true,
        "registry-only application state does not absorb actual-command exceptions");
    ext::SetManagementCommon(prefs, groups.front(), ext::Category::Objects, false);
    auto future = second; future.provider = "verb:future"; future.registration = extension.id;
    Expect(ext::VisibleSnapshot(prefs, {true, {future}, {}}, 1).empty(),
        "a whole-extension hide applies to future commands without sampling them first");
    ext::SetManagementCommon(prefs, groups.front(), ext::Category::Objects, true);
    auto inheritedFirst = first, inheritedSecond = second;
    inheritedFirst.registration = inheritedSecond.registration = extension.id;
    Expect(ext::HasVisibilityRule(prefs, first.provider, ext::Context::File) &&
        ext::VisibleSnapshot(prefs, {true, {inheritedFirst, inheritedSecond, future}, {}}, 1).size() == 2,
        "registration-only actions preserve independently configured command exceptions");
    ext::SetManagementCommon(prefs, groups.front(), ext::Category::Objects, false);
    ext::SetCommon(prefs, first.provider, ext::Category::Objects, true);
    const auto individualPlan = ext::PlanDemandQueries(inventory, view.catalogue, prefs, target, 1);
    Expect(!individualPlan.full && individualPlan.sources.size() == 1,
        "a command observed from an explicitly selected source retains its exact routing proof after the group is hidden");
    inventory.associations.push_back({first.provider, extension.id, ext::Context::File});
    ext::Preferences individual; ext::SetCommon(individual, first.provider, ext::Category::Objects, true);
    ext::MigrateAssociations(individual, inventory);
    auto commandPacket = view.catalogue; commandPacket.associations = inventory.associations;
    ext::MigrateAssociations(individual, commandPacket);
    Expect(ext::IsHidden(individual, extension.id, ext::Context::File),
        "legacy command migration cannot silently enable an entire dynamic handler");
    ext::ManagementRow alias; alias.display = first; alias.display.registration = "reg:static";
    alias.members = {{"reg:static", 1}};
    ext::SetManagementCommon(individual, alias, ext::Category::Objects, false);
    auto staticEntry = first; staticEntry.registration = "reg:static";
    Expect(ext::VisibleSnapshot(individual, {true, {staticEntry}, {}}, 1).empty(),
        "an explicit static registration setting overrides its retained legacy canonical alias");
    ext::Preferences legacy; ext::SetCommon(legacy, first.provider, ext::Category::Objects, true);
    ext::SetManagementOverride(legacy, alias, ext::Context::File, ext::Visibility::Hide);
    auto sibling = first; sibling.registration = "reg:other-application";
    Expect(ext::VisibleSnapshot(legacy, {true, {staticEntry}, {}}, 1).empty() &&
        ext::VisibleSnapshot(legacy, {true, {staticEntry}, {}}, 2).size() == 1 &&
        ext::VisibleSnapshot(legacy, {true, {sibling}, {}}, 1).size() == 1,
        "a static location edit preserves the legacy alias in other locations and applications");
    const auto encoded = snowdesktop::settings_ipc::Pack(service.Inspect(installed));
    const auto installedPacket = snowdesktop::settings_ipc::Unpack<ext::CatalogueView>(encoded);
    Expect(installedPacket.registrations.rows.size() == 1 && installedPacket.catalogue.rows.empty(),
        "settings IPC keeps installed sources separate from command observations");
}

void CheckSoftwareManagementPresentation()
{
    ext::Registration first; first.id = "reg:document\\shell\\open";
    first.kind = ext::RegistrationKind::Verb; first.contexts = 1; first.types = {L".one"};
    first.display.label = L"open"; first.application = {"software-one", L"本地化应用"};
    auto second = first; second.id = "reg:other\\shell\\open"; second.types = {L".two"};
    auto handler = first; handler.kind = ext::RegistrationKind::Handler; handler.id = "clsid:provider";
    handler.contexts = 3; handler.display.label = L"InternalProvider";
    auto other = first; other.id = "reg:unrelated"; other.application.id = "software-two";
    auto opaque = first; opaque.id = "package:opaque"; opaque.kind = ext::RegistrationKind::Packaged;
    opaque.application = {"opaque", L"0BF99681-825C-4B2A-A14F-2AC01DB9B70E"}; opaque.display.label = opaque.application.name;
    ext::Catalogue installed; installed.rows = {first, second, handler, other, opaque};
    ext::Registration stale; stale.kind = ext::RegistrationKind::Observed;
    stale.id = "observed:unknown"; stale.display.label = L"Old unknown command"; stale.contexts = 1; installed.rows.push_back(stale);
    const auto rows = ext::ExtensionManagementRows(installed, ext::Category::Objects);
    Expect(rows.size() == 2 && rows.front().display.label == L"本地化应用" && rows.front().members.size() == 3,
        "software view merges registrations by identity, keeps same-name applications separate and omits opaque/stale rows");
    const auto searched = ext::ExtensionManagementRows(installed, ext::Category::Objects, L".two");
    Expect(searched.size() == 1 && searched.front().members.size() == 3,
        "a type search returns the complete software switch rather than narrowing its authority to the matched registration");
    ext::Preferences prefs; ext::SetManagementCommon(prefs, searched.front(), ext::Category::Objects, true);
    Expect(ext::CommonShown(prefs, first.id, ext::Category::Objects) && ext::CommonShown(prefs, second.id, ext::Category::Objects) &&
        ext::CommonShown(prefs, handler.id, ext::Category::Objects) && !ext::CommonShown(prefs, other.id, ext::Category::Objects),
        "one software switch updates all of its original registrations and leaves the other application unchanged");
    Expect(!ext::UsableShellDisplayName(L"@missing.dll,-1") && !ext::UsableShellDisplayName(L"ms-resource:AppName") &&
        !ext::UsableShellDisplayName(L"Vendor App<Product") &&
        ext::UsableShellDisplayName(L"7-Zip") && ext::UsableShellDisplayName(L"本地化应用"),
        "primary names accept real brands/localized captions and reject unresolved resources");
    installed.rows = {first, second, other};
    installed.rows[0].application = {"ten", L"tool 10"};
    installed.rows[1].application = {"two", L"Tool 2"};
    installed.rows[2].application = {"alpha", L"alpha"};
    const auto sorted = ext::ExtensionManagementRows(installed, ext::Category::Objects);
    Expect(sorted.size() == 3 && sorted[0].display.label == L"alpha" && sorted[1].display.label == L"Tool 2" &&
        sorted[2].display.label == L"tool 10", "software presentation uses case-insensitive natural name ordering");
}

void CheckInstalledApplicationProfiles()
{
    ext::ApplicationProfileIndex index;
    index.Add({L"C:\\Creative", L"After Effects", {}, 30});
    index.Add({L"C:\\Creative", L"Photoshop", {}, 30});
    index.Add({L"C:\\Creative\\After Effects", L"After Effects", L"ae.exe", 20});
    index.Add({L"C:\\Creative\\Photoshop", L"Photoshop", L"ps.exe", 20});
    Expect(!index.Find(L"C:\\Creative\\Media Encoder\\encoder.exe"),
        "a shared custom installation parent never attributes another Adobe product to After Effects");
    const auto photoshop = index.Find(L"c:\\creative\\Photoshop\\Plug-ins\\shell.dll");
    Expect(photoshop && photoshop->name == L"Photoshop", "a precise installation boundary attributes its nested extension");
    index.Add({L"C:\\Tools\\PowerToys", L"PowerToys (Preview)", L"powertoys.exe", 30});
    index.Add({L"C:\\Tools\\PowerToys\\", L"PowerToys (Preview) x64", {}, 30});
    const auto toys = index.Find(L"C:\\Tools\\PowerToys\\WinUI3Apps\\PowerRename.dll");
    Expect(toys && toys->name == L"PowerToys (Preview)" && toys->icon == L"powertoys.exe",
        "architecture duplicates share the installation and its main application icon");
    Expect(!index.Find(L"C:\\Tools\\PowerToysOther\\shell.dll"), "path prefixes require an actual directory boundary");
    index.Add({L"C:\\MWORKS\\Syslab 2026b", L"MWORKS.Syslab 2026b", {}, 20});
    index.Add({L"C:\\MWORKS\\Sysplorer 2026b", L"MWORKS.Sysplorer 2026b", {}, 20});
    const auto lab = index.Find(L"C:\\MWORKS\\Syslab 2026b\\Bin\\syslab.exe");
    const auto plorer = index.Find(L"C:\\MWORKS\\Sysplorer 2026b\\Bin64\\activate_sysplorer.exe");
    Expect(lab && plorer && lab->root != plorer->root && plorer->name == L"MWORKS.Sysplorer 2026b",
        "sibling products stay distinct while a helper belongs to its own installed product");
}

void CheckShowAllPolicy()
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"actual.snowmetadata"; std::ofstream(file) << "private";
    ext::Request target; target.paths = {file.wstring()};
    ext::Entry known; known.provider = "verb:known"; known.key = "known"; known.label = L"Known";
    auto unknown = known; unknown.provider = "menu:unattributed"; unknown.key.clear(); unknown.label = L"Unattributed";
    auto disabled = known; disabled.provider = "verb:disabled"; disabled.key = "disabled"; disabled.enabled = false;
    ext::Entry child; child.provider = "verb:child"; child.key = "child"; child.label = L"Prepared child";
    unknown.children = {child};
    const ext::Reply complete{true, {known, unknown, disabled}, {}};
    ext::Preferences selective; ext::SetCommon(selective, known.provider, ext::Category::Objects, true);
    ext::SetOverride(selective, unknown.provider, ext::Context::File, ext::Visibility::Hide);
    auto all = selective; all.showAll = true;
    const auto savedRules = all.rules; const auto savedOverrides = all.overrides;
    const auto packet = ext::WritePreferences(all);
    JsonValue json; Expect(ParseJson(packet, json), "show-all preferences serialize");
    const auto restored = ext::ReadPreferences(&json);
    Expect(restored == all, "show-all preserves every stored individual rule through persistence");
    for (int i = 0; i < 4; ++i)
        Expect(ext::HasOptIns(all, static_cast<ext::Context>(i)) && ext::VisibleSnapshot(all, complete, 1u << i).size() == 3,
            "show-all includes unattributed and disabled roots in every context without changing their Shell state");
    auto filtered = ext::VisibleSnapshot(selective, complete, 1);
    Expect(filtered.size() == 1 && filtered.front().provider == known.provider,
        "turning off show-all restores existing visibility rules");
    std::atomic_uint calls = 0; std::atomic_bool projected = false;
    ext::MenuService service(temp.path / L"all-cache", [&](const ext::Request& request) {
        if (request.paths == target.paths) { ++calls; projected = request.visibleMenuProviders && *request.visibleMenuProviders == std::vector<std::string>{"*"}; }
        return ext::QueryWork{[=] { return complete; }, {}};
    }, [] { ext::Catalogue value; value.revision = 1; return value; });
    service.Configure(all); service.Prewarm(target);
    PumpUntil([&] { const auto view = service.MenuDisplay(target, all); return view.snapshot && !view.sourcesOnly && view.snapshot->entries.size() == 3; },
        "show-all prewarms the full actual menu even with no source attribution or per-command opt-ins");
    Expect(calls > 0 && projected && all.rules == savedRules && all.overrides == savedOverrides,
        "the full read bypasses projection and does not rewrite selective settings");
    const auto full = service.MenuDisplay(target, all);
    Expect(full.snapshot->entries[1].children.size() == 1 && !full.snapshot->entries[2].enabled,
        "show-all retains prepared children and native disabled state");
    service.Configure(selective); service.Query(target);
    PumpUntil([&] { const auto view = service.MenuDisplay(target, selective); return view.snapshot && !view.pending && view.snapshot->entries.size() == 1; },
        "disabling show-all cannot leak the formerly unfiltered snapshot");
}

void CheckColdExtensionRecovery()
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"actual.snowmetadata"; std::ofstream(file) << "private";
    ext::Request target; target.paths = {file.wstring()};
    ext::Registration extension; extension.id = "clsid:{11111111-1111-1111-1111-111111111111}";
    extension.kind = ext::RegistrationKind::Handler; extension.contexts = 1; extension.types = {L"*"};
    extension.sources = {L"*\\shellex\\ContextMenuHandlers\\Example"};
    extension.verbs = {"{11111111-1111-1111-1111-111111111111}"}; extension.revision = extension.menuRevision = 17;
    ext::Catalogue inventory; inventory.revision = 7; inventory.rows = {extension};
    ext::Entry action; action.provider = "verb:actual"; action.key = "actual"; action.label = L"Actual command";
    std::atomic_uint globals = 0, checks = 0, sources = 0, aggregates = 0;
    ext::MenuService service(temp.path / L"empty-cache", [&](const ext::Request& request) {
        if (request.sourceClsid.empty()) ++aggregates; else ++sources;
        return ext::QueryWork{[=] { return ext::Reply{true, {action}, {}}; }, {}};
    }, [&] { ++globals; return inventory; }, {}, {}, [&](const auto& seeds) {
        ++checks;
        Expect(seeds.size() == 1 && seeds.front().id == extension.id,
            "cold extension recovery asks the real source-verification path for the selected CLSID only");
        return inventory;
    });
    ext::Preferences prefs; ext::SetCommon(prefs, extension.id, ext::Category::Objects, true);
    service.Configure(prefs); service.Prewarm(target);
    PumpUntil([&] { const auto view = service.MenuDisplay(target, prefs); return !view.pending && view.sourcesOnly; },
        "cold group demand reaches its normal planning/publication boundary");
    const auto view = service.MenuDisplay(target, prefs);
    Expect(view.sources.size() == 1, "CACHE_LOSS_MUST_NOT_DISABLE_GROUP: an extension opt-in works without opening settings or a saved inventory");
    PumpUntil([&] { return service.MenuDisplay(view.sources.front(), prefs).snapshot.has_value(); }, "recovered source roots arrive");
    Expect(checks == 1 && globals == 0 && sources == 1 && aggregates == 0 &&
        service.MenuDisplay(view.sources.front(), prefs).snapshot->entries.size() == 1,
        "source recovery does not pretend to verify a full inventory or synthesize unrelated menus");
}

void CheckWorkerReuseBounds()
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"prepared-retention.txt"; std::ofstream(file) << "private";
    ext::Request request; request.paths = {file.wstring()};
    ext::TakeMenuRegistryChanges(); ext::Session::ReleaseIdleWorker();
    const auto query = [&](bool metadata) {
        request.rootMetadataOnly = metadata;
        ext::Session session(request, 2000, true);
        const auto pid = session.ProcessId();
        std::optional<ext::Reply> reply;
        PumpUntil([&] { if (!reply) reply = session.Poll(); return reply.has_value(); }, "bounded query worker replies");
        Expect(reply->ok, "a healthy metadata/ordinary worker completes its private query");
        if (metadata)
        {
            Expect(reply->entries.size() == 1 && !reply->entries.front().token && reply->entries.front().children.empty() && !session.PollRoots(),
                "metadata consumers receive only roots even from a substituted helper executor");
            bool denied = false;
            try { session.Invoke(1, {}); }
            catch (const std::exception& error) { denied = std::string(error.what()).find("metadata") != std::string::npos; }
            Expect(denied, "metadata sessions reject invocation before touching the helper");
        }
        return pid;
    };
    for (const auto limit : {8u, 32u})
    {
        ext::Session::ReleaseIdleWorker();
        DWORD first = 0;
        for (unsigned i = 0; i <= limit; ++i)
        {
            const auto pid = query(limit == 32);
            if (!i) first = pid;
            Expect(i == limit ? pid != first : pid == first,
                "ordinary workers retire at eight queries; metadata workers retire at thirty-two");
        }
    }
    ext::Session::ReleaseIdleWorker();
    DWORD metadataPid = 0;
    for (unsigned i = 0; i < 9; ++i) metadataPid = query(true);
    Expect(query(false) != metadataPid,
        "a full menu never inherits a metadata worker beyond the ordinary query lifetime");
    ext::Session::ReleaseIdleWorker();

    // Exercise the real parent process budget as well as the service scheduler.
    std::vector<std::unique_ptr<ext::Session>> held;
    for (unsigned i = 0; i < 5; ++i)
    {
        request.rootMetadataOnly = i < 3;
        held.push_back(std::make_unique<ext::Session>(request, 2000, true));
    }
    for (bool metadata : {true, false})
    {
        request.rootMetadataOnly = metadata;
        bool denied = false;
        try { ext::Session excess(request, 2000, true); }
        catch (const std::exception& error) { denied = std::string(error.what()).find("too many") != std::string::npos; }
        Expect(denied, "metadata and complete-menu process budgets both reject overflow");
    }
    for (const auto& session : held)
    {
        std::optional<ext::Reply> reply;
        PumpUntil([&] { if (!reply) reply = session->Poll(); return reply.has_value(); },
            "each separately budgeted worker returns its own reply");
        Expect(reply->ok, "full metadata capacity leaves two healthy actual-menu workers available");
    }
    held.clear();
    ext::Session::ReleaseIdleWorker();
}

void Run()
{
    // Independent fixtures report all broken boundaries in one execution.
    // A failure must not prevent the other new production paths from running.
    std::string failures;
    const auto check = [&](const char* name, const auto& run) {
        try { run(); }
        catch (const std::exception& error) {
            const auto failure = std::string(name) + ": " + error.what();
            std::cerr << "FAILED CASE " << failure << std::endl;
            if (!failures.empty()) failures += "; ";
            failures += failure;
        }
    };
    check("metadata isolation", CheckMetadataIsolation);
    check("source retry", [] { CheckUnrelatedRegistrationDoesNotRetry(false); });
    check("source ambiguity", [] { CheckUnrelatedRegistrationDoesNotRetry(true); });
    check("no format sampling", CheckManagementNeverExpands);
    check("management budget", CheckManagementBudgetIsolation);
    check("registry-only mode switch", CheckManagementRegistrySwitch);
    check("extension and command controls", CheckExtensionAndCommandControls);
    check("software management presentation", CheckSoftwareManagementPresentation);
    check("installed application profiles", CheckInstalledApplicationProfiles);
    check("show all extensions", CheckShowAllPolicy);
    check("cold extension recovery", CheckColdExtensionRecovery);
    check("worker bounds", CheckWorkerReuseBounds);
    if (!failures.empty()) throw std::runtime_error(failures);
}
}
