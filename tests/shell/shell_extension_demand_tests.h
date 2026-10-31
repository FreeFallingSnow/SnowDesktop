#pragma once
#include "shell/shell_extension_demand.h"
#include "shell/shell_extension_source.h"
#include "shell/shell_extension_diagnostics.h"
#include <exception>

namespace demand_tests
{
namespace ext = snowdesktop::shell_extensions;
inline ext::Catalogue Inventory()
{
    ext::Catalogue result; result.revision = 57;
    for (int i = 0; i < 3; ++i)
    {
        ext::Registration source;
        const auto clsid = "{185581D0-0F34-4419-9439-36D1032D901" + std::to_string(i) + "}";
        source.id = "clsid:" + clsid;
        source.kind = ext::RegistrationKind::Handler;
        source.verbs = {clsid}; source.contexts = 3; source.types = {L"*"};
        source.sources = {L"*\\shellex\\ContextMenuHandlers\\Provider" + std::to_wstring(i),
            L"Directory\\shellex\\ContextMenuHandlers\\Provider" + std::to_wstring(i)};
        source.revision = source.menuRevision = 57;
        result.rows.push_back(std::move(source));
    }
    return result;
}
inline ext::Entry Entry(int index, UINT token = 900)
{
    ext::Entry entry; entry.key = "action" + std::to_string(index);
    entry.provider = "verb:" + entry.key;
    entry.label = L"Independent action " + std::to_wstring(index); entry.token = token;
    return entry;
}
inline ext::Catalogue Observed(const ext::Catalogue &catalogue)
{
    ext::Catalogue result;
    for (int i = 0; i < 3; ++i)
    {
        ext::Registration item; item.display = Entry(i); item.id = item.display.provider;
        item.kind = ext::RegistrationKind::Observed; item.contexts = 3; item.linked = true;
        const auto &id = catalogue.rows[i].id;
        item.sources = {L"routing:3:" + std::wstring(id.begin(), id.end())};
        result.rows.push_back(std::move(item));
    }
    return result;
}
inline void Plan()
{
    auto catalogue = Inventory(), observed = Observed(catalogue);
    ext::Preferences prefs;
    ext::SetCommon(prefs, "verb:action0", ext::Category::Objects, true);
    ext::SetCommon(prefs, "verb:action1", ext::Category::Objects, true);
    ext::Request request; request.paths = {L"C:\\Private\\sample.txt"};
    auto plan = ext::PlanDemandQueries(catalogue, observed, prefs, request, 1);
    Expect(!plan.full && plan.sources.size() == 2 && plan.sources.front().sourceKey == L"*",
        "only the two enabled, proven providers are queried; a disabled third source is omitted");
    auto folder = ext::PlanDemandQueries(catalogue, observed, prefs, request, 2);
    Expect(!folder.full && folder.sources.front().sourceKey == L"Directory",
        "folder selection uses its own initialization key, not the first merged registration");
    observed.rows.front().sources = {L"routing:1:" + std::wstring(catalogue.rows[0].id.begin(), catalogue.rows[0].id.end())};
    Expect(ext::DemandSource(catalogue, observed, observed.rows.front().id, 2) == nullptr,
        "file-only routing evidence cannot authorize a folder query even when observations cover both contexts");
    observed = Observed(catalogue);
    auto mixed = ext::PlanDemandQueries(catalogue, observed, prefs, request, 3);
    Expect(mixed.full && mixed.sources.empty(), "mixed selection cannot borrow a file-only or directory-only key");
    ext::SetOverride(prefs, "verb:action1", ext::Context::File, ext::Visibility::Hide);
    Expect(ext::PlanDemandQueries(catalogue, observed, prefs, request, 1).sources.size() == 1,
        "per-context overrides are applied before dispatch");
    ext::SetCommon(prefs, "verb:unknown", ext::Category::Objects, true);
    plan = ext::PlanDemandQueries(catalogue, observed, prefs, request, 1);
    Expect(plan.full && plan.sources.size() == 1, "unknown commands retain compatibility alongside a useful independent source");
    observed.rows.front().sources.push_back(observed.rows.back().sources.front());
    Expect(ext::PlanDemandQueries(catalogue, observed, prefs, request, 1).sources.empty(),
        "ambiguous ownership is never resolved by a title or application name");
    observed = Observed(catalogue); catalogue.rows.front().systemEnabled = false;
    Expect(ext::PlanDemandQueries(catalogue, observed, prefs, request, 1).sources.empty(),
        "system-disabled providers are not activated through their stored attribution");
    catalogue = Inventory(); observed.rows.front().display.key.clear();
    Expect(ext::PlanDemandQueries(catalogue, observed, prefs, request, 1).sources.empty(),
        "a caption-only command cannot bypass aggregate command identification");
    auto &legacy = observed.rows.front();
    legacy.id = legacy.display.provider = "menu:legacy";
    legacy.display.width = legacy.display.height = 1; legacy.display.pixels = {1, 2, 3, 255};
    ext::SetCommon(prefs, legacy.id, ext::Category::Objects, true);
    Expect(ext::PlanDemandQueries(catalogue, observed, prefs, request, 1).sources.size() == 1,
        "an exact legacy icon/tree attribution can route a command without a canonical verb");
    legacy.sources.clear();
    Expect(ext::PlanDemandQueries(catalogue, observed, prefs, request, 1).sources.empty(),
        "an icon and caption without verified provider evidence cannot route a legacy command");
    observed = Observed(catalogue); catalogue.rows.front().sources = {L"Other.ProgID\\shellex\\ContextMenuHandlers\\Provider"};
    Expect(ext::PlanDemandQueries(catalogue, observed, prefs, request, 1).sources.empty(),
        "unknown ProgID applicability preserves the aggregate path");
}
inline void Write(const std::filesystem::path &path, const snowdesktop::settings_ipc::Bytes &bytes)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Expect(static_cast<bool>(output), "write isolated demand query evidence");
}
inline void Streams(int mode)
{
    namespace menu = snowdesktop::modern_menu;
    TemporaryDirectory temp;
    const auto file = temp.path / L"selected.txt", cachePath = temp.path / L"cache";
    std::ofstream(file) << "isolated target";
    std::filesystem::create_directory(cachePath);
    auto catalogue = Inventory(); const auto observed = Observed(catalogue);
    std::vector<std::pair<std::string, std::uint64_t>> proof;
    for (const auto &row : catalogue.rows) proof.emplace_back(row.id, row.menuRevision);
    Write(cachePath / L"catalogue.bin", snowdesktop::settings_ipc::Pack(std::uint32_t(6), catalogue, proof));
    Write(cachePath / L"observed.dat", snowdesktop::settings_ipc::Pack(std::uint32_t(4), observed.rows));
    ext::Request request; request.paths = {file.wstring()};
    ext::Preferences prefs;
    ext::SetCommon(prefs, "verb:action0", ext::Category::Objects, true);
    ext::SetCommon(prefs, "verb:action1", ext::Category::Objects, true);
    if (mode == 1) ext::SetCommon(prefs, "verb:compatibility", ext::Category::Objects, true);
    std::atomic<unsigned> first = 0, second = 0, full = 0, invoked = 0;
    std::atomic<bool> fullAfterSources = false;
    std::atomic<bool> releaseSecond = false, releaseFull = false, completed = false, succeeded = false;
    ext::MenuService service(cachePath, [&](const ext::Request &target) -> ext::QueryWork {
        if (target.paths != request.paths) return {[] { return ext::Reply{true, {}, {}}; }, {}};
        if (!target.sourceClsid.empty())
        {
            const int index = target.sourceClsid == std::wstring(catalogue.rows[0].verbs[0].begin(), catalogue.rows[0].verbs[0].end()) ? 0 : 1;
            if (index == 0) ++first; else ++second;
            return {[&, index]() -> std::optional<ext::Reply> {
                if (index == 1 && !releaseSecond) return {};
                if (mode == 2 && index == 1) return ext::Reply{false, {}, "isolated provider failed"};
                return ext::Reply{true, {Entry(index)}, {}};
            }, [](UINT, POINT) { throw std::runtime_error("metadata source must never invoke"); }};
        }
        fullAfterSources = first >= 1 && second >= 1;
        ++full;
        return {[&]() -> std::optional<ext::Reply> {
            if (!releaseFull) return {};
            auto compatibility = Entry(2, 79); compatibility.key = "compatibility"; compatibility.provider = "verb:compatibility";
            return ext::Reply{true, {Entry(0, 77), Entry(1, 78), compatibility}, {}};
        }, [&](UINT token, POINT) { invoked = token; }};
    }, [catalogue] { return catalogue; }, {}, {}, [catalogue](const auto &seeds) { return shell_service_fixture::Sources(catalogue, seeds); });
    service.Configure(prefs);
    ext::Presentation popup(request, prefs, L"", L"", service, [&](bool ok) { succeeded = ok; completed = true; });
    menu::Item more; more.command = 7; more.label = L"More";
    std::vector<menu::Item> items{more}; menu::Options options;
    popup.Attach(items, options, 7);
    Expect(static_cast<bool>(options.pollItems), "cold independent streams subscribe without blocking the first menu");
    auto poll = [&] {
        if (auto changed = options.pollItems(items, true)) items = std::move(*changed);
    };
    auto actions = [&] { return std::count_if(items.begin(), items.end(), [](const auto &item) { return !item.separator; }); };
    PumpUntil([&] { poll(); return actions() == 2; }, "first provider publishes while another provider remains held");
    Expect(items.front().label == Entry(0).label && items.back().command == 7 &&
        first == 1 && (mode == 1 ? full <= 1 : full == 0) && !options.pollItemsFinished(),
        "independent publication preserves the More footer and does not wait for the slow stream");
    const auto firstCommand = items.front().command;
    PumpUntil([&] { return second == 1; }, "second requested provider is scheduled");
    if (mode == 1) {
        PumpUntil([&] { return full == 1; }, "compatibility fallback starts after the independent streams");
        Expect(fullAfterSources, "known enabled sources are dispatched before the compatibility aggregate");
    }
    releaseSecond = true;
    if (mode == 2)
    {
        PumpUntil([&] { poll(); return options.pollItemsFinished(); }, "a failed source finishes without an automatic aggregate query");
        Expect(full == 0 && actions() == 2 && items.front().command == firstCommand,
            "source failure preserves the successful command and cannot silently broaden the query to hidden handlers");
    }
    else
    {
        PumpUntil([&] { poll(); return actions() == 3; }, "second provider appears without waiting for compatibility");
        Expect(items.front().command == firstCommand, "later publication preserves existing command identities");
    }
    if (mode == 1)
    {
        releaseFull = true;
        PumpUntil([&] { poll(); return options.pollItemsFinished(); }, "compatibility stream completes");
        Expect(actions() == (mode == 1 ? 4 : 3) && items.front().command == firstCommand && full == 1,
            "aggregate fallback adds missing commands without duplicating or renumbering displayed source rows");
    }
    else
    {
        Expect(options.pollItemsFinished() && !service.View(request).snapshot,
            "source-only completion never publishes an incomplete aggregate snapshot");
        ext::MenuSnapshotCache disk(cachePath);
        for (const auto &[saved, reply] : disk.Warm())
        {
            (void)reply;
            Expect(saved.paths != request.paths || !saved.sourceClsid.empty(),
                "partial provider snapshots remain separate on disk");
        }
    }
    releaseFull = true;
    Expect(popup.Invoke(firstCommand, {}), "displayed independent command reaches normal execution routing");
    PumpUntil([&] { return completed.load(); }, "fresh aggregate execution completes");
    Expect(succeeded && invoked == 77, "click uses the fresh aggregate token, never source token 900");
    completed = false; succeeded = true; invoked = 0;
    ext::SetCommon(prefs, "verb:action0", ext::Category::Objects, false);
    service.Configure(prefs);
    Expect(popup.Invoke(firstCommand, {}), "an already open popup still rechecks current visibility at execution");
    PumpUntil([&] { return completed.load(); }, "disabled command callback completes");
    Expect(!succeeded && invoked == 0, "disabling a published command prevents its invocation");
    service.Shutdown();
}
inline void WarmWorkerLifetime()
{
    ext::Session::ReleaseIdleWorker();
    DWORD firstProcess = 0;
    for (unsigned query = 0; query < 9; ++query)
    {
        ext::Request request; request.paths = {query == 0 ? L"synthetic-first" : L"synthetic-second"};
        ext::Session session(request, 8000, true);
        const auto process = session.ProcessId();
        if (!query) firstProcess = process;
        else if (query < 8) Expect(process == firstProcess, "healthy opted-in queries retain the owning STA and loaded providers");
        else Expect(process != firstProcess, "a warm worker retires after its bounded query lifetime");
        std::optional<ext::Reply> reply;
        PumpUntil([&] { if (!reply) reply = session.Poll(); return reply.has_value(); }, "warm worker completes the current selection");
        Expect(reply->ok && !reply->entries.empty() && reply->entries.front().key == (query ? "second" : "first"),
            "worker reuse returns a fresh result for the current target instead of the previous menu");
    }
    DWORD failedProcess = 0;
    {
        ext::Request request; request.paths = {L"synthetic-hang"};
        ext::Session session(request, 100, true); failedProcess = session.ProcessId();
        std::optional<ext::Reply> reply;
        PumpUntil([&] { if (!reply) reply = session.Poll(); return reply.has_value(); }, "a stalled warm worker retains its query deadline");
        Expect(!reply->ok, "a timed-out warm worker cannot return a successful cached result");
    }
    {
        ext::Request request; request.paths = {L"synthetic-second"};
        ext::Session session(request, 8000, true);
        Expect(session.ProcessId() != failedProcess, "a failed or timed-out worker is never reused");
        std::optional<ext::Reply> reply;
        PumpUntil([&] { if (!reply) reply = session.Poll(); return reply.has_value(); }, "a healthy worker replaces the timed-out worker");
        Expect(reply->ok, "a prior worker failure does not poison the next selection");
    }
    ext::Session::ReleaseIdleWorker();
}
inline void QueuedWorkAfterStalledWarmWorker()
{
    TemporaryDirectory temp;
    const auto poisoned = temp.path / L"pool-idle-poison.txt";
    const auto blocked = temp.path / L"pool-next.txt";
    const auto following = temp.path / L"pool-following.txt";
    for (const auto& path : {poisoned, blocked, following}) std::ofstream(path) << "private";
    std::atomic<DWORD> firstProcess = 0, blockedProcess = 0, followingProcess = 0;
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request& request) {
        const bool next = request.paths.front() == blocked.wstring();
        auto session = std::make_shared<ext::Session>(request, next ? 100 : 2500, true);
        if (request.paths.front() == poisoned.wstring()) firstProcess = session->ProcessId();
        else if (next) blockedProcess = session->ProcessId();
        else followingProcess = session->ProcessId();
        return ext::QueryWork{[session] { return session->Poll(); }, {}};
    }, [] { return ext::Catalogue{}; });
    ext::Preferences prefs; ext::SetCommon(prefs, "verb:first", ext::Category::Objects, true);
    service.Configure(prefs);
    ext::Request first; first.paths = {poisoned.wstring()}; service.Query(first);
    PumpUntil([&] { return service.View(first).snapshot.has_value(); }, "a worker succeeds before its vendor callback stalls");
    const auto callbackDeadline = GetTickCount64() + 100;
    while (GetTickCount64() < callbackDeadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    ext::Request second; second.paths = {blocked.wstring()}; service.Query(second);
    PumpUntil([&] { const auto view = service.View(second); return !view.pending && !view.error.empty(); },
        "the next query retires an already pooled worker whose idle callback is stuck");
    Expect(firstProcess && blockedProcess == firstProcess, "the controlled stall affects a reused worker");
    ext::Request third; third.paths = {following.wstring()}; service.Query(third);
    PumpUntil([&] { return service.View(third).snapshot.has_value(); },
        "queued new selections continue after the stalled warm worker is discarded");
    Expect(followingProcess && followingProcess != blockedProcess,
        "discarding one stalled warm worker restores the real scheduler with a new process");
    service.Shutdown();
}
inline void CancelQueued()
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"selected.txt", cache = temp.path / L"cache";
    std::ofstream(file) << "private"; std::filesystem::create_directory(cache);
    const auto catalogue = Inventory(), observed = Observed(catalogue);
    std::vector<std::pair<std::string, std::uint64_t>> proof;
    for (const auto &source : catalogue.rows) proof.emplace_back(source.id, source.menuRevision);
    Write(cache / L"catalogue.bin", snowdesktop::settings_ipc::Pack(std::uint32_t(6), catalogue, proof));
    Write(cache / L"observed.dat", snowdesktop::settings_ipc::Pack(std::uint32_t(4), observed.rows));
    ext::Request request; request.paths = {file.wstring()};
    ext::Preferences prefs;
    for (int i = 0; i < 3; ++i) ext::SetCommon(prefs, Entry(i).provider, ext::Category::Objects, true);
    const auto plan = ext::PlanDemandQueries(catalogue, observed, prefs, request, 1);
    std::atomic<unsigned> started = 0, disabledCalls = 0;
    std::atomic<bool> release = false;
    ext::MenuService service(cache, [&](const ext::Request &target) -> ext::QueryWork {
        ++started;
        const bool disabled = target.sourceClsid == plan.sources.back().sourceClsid;
        if (disabled) ++disabledCalls;
        return {[&]() -> std::optional<ext::Reply> {
            if (!release) return {};
            return ext::Reply{true, {Entry(0)}, {}};
        }, {}};
    }, [catalogue] { return catalogue; }, {}, {}, [catalogue](const auto &seeds) { return shell_service_fixture::Sources(catalogue, seeds); });
    service.Configure(prefs); service.Query(request);
    PumpUntil([&] { return started == 2; }, "two source sessions occupy the bounded worker pool");
    Expect(disabledCalls == 0 && service.View(plan.sources.back()).pending,
        "third source is queued behind the two active sessions");
    ext::SetCommon(prefs, Entry(2).provider, ext::Category::Objects, false);
    service.Configure(prefs);
    Expect(!service.View(plan.sources.back()).pending && !service.MenuEnabled(plan.sources.back(), prefs),
        "disabling a source cancels its queued work even while other sources stay enabled");
    release = true;
    PumpUntil([&] { return !service.View(plan.sources.front()).pending && !service.View(plan.sources[1]).pending; },
        "remaining enabled sources complete normally");
    Expect(disabledCalls == 0, "the disabled queued provider is never activated");
    service.Shutdown();
}
inline void LiveSourceRegistration()
{
    TemporaryDirectory temp;
    struct Fixture {
        HKEY key = nullptr; std::wstring path; bool owned = false;
        ~Fixture() { if (key) RegCloseKey(key); if (owned) RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    } registry{nullptr, L"Software\\SnowDesktopTests\\SourceRegistration-" + temp.path.filename().wstring()};
    DWORD disposition = 0;
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, registry.path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
        nullptr, &registry.key, &disposition) == ERROR_SUCCESS, "create isolated source registration root");
    registry.owned = disposition == REG_CREATED_NEW_KEY;
    Expect(registry.owned, "isolated source root is uniquely owned");
    const std::wstring clsid = L"{185581d0-0f34-4419-9439-36d1032d9010}";
    Expect(!ext::SourceRegistered(clsid, L"*", registry.key), "an absent source scope cannot authorize metadata");
    HKEY key = nullptr;
    const std::wstring path = L"*\\shellex\\ContextMenuHandlers\\Provider";
    Expect(RegCreateKeyExW(registry.key, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS,
        "create an exact scope registration");
    const auto write = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(clsid.c_str()),
        static_cast<DWORD>((clsid.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    Expect(write == ERROR_SUCCESS && ext::SourceRegistered(clsid, L"*", registry.key), "a matching live CLSID is accepted");
    Expect(!ext::SourceRegistered(clsid, L"Directory", registry.key), "file registration cannot prove a folder scope");
    Expect(!ext::SourceRegistered(L"{185581d0-0f34-4419-9439-36d1032d9011}", L"*", registry.key),
        "a different live provider cannot borrow a former routing record");
    RegDeleteTreeW(registry.key, path.c_str());
    Expect(!ext::SourceRegistered(clsid, L"*", registry.key), "removing a registration immediately revokes the proof");
    const auto named = L"*\\shellex\\ContextMenuHandlers\\" + clsid;
    Expect(RegCreateKeyExW(registry.key, named.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS,
        "create a CLSID-named registration without a default value");
    RegCloseKey(key);
    Expect(ext::SourceRegistered(clsid, L"*", registry.key), "CLSID-named registrations follow the catalogue rules");
}
inline void OrdinaryMenusDoNotProbe(bool previouslyInspected)
{
    TemporaryDirectory temp;
    const auto cache = temp.path / L"cache";
    ext::Request managed; managed.paths = {(temp.path / L"managed.txt").wstring()};
    std::ofstream(managed.paths.front()) << "private management target";
    auto catalogue = Inventory();
    for (auto &source : catalogue.rows) source.application = {"test-app", L"Test application"};
    auto canonical = Entry(0); canonical.key = "unmapped"; canonical.provider = "verb:unmapped";
    auto legacy = Entry(1); legacy.key.clear(); legacy.provider = "menu:unmapped-legacy";
    legacy.width = legacy.height = 1; legacy.pixels = {7, 8, 9, 255};
    auto management = Entry(2);
    ext::Preferences prefs;
    ext::SetCommon(prefs, canonical.provider, ext::Category::Objects, true);
    ext::SetCommon(prefs, legacy.provider, ext::Category::Objects, true);
    std::atomic_uint probes = 0, aggregates = 0, invokes = 0;
    auto observe = [&](unsigned expected) {
        // Observe absence across several real scheduler passes, rather than
        // racing the source dispatch immediately after snapshot publication.
        const auto until = GetTickCount64() + 150;
        do
        {
            Expect(probes == expected, "ORDINARY_UNKNOWN_MUST_NOT_PROBE: ordinary display/refresh/execution cannot activate unrelated handlers");
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
            MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() < until);
    };
    for (int round = 0; round < 2; ++round)
    {
        std::vector<ext::Request> ordinary;
        for (int i = 0; i < 3; ++i)
        {
            ext::Request request;
            request.paths = {(temp.path / (L"ordinary-" + std::to_wstring(round) + L"-" + std::to_wstring(i) + L".txt")).wstring()};
            std::ofstream(request.paths.front()) << "private ordinary target";
            ordinary.push_back(std::move(request));
        }
        ext::MenuService service(cache, [&](const ext::Request &request) {
            ext::Reply reply{true, {}, {}};
            if (!request.sourceClsid.empty()) { ++probes; reply.entries = {management}; }
            else if (request.paths == managed.paths) reply.entries = {management};
            else if (std::any_of(ordinary.begin(), ordinary.end(), [&](const auto &target) { return target.paths == request.paths; }))
            { ++aggregates; reply.entries = {canonical, legacy}; }
            return ext::QueryWork{[reply] { return std::optional{reply}; },
                [&](UINT token, POINT) { Expect(token == canonical.token, "ordinary execution resolves its fresh visible token"); ++invokes; }};
        }, [catalogue] { return catalogue; }, {}, {}, [catalogue](const auto &seeds) { return shell_service_fixture::Sources(catalogue, seeds); });
        service.Configure(prefs);
        if (previouslyInspected && round == 0)
        {
            service.Inspect(managed);
            auto metadata = managed; metadata.rootMetadataOnly = true;
            PumpUntil([&] { return !service.Inspect(managed).scanning && service.View(metadata).snapshot.has_value(); },
                "explicit management root metadata finishes before ordinary work");
            Expect(probes == 0, "settings inspection never adds source attribution work");
            service.EndInspection();
        }
        const auto before = probes.load(), beforeAggregates = aggregates.load();
        service.Query(ordinary[0]);
        PumpUntil([&] { const auto view = service.MenuDisplay(ordinary[0], prefs);
            return view.snapshot && !view.pending && view.snapshot->entries.size() == 2; },
            "unmapped canonical and legacy opt-ins retain their ordinary aggregate entries");
        observe(before);
        service.Prewarm(ordinary[1]);
        PumpUntil([&] { return !service.View(ordinary[1]).pending; }, "unknown prewarm finishes without querying an aggregate");
        Expect(!service.View(ordinary[1]).snapshot && aggregates == beforeAggregates + 1,
            "UNKNOWN_PREWARM_ZERO_ROOT: background work cannot read hidden full roots");
        observe(before);
        service.Query(ordinary[1]);
        PumpUntil([&] { auto view = service.View(ordinary[1]); return view.snapshot && !view.pending; },
            "a real Menu immediately promotes a previously skipped unknown prewarm");
        const auto beforeInvokes = invokes.load();
        service.Execute(ordinary[2], ext::AppendReference({}, canonical), {}, {});
        PumpUntil([&] { return invokes > beforeInvokes; }, "ordinary command execution completes without metadata learning");
        observe(before);
        service.Invalidate(ordinary[0]);
        PumpUntil([&] { const auto view = service.View(ordinary[0]);
            return aggregates == beforeAggregates + 4 && view.snapshot && !view.pending; },
            "ordinary forced refresh preserves selected commands without handler attribution");
        observe(before);
        Expect(aggregates == beforeAggregates + 4, "unknown opt-ins use their exact aggregate fallback without extra catalogue queries");
        service.Shutdown();
    }
}
inline void LearnFromManagement()
{
    TemporaryDirectory temp;
    struct Fixture {
        HKEY key = nullptr; std::wstring path; bool owned = false;
        ~Fixture() { if (key) RegCloseKey(key); if (owned) RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    } noise{nullptr, L"Software\\Classes\\Local Settings\\SnowDesktopRouting-" + temp.path.filename().wstring()};
    DWORD disposition = 0;
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, noise.path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
        nullptr, &noise.key, &disposition) == ERROR_SUCCESS, "create private routing notification fixture");
    noise.owned = disposition == REG_CREATED_NEW_KEY;
    Expect(noise.owned, "routing notification fixture has a unique owner");
    const auto cache = temp.path / L"cache";
    ext::Request first, second;
    first.paths = {(temp.path / L"first.txt").wstring()};
    second.paths = {(temp.path / L"second.txt").wstring()};
    std::ofstream(first.paths.front()) << "private";
    std::ofstream(second.paths.front()) << "private";
    auto catalogue = Inventory(); catalogue.rows.resize(2);
    auto canonical = Entry(0), legacy = Entry(1);
    legacy.key.clear(); legacy.provider = "menu:legacy-enabled";
    legacy.width = legacy.height = 1; legacy.pixels = {9, 8, 7, 255};
    const std::wstring clsid0(catalogue.rows[0].verbs.front().begin(), catalogue.rows[0].verbs.front().end());
    std::atomic<unsigned> aggregate = 0, probes = 0;
    std::atomic<bool> notified = false;
    ext::Preferences prefs;
    ext::SetCommon(prefs, canonical.provider, ext::Category::Objects, true);
    ext::SetCommon(prefs, legacy.provider, ext::Category::Objects, true);
    auto factory = [&](const ext::Request &request) -> ext::QueryWork {
        ext::Reply reply{true, {}, {}};
        if (request.sourceClsid.empty())
        {
            if (request.paths == first.paths || request.paths == second.paths)
            { ++aggregate; reply.entries = {canonical, legacy}; }
        }
        else {
            ++probes; reply.entries = {request.sourceClsid == clsid0 ? canonical : legacy};
            if (!notified.exchange(true)) {
                const DWORD value = 1;
                Expect(RegSetValueExW(noise.key, L"Noise", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS,
                    "invalidate the first in-flight routing probe with an unrelated notification");
            }
        }
        return {[reply] { return reply; }, {}};
    };
    {
        ext::MenuService service(cache, factory, [catalogue] { return catalogue; }, {}, {}, [catalogue](const auto &seeds) { return shell_service_fixture::Sources(catalogue, seeds); });
        service.Configure(prefs); service.Query(first, ext::QueryPriority::Inspect);
        PumpUntil([&] {
            if (probes < 2) return false;
            try {
                std::ifstream in(cache / L"observed.dat", std::ios::binary);
                std::vector<char> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                snowdesktop::settings_ipc::Bytes bytes(data.size());
                if (!data.empty()) memcpy(bytes.data(), data.data(), data.size());
                auto [schema, rows] = snowdesktop::settings_ipc::Unpack<std::tuple<std::uint32_t, std::vector<ext::Registration>>>(bytes);
                ext::Catalogue observed; observed.rows = std::move(rows);
                const auto plan = ext::PlanDemandQueries(catalogue, observed, prefs, second, 1);
                return schema == 4 && !plan.full && plan.sources.size() == 2;
            } catch (...) { return false; }
        }, "explicit complete source proof persists canonical and legacy routing evidence");
        Expect(aggregate == 1, "management source learning does not repeat the selected aggregate query");
        service.Shutdown();
    }
    // A new service and a different uncached selection must consume the real
    // learned evidence. No test-only catalogue/observed seed is supplied.
    ext::MenuService reopened(cache, factory, [catalogue] { return catalogue; }, {}, {}, [catalogue](const auto &seeds) { return shell_service_fixture::Sources(catalogue, seeds); });
    reopened.Configure(prefs);
    ext::Presentation popup(second, prefs, L"", L"", reopened);
    std::vector<snowdesktop::modern_menu::Item> items;
    snowdesktop::modern_menu::Options options; popup.Attach(items, options, 0);
    PumpUntil([&] {
        if (options.pollItems) if (auto updated = options.pollItems(items, true)) items = std::move(*updated);
        return std::count_if(items.begin(), items.end(), [](const auto &item) { return !item.separator; }) == 2;
    }, "new cold selection publishes both learned sources after service reconstruction");
    Expect(aggregate == 1 && reopened.View(second).sourcesOnly,
        "persisted management evidence enables ordinary demand querying without an aggregate fallback");
    reopened.Shutdown();
}
// With no selected provider there is no reason to inventory or prepare any
// extension. Explicit management is the sole full-reader demand and its late
// result must not revive work after the page closes.
inline void InspectionLifecycle()
{
    TemporaryDirectory temp;
    ext::Request target; target.paths = {(temp.path / L"selected.txt").wstring()};
    std::ofstream(target.paths.front()) << "private";
    std::atomic_uint fullReads = 0, sourceReads = 0, factories = 0, scopes = 0, callbacks = 0;
    std::atomic_bool hold = true, entered = false, readerExited = false;
    struct Trace
    {
        std::wstring previous; std::filesystem::path file;
        explicit Trace(const std::filesystem::path &temp)
        {
            wchar_t value[32768]{}; GetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", value, 32768); previous = value;
            const auto base = temp / L"closed-inspection";
            file = base.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".log";
            SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", base.c_str());
        }
        bool Discarded() const
        {
            std::ifstream input(file, std::ios::binary);
            const std::string value{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            return value.find("reason=closed_generation_discarded") != std::string::npos;
        }
        ~Trace() { SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", previous.empty() ? nullptr : previous.c_str()); }
    } trace(temp.path);
    struct Notification
    {
        HKEY key = nullptr; std::wstring path;
        explicit Notification(const std::filesystem::path &temp)
        {
            path = L"Software\\Classes\\SnowDesktopClosedInspect-" + temp.filename().wstring();
            DWORD created = 0;
            Expect(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                nullptr, &key, &created) == ERROR_SUCCESS && created == REG_CREATED_NEW_KEY, "own closed-inspection notification key");
        }
        void Signal()
        {
            const DWORD value = 1;
            Expect(RegSetValueExW(key, L"Sequence", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS,
                "notify registry after settings close with all switches off");
        }
        ~Notification() { if (key) { RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); } }
    } notification(temp.path);
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request &) {
        ++factories; return ext::QueryWork{[] { return ext::Reply{true, {}, {}}; }, {}};
    }, [&] {
        ++fullReads; entered = true;
        const auto deadline = GetTickCount64() + 5000;
        while (hold && GetTickCount64() < deadline) Sleep(1);
        readerExited = true; return Inventory();
    }, [&] { ++scopes; return std::uint64_t(57); },
       [&](ext::Context) { ++scopes; return std::uint64_t(57); },
       [&](const auto &seeds) { ++sourceReads; return shell_service_fixture::Sources(Inventory(), seeds); });
    struct Release { std::atomic_bool &gate; ~Release() { gate = false; } } cleanup{hold};
    service.Configure({}); service.Query(target); service.Prewarm(target);
    service.Execute(target, {}, {}, [&](bool ok) { Expect(!ok, "all-off execution rejects before querying"); ++callbacks; });
    Expect(callbacks == 1 && !fullReads && !sourceReads && !factories && !scopes,
        "ALL_OFF_ZERO_EXTENSION_WORK: no reader, factory, scope verification or execution dispatch");
    service.Inspect(target);
    PumpUntil([&] { return entered.load(); }, "explicit all-off management starts its full reader");
    service.EndInspection(); hold = false;
    PumpUntil([&] { return readerExited && trace.Discarded(); },
        "worker consumes the closed custom reader result without publishing its generation");
    const auto stoppedFactories = factories.load(), stoppedReads = fullReads.load();
    notification.Signal();
    service.Query(target); service.Prewarm(target);
    service.Execute(target, {}, {}, [&](bool ok) { Expect(!ok, "closed all-off execution stays rejected"); ++callbacks; });
    const auto quietUntil = GetTickCount64() + 150;
    do
    {
        Expect(factories == stoppedFactories && fullReads == stoppedReads && sourceReads == 0 && scopes == 0,
            "CLOSED_ALL_OFF_STAYS_QUIET: late results, registry notifications and ordinary calls cannot rearm work");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < quietUntil);
    // Reopening starts a fresh generation after the canceled scan drains.
    service.Inspect(target);
    PumpUntil([&] { return fullReads >= 2 && !service.Inspect(target).scanning; },
        "reopening explicit management drains old generation and runs a fresh full reader");
    service.EndInspection();
    Expect(callbacks == 2 && sourceReads == 0 && scopes == 0 && factories >= stoppedFactories,
        "late full-reader result cannot start ordinary source work or steal rejected callbacks");
    service.Shutdown();
}
// A held old management reply crosses EndInspection before a new same-key
// Execute. This preserves the production queue/sequence/callback boundaries.
inline void ClosedInspectionCompletion(int mode)
{
    TemporaryDirectory temp;
    ext::Request request; request.paths = {(temp.path / L"held.txt").wstring()};
    std::ofstream(request.paths.front()) << "private";
    auto action = Entry(0, 71);
    std::atomic_uint factories = 0, callbacks = 0, invoked = 0;
    std::atomic_bool oldPollEntered = false, release = false, oldRetired = false, succeeded = false;
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request &) {
        const auto number = ++factories;
        auto lease = std::shared_ptr<int>(new int, [&, number](int *value) {
            delete value; if (number == 1) oldRetired = true;
        });
        return ext::QueryWork{[&, number, lease]() -> std::optional<ext::Reply> {
            if (number == 1)
            {
                oldPollEntered = true;
                const auto deadline = GetTickCount64() + 5000;
                while (!release && GetTickCount64() < deadline) Sleep(1);
            }
            auto item = action; item.token = number == 1 ? 71 : 72;
            return ext::Reply{true, {item}, {}};
        }, [&](UINT token, POINT) { invoked = token; }};
    }, [] { ext::Catalogue value; value.revision = 1; return value; });
    struct Release { std::atomic_bool &gate; ~Release() { gate = true; } } cleanup{release};
    ext::Preferences prefs; ext::SetCommon(prefs, action.provider, ext::Category::Objects, true);
    service.Configure(prefs); service.Query(request, ext::QueryPriority::Inspect);
    PumpUntil([&] { return oldPollEntered.load(); }, "old management poll is held after dispatch");
    if (mode == 2)
        service.Execute(request, ext::AppendReference({}, action), {}, [&](bool ok) { succeeded = ok; ++callbacks; });
    service.EndInspection();
    if (mode == 0)
        service.Execute(request, ext::AppendReference({}, action), {}, [&](bool ok) { succeeded = ok; ++callbacks; });
    else if (mode == 1)
        std::ofstream(request.paths.front(), std::ios::app) << " change target before old reply";
    else
        service.Configure({});
    release = true;
    PumpUntil([&] { return oldRetired && (mode == 1 || callbacks == 1); }, "closed old work retires without stranding accepted execution");
    if (mode == 0)
        Expect(callbacks == 1 && succeeded && invoked == 72 && factories == 2,
            "CLOSED_OLD_SEQUENCE_CANNOT_STEAL_NEW_CLICK: fresh Execute owns its token and single callback");
    else if (mode == 1)
        Expect(callbacks == 0 && factories == 1 && !service.View(request).pending,
            "CLOSED_MANAGEMENT_CANNOT_REQUEUE: changed old target cannot create an ordinary query after End");
    else
        Expect(callbacks == 1 && !succeeded && !invoked && factories == 1,
            "accepted same-sequence execution completes once after close and rechecks current visibility");
    service.Shutdown();
}
inline void UnprovenSourceFallback(int mode)
{
    TemporaryDirectory temp;
    auto truth = Inventory(); truth.rows.resize(1);
    auto &source = truth.rows.front();
    if (mode == 0) { source.kind = ext::RegistrationKind::Packaged; source.id = "package:unverified"; }
    else { source.kind = ext::RegistrationKind::Verb; source.id = "reg:*\\shell\\unverified"; source.verbs = {"unverified"}; }
    const auto cache = temp.path / L"cache";
    shell_service_fixture::SeedMetadata(cache, truth);
    ext::Request request; request.paths = {(temp.path / L"selected.txt").wstring()};
    std::ofstream(request.paths.front()) << "private";
    std::atomic_uint fullReads = 0, sourceReads = 0, aggregates = 0;
    auto action = Entry(0); action.provider = source.id; action.registration = source.id;
    ext::MenuService service(cache, [&](const ext::Request &) {
        ++aggregates; return ext::QueryWork{[action] { return ext::Reply{true, {action}, {}}; }, {}};
    }, [&] { ++fullReads; return truth; }, {}, {}, [&](const auto &seeds) {
        ++sourceReads;
        Expect(mode != 0, "unsupported packaged registration cannot enter exact-path inventory");
        if (mode == 2) throw std::runtime_error("controlled exact reader failure");
        if (mode == 3) { auto disabled = shell_service_fixture::Sources(truth, seeds); disabled.rows.front().systemEnabled = false; return disabled; }
        ext::Catalogue missing; missing.revision = 1; return missing;
    });
    ext::Preferences prefs; ext::SetCommon(prefs, source.id, ext::Category::Objects, true);
    service.Configure(prefs); service.Prewarm(request);
    PumpUntil([&] { return !service.View(request).pending; }, "unproven background selection finishes without a root query");
    Expect(aggregates == 0 && fullReads == 0, "unproven source cannot trigger hidden root prewarming or a complete inventory");
    service.Query(request);
    PumpUntil([&] { auto view = service.View(request); return !view.pending && (mode == 3 || view.snapshot.has_value()); },
        "actual menu retains compatibility unless current exact policy proves disabled");
    Expect(aggregates == (mode == 3 ? 0u : 1u) && fullReads == 0 && sourceReads == (mode == 0 ? 0u : 1u),
        "UNPROVEN_IS_NOT_DISABLED: unsupported, missing and failed exact reads preserve the selected root fallback");
    service.Shutdown();
}
inline void KnownSourcePrewarm()
{
    TemporaryDirectory temp;
    auto catalogue = Inventory(); const auto observed = Observed(catalogue);
    const auto cache = temp.path / L"cache";
    shell_service_fixture::SeedMetadata(cache, catalogue);
    Write(cache / L"observed.dat", snowdesktop::settings_ipc::Pack(std::uint32_t(4), observed.rows));
    ext::Request request; request.paths = {(temp.path / L"selected.txt").wstring()};
    std::ofstream(request.paths.front()) << "private";
    std::atomic_uint fullReads = 0, sourceReads = 0, aggregates = 0, sources = 0;
    ext::MenuService service(cache, [&](const ext::Request &target) {
        if (target.sourceClsid.empty()) ++aggregates; else ++sources;
        return ext::QueryWork{[] { return ext::Reply{true, {Entry(0)}, {}}; }, {}};
    }, [&] { ++fullReads; return catalogue; }, {}, {}, [&](const auto &seeds) {
        ++sourceReads; return shell_service_fixture::Sources(catalogue, seeds);
    });
    ext::Preferences prefs; ext::SetCommon(prefs, Entry(0).provider, ext::Category::Objects, true);
    service.Configure(prefs); service.Prewarm(request);
    PumpUntil([&] { const auto view = service.View(request); return !view.pending && view.sourcesOnly &&
        view.sources.size() == 1 && service.View(view.sources.front()).snapshot.has_value(); },
        "proven enabled source may prewarm independently of the aggregate");
    Expect(fullReads == 0 && sourceReads == 1 && aggregates == 0 && sources == 1,
        "KNOWN_PREWARM_IS_SOURCE_ONLY: no unrelated inventory, handler or root is read");
    service.Query(request);
    PumpUntil([&] { return !service.View(request).pending; }, "foreground menu reuses ready source-only selection");
    Expect(aggregates == 0 && sources == 1, "foreground publication reuses the exact proven source result");
    service.Shutdown();
}
inline void LimitedReaderClosedGeneration()
{
    TemporaryDirectory temp;
    auto catalogue = Inventory(); const auto observed = Observed(catalogue);
    const auto cache = temp.path / L"cache";
    shell_service_fixture::SeedMetadata(cache, catalogue);
    Write(cache / L"observed.dat", snowdesktop::settings_ipc::Pack(std::uint32_t(4), observed.rows));
    ext::Request request; request.paths = {(temp.path / L"selected.txt").wstring()};
    std::ofstream(request.paths.front()) << "private";
    std::atomic_uint sourceReads = 0, fullReads = 0, factories = 0;
    std::atomic_bool release = false, firstExited = false;
    ext::MenuService service(cache, [&](const ext::Request &target) {
        ++factories;
        Expect(!target.sourceClsid.empty(), "reopened known selection uses its independently proven source");
        return ext::QueryWork{[] { return ext::Reply{true, {Entry(0)}, {}}; }, {}};
    }, [&] { ++fullReads; return catalogue; }, {}, {}, [&](const auto &seeds) {
        const auto n = ++sourceReads;
        if (n == 1)
        {
            const auto until = GetTickCount64() + 5000;
            while (!release && GetTickCount64() < until) Sleep(1);
            firstExited = true;
        }
        return shell_service_fixture::Sources(catalogue, seeds);
    });
    struct Release { std::atomic_bool &gate; ~Release() { gate = true; } } cleanup{release};
    ext::Preferences prefs; ext::SetCommon(prefs, Entry(0).provider, ext::Category::Objects, true);
    service.Configure(prefs);
    PumpUntil([&] { return sourceReads == 1; }, "ordinary selected-source reader is held in flight");
    service.Configure({}); service.EndInspection(); release = true;
    PumpUntil([&] { return firstExited.load(); }, "custom limited reader returns after its demand closes");
    service.Query(request); service.Prewarm(request);
    Expect(!factories && !fullReads, "closed limited result cannot create menu, discovery or full inventory work");
    service.Configure(prefs); service.Prewarm(request);
    PumpUntil([&] { auto view = service.View(request); return sourceReads == 2 && !view.pending && view.sourcesOnly &&
        view.sources.size() == 1 && service.View(view.sources.front()).snapshot.has_value(); },
        "reenabling drains the old limited future and establishes a fresh cancellable generation");
    Expect(factories == 1 && fullReads == 0 && sourceReads == 2,
        "LATE_LIMITED_RESULT_CANNOT_REARM: one new exact check and selected-source prewarm resume without a full root");
    service.Shutdown();
}
inline void Run()
{
    Plan();
    LiveSourceRegistration();
    for (int mode = 0; mode < 3; ++mode) Streams(mode);
    WarmWorkerLifetime();
    QueuedWorkAfterStalledWarmWorker();
    CancelQueued();
    OrdinaryMenusDoNotProbe(false);
    OrdinaryMenusDoNotProbe(true);
    LearnFromManagement();
    InspectionLifecycle();
    for (int mode = 0; mode < 4; ++mode) UnprovenSourceFallback(mode);
    KnownSourcePrewarm();
    LimitedReaderClosedGeneration();
    for (int mode = 0; mode < 3; ++mode) ClosedInspectionCompletion(mode);
}
}
