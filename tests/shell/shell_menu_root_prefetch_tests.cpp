#include "shell/shell_extension_menu_presentation.h"
#include "shell/shell_menu_read_projection.h"
#include "ui/menu/modern_menu_dynamic_items_rules.h"
#include "shell_service_catalogue_fixture.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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
void SetImage(ext::Entry& entry, int size, unsigned char color)
{
    entry.width = entry.height = size;
    entry.pixels.resize(static_cast<size_t>(size) * size * 4);
    for (size_t i = 0; i < entry.pixels.size(); i += 4)
    {
        entry.pixels[i] = entry.pixels[i + 1] = entry.pixels[i + 2] = color;
        entry.pixels[i + 3] = 255;
    }
}
bool ImageMatches(HBITMAP image, const ext::Entry& entry)
{
    if (entry.pixels.empty()) return image == nullptr;
    BITMAP bitmap{};
    return image && GetObjectW(image, sizeof(bitmap), &bitmap) && bitmap.bmBits &&
        bitmap.bmWidth == entry.width && bitmap.bmHeight == entry.height &&
        std::memcmp(bitmap.bmBits, entry.pixels.data(), entry.pixels.size()) == 0;
}
}

// Manual observation of installed providers. No popup is shown and no command
// is invoked; both publications come from the same real helper session.
void ProbeShellMenuIconStages()
{
    Workspace workspace;
    const auto selected = workspace.path / L"selected.txt";
    std::ofstream(selected) << "private icon stage sample";
    const auto report = [](const char* phase, const ext::Reply& reply) {
        for (const auto& entry : reply.entries)
        {
            if (entry.label != L"7-Zip" && entry.label != L"WinRAR") continue;
            int left = entry.width, top = entry.height, right = 0, bottom = 0;
            std::uint64_t hash = 14695981039346656037ull;
            for (const auto byte : entry.pixels) { hash ^= byte; hash *= 1099511628211ull; }
            if (entry.pixels.size() == static_cast<size_t>(entry.width * entry.height * 4))
                for (int y = 0; y < entry.height; ++y)
                    for (int x = 0; x < entry.width; ++x)
                        if (entry.pixels[(static_cast<size_t>(y) * entry.width + x) * 4 + 3] > 127)
                        {
                            left = std::min(left, x); top = std::min(top, y);
                            right = std::max(right, x + 1); bottom = std::max(bottom, y + 1);
                        }
            std::cout << "icon_stage phase=" << phase << " key=" << entry.key
                << " width=" << entry.width << " height=" << entry.height
                << " alpha_bounds=" << left << ',' << top << ',' << right << ',' << bottom
                << " pixels_hash=" << hash << " children=" << entry.children.size() << std::endl;
        }
    };
    for (const bool source : {false, true})
    {
        ext::Request request; request.paths = {selected.wstring()}; request.context = ext::Context::File;
        request.visibleMenuProviders = std::vector<std::string>{"*"};
        if (source) { request.sourceClsid = L"{23170f69-40c1-278a-1000-000100020000}"; request.sourceKey = L"*"; }
        ext::Session session(request, 8000, false);
        std::optional<ext::Reply> complete;
        const auto deadline = GetTickCount64() + 10000;
        while (!complete && GetTickCount64() < deadline)
        {
            MSG event{};
            while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&event); DispatchMessageW(&event); }
            if (auto roots = session.PollRoots()) report("preview", *roots);
            complete = session.Poll();
            if (!complete) MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        if (auto roots = session.PollRoots()) report("preview", *roots);
        Check(complete && complete->ok, "real installed-provider icon observation completes");
        report(source ? "source" : "aggregate", *complete);
    }
}

static void RunRootPrefetchCase(const wchar_t* extension, bool folder = false, bool multiple = false)
{
    Workspace workspace;
    const auto file = workspace.path / (std::wstring(L"selected") + extension);
    if (folder) std::filesystem::create_directory(file);
    else std::ofstream(file) << "private prefetch fixture";
    ext::Request request; request.paths = {file.wstring()};
    request.context = folder ? ext::Context::Folder : ext::Context::File;
    if (multiple)
    {
        const auto second = workspace.path / L"second.txt";
        std::ofstream(second) << "second private selection";
        request.paths.push_back(second.wstring());
    }
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
        work.selectionContexts = [folder] { return ext::ContextBit(folder ? ext::Context::Folder : ext::Context::File); };
        return work;
    }, [] { return ext::Catalogue{}; });
    service.Configure(prefs);
    ext::Presentation popup(request, prefs, L"", L"", service);
    std::vector<menu::Item> items; menu::Options options;
    popup.Attach(items, options, 0);
    auto poll = [&] { if (options.pollItems) if (auto next = options.pollItems(items, true)) items = std::move(*next); };
    Until([&] { poll(); return !items.empty(); }, "root becomes visible while the full query is deliberately blocked");
    Check(items.size() == 2 && items.front().label == L"Provider" && items.front().childrenPending,
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
    Check(items.size() == 2 && !items.front().separator && !items.back().separator,
        "top-level extensions remain one section without native or publication-batch dividers");
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

static void RunStableSourceCompletion(bool footer, bool previewImage = false,
                                     int finalSize = 64, bool finalImage = true)
{
    Workspace workspace;
    const auto selected = workspace.path / L"selected.txt", cache = workspace.path / L"cache";
    std::ofstream(selected) << "private stable menu fixture";
    ext::Request request; request.paths = {selected.wstring()}; request.context = ext::Context::File;
    ext::Catalogue catalogue; catalogue.revision = 1;
    for (int index = 0; index < 2; ++index)
    {
        ext::Registration source;
        const auto clsid = "{185581d0-0f34-4419-9439-36d1032d901" + std::to_string(index) + "}";
        source.id = "clsid:" + clsid; source.verbs = {clsid};
        source.kind = ext::RegistrationKind::Handler; source.contexts = 1; source.types = {L"*"};
        source.sources = {L"*\\shellex\\ContextMenuHandlers\\Stable" + std::to_wstring(index)};
        source.revision = source.menuRevision = 1;
        catalogue.rows.push_back(std::move(source));
    }
    shell_service_fixture::SeedMetadata(cache, catalogue);
    ext::Entry rar; rar.provider = "verb:rar-stable"; rar.key = "rar-stable"; rar.label = L"WinRAR";
    ext::Entry seven; seven.provider = "verb:seven-stable"; seven.key = "seven-stable"; seven.label = L"7-Zip";
    ext::Entry child; child.key = "archive-action"; child.label = L"Archive action"; child.token = 91;
    seven.children = {child};
    auto sourceSeven = seven;
    SetImage(sourceSeven, 32, 80);
    SetImage(sourceSeven.children.front(), 24, 90);
    auto finalSeven = sourceSeven;
    SetImage(finalSeven, finalImage ? finalSize : 0, 160);
    SetImage(finalSeven.children.front(), finalImage ? finalSize : 0, 170);
    auto otherSeven = seven;
    otherSeven.provider = "verb:other-seven"; otherSeven.key = "other-seven";
    otherSeven.children.front().label = L"Other archive action";
    otherSeven.children.front().token = 92;
    auto preview = seven;
    if (previewImage)
    {
        SetImage(preview, 16, 40);
    }
    ext::Entry loading; loading.label = L"…"; loading.enabled = false; preview.children = {loading};
    ext::Entry divider; divider.separator = true;
    ext::Preferences prefs;
    for (const auto &source : catalogue.rows) ext::SetCommon(prefs, source.id, ext::Category::Objects, true);
    for (const auto &entry : {rar, seven, otherSeven}) ext::SetCommon(prefs, entry.provider, ext::Category::Objects, true);
    std::atomic_bool releaseSeven = false, releaseAggregate = false;
    std::atomic<unsigned> invoked = 0;
    ext::MenuService service(cache, [&](const ext::Request &target) {
        const bool source = !target.sourceClsid.empty();
        const bool archiveSource = target.sourceClsid == std::wstring(catalogue.rows.back().verbs.front().begin(),
            catalogue.rows.back().verbs.front().end());
        auto sent = std::make_shared<bool>(false);
        ext::QueryWork work;
        work.poll = [&, source, archiveSource]() -> std::optional<ext::Reply> {
            if (source)
            {
                if (archiveSource && !releaseSeven) return {};
                return ext::Reply{true, {archiveSource ? sourceSeven : rar}, {}};
            }
            if (!releaseAggregate) return {};
            // Reverse the root order and retain legitimate same-caption commands.
            return ext::Reply{true, {finalSeven, divider, rar, divider, otherSeven}, {}};
        };
        if (!source) work.pollRoots = [&, sent]() -> std::optional<ext::Reply> {
            if (std::exchange(*sent, true)) return {};
            return ext::Reply{true, {preview, divider, rar}, {}};
        };
        work.selectionContexts = [] { return ext::ContextBit(ext::Context::File); };
        work.invoke = [&, source](UINT token, POINT) {
            Check(!source && token == 91, "stable row invocation uses the aggregate's exact fresh child reference");
            ++invoked;
        };
        return work;
    }, [catalogue] { return catalogue; }, {}, {},
       [catalogue](const auto &seeds) { return shell_service_fixture::Sources(catalogue, seeds); });
    service.Configure(prefs);
    ext::Presentation popup(request, prefs, L"", L"", service);
    menu::Item host; host.command = 5; host.label = L"Host action";
    menu::Item more; more.command = 7; more.label = L"More";
    std::vector<menu::Item> items{host}; if (footer) items.push_back(more);
    menu::Options options; popup.Attach(items, options, footer ? 7 : 0);
    const auto poll = [&] { if (auto next = options.pollItems(items, true)) items = std::move(*next); };
    const auto archives = [&] { return std::count_if(items.begin(), items.end(), [](const auto &item) { return item.label == L"7-Zip"; }); };
    Until([&] { poll(); return archives() == 1; }, "aggregate preview appears while the independent archive source is held");
    const auto archive = std::find_if(items.begin(), items.end(), [](const auto &item) { return item.label == L"7-Zip"; });
    const auto position = std::distance(items.begin(), archive);
    const auto command = archive->command;
    const auto firstImage = archive->image;
    Check(archive->childrenPending && (firstImage != nullptr) == previewImage,
        "private archive preview exposes the selected early-icon scenario");
    const auto rowCommands = [&] {
        std::vector<UINT> commands;
        for (const auto &item : items) if (!item.separator && item.command != 7) commands.push_back(item.command);
        return commands;
    };
    const auto initialCommands = rowCommands();
    releaseSeven = true;
    Until([&] {
        poll();
        const auto view = service.MenuDisplay(request, prefs, true);
        return std::any_of(view.sources.begin(), view.sources.end(), [&](const auto &source) {
            if (source.sourceClsid != std::wstring(catalogue.rows.back().verbs.front().begin(),
                    catalogue.rows.back().verbs.front().end())) return false;
            const auto ready = service.MenuDisplay(source, prefs, true);
            return ready.snapshot.has_value() && !ready.pending && !ready.attributionPending;
        });
    },
        "independent archive payload completes while the aggregate stays held");
    poll();
    Check(archives() == 1 && rowCommands() == initialCommands &&
        items[static_cast<size_t>(position)].command == command && items[static_cast<size_t>(position)].image &&
        !items[static_cast<size_t>(position)].childrenPending,
        "LATE_SOURCE_NO_DUPLICATE: ownership and icon arrival complete the same preview row in place");
    Check(ImageMatches(items[static_cast<size_t>(position)].image, sourceSeven) &&
        ImageMatches(items[static_cast<size_t>(position)].children.front().image, sourceSeven.children.front()),
        "INTERIM_ICON_RECOVERS: complete source pixels replace the provisional image without resampling");
    if (firstImage) Check(items[static_cast<size_t>(position)].image != firstImage,
        "a small or otherwise incorrect first bitmap is not locked for the whole popup");
    Check(std::count_if(items.begin(), items.end(), [](const auto &item) { return item.separator; }) == 1,
        "ONE_EXTENSION_BOUNDARY: independent batches do not introduce per-provider dividers");
    const auto childCommand = items[static_cast<size_t>(position)].children.front().command;
    const auto sourceImage = items[static_cast<size_t>(position)].image;
    auto hovered = items;
    releaseAggregate = true;
    Until([&] { poll(); return options.pollItemsFinished(); }, "aggregate finalizes without reordering visible source rows");
    const auto completedCommands = rowCommands();
    auto retained = completedCommands;
    std::erase_if(retained, [&](UINT id) {
        return std::find(initialCommands.begin(), initialCommands.end(), id) == initialCommands.end();
    });
    Check(completedCommands.size() == initialCommands.size() + 1 && retained == initialCommands && archives() == 2,
        "STABLE_ROOT_ORDER: fixed ordering retains existing relative order and distinct same-caption commands");
    const auto completedArchive = std::find_if(items.begin(), items.end(),
        [=](const auto &item) { return item.command == command; });
    Check(completedArchive != items.end() && completedArchive->children.front().command == childCommand,
        "root and child command IDs survive aggregate ownership reconciliation");
    const auto& expectedImage = finalImage ? finalSeven : sourceSeven;
    Check(ImageMatches(completedArchive->image, expectedImage) &&
        ImageMatches(completedArchive->children.front().image, expectedImage.children.front()),
        "FINAL_ICON_AUTHORITATIVE: final root/child pixels correct interim content; missing images retain valid source pixels");
    const auto hoveredArchive = [&]() -> const menu::Item& {
        return *std::find_if(hovered.begin(), hovered.end(), [=](const auto& row) { return row.command == command; });
    };
    unsigned replacedChildren = 0;
    const auto before = [&](auto&) { ++replacedChildren; };
    const auto after = [](auto&) {};
    Check(!menu::ApplyPrefetchedChildren(hovered, items, false, before, after) && hoveredArchive().image == sourceImage,
        "pointer press defers icon updates as well as subtree completion");
    const bool imagesChanged = menu::ApplyPrefetchedChildren(hovered, items, true, before, after);
    Check(imagesChanged == finalImage && replacedChildren == 0 &&
        ImageMatches(hoveredArchive().image, expectedImage) &&
        ImageMatches(hoveredArchive().children.front().image, expectedImage.children.front()),
        "HOVERED_ICON_RECOVERS: valid images update loaded roots/children without replacing their backing arrays");
    Check(std::count_if(items.begin(), items.end(), [](const auto &item) { return item.separator; }) == 1 &&
        !items.front().separator && !items.back().separator,
        "completed extension section has exactly one host boundary with or without the footer");
    const auto renderOrder = [](const auto &rows) {
        std::vector<std::pair<std::wstring, std::wstring>> result;
        for (const auto &row : rows) if (!row.separator && row.command >= ext::Presentation::FirstCommand)
            result.emplace_back(row.label, row.children.empty() ? L"" : row.children.front().label);
        return result;
    };
    ext::Presentation warm(request, prefs, L"", L"", service);
    std::vector<menu::Item> warmItems{host}; if (footer) warmItems.push_back(more);
    menu::Options warmOptions; warm.Attach(warmItems, warmOptions, footer ? 7 : 0);
    Check(renderOrder(items) == renderOrder(warmItems),
        "COLD_WARM_SAME_ORDER: native aggregate order and cache hits use the same deterministic root ordering");
    if (finalImage)
    {
        const auto warmArchive = std::find_if(warmItems.begin(), warmItems.end(), [](const auto& row) {
            return row.label == L"7-Zip" && !row.children.empty() && row.children.front().label == L"Archive action";
        });
        Check(warmArchive != warmItems.end() && ImageMatches(warmArchive->image, finalSeven) &&
            ImageMatches(warmArchive->children.front().image, finalSeven.children.front()),
            "COLD_WARM_SAME_ICON: both openings use the authoritative complete image pixels");
    }
    Check(popup.Invoke(childCommand, {}), "the retained child command remains invokable");
    Until([&] { return invoked == 1; }, "retained command resolves against a fresh aggregate exactly once");
    service.Shutdown();
}

void TestShellMenuRootPrefetch()
{
    {
        ext::Entry original; original.provider = "verb:stable"; original.key = "stable"; original.label = L"7-Zip";
        auto owned = original; owned.registration = "clsid:first";
        Check(ext::presentation_detail::SameDisplayEntry(original, owned), "late ownership is display metadata");
        auto different = owned; different.registration = "clsid:second";
        Check(!ext::presentation_detail::SameDisplayEntry(owned, different), "known distinct owners never collapse by title");
        different = original; different.key = "different";
        Check(!ext::presentation_detail::SameDisplayEntry(original, different), "same-caption different canonical commands stay distinct");
        original.children = {owned, owned}; owned.children = {different};
        auto incoming = original; incoming.children.push_back(owned);
        const auto merged = ext::presentation_detail::MergeEntry(original, incoming, false);
        Check(merged.children.size() == 3 && merged.children.back().children.size() == 1,
            "child supplementation preserves repeated command multiplicity and new subtrees");
    }
    RunStableSourceCompletion(false);
    RunStableSourceCompletion(true);
    RunStableSourceCompletion(false, true);
    RunStableSourceCompletion(true, true);
    RunStableSourceCompletion(false, true, 32); // Same size, corrected pixels.
    RunStableSourceCompletion(true, true, 13); // A valid final image may be smaller.
    RunStableSourceCompletion(false, true, 64, false); // No final image must not erase a valid one.
    ext::Request policy; policy.visibleMenuProviders = std::vector<std::string>{"*"};
    for (const auto context : {ext::Context::File, ext::Context::Folder, ext::Context::FolderBackground, ext::Context::Desktop})
        for (const auto path : {L"C:\\Private\\file.txt", L"C:\\Private\\program.lnk", L"C:\\Private\\folder", L"::{645FF040-5081-101B-9F08-00AA002F954E}"})
        {
            policy.context = context; policy.background = context == ext::Context::FolderBackground || context == ext::Context::Desktop;
            policy.paths = {path};
            Check(ext::CanPreviewPopupRoots(policy, 0), "root preview policy covers file, shortcut, folder, namespace and background contexts");
        }
    policy.originalShortcutOnly = true;
    Check(ext::CanPreviewPopupRoots(policy, 0), "original-shortcut recovery uses the same root preview policy");
    Check(!ext::CanPreviewPopupRoots(policy, 1), "only roots publish early headers");
    policy.catalogueOnly = true;
    Check(!ext::CanPreviewPopupRoots(policy, 0), "management metadata does not publish popup headers");
    policy.catalogueOnly = false; policy.startPinOnly = true;
    Check(!ext::CanPreviewPopupRoots(policy, 0), "Start state metadata keeps its independent lane");
    policy.startPinOnly = false; policy.sourceClsid = L"private source";
    Check(!ext::CanPreviewPopupRoots(policy, 0), "source attribution metadata cannot introduce provisional menu entries");
    for (const auto extension : {L".txt", L".png", L".zip", L".pdf", L".lnk", L".url"})
        RunRootPrefetchCase(extension);
    RunRootPrefetchCase(L"", true);
    RunRootPrefetchCase(L".txt", false, true);
}
