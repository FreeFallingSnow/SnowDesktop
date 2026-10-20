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
    Write(cachePath / L"catalogue.bin", snowdesktop::settings_ipc::Pack(std::uint32_t(5), catalogue, proof));
    Write(cachePath / L"observed.dat", snowdesktop::settings_ipc::Pack(std::uint32_t(3), observed.rows));
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
    }, [catalogue] { return catalogue; });
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
        PumpUntil([&] { poll(); return full == 1; }, "a failed source triggers one real aggregate compatibility query");
    else
    {
        PumpUntil([&] { poll(); return actions() == 3; }, "second provider appears without waiting for compatibility");
        Expect(items.front().command == firstCommand, "later publication preserves existing command identities");
    }
    if (mode)
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
inline void CancelQueued()
{
    TemporaryDirectory temp;
    const auto file = temp.path / L"selected.txt", cache = temp.path / L"cache";
    std::ofstream(file) << "private"; std::filesystem::create_directory(cache);
    const auto catalogue = Inventory(), observed = Observed(catalogue);
    std::vector<std::pair<std::string, std::uint64_t>> proof;
    for (const auto &source : catalogue.rows) proof.emplace_back(source.id, source.menuRevision);
    Write(cache / L"catalogue.bin", snowdesktop::settings_ipc::Pack(std::uint32_t(5), catalogue, proof));
    Write(cache / L"observed.dat", snowdesktop::settings_ipc::Pack(std::uint32_t(3), observed.rows));
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
    }, [catalogue] { return catalogue; });
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
inline void LearnFromOrdinaryMenus()
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
        if (request.sourceClsid.empty()) { ++aggregate; reply.entries = {canonical, legacy}; }
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
        ext::MenuService service(cache, factory, [catalogue] { return catalogue; });
        service.Configure(prefs); service.Query(first);
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
                return schema == 3 && !plan.full && plan.sources.size() == 2;
            } catch (...) { return false; }
        }, "ordinary use learns and persists canonical and legacy sources without opening settings");
        Expect(aggregate == 1, "source learning does not repeat the aggregate query");
        service.Shutdown();
    }
    // A new service and a different uncached selection must consume the real
    // learned evidence. No test-only catalogue/observed seed is supplied.
    ext::MenuService reopened(cache, factory, [catalogue] { return catalogue; });
    reopened.Configure(prefs);
    ext::Presentation popup(second, prefs, L"", L"", reopened);
    std::vector<snowdesktop::modern_menu::Item> items;
    snowdesktop::modern_menu::Options options; popup.Attach(items, options, 0);
    PumpUntil([&] {
        if (options.pollItems) if (auto updated = options.pollItems(items, true)) items = std::move(*updated);
        return std::count_if(items.begin(), items.end(), [](const auto &item) { return !item.separator; }) == 2;
    }, "new cold selection publishes both learned sources after service reconstruction");
    Expect(aggregate == 1 && reopened.View(second).sourcesOnly,
        "persisted ordinary-use evidence enables demand querying without an aggregate fallback");
    reopened.Shutdown();
}
inline void PreparationAfterRegistryNoise()
{
    TemporaryDirectory temp;
    struct Fixture
    {
        HKEY key = nullptr; std::wstring path; bool owned = false;
        ~Fixture() { if (key) RegCloseKey(key); if (owned) RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    } noise{nullptr, L"Software\\Classes\\Local Settings\\SnowDesktopPreparation-" + temp.path.filename().wstring()};
    DWORD disposition = 0;
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, noise.path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
        nullptr, &noise.key, &disposition) == ERROR_SUCCESS, "create private preparation notification fixture");
    noise.owned = disposition == REG_CREATED_NEW_KEY;
    Expect(noise.owned, "preparation notification fixture has a unique owner");
    struct TraceEnvironment
    {
        std::wstring previous, base;
        TraceEnvironment(const std::wstring &path)
            : base(path)
        {
            wchar_t value[32768]{};
            GetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", value, static_cast<DWORD>(std::size(value)));
            previous = value;
            SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", path.c_str());
        }
        ~TraceEnvironment()
        {
            if (std::uncaught_exceptions())
            {
                try
                {
                    // The service has already joined. Retain its evidence in
                    // CTest output before TemporaryDirectory removes the file.
                    std::ifstream input(base + L"." + std::to_wstring(GetCurrentProcessId()) + L".log", std::ios::binary);
                    std::cerr << "Preparation trace before fixture cleanup:\n" << input.rdbuf();
                }
                catch (...) {}
            }
            SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", previous.empty() ? nullptr : previous.c_str());
        }
    } environment((temp.path / L"preparation").wstring());
    const auto trace = temp.path / (L"preparation." + std::to_wstring(GetCurrentProcessId()) + L".log");
    auto logged = [&](const char *message) {
        std::ifstream input(trace, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        return text.find(message) != std::string::npos;
    };
    ext::MenuTrace("fixture", "reader_baseline");
    {
        std::ifstream reader(trace, std::ios::binary);
        Expect(static_cast<bool>(reader), "open a real concurrent diagnostic reader");
        std::thread writer([] { ext::MenuTrace("fixture", "reader_held"); });
        writer.join();
    }
    Expect(logged("phase=fixture reason=reader_held"),
        "a concurrent diagnostic reader cannot suppress a real preparation trace record");
    std::atomic<unsigned> scans = 0;
    std::atomic<bool> release = false, holdRefresh = false, releaseRefresh = false;
    const auto cache = temp.path / L"cache";
    std::filesystem::create_directory(cache);
    Write(cache / L"observed.dat", snowdesktop::settings_ipc::Pack(std::uint32_t(3), Observed(Inventory()).rows));
    // Release the held reader before service destruction, including assertion
    // failures. The real STA/process preparation remains under test.
    ext::MenuService service(cache, {}, [&] {
        if (++scans == 1) while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (holdRefresh) while (!releaseRefresh) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return Inventory();
    });
    struct Release { std::atomic<bool> &value, &refresh; ~Release() { value = true; refresh = true; } } cleanup{release, releaseRefresh};
    ext::Preferences prefs;
    ext::SetCommon(prefs, "verb:unmapped-enabled-command", ext::Category::Objects, true);
    for (int i = 0; i < 2; ++i) ext::SetCommon(prefs, Entry(i).provider, ext::Category::Objects, true);
    service.Configure(prefs);
    PumpUntil([&] { return scans == 1; }, "initial preparation inventory starts without an open menu");
    const DWORD value = 1;
    Expect(RegSetValueExW(noise.key, L"Noise", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS,
        "emit an unrelated Classes notification during the held inventory scan");
    PumpUntil([&] { return logged("reason=stale.registry"); }, "the preparation service observes the registry notification");
    release = true;
    PumpUntil([&] { return scans >= 2 && logged("phase=preparation reason=ready"); },
        "pending preparation verifies registry noise and becomes ready without a foreground query");
    Expect(!logged("phase=helper_start"), "background preparation does not need a user query to unblock itself");
    ext::Request request; request.paths = {(temp.path / L"routing-slots.txt").wstring()};
    std::ofstream(request.paths.front()) << "private native scheduler fixture";
    const auto plan = ext::PlanDemandQueries(Inventory(), Observed(Inventory()), prefs, request, 1);
    Expect(plan.full && plan.sources.size() == 2, "mixed native scheduling fixture has two sources and a fallback");
    const auto previousScans = scans.load();
    holdRefresh = true;
    const DWORD laterNoise = 2;
    Expect(RegSetValueExW(noise.key, L"Noise", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&laterNoise), sizeof(laterNoise)) == ERROR_SUCCESS,
        "start unrelated global verification before the cold object query");
    PumpUntil([&] { return scans > previousScans; }, "global verification remains held during the source queries");
    service.Query(request);
    PumpUntil([&] {
        return std::all_of(plan.sources.begin(), plan.sources.end(), [&](const auto &source) {
            const auto view = service.View(source);
            Expect(view.error.empty(), "a reserved prepared worker cannot cause metadata helper startup failure");
            return !view.pending && view.snapshot.has_value();
        });
    }, "fresh verified sources publish independently of the held global inventory and reserved worker");
    Expect(!service.View(request).snapshot, "the aggregate still waits for its own complete registration proof");
    releaseRefresh = true;
    PumpUntil([&] { return service.View(request).snapshot.has_value(); },
        "aggregate fallback completes after global verification without losing the early source results");
    service.Shutdown();
}
inline void Run()
{
    Plan();
    LiveSourceRegistration();
    for (int mode = 0; mode < 3; ++mode) Streams(mode);
    CancelQueued();
    LearnFromOrdinaryMenus();
    PreparationAfterRegistryNoise();
}
}
