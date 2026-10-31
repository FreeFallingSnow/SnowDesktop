#pragma once

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
    const auto selected = service.View(request);
    Expect(metadata == 4 && complete == 1 && !wrongMode,
        "all automatic baseline contexts use metadata; the selected target has one complete query");
    Expect(sources == 0,
        "PROVEN_REGISTRATION_MUST_NOT_PROBE: a known registration with absent application metadata never queries unrelated handlers or packages");
    Expect(selected.snapshot && selected.snapshot->entries.size() == 1 &&
        selected.snapshot->entries.front().children.size() == 1,
        "metadata discovery cannot replace the explicit target's complete children");
    auto metadataRequest = request; metadataRequest.rootMetadataOnly = true;
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
    service.Inspect(metadataRequest, true);
    PumpUntil([&] { return !service.Inspect(request).scanning; },
        "explicit inspection always promotes a metadata request to a complete read");
    Expect(complete == 2 && service.View(request).snapshot->entries.front().children.size() == 1,
        "explicit refresh keeps all children even if its input came from automatic discovery");
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
    service.Inspect(request);
    PumpUntil([&] { return !service.Inspect(request).scanning; }, "initial source proof settles");
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
        PumpUntil([&] { const auto view = service.Inspect(request); return !view.scanning && view.menu.snapshot && view.menu.snapshot->entries.size() == 2; },
            "a new proven registration joins the refreshed actual menu");
        Expect(probes == 1,
            "UNRELATED_ROOT_MUST_NOT_REPROBE: a new proven root cannot retry old unknown-action attribution");
    }
}
void Run()
{
    CheckMetadataIsolation();
    CheckUnrelatedRegistrationDoesNotRetry(false);
    CheckUnrelatedRegistrationDoesNotRetry(true);
}
}
