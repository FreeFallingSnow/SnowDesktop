#include <windows.h>
#include <sddl.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "shell/shell_extension_catalogue.h"
#include "shell/shell_catalogue_workers.h"
#include "shell/shell_menu_registry_watch.h"
#include "platform/foreground_fullscreen.h"

namespace
{
namespace ext = snowdesktop::shell_extensions;

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

struct OwnedKey
{
    HKEY value = nullptr;
    OwnedKey(HKEY root, const std::wstring &path)
    {
        Check(RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
            nullptr, &value, nullptr) == ERROR_SUCCESS, "create owned association key");
    }
    OwnedKey(const OwnedKey &) = delete;
    ~OwnedKey() { Close(); }
    void Close() { if (value) { RegCloseKey(value); value = nullptr; } }
};

struct PrivateRegistry
{
    HKEY value = nullptr;
    std::wstring path;
    bool owned = false;
    PrivateRegistry()
    {
        GUID id{};
        Check(SUCCEEDED(CoCreateGuid(&id)), "association fixture GUID");
        wchar_t text[40]{};
        Check(StringFromGUID2(id, text, 40) != 0, "association fixture GUID text");
        path = L"Software\\SnowDesktopCatalogueHandleTests\\" + std::wstring(text);
        DWORD disposition = 0;
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
            KEY_ALL_ACCESS, nullptr, &value, &disposition) == ERROR_SUCCESS,
            "create isolated association root");
        owned = disposition == REG_CREATED_NEW_KEY;
        if (!owned)
        {
            RegCloseKey(value); value = nullptr;
            throw std::runtime_error("never adopt an existing registry fixture");
        }
    }
    void Cleanup()
    {
        if (value) { RegCloseKey(value); value = nullptr; }
        if (owned)
        {
            Check(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS,
                "remove only this fixture's GUID root");
            owned = false;
        }
    }
    ~PrivateRegistry()
    {
        if (value) RegCloseKey(value);
        if (owned && RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) != ERROR_SUCCESS)
            fputs("FIXTURE_CLEANUP_FAILURE\n", stderr);
    }
};

// RegOverridePredefKey affects this test process only. No existing association
// or user security descriptor is changed; supplied hives refer to our GUID tree.
struct RegistryOverrides
{
    bool classes = false, user = false, machine = false;
    RegistryOverrides(HKEY classKey, HKEY userKey, HKEY machineKey = nullptr)
    {
        Check(RegOverridePredefKey(HKEY_CURRENT_USER, userKey) == ERROR_SUCCESS,
            "override user within the fixture process");
        user = true;
        if (RegOverridePredefKey(HKEY_CLASSES_ROOT, classKey) != ERROR_SUCCESS)
        {
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); user = false;
            throw std::runtime_error("override classes within the fixture process");
        }
        classes = true;
        if (machineKey)
        {
            if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, machineKey) != ERROR_SUCCESS)
            {
                RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr); classes = false;
                RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); user = false;
                throw std::runtime_error("override machine within the fixture process");
            }
            machine = true;
        }
    }
    void Restore()
    {
        if (machine)
        {
            Check(RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr) == ERROR_SUCCESS,
                "restore predefined machine mapping");
            machine = false;
        }
        if (classes)
        {
            Check(RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr) == ERROR_SUCCESS,
                "restore predefined classes mapping");
            classes = false;
        }
        if (user)
        {
            Check(RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) == ERROR_SUCCESS,
                "restore predefined user mapping");
            user = false;
        }
    }
    ~RegistryOverrides()
    {
        if (machine) RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
        if (classes) RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
        if (user) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    }
};

std::wstring CurrentUserSid()
{
    HANDLE token = nullptr;
    Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
        "query fixture user token");
    struct CloseToken { HANDLE value; ~CloseToken() { CloseHandle(value); } } close{token};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    Check(bytes > 0, "query token size");
    std::vector<BYTE> data(bytes);
    Check(GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes) != FALSE,
        "read fixture user token");
    PWSTR sid = nullptr;
    Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid,
        &sid) != FALSE, "format fixture user SID");
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

struct RestrictedKey
{
    HKEY key;
    std::vector<BYTE> original;
    bool armed = false;
    RestrictedKey(HKEY target, const std::wstring &sid, const wchar_t *denied) : key(target)
    {
        DWORD bytes = 0;
        Check(RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, nullptr, &bytes) ==
            ERROR_INSUFFICIENT_BUFFER, "query private DACL size");
        original.resize(bytes);
        Check(RegGetKeySecurity(key, DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data()), &bytes) ==
            ERROR_SUCCESS, "read private DACL");
        const auto sddl = L"D:P(D;;" + std::wstring(denied) + L";;;" + sid +
            L")(A;;KA;;;" + sid + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "construct private DACL");
        const auto status = RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, descriptor);
        LocalFree(descriptor);
        Check(status == ERROR_SUCCESS, "restrict only the owned fixture key");
        armed = true;
    }
    void Restore()
    {
        if (armed)
        {
            Check(RegSetKeySecurity(key, DACL_SECURITY_INFORMATION,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data())) == ERROR_SUCCESS,
                "restore owned DACL");
            armed = false;
        }
    }
    ~RestrictedKey()
    {
        if (armed && RegSetKeySecurity(key, DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data())) != ERROR_SUCCESS)
            fputs("DACL_RESTORE_FAILURE\n", stderr);
    }
};

void Put(HKEY root, const std::wstring &path, const wchar_t *name, const std::wstring &value)
{
    OwnedKey key(root, path);
    Check(RegSetValueExW(key.value, name, 0, REG_SZ,
        reinterpret_cast<const BYTE *>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS,
        "write private association");
}

std::vector<std::wstring> Types(const ext::Catalogue &catalogue, const std::string &id)
{
    const auto row = std::find_if(catalogue.rows.begin(), catalogue.rows.end(),
        [&](const auto &item) { return item.id == id; });
    Check(row != catalogue.rows.end(), "expected private command exists");
    return row->types;
}

void CheckCatalogueWorkerBudget(bool foregroundAtStart, bool promote, int expected, bool fail = false)
{
    ext::CatalogueForegroundDemand foreground{foregroundAtStart};
    std::atomic_int entered{0};
    std::mutex mutex;
    std::condition_variable changed;
    bool release = false;
    auto scan = std::async(std::launch::async, [&] {
        return ext::detail::RunCatalogueWorkers(8, &foreground, [&] {
            entered.fetch_add(1);
            changed.notify_all();
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return release; });
            if (fail) throw std::runtime_error("injected catalogue worker failure");
        });
    });
    auto waitFor = [&](int count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5),
            [&] { return entered.load() >= count; });
    };
    const bool initialReady = waitFor(foregroundAtStart ? 8 : 1);
    if (promote) foreground.Promote();
    const bool promoted = !promote || waitFor(8);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    const bool completed = scan.get();
    Check(initialReady && promoted && completed == !fail && entered.load() == expected,
        "background catalogue uses one worker and foreground demand restores eight");
}

void CheckCataloguePauseWake(int action)
{
    std::atomic_bool fullscreen{true};
    std::promise<void> observed;
    auto ready = observed.get_future();
    bool first = true;
    ext::CatalogueForegroundDemand demand{false, [&] {
        if (first) { first = false; observed.set_value(); }
        return fullscreen.load();
    }};
    auto work = std::async(std::launch::async, [&] {
        try { demand.WaitUntilAllowed(); return 1; }
        catch (const std::runtime_error& error) {
            return std::string(error.what()) == "catalogue scan cancelled" ? 0 : -1;
        }
    });
    const bool observedPause = ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const bool blocked = work.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout;
    if (action == 0) fullscreen.store(false);
    else if (action == 1) demand.Promote();
    else demand.Cancel();
    const bool woke = work.wait_for(action == 0 ? std::chrono::seconds(2) :
        std::chrono::milliseconds(100)) == std::future_status::ready;
    if (!woke) demand.Cancel(); // Failure must not strand a paused future.
    const int result = work.get();
    Check(observedPause && blocked && woke && result == (action == 2 ? 0 : 1),
        "fullscreen pause resumes, foreground promotion wakes it, and shutdown cancels it");
    Check(demand.PausedMilliseconds() >= 60,
        "paused duration is recorded for the active scan timeout budget");
    demand.Reset(true);
    demand.WaitUntilAllowed();
    Check(demand.PausedMilliseconds() == 0,
        "a new foreground scan resets cancellation and paused time");
}

void CheckCataloguePauseProgress()
{
    std::atomic_bool fullscreen{false};
    std::atomic_int completed{0};
    std::promise<void> paused;
    auto pausedReady = paused.get_future();
    bool reported = false;
    ext::CatalogueForegroundDemand demand{false, [&] {
        const bool blocked = fullscreen.load();
        if (blocked && !reported) { reported = true; paused.set_value(); }
        return blocked;
    }};
    auto work = std::async(std::launch::async, [&] {
        return ext::detail::RunCatalogueWorkers(8, &demand, [&] {
            for (int i = 0; i < 40; ++i)
            {
                demand.WaitUntilAllowed();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                completed.fetch_add(1);
            }
        });
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (completed.load() < 3 && std::chrono::steady_clock::now() < deadline &&
        work.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {}
    fullscreen.store(true);
    const bool stopped = pausedReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const int progress = completed.load();
    const bool waiting = work.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout;
    const bool unchanged = completed.load() == progress;
    fullscreen.store(false);
    // Each of the forty 20 ms batches can now incur up to 100 ms of
    // cooperative background rest. Keep all queue/pause assertions, but allow
    // that intentional idle time plus two seconds for scheduling and probing.
    constexpr auto resumeAllowance = 40 * std::chrono::milliseconds(20 + 100) +
        std::chrono::seconds(2);
    if (work.wait_for(resumeAllowance) != std::future_status::ready) demand.Cancel();
    const bool successful = work.get();
    Check(stopped && waiting && unchanged && successful && completed.load() == 40,
        "a fullscreen change pauses an active single-worker scan and resumes its existing queue exactly once");
}

void CheckPausedRegistryCatalogue(const ext::Catalogue& expected)
{
    std::atomic_bool fullscreen{true};
    std::promise<void> observed;
    auto ready = observed.get_future();
    bool first = true;
    ext::CatalogueForegroundDemand demand{false, [&] {
        if (first) { first = false; observed.set_value(); }
        return fullscreen.load();
    }};
    auto work = std::async(std::launch::async, [&] {
        return ext::ReadCatalogueWithForegroundDemand(HKEY_CLASSES_ROOT, false, demand);
    });
    const bool observedPause = ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const bool blocked = work.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout;
    fullscreen.store(false);
    if (work.wait_for(std::chrono::seconds(3)) != std::future_status::ready) demand.Cancel();
    const auto result = work.get();
    Check(observedPause && blocked && snowdesktop::settings_ipc::Pack(result) ==
        snowdesktop::settings_ipc::Pack(expected),
        "the real registry inventory waits while fullscreen and resumes without losing associations or fingerprints");
}
void CheckScopeReaderCancellation(HKEY classes)
{
    for (int scope = 0; scope < 3; ++scope)
    {
        const auto read = [&](ext::CatalogueForegroundDemand *demand) {
            return scope == 0 ? ext::ReadFolderCatalogueRevision(classes, false, demand) :
                ext::ReadBackgroundCatalogueRevision(scope == 1 ? ext::Context::FolderBackground : ext::Context::Desktop,
                    classes, false, demand);
        };
        const auto original = read(nullptr);
        ext::CatalogueForegroundDemand foreground{true};
        Check(read(&foreground) == original,
            "per-scope cancellation token preserves an uncancelled production proof");
        std::promise<void> paused;
        auto entered = paused.get_future();
        bool announced = false;
        ext::CatalogueForegroundDemand cancelled{false, [&] {
            if (!announced) { announced = true; paused.set_value(); }
            return true;
        }};
        auto work = std::async(std::launch::async, [&] {
            try { (void)read(&cancelled); return false; }
            catch (const std::runtime_error &) { return true; }
        });
        const bool observed = entered.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        cancelled.Cancel();
        const bool completed = work.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        const bool rejected = completed && work.get();
        Check(observed && completed && rejected,
            "cancelled production scope reader exits without returning valid complete proof");
        cancelled.Reset(true);
        Check(read(&cancelled) == original,
            "next scope generation can reset its own completed cancellation token");
    }
}
void CheckSelectedCatalogueSources()
{
    PrivateRegistry registry;
    wchar_t temporary[MAX_PATH]{};
    const auto temporaryLength = GetTempPathW(MAX_PATH, temporary);
    Check(temporaryLength && temporaryLength < MAX_PATH, "get selected-source private resource parent");
    const auto directory = std::filesystem::path(temporary) /
        (L"SnowDesktopSelectedCatalogue-" + registry.path.substr(registry.path.find_last_of(L'\\') + 1));
    Check(std::filesystem::create_directory(directory), "create new selected-source private resource directory");
    struct DirectoryCleanup
    {
        std::filesystem::path path;
        ~DirectoryCleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    const auto resource = directory / L"command-resource.bin";
    std::ofstream(resource, std::ios::binary) << "private";
    {
        OwnedKey classes(registry.value, L"Classes"), user(registry.value, L"User"), machine(registry.value, L"Machine");
        RegistryOverrides overrides(classes.value, user.value, machine.value);
        const std::wstring clsid = L"{6C6FC463-71EF-47AD-8148-6FF213EE19D8}";
        const std::wstring verbPath = L"Selected.Type\\shell\\inspect";
        const std::wstring handlerPath = L"Directory\\shellex\\ContextMenuHandlers\\Selected";
        const std::wstring guidPath = L"Directory\\shellex\\ContextMenuHandlers\\" + clsid;
        const std::wstring policy = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\";
        const std::wstring command = L"\"" + resource.wstring() + L"\" \"%1\"";
        Put(classes.value, L".selected", nullptr, L"Selected.Type");
        Put(classes.value, verbPath, L"MUIVerb", L"Selected command");
        Put(classes.value, verbPath + L"\\command", nullptr, command);
        Put(classes.value, handlerPath, nullptr, clsid);
        Put(classes.value, guidPath, nullptr, L"");
        Put(classes.value, L"Selected.Type\\shellex\\ContextMenuHandlers\\Selected", nullptr, clsid);
        Put(classes.value, L"CLSID\\" + clsid, nullptr, L"Selected handler");
        Put(classes.value, L"CLSID\\" + clsid + L"\\InprocServer32", nullptr, resource.wstring());
        Put(classes.value, L"Unrelated.Type\\shell\\ignored\\command", nullptr, L"unused.exe");
        const auto full = ext::ReadCatalogue(HKEY_CLASSES_ROOT, false);
        CheckScopeReaderCancellation(HKEY_CLASSES_ROOT);
        std::vector<ext::Registration> seeds;
        for (const auto &row : full.rows)
            if (row.id == "reg:selected.type\\shell\\inspect" || row.kind == ext::RegistrationKind::Handler)
                seeds.push_back(row);
        Check(seeds.size() == 2, "select exact static and merged handler seeds from full metadata");
        const auto limited = ext::ReadCatalogueSources(seeds);
        Check(limited.rows.size() == seeds.size() && limited.revision && limited.revision != full.revision &&
            !limited.folderRevision && !limited.backgroundRevisions[0] && !limited.backgroundRevisions[1],
            "selected proof never advertises complete inventory or scope verification");
        for (const auto &row : limited.rows)
        {
            const auto original = std::find_if(seeds.begin(), seeds.end(), [&](const auto &seed) { return seed.id == row.id; });
            Check(original != seeds.end() && snowdesktop::settings_ipc::Pack(row) == snowdesktop::settings_ipc::Pack(*original) &&
                row.menuRevision == original->menuRevision,
                "exact rows share full-reader command and resource signature construction");
        }
        HKEY queryRoot = nullptr;
        Check(RegOpenKeyExW(registry.value, L"Classes", 0, KEY_QUERY_VALUE, &queryRoot) == ERROR_SUCCESS,
            "open exact classes root without enumeration rights");
        struct CloseQueryRoot { HKEY value; ~CloseQueryRoot() { RegCloseKey(value); } } closeQueryRoot{queryRoot};
        wchar_t childName[64]{}; DWORD childLength = 64;
        Check(RegEnumKeyExW(queryRoot, 0, childName, &childLength, nullptr, nullptr, nullptr, nullptr) == ERROR_ACCESS_DENIED,
            "provided classes root cannot enumerate file types");
        Check(ext::ReadCatalogueSources(seeds, nullptr, queryRoot).revision ==
            ext::ReadCatalogueSources(seeds, nullptr, classes.value).revision,
            "exact source reading does not require enumerating the classes root");
        // A whole-class reader cannot enumerate this shell parent. The exact
        // child remains readable through its full path under the private DACL.
        OwnedKey shellParent(classes.value, L"Selected.Type\\shell");
        RestrictedKey noEnumeration(shellParent.value, CurrentUserSid(), L"0x8");
        HKEY denied = nullptr;
        const auto deniedStatus = RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Selected.Type\\shell", 0, KEY_ENUMERATE_SUB_KEYS, &denied);
        if (denied) RegCloseKey(denied);
        Check(deniedStatus == ERROR_ACCESS_DENIED, "private shell parent rejects enumeration");
        Check(ext::ReadCatalogueSources(seeds).revision == limited.revision,
            "selected source reading succeeds while shell-parent enumeration is denied");
        Put(classes.value, L"Unrelated.Type\\shell\\new-unselected\\command", nullptr, L"changed.exe");
        Check(ext::ReadCatalogueSources(seeds).revision == limited.revision,
            "unrelated registration changes cannot alter selected-source proof");
        noEnumeration.Restore();
        auto direct = seeds.front(); direct.sources.clear();
        if (direct.kind != ext::RegistrationKind::Verb) direct = seeds.back();
        direct.sources.clear(); direct.types.clear(); direct.contexts = ext::ContextBit(ext::Context::File);
        const auto explicitVerb = ext::ReadCatalogueSources({direct});
        Check(explicitVerb.rows.size() == 1 && explicitVerb.rows.front().types == std::vector<std::wstring>{L"progid:selected.type"},
            "explicit reg IDs can be read without inventing unproven file-type coverage");
        auto handler = *std::find_if(seeds.begin(), seeds.end(), [](const auto &seed) { return seed.kind == ext::RegistrationKind::Handler; });
        handler.sources = {guidPath}; handler.types.clear(); handler.contexts = ext::ContextBit(ext::Context::Folder);
        const auto scoped = ext::ReadCatalogueSources({handler});
        Check(scoped.rows.size() == 1 && scoped.rows.front().sources.size() == 1 &&
            scoped.rows.front().contexts == ext::ContextBit(ext::Context::Folder),
            "same CLSID proof is restricted to supplied paths and selection scope");
        Put(user.value, policy + L"Blocked", clsid.c_str(), L"");
        const auto blocked = ext::ReadCatalogueSources({handler});
        Check(blocked.rows.size() == 1 && !blocked.rows.front().systemEnabled && blocked.revision != scoped.revision,
            "selected handler observes current Blocked policy");
        OwnedKey blockedKey(user.value, policy + L"Blocked");
        Check(RegDeleteValueW(blockedKey.value, clsid.c_str()) == ERROR_SUCCESS, "remove only owned blocked policy");
        OwnedKey policyKey(machine.value, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
        const DWORD enforce = 1;
        Check(RegSetValueExW(policyKey.value, L"EnforceShellExtensionSecurity", 0, REG_DWORD,
            reinterpret_cast<const BYTE *>(&enforce), sizeof(enforce)) == ERROR_SUCCESS, "set isolated approval requirement");
        Check(!ext::ReadCatalogueSources({handler}).rows.front().systemEnabled,
            "selected handler enforces approval policy");
        Put(machine.value, policy + L"Approved", clsid.c_str(), L"");
        Check(ext::ReadCatalogueSources({handler}).rows.front().systemEnabled,
            "selected handler rechecks current Approved registration");
        std::ofstream(resource, std::ios::binary | std::ios::app) << " changed";
        const auto resourceChanged = ext::ReadCatalogueSources(seeds);
        Check(resourceChanged.revision != limited.revision,
            "local command and DLL resource changes invalidate selected proof without registry mutation");
        Put(classes.value, verbPath + L"\\command", nullptr, command + L" --changed");
        const auto commandChanged = ext::ReadCatalogueSources(seeds);
        Check(commandChanged.revision != resourceChanged.revision,
            "selected command payload changes invalidate proof");
        Put(classes.value, L".selected", nullptr, L"Other.Type");
        Check(ext::ReadCatalogueSources(seeds).revision != commandChanged.revision,
            "exact seed extension associations are read live");
        Check(RegDeleteTreeW(classes.value, guidPath.c_str()) == ERROR_SUCCESS, "delete owned GUID-named registration");
        Check(ext::ReadCatalogueSources({handler}).rows.empty(),
            "a deleted GUID-named handler cannot be recreated from its cached key name");
        ext::Registration packaged; packaged.kind = ext::RegistrationKind::Packaged;
        packaged.id = "package:unknown"; packaged.sources = {L"package:unknown:verb"};
        Check(ext::ReadCatalogueSources({packaged}).rows.empty(),
            "unsupported packaged provenance never falls back to package discovery");
        overrides.Restore();
    }
    registry.Cleanup();
}
}

void TestCatalogueRegistryHandles()
{
    CheckSelectedCatalogueSources();
    // Real subscriptions: nested browse-state writes must not signal menu
    // invalidation, including when a genuine registration changes concurrently.
    {
        PrivateRegistry fixture;
        {
            const std::wstring packages = L"Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";
            Put(fixture.value, L"Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\BagMRU", L"state", L"one");
            Put(fixture.value, packages + L"\\Example", L"PackageRootFolder", L"private");
            Put(fixture.value, L"Classes\\Type\\shell\\open\\command", nullptr, L"unused.exe");
            ext::detail::MenuRegistryWatch watch(fixture.value, L"Classes", 0, true);
            Check(watch.armed, "split Classes notifications are armed");
            Put(fixture.value, L"Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\BagMRU", L"state", L"two");
            Check(!watch.Changed(), "browse state alone cannot invalidate menu registration");
            Put(fixture.value, L"Classes\\Type\\shell\\open\\command", nullptr, L"changed.exe");
            Check(watch.Changed(), "a real registration is retained alongside browse noise");
            Put(fixture.value, packages + L"\\Example", L"PackageRootFolder", L"changed");
            Check(watch.Changed(), "package metadata remains watched under Local Settings");
            Put(fixture.value, L"Classes\\New.Type\\shell\\open", L"MUIVerb", L"new");
            Check(watch.Changed(), "a new class root is detected and subscribed");
            Put(fixture.value, L"Classes\\New.Type\\shell\\open", L"MUIVerb", L"edited");
            Check(watch.Changed(), "a newly subscribed root retains nested value coverage");
            Check(RegDeleteTreeW(fixture.value, L"Classes\\New.Type") == ERROR_SUCCESS, "remove owned type");
            Check(watch.Changed(), "class root removal remains observable");
        }
        fixture.Cleanup();
    }
    // The production watch previously invalidated the complete catalogue for
    // any value under the parent of a missing optional registration.
    {
        PrivateRegistry fixture;
        {
            OwnedKey parent(fixture.value, L"AppPaths");
            ext::detail::MenuRegistryWatch watch(fixture.value, L"AppPaths\\Optional\\Provider", 0);
            Check(watch.armed, "watch the nearest existing optional-registration ancestor");
            Put(fixture.value, L"AppPaths\\Unrelated", nullptr, L"other.exe");
            Check(!watch.Changed(), "an unrelated child must not invalidate the menu inventory");
            Put(fixture.value, L"AppPaths", L"cache", L"one");
            Check(!watch.Changed(), "an ancestor cache value must not invalidate an absent target");
            {
                OwnedKey intermediate(fixture.value, L"AppPaths\\Optional");
                Check(!watch.Changed() && watch.armed, "follow intermediate creation without declaring a target change");
            }
            Put(fixture.value, L"AppPaths\\Optional\\Provider", nullptr, L"provider.exe");
            Check(watch.Changed(), "new target registration invalidates the inventory");
            Put(fixture.value, L"AppPaths\\Optional\\Provider", nullptr, L"replacement.exe");
            Check(watch.Changed(), "an existing target value change remains observable");
            Check(RegDeleteTreeW(fixture.value, L"AppPaths\\Optional") == ERROR_SUCCESS,
                "remove only the optional subtree in the private fixture");
            Check(watch.Changed() && watch.armed, "target deletion invalidates and rearms on its ancestor");
            Put(fixture.value, L"AppPaths\\Unrelated", nullptr, L"again.exe");
            Check(!watch.Changed(), "deletion does not restore broad unrelated ancestor invalidations");
        }
        fixture.Cleanup();
    }
    CheckCatalogueWorkerBudget(false, false, 1);
    CheckCatalogueWorkerBudget(false, true, 8);
    CheckCatalogueWorkerBudget(true, false, 8);
    CheckCatalogueWorkerBudget(false, false, 1, true);
    for (int action = 0; action < 3; ++action) CheckCataloguePauseWake(action);
    CheckCataloguePauseProgress();
    for (bool cancel : {false, true})
    {
        ext::CatalogueForegroundDemand budget;
        std::atomic_bool entered = false;
        auto work = std::async(std::launch::async, [&] {
            try {
                budget.WaitUntilAllowed();
                // Simulate an indivisible system call exceeding one work slice.
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                entered = true; budget.WaitUntilAllowed(); return 1;
            } catch (const std::runtime_error &) { return 0; }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while ((!entered || !budget.BudgetWaitMilliseconds()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        const bool throttled = budget.BudgetWaitMilliseconds() > 0;
        if (cancel) budget.Cancel(); else budget.Promote();
        const bool woke = work.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
        if (!woke) budget.Cancel();
        const int result = work.get();
        Check(throttled && woke && result == (cancel ? 0 : 1),
            "background soft budget yields after a long batch and foreground/cancel wakes it");
        budget.Reset(true); budget.WaitUntilAllowed();
        Check(budget.BudgetWaitMilliseconds() == 0, "new foreground scan resets the background budget");
    }
    PrivateRegistry registry;
    {
        OwnedKey classes(registry.value, L"Classes"), user(registry.value, L"User");
        const auto sid = CurrentUserSid();
        const std::wstring choiceParent =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts";
        // Eighty extensions exercise the actual parallel association reader and
        // metadata workers, including the predefined-root ownership boundary.
        for (int i = 0; i < 80; ++i)
            Put(classes.value, L".snapshot" + std::to_wstring(i), nullptr, L"Choice.A");
        for (const auto *name : {L"Choice.A", L"Choice.B", L"Choice.C"})
            Put(classes.value, std::wstring(name) + L"\\shell\\inspect\\command",
                nullptr, L"unused.exe %1");
        Put(classes.value, L".snapshot0\\OpenWithProgids", L"Choice.C", L"");
        Put(user.value, choiceParent + L"\\.snapshot0\\UserChoice", L"ProgId", L"Choice.B");
        OwnedKey extension(classes.value, L".snapshot0"), choiceRoot(user.value, choiceParent),
            choice(user.value, choiceParent + L"\\.snapshot0\\UserChoice");
        RegistryOverrides overrides(classes.value, user.value);
        wchar_t raw[64]{};
        DWORD bytes = sizeof(raw);
        Check(RegGetValueW(HKEY_CLASSES_ROOT, L".snapshot0", nullptr, RRF_RT_REG_SZ,
            nullptr, raw, &bytes) == ERROR_SUCCESS && std::wstring(raw) == L"Choice.A",
            "isolated classes mapping is active before production catalogue reads");

        const auto first = ext::ReadCatalogue(HKEY_CLASSES_ROOT, false);
        CheckPausedRegistryCatalogue(first);
        ext::CatalogueForegroundDemand foreground;
        Check(snowdesktop::settings_ipc::Pack(ext::ReadCatalogueWithForegroundDemand(
            HKEY_CLASSES_ROOT, false, foreground)) == snowdesktop::settings_ipc::Pack(first),
            "throttled background catalogue preserves the full association inventory");
        Check(first.rows.size() == 3 && Types(first, "reg:choice.a\\shell\\inspect").size() == 80 &&
            Types(first, "reg:choice.b\\shell\\inspect") == std::vector<std::wstring>{L".snapshot0"} &&
            Types(first, "reg:choice.c\\shell\\inspect") == std::vector<std::wstring>{L".snapshot0"},
            "default, user choice and alternate retain all independent associations");
        bytes = sizeof(raw);
        Check(RegGetValueW(HKEY_CLASSES_ROOT, L".snapshot0", nullptr, RRF_RT_REG_SZ,
            nullptr, raw, &bytes) == ERROR_SUCCESS && std::wstring(raw) == L"Choice.A",
            "catalogue does not close the borrowed predefined classes key");

        RestrictedKey choiceRestriction(choiceRoot.value, sid, L"0x1"),
            extensionRestriction(extension.value, sid, L"0x8");
        HKEY denied = nullptr;
        const auto deniedStatus = RegOpenKeyExW(HKEY_CURRENT_USER, choiceParent.c_str(),
            0, KEY_QUERY_VALUE, &denied);
        if (denied) RegCloseKey(denied);
        Check(deniedStatus == ERROR_ACCESS_DENIED, "owned parent rejects query access");
        bytes = sizeof(raw);
        Check(RegGetValueW(HKEY_CURRENT_USER, (choiceParent + L"\\.snapshot0\\UserChoice").c_str(),
            L"ProgId", RRF_RT_REG_SZ, nullptr, raw, &bytes) == ERROR_SUCCESS &&
            std::wstring(raw) == L"Choice.B", "child remains readable through its full path");
        Check(snowdesktop::settings_ipc::Pack(ext::ReadCatalogue(HKEY_CLASSES_ROOT, false)) ==
            snowdesktop::settings_ipc::Pack(first),
            "restricted parent access preserves readable children and association fingerprints");

        Put(choice.value, L"", L"ProgId", L"CHOICE.B");
        const auto caseChanged = ext::ReadCatalogue(HKEY_CLASSES_ROOT, false);
        Check(Types(caseChanged, "reg:choice.b\\shell\\inspect") ==
            std::vector<std::wstring>{L".snapshot0"} && caseChanged.revision != first.revision &&
            caseChanged.folderRevision == first.folderRevision,
            "next scan preserves the raw user choice case in row fingerprints");
        Put(choice.value, L"", L"ProgId", L"Choice.C");
        const auto changed = ext::ReadCatalogue(HKEY_CLASSES_ROOT, false);
        Check(Types(changed, "reg:choice.b\\shell\\inspect") ==
            std::vector<std::wstring>{L"progid:choice.b"} && changed.revision != first.revision &&
            changed.folderRevision == first.folderRevision,
            "next scan follows live user choice under parent restrictions");

        choiceRestriction.Restore();
        choice.Close(); choiceRoot.Close();
        Check(RegDeleteTreeW(user.value, choiceParent.c_str()) == ERROR_SUCCESS,
            "replace only the owned choice parent");
        Put(user.value, choiceParent + L"\\.snapshot0\\UserChoice", L"ProgId", L"Choice.B");
        Check(snowdesktop::settings_ipc::Pack(ext::ReadCatalogue(HKEY_CLASSES_ROOT, false)) ==
            snowdesktop::settings_ipc::Pack(first),
            "new scan reopens a replaced parent instead of retaining deleted handles");
        extensionRestriction.Restore();
        overrides.Restore();
    }
    registry.Cleanup();
}

// Opt-in native observation fixture for the running host, never an automatic
// desktop interaction test. It stays at the bottom of the Z order, does not
// activate or capture input, and destroys only its own HWND after ten seconds.
int RunCatalogueFullscreenFixture()
{
    const auto instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"SnowDesktopCatalogueFullscreenFixture";
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpszClassName = name;
    windowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
        if (message == WM_TIMER && wp == 1) { DestroyWindow(window); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, wp, lp);
    };
    Check(RegisterClassW(&windowClass) != 0, "register the private fullscreen fixture");
    struct RegistrationCleanup {
        HINSTANCE instance; const wchar_t* name;
        ~RegistrationCleanup() { UnregisterClassW(name, instance); }
    } registration{instance, name};
    MONITORINFO monitor{sizeof(monitor)};
    Check(GetMonitorInfoW(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE,
        "read the fixture monitor geometry");
    const auto bounds = monitor.rcMonitor;
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, name, L"", WS_POPUP,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, nullptr);
    Check(window != nullptr, "create only the private fixture window");
    struct WindowCleanup {
        HWND window;
        ~WindowCleanup() { if (IsWindow(window)) DestroyWindow(window); }
    } cleanup{window};
    Check(SetWindowPos(window, HWND_BOTTOM, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE, "show the fixture behind existing windows without activation");
    Check(snowdesktop::fullscreen::ObserveWindow(window, 0).has_value(),
        "the real native fullscreen observer accepts the external fixture geometry");
    Check(SetTimer(window, 1, 10000, nullptr) != 0, "bound the native fixture lifetime");
    std::cout << "FULLSCREEN_FIXTURE_READY pid=" << GetCurrentProcessId() << std::endl;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    std::cout << "FULLSCREEN_FIXTURE_CLOSED" << std::endl;
    return 0;
}
