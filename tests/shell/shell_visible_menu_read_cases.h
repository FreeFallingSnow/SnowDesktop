#pragma once
#include "shell/shell_menu_read_projection.h"

namespace visible_read_tests
{
namespace ext = snowdesktop::shell_extensions;

void CheckProjectionAndCache()
{
    ext::Catalogue catalogue; catalogue.revision = 1;
    ext::Registration action; action.id = "reg:private\\shell\\inspect";
    action.kind = ext::RegistrationKind::Verb; action.verbs = {"inspect"}; catalogue.rows = {action};
    ext::Preferences prefs;
    ext::SetCommon(prefs, action.id, ext::Category::Objects, true);
    ext::SetCommon(prefs, "menu:private cascade:14695981039346656037", ext::Category::Objects, true);
    ext::Request request;
    request.visibleMenuProviders = ext::MenuReadProjection(prefs, catalogue, ext::Category::Objects);
    Expect(ext::ReadMenuRootDetails(request, "verb:inspect", "menu:other:") &&
        ext::ReadMenuRootDetails(request, "", "menu:private cascade:") &&
        !ext::ReadMenuRootDetails(request, "verb:hidden", "menu:hidden:"),
        "projection retains registered commands and legacy cascade IDs while excluding unrelated roots");
    ext::SetCommon(prefs, "clsid:unknown", ext::Category::Objects, true);
    request.visibleMenuProviders = ext::MenuReadProjection(prefs, catalogue, ext::Category::Objects);
    Expect(ext::ReadMenuRootDetails(request, "verb:unidentified", "menu:unidentified:"),
        "unknown registration ownership conservatively retains complete menu details");

    TemporaryDirectory directory; const auto path = directory.path / L"cache-target.txt";
    std::ofstream(path) << "private"; request = {}; request.paths = {path.wstring()};
    ext::MenuSnapshotCache cache(directory.path / L"cache");
    const auto full = cache.Capture(request);
    request.visibleMenuProviders = std::vector<std::string>{"verb:inspect"};
    const auto projected = cache.Capture(request);
    request.visibleMenuProviders = std::vector<std::string>{"verb:other"};
    const auto changed = cache.Capture(request);
    ext::Reply reply{true, {}, {}};
    Expect(full && projected && changed && full.identity != projected.identity &&
        projected.identity != changed.identity && cache.Store(projected, reply) &&
        cache.Find(projected).has_value() && !cache.Find(full) && !cache.Find(changed),
        "projected snapshots cannot satisfy complete reads or another visibility policy");
}

void CheckNativeProjection()
{
    TemporaryDirectory directory; TemporaryVerb fixture;
    const auto hiddenRoot = fixture.owned[1] + L"\\shell\\ProjectionHidden";
    HKEY key = nullptr;
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, hiddenRoot.c_str(), 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS, "create owned hidden cascade");
    TemporaryVerb::Value(key, L"MUIVerb", L"SnowDesktop hidden cascade");
    TemporaryVerb::Value(key, L"ExtendedSubCommandsKey", fixture.progId + L"\\ProjectionCommands");
    RegCloseKey(key);
    const auto child = fixture.owned[1] + L"\\ProjectionCommands\\shell\\Child";
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, child.c_str(), 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS, "create owned cascade child");
    TemporaryVerb::Value(key, L"MUIVerb", L"Private cascade child"); RegCloseKey(key);
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, (child + L"\\command").c_str(), 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS, "create owned child command metadata");
    TemporaryVerb::Value(key, nullptr, L"notepad.exe \"%1\""); RegCloseKey(key); // Never invoked.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    const auto path = directory.path / (L"private" + fixture.extension);
    std::ofstream(path) << "private projection regression";

    struct Environment
    {
        std::wstring mode, trace;
        Environment(const std::filesystem::path &base)
        {
            wchar_t value[32768]{};
            GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", value, 32768); mode = value;
            value[0] = 0;
            GetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", value, 32768); trace = value;
            SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
            SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", base.c_str());
        }
        ~Environment()
        {
            SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", mode.empty() ? nullptr : mode.c_str());
            SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", trace.empty() ? nullptr : trace.c_str());
        }
    } environment(directory.path / L"read-trace");
    auto query = [&](ext::Request request) {
        ext::Session session(request); const auto pid = session.ProcessId();
        std::optional<ext::Reply> reply;
        PumpUntil([&] { reply = session.Poll(); return reply.has_value(); }, "native projected query completes");
        Expect(reply->ok, "native projected query succeeds");
        const auto logPath = std::filesystem::path((directory.path / L"read-trace").wstring() +
            L"." + std::to_wstring(pid) + L".log");
        std::ifstream log(logPath, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
        return std::pair{std::move(*reply), std::move(text)};
    };
    ext::Request request; request.paths = {path.wstring()};
    const auto [complete, completeLog] = query(request);
    const auto visible = std::find_if(complete.entries.begin(), complete.entries.end(), [](const auto &e) {
        return e.label == L"SnowDesktop isolated policy probe";
    });
    const auto hidden = std::find_if(complete.entries.begin(), complete.entries.end(), [](const auto &e) {
        return e.label == L"SnowDesktop hidden cascade";
    });
    Expect(visible != complete.entries.end() && hidden != complete.entries.end() && !hidden->children.empty() &&
        completeLog.find("read popup: SnowDesktop hidden cascade") != std::string::npos,
        "complete native query reads the real owned cascade and its children");
    request.visibleMenuProviders = std::vector<std::string>{visible->provider};
    const auto [projected, projectedLog] = query(request);
    const auto retained = std::find_if(projected.entries.begin(), projected.entries.end(), [&](const auto &e) {
        return e.provider == visible->provider && e.key == visible->key && e.label == visible->label;
    });
    Expect(retained != projected.entries.end() && retained->token &&
        std::none_of(projected.entries.begin(), projected.entries.end(), [&](const auto &e) { return e.label == hidden->label; }) &&
        projectedLog.find("read_projection reason=root_omitted") != std::string::npos &&
        projectedLog.find("read popup: SnowDesktop hidden cascade") == std::string::npos,
        "VISIBLE_NATIVE_PROJECTION: actual helper skips hidden cascade work and preserves visible command identity");
    auto completeRequest = request; completeRequest.visibleMenuProviders.reset();
    const auto [again, againLog] = query(completeRequest);
    Expect(std::any_of(again.entries.begin(), again.entries.end(), [&](const auto &e) { return e.label == hidden->label; }) &&
        againLog.find("read popup: SnowDesktop hidden cascade") != std::string::npos,
        "a subsequent complete query is unaffected by the projected query");
}

void CheckServiceProjection()
{
    TemporaryDirectory directory; const auto file = directory.path / L"service.txt";
    std::ofstream(file) << "private service projection";
    ext::Request request; request.paths = {file.wstring()};
    ext::Entry shown; shown.provider = "verb:visible"; shown.key = "visible"; shown.label = L"Visible"; shown.token = 42;
    auto hidden = shown; hidden.provider = "verb:hidden"; hidden.key = "hidden"; hidden.label = L"Hidden";
    std::atomic_uint projected = 0, complete = 0, invoked = 0;
    ext::MenuService service(directory.path / L"cache", [&](const ext::Request &target) {
        if (target.paths == request.paths && target.sourceClsid.empty())
        {
            if (target.visibleMenuProviders) ++projected; else ++complete;
        }
        return ext::QueryWork{[=] { return std::optional{ext::Reply{true, {shown, hidden}, {}}}; },
            [&](UINT token, POINT) { Expect(token == 42, "execution resolves the fresh command token"); ++invoked; }};
    }, [] { ext::Catalogue catalogue; catalogue.revision = 1; return catalogue; });
    ext::Preferences prefs; ext::SetCommon(prefs, shown.provider, ext::Category::Objects, true);
    service.Configure(prefs); service.Query(request);
    PumpUntil([&] {
        const auto view = service.MenuDisplay(request, prefs);
        return !view.pending && view.snapshot && view.snapshot->entries.size() == 1;
    }, "service projects only the configured visible command");
    Expect(projected == 1 && complete == 0, "ordinary service dispatch carries its visible read projection");
    service.Execute(request, ext::AppendReference({}, shown), {});
    PumpUntil([&] { return invoked == 1; }, "visible command executes through a fresh query");
    Expect(complete >= 1, "fresh execution bypasses the display projection and retains complete command resolution");
    service.Shutdown();
}

void Run() { CheckProjectionAndCache(); CheckServiceProjection(); CheckNativeProjection(); }
}
