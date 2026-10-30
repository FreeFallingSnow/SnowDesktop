#include "shell/shell_extension_menu_presentation.h"
#include "ui/menu/modern_menu_dynamic_items_rules.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <objbase.h>
#include <stdexcept>

namespace
{
namespace ext = snowdesktop::shell_extensions;
namespace menu = snowdesktop::modern_menu;
void Check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template<class Predicate> void Until(Predicate ready, const char *message)
{
    const auto deadline = GetTickCount64() + 5000;
    while (!ready() && GetTickCount64() < deadline)
    {
        MSG event{};
        while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&event); DispatchMessageW(&event); }
        Sleep(1);
    }
    Check(ready(), message);
}
struct Workspace
{
    std::filesystem::path path;
    Workspace()
    {
        GUID guid{}; Check(SUCCEEDED(CoCreateGuid(&guid)), "create prefetch workspace ID");
        wchar_t text[40]{}; StringFromGUID2(guid, text, 40);
        path = std::filesystem::temp_directory_path() / (std::wstring(L"SnowDesktop-RootPrefetch-") + text);
        Check(std::filesystem::create_directory(path), "own prefetch workspace");
    }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(path, error); }
};
}

void TestShellMenuRootPrefetch()
{
    Workspace workspace;
    const auto file = workspace.path / L"selected.txt";
    std::ofstream(file) << "private prefetch fixture";
    ext::Request request; request.paths = {file.wstring()}; request.context = ext::Context::File;
    ext::Preferences prefs; ext::SetCommon(prefs, "verb:prefetch-test", ext::Category::Objects, true);
    ext::SetCommon(prefs, "verb:prefetch-leaf", ext::Category::Objects, true);
    std::atomic_bool finish = false;
    std::atomic<unsigned> invocations = 0, queries = 0;
    const bool suppressRoots = GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_SUPPRESS_ROOT_PREVIEW", nullptr, 0) != 0;
    ext::Entry root; root.provider = "verb:prefetch-test"; root.key = "prefetch-test"; root.label = L"Provider";
    root.width = root.height = 1; root.pixels = {10, 20, 30, 255};
    ext::Entry loading; loading.label = L"…"; loading.enabled = false;
    auto headers = root; headers.children = {loading};
    ext::Entry child; child.key = "prefetched-action"; child.label = L"Ready action"; child.token = 77;
    root.children = {child};
    ext::Entry leaf; leaf.provider = "verb:prefetch-leaf"; leaf.key = "prefetch-leaf";
    leaf.label = L"Standalone"; leaf.token = 88;
    leaf.width = leaf.height = 1; leaf.pixels = {20, 30, 40, 255};
    auto leafHeader = leaf; leafHeader.enabled = false; leafHeader.token = 0;
    leafHeader.width = leafHeader.height = 0; leafHeader.pixels.clear();
    ext::Entry separator; separator.separator = true;
    const auto cachePath = workspace.path / L"cache";
    ext::MenuService service(cachePath, [&](const ext::Request &) {
        ++queries;
        auto rootsSent = std::make_shared<std::atomic_bool>(false);
        ext::QueryWork work;
        work.poll = [&]() -> std::optional<ext::Reply> {
            if (!finish) return {};
            return ext::Reply{true, {root, separator, leaf}, {}};
        };
        if (!suppressRoots) work.pollRoots = [&, rootsSent]() -> std::optional<ext::Reply> {
            if (rootsSent->exchange(true)) return {};
            return ext::Reply{true, {headers, separator, leafHeader}, {}};
        };
        work.invoke = [&](UINT token, POINT) { Check(token == 77, "invoke the final fresh child token"); ++invocations; };
        work.selectionContexts = [] { return ext::ContextBit(ext::Context::File); };
        return work;
    }, [] { return ext::Catalogue{}; });
    service.Configure(prefs);
    ext::Presentation popup(request, prefs, L"", L"", service);
    std::vector<menu::Item> items; menu::Options options;
    popup.Attach(items, options, 0);
    auto poll = [&] { if (options.pollItems) if (auto next = options.pollItems(items, true)) items = std::move(*next); };
    Until([&] { poll(); return !items.empty(); }, "root becomes visible while the full query is deliberately blocked");
    Check(items.size() == 3 && items.front().label == L"Provider" && items.front().childrenPending,
        "root carries a transient pending subtree");
    Check(items.front().children.size() == 1 && !items.front().children.front().enabled,
        "unfinished children expose no enabled command");
    Check(options.pollPrefetchedSubmenus && !options.pollItemsFinished(), "root display does not finish polling");
    const auto partial = service.View(request);
    Check(partial.pending && !partial.snapshot && partial.roots.has_value(), "headers are not a complete snapshot");
    ext::MenuSnapshotCache cache(cachePath);
    Check(!cache.Find(cache.Capture(request)), "root headers never become a durable cache hit");
    if (std::filesystem::exists(cachePath))
        for (const auto &entry : std::filesystem::directory_iterator(cachePath))
            Check(entry.path().extension() != L".bin" || entry.path().filename().wstring().size() != 68,
                "no projected or full snapshot file can be written for provisional roots");
    Check(items.back().childrenPending && !items.back().enabled && !items.back().image,
        "a provisional standalone root remains non-executable and may lack an icon");
    const auto parentCommand = items.front().command;
    const auto image = items.front().image;
    auto open = items;
    auto *backing = &open.front().children;

    // No hover/open callback occurs: releasing the already-running query is
    // sufficient for every child to finish and publish automatically.
    finish = true;
    Until([&] { poll(); return options.pollItemsFinished(); }, "children finish without an enter-submenu request");
    Check(items.size() == 3 && !items.front().separator && items[1].separator && !items.back().separator,
        "final publication replaces provisional dividers without leading or duplicate separators");
    Check(queries == 1 && items.front().command == parentCommand && items.front().image == image,
        "one query retains root command identity and image while completing children");
    Check(!items.front().childrenPending && items.front().children.front().label == L"Ready action",
        "the complete result replaces loading children");
    Check(items.back().enabled && items.back().image, "the complete standalone root becomes enabled with its final icon");
    unsigned before = 0, after = 0;
    auto begin = [&](auto &children) { if (&children == backing) ++before; };
    auto end = [&](auto &children) { if (&children == backing) ++after; };
    Check(!menu::ApplyPrefetchedChildren(open, items, false, begin, end) && open.front().childrenPending,
        "a held pointer prevents replacing even a loading cascade");
    Check(menu::ApplyPrefetchedChildren(open, items, true, begin, end), "open loading cascade accepts prefetched children");
    Check(before == 1 && after == 1 && open.front().children.front().label == L"Ready action" &&
        open.front().command == parentCommand, "completion retires the old popup before rebinding its content");
    Check(open.back().enabled && open.back().image && !open.back().childrenPending,
        "a hovered standalone header becomes enabled without pointer leave or another query");
    Check(!menu::ApplyPrefetchedChildren(open, items, true, begin, end), "already loaded menus are not replaced again");
    Check(popup.Invoke(items.front().children.front().command, {}), "final child uses the normal command route");
    Until([&] { return invocations == 1; }, "execution resolves a fresh complete token, not a header token");
    service.Shutdown();
}
