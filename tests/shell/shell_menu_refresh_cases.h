#pragma once

namespace menu_refresh_tests
{
namespace ext = snowdesktop::shell_extensions;

// Own only a GUID registration root; never overwrite an existing association.
// Subscriptions, scheduling, target checks and publication remain real.
struct Registry
{
    std::wstring path;
    HKEY root = nullptr, signal = nullptr;
    explicit Registry(const TemporaryDirectory &temp)
    {
        path = L"Software\\Classes\\SnowDesktop.MenuRefresh-" + temp.path.filename().wstring();
        DWORD created = 0;
        Expect(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
            nullptr, &root, &created) == ERROR_SUCCESS && created == REG_CREATED_NEW_KEY, "own refresh registry fixture");
        Expect(RegCreateKeyExW(root, L"shell\\probe", 0, nullptr, 0,
            KEY_ALL_ACCESS, nullptr, &signal, nullptr) == ERROR_SUCCESS, "create real notification input");
        // Drain creation / previous fixture teardown before a fresh service
        // takes its baseline from the process-shared notification source.
        ext::TakeMenuRegistryChanges();
    }
    void Notify(unsigned value)
    {
        Expect(RegSetValueExW(signal, L"Sequence", 0, REG_DWORD,
            reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS, "emit registry notification");
    }
    ~Registry()
    {
        if (signal) RegCloseKey(signal);
        if (root) RegCloseKey(root);
        if (root && RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) != ERROR_SUCCESS)
            fputs("REFRESH_FIXTURE_CLEANUP_FAILURE\n", stderr);
    }
};
struct ReleaseOnExit
{
    std::atomic_bool &gate;
    ~ReleaseOnExit() { gate = true; }
};
std::string ReadText(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
struct Trace
{
    std::wstring previous;
    std::filesystem::path file;
    explicit Trace(const TemporaryDirectory &temp)
    {
        wchar_t value[32768]{};
        GetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", value, 32768); previous = value;
        const auto base = temp.path / L"trace";
        file = std::filesystem::path(base.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".log");
        SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", base.c_str());
    }
    bool Has(const char *text) const { return ReadText(file).find(text) != std::string::npos; }
    ~Trace() { SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", previous.empty() ? nullptr : previous.c_str()); }
};
ext::Catalogue Inventory(unsigned revision = 101)
{
    ext::Catalogue value; value.revision = revision;
    ext::Registration provider; provider.id = "clsid:{11111111-1111-1111-1111-111111111111}";
    provider.kind = ext::RegistrationKind::Handler; provider.contexts = 1; provider.types = {L"*"};
    provider.sources = {L"*\\shellex\\ContextMenuHandlers\\RefreshProbe"};
    provider.verbs = {"{11111111-1111-1111-1111-111111111111}"};
    provider.application = {"refresh-app", L"Refresh application"};
    provider.revision = provider.menuRevision = revision;
    value.rows = {provider}; return value;
}

// 0: repeated unrelated notifications; 1: changed registration; 2: superseded
// aggregate; 3: replaced file; 4: disabled configuration. Only external query
// completion and registry reads are gated. No command is executed.
void CheckSourceProof(int mode)
{
    TemporaryDirectory temp;
    Registry registry(temp); Trace trace(temp);
    const auto file = temp.path / L"selected.refresh"; std::ofstream(file) << "private";
    ext::Request request; request.paths = {file.wstring()};
    ext::Entry action; action.provider = "verb:refresh-proof"; action.key = "refresh-proof";
    action.label = L"Observed action"; action.token = 42;
    const ext::Reply reply{true, {action}, {}};
    std::atomic_uint starts = 0, reads = 0, polls = 0, aggregates = 0;
    std::atomic_bool allowRead = false, allowReply = false, firstDone = false;
    std::atomic_uint version = 101;
    const auto cache = temp.path / L"cache";
    ext::MenuService service(cache, [&](const ext::Request &target) {
        const bool source = !target.sourceClsid.empty();
        const unsigned n = source ? ++starts : ++aggregates;
        auto lease = std::shared_ptr<int>(new int, [&, source, n](int *p) {
            delete p; if (source && n == 1) firstDone = true;
        });
        return ext::QueryWork{[&, source, n, lease]() -> std::optional<ext::Reply> {
            if (source && (!allowReply || n > 1)) return {};
            if (source) ++polls;
            return reply;
        }, {}, {}, [] { return 1u; }};
    }, [&] {
        const auto n = ++reads;
        const auto deadline = GetTickCount64() + 5000;
        while (n > 1 && !allowRead && GetTickCount64() < deadline) Sleep(1);
        return Inventory(version.load());
    });
    ReleaseOnExit release{allowRead};
    ext::Preferences prefs; ext::SetHidden(prefs, action.provider, ext::Context::File, false);
    service.Configure(prefs); service.Query(request);
    PumpUntil([&] { return starts == 1; }, "source query starts against the published selection");
    if (mode <= 1)
    {
        for (unsigned i = 1; i <= (mode ? 1u : 3u); ++i)
        {
            const auto before = service.View(request).revision;
            if (mode) version = 102;
            registry.Notify(i);
            PumpUntil([&] { return reads >= 2 && service.View(request).revision > before; }, "notification enters real service verification");
        }
        allowReply = true;
        PumpUntil([&] { return polls > 0; }, "successful source reply arrives while verification is held");
        Expect(!firstDone, "successful source reply is retained while its registry proof is pending");
        allowRead = true;
    }
    else if (mode == 2)
    {
        const auto before = service.View(request).revision;
        service.Query(request, ext::QueryPriority::Menu, true);
        PumpUntil([&] { return aggregates >= 2 && service.View(request).revision > before; }, "new aggregate replaces the source's original snapshot");
        allowReply = true;
    }
    else if (mode == 3)
    {
        std::ofstream(file, std::ios::app) << " changed identity";
        allowReply = true;
    }
    else
    {
        service.Configure({}); allowReply = true;
    }
    PumpUntil([&] { return firstDone.load(); }, "source proof completes without a stranded task");
    if (!mode)
        Expect(starts == 1 && reads <= 3, "NOISE_MUST_NOT_REQUERY_SOURCE: unchanged proof survives three notifications with one coalesced follow-up scan");
    if (mode == 1) Expect(!service.View(request).snapshot, "actual registration change removes the old snapshot");
    service.Shutdown();
    const auto raw = ReadText(cache / L"observed.dat");
    Expect(!raw.empty(), "observed metadata was persisted by the real service");
    snowdesktop::settings_ipc::Bytes bytes(raw.size()); memcpy(bytes.data(), raw.data(), raw.size());
    const auto [schema, observed] = snowdesktop::settings_ipc::Unpack<std::tuple<std::uint32_t, std::vector<ext::Registration>>>(bytes);
    const auto found = std::find_if(observed.begin(), observed.end(), [&](const auto &r) { return r.id == action.provider; });
    const bool attributed = found != observed.end() && found->application.id == "refresh-app";
    Expect(schema == 3 && attributed == (mode == 0), "only current target, snapshot, configuration and registration may acquire source proof");
}

void CheckPreparedRetention(bool changed)
{
    TemporaryDirectory temp;
    Registry registry(temp); Trace trace(temp);
    const auto file = temp.path / L"prepared-retention.txt"; std::ofstream(file) << "private";
    ext::Request request; request.paths = {file.wstring()};
    std::atomic_uint reads = 0;
    std::atomic_bool allowRead = false;
    ext::MenuService service(temp.path / L"cache", {}, [&] {
        const auto n = ++reads;
        const auto deadline = GetTickCount64() + 5000;
        while (n > 1 && !allowRead && GetTickCount64() < deadline) Sleep(1);
        auto catalogue = Inventory(changed && n > 1 ? 102 : 101);
        catalogue.rows.front().kind = ext::RegistrationKind::Verb;
        catalogue.rows.front().verbs = {"unrelated"};
        return catalogue;
    });
    ReleaseOnExit release{allowRead};
    ext::Preferences prefs;
    ext::SetHidden(prefs, "verb:prepared", ext::Context::File, false);
    ext::SetHidden(prefs, "verb:cold", ext::Context::File, false);
    service.Configure(prefs);
    PumpUntil([&] { return trace.Has("phase=preparation reason=ready"); }, "native prepared helper is ready on the service STA");
    registry.Notify(1);
    PumpUntil([&] { return reads >= 2; }, "registry verification holds the prepared helper");
    service.Query(request);
    PumpUntil([&] {
        return trace.Has("reason=prepared_verification_wait") || trace.Has("phase=helper_start reason=cold");
    }, "observe verification wait or the premature cold-query regression before releasing the reader");
    allowRead = true;
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "native query publishes after verification");
    Expect(service.View(request).snapshot->entries.front().key == (changed ? "cold" : "prepared"),
        "PREPARED_UNCHANGED_MUST_SURVIVE: unchanged verification reuses the real helper; changed registration retires it");
    service.Shutdown();
}
void CheckRegistryFanout()
{
    TemporaryDirectory temp; Registry registry(temp);
    std::atomic_bool readyA = false, readyB = false, allowA = false, allowB = false;
    std::promise<bool> firstResult, laterResult;
    auto firstValue = firstResult.get_future(), laterValue = laterResult.get_future();
    // MSVC's async executor can retain TLS on reused pool threads. Explicit
    // threads make these distinct subscribers and join their TLS teardown.
    std::jthread first([&] {
        ext::TakeMenuRegistryChanges(); readyA = true;
        const auto until = GetTickCount64() + 5000;
        while (!allowA && GetTickCount64() < until) Sleep(1);
        firstResult.set_value(ext::TakeMenuRegistryChanges());
    });
    std::jthread later([&] {
        ext::TakeMenuRegistryChanges(); readyB = true;
        const auto until = GetTickCount64() + 5000;
        while (!allowB && GetTickCount64() < until) Sleep(1);
        bool complete = false;
        const auto handles = ext::MenuRegistryWaitHandles(complete);
        const bool woke = !handles.empty() && WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
            handles.data(), FALSE, 100) < static_cast<DWORD>(handles.size());
        laterResult.set_value(complete && woke && ext::TakeMenuRegistryChanges());
    });
    ReleaseOnExit releaseA{allowA}, releaseB{allowB};
    PumpUntil([&] { return readyA && readyB; }, "both STAs register independent notification consumers");
    registry.Notify(1); allowA = true;
    const bool firstChanged = firstValue.get(); first.join();
    // The first consumer has reset/rearmed the OS subscription before the
    // second even starts waiting. Its own broadcast event must retain the edge.
    allowB = true;
    const bool laterChanged = laterValue.get(); later.join();
    Expect(firstChanged && laterChanged, "SHARED_REGISTRY_WAKE_MUST_REACH_EVERY_CONSUMER: late wait cannot lose a sibling's notification");
    registry.Notify(2);
    std::promise<bool> joiningResult;
    auto joiningValue = joiningResult.get_future();
    std::jthread joining([&] { joiningResult.set_value(ext::TakeMenuRegistryChanges()); });
    const bool replayedOldChange = joiningValue.get(); joining.join();
    const bool existingChanged = ext::TakeMenuRegistryChanges();
    Expect(!replayedOldChange && existingChanged,
        "NEW_REGISTRY_CONSUMER_STARTS_CURRENT: drain older notifications to existing consumers before subscribing a fresh service");
}
void CheckBackgroundProcessSlot()
{
    TemporaryDirectory temp; Registry registry(temp); Trace trace(temp);
    const auto held = temp.path / L"menu-slot-held.txt"; std::ofstream(held) << "private";
    ext::MenuService service(temp.path / L"cache", {}, [] {
        auto value = Inventory(); value.rows.front().kind = ext::RegistrationKind::Verb;
        value.rows.front().verbs = {"unrelated"}; return value;
    });
    ext::Preferences prefs;
    ext::SetHidden(prefs, "verb:first", ext::Context::File, false);
    ext::SetHidden(prefs, "verb:first", ext::Context::FolderBackground, false);
    service.Configure(prefs);
    PumpUntil([&] { return trace.Has("phase=preparation reason=ready"); }, "reserve one ordinary preparation slot");
    ext::Request file; file.paths = {held.wstring()};
    service.Query(file, ext::QueryPriority::Inspect, true);
    PumpUntil([&] { return std::filesystem::exists(temp.path / L"slot-entered"); }, "a real inspected query holds the other ordinary slot");
    ext::Request background; background.paths = {temp.path.wstring()};
    background.background = true; background.context = ext::Context::FolderBackground;
    service.Query(background);
    PumpUntil([&] {
        const auto view = service.View(background);
        Expect(view.error.empty(), "BACKGROUND_DEMAND_MUST_TAKE_RESERVED_SLOT: background demand cannot fail because preparation occupies the remaining process slot");
        return view.snapshot.has_value();
    }, "explicit background demand replaces incompatible preparation without exceeding two processes");
    std::ofstream(temp.path / L"release-slot") << "release private query";
    service.Shutdown();
}
void Run()
{
    for (int mode = 0; mode < 5; ++mode) CheckSourceProof(mode);
    CheckPreparedRetention(false); CheckPreparedRetention(true);
    CheckRegistryFanout();
    CheckBackgroundProcessSlot();
}
}
