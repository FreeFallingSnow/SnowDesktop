#pragma once
#include "shell/shell_extension_menu.h"
#include "shell/shell_extension_catalogue.h"
#include "shell/shell_extension_menu_presentation.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <wrl/implements.h>
#include <atomic>
#include <shlwapi.h>

namespace shell_icon_stage_probe
{
namespace ext = snowdesktop::shell_extensions;
inline void Report(const char* phase, const ext::Reply& reply)
{
    std::cout << "icon_lane phase=" << phase << " entries=" << reply.entries.size() << std::endl;
    for (const auto& entry : reply.entries)
    {
        if (entry.separator || (entry.label != L"7-Zip" && entry.label != L"WinRAR" &&
            std::string_view(phase) != "source-winrar")) continue;
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
        std::cout << "icon_stage phase=" << phase << " title=" <<
            (entry.label == L"7-Zip" ? "7-Zip" : entry.label == L"WinRAR" ? "WinRAR" : "other-WinRAR-command")
            << " provider=" << entry.provider << " key=" << entry.key << " registration=" << entry.registration
            << " width=" << entry.width << " height=" << entry.height
            << " logical_size=" << entry.imageLogicalSize
            << " alpha_bounds=" << left << ',' << top << ',' << right << ',' << bottom
            << " pixels_hash=" << hash << " children=" << entry.children.size() << std::endl;
    }
}
inline void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> inline void Until(Predicate ready, const char* message)
{
    const auto deadline = GetTickCount64() + 5000;
    while (!ready() && GetTickCount64() < deadline)
    {
        MSG event{};
        while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&event); DispatchMessageW(&event); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    Check(ready(), message);
}
// Exercises the real icon extractor, codec, disk cache, asynchronous service
// and popup conversion. Only the external command/query boundary is replaced.
inline void TestContracts(const std::filesystem::path& directory)
{
    struct Command final : Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IExplorerCommand>
    {
        std::wstring location; unsigned unrelatedCalls = 0;
        GUID canonical = {0xb41db860, 0x64e4, 0x11d2, {0x99, 0x06, 0xe4, 0x9f, 0xad, 0xc1, 0x73, 0xca}};
        IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR*) override { ++unrelatedCalls; return E_NOTIMPL; }
        IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* value) override { return SHStrDupW(location.c_str(), value); }
        IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR*) override { ++unrelatedCalls; return E_NOTIMPL; }
        IFACEMETHODIMP GetCanonicalName(GUID* value) override { *value = canonical; return S_OK; }
        IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE*) override { ++unrelatedCalls; return E_NOTIMPL; }
        IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override { ++unrelatedCalls; return E_NOTIMPL; }
        IFACEMETHODIMP GetFlags(EXPCMDFLAGS*) override { ++unrelatedCalls; return E_NOTIMPL; }
        IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand**) override { ++unrelatedCalls; return E_NOTIMPL; }
    };
    auto command = Microsoft::WRL::Make<Command>();
    wchar_t system[MAX_PATH]{};
    Check(GetSystemDirectoryW(system, MAX_PATH) != 0, "find system icon fixture");
    command->location = (std::filesystem::path(system) / L"shell32.dll").wstring() + L",0";
    auto image = ext::ReadMenuCommandRootIcon(command.Get(), nullptr, "fallback");
    Check(image && image->width == 32 && image->height == 32 && image->imageLogicalSize == 16 &&
        image->key == "{B41DB860-64E4-11D2-9906-E49FADC173CA}" && command->unrelatedCalls == 0,
        "modern preview reads only canonical identity/icon and retains resolution independently from logical size");
    command->canonical = GUID_NULL;
    auto fallback = ext::ReadMenuCommandRootIcon(command.Get(), nullptr, "fallback");
    Check(fallback && fallback->key == "fallback", "null command identity retains its proven activation key");
    command->location = (directory / L"missing-icon.dll").wstring() + L",0";
    Check(!ext::ReadMenuCommandRootIcon(command.Get(), nullptr, "fallback"), "missing modern artwork cannot replace a valid native image");

    ext::Request request; request.paths = {(directory / L"selected.txt").wstring()}; request.context = ext::Context::File;
    std::ofstream(directory / L"selected.txt") << "private icon contract selection";
    image->provider = "verb:icon-contract"; image->key = "icon-contract"; image->label = L"Provider";
    ext::Entry child; child.key = "action"; child.label = L"Action"; child.token = 77; image->children = {child};
    ext::MenuSnapshotCache cache(directory / L"contract-cache");
    const auto ticket = cache.Begin(cache.Capture(request));
    Check(cache.Store(ticket, {true, {*image}, {}}), "store modern image metadata");
    ext::MenuSnapshotCache disk(cache.Directory());
    const auto restored = disk.Find(disk.Capture(request));
    Check(restored && restored->entries.front().imageLogicalSize == 16 &&
        restored->entries.front().pixels == image->pixels, "disk cache preserves icon density as well as pixels");
    auto invalid = *image; invalid.imageLogicalSize = -1;
    Check(!cache.Store(cache.Begin(cache.Capture(request)), {true, {invalid}, {}}), "invalid icon density is rejected at the cache boundary");

    struct DpiCase { UINT dpi; int expected; };
    for (const auto sample : {DpiCase{96, 16}, {120, 20}, {144, 24}, {192, 32}})
    {
        auto final = *image;
        // Explicit opaque pixels give an independent exact output observation.
        for (size_t i = 0; i < final.pixels.size(); i += 4)
        { final.pixels[i] = 80; final.pixels[i + 1] = final.pixels[i + 2] = 0; final.pixels[i + 3] = 255; }
        auto preview = final;
        for (size_t i = 0; i < preview.pixels.size(); i += 4) preview.pixels[i] = 40;
        preview.children.front().enabled = false;
        ext::Entry normal; normal.provider = "verb:normal-icon"; normal.key = "normal-icon"; normal.label = L"Normal";
        normal.width = normal.height = 20; normal.pixels.assign(20 * 20 * 4, 255); normal.token = 88;
        ext::Preferences prefs;
        ext::SetCommon(prefs, final.provider, ext::Category::Objects, true);
        ext::SetCommon(prefs, normal.provider, ext::Category::Objects, true);
        std::atomic_bool finish = false;
        ext::MenuService service(directory / std::to_wstring(sample.dpi), [&](const ext::Request&) {
            auto sent = std::make_shared<bool>(false);
            ext::QueryWork work;
            work.pollRoots = [&, sent]() -> std::optional<ext::Reply> {
                if (std::exchange(*sent, true)) return {};
                return ext::Reply{true, {preview, normal}, {}};
            };
            work.poll = [&]() -> std::optional<ext::Reply> {
                if (!finish) return {};
                return ext::Reply{true, {final, normal}, {}};
            };
            work.selectionContexts = [] { return ext::ContextBit(ext::Context::File); };
            return work;
        }, [] { return ext::Catalogue{}; });
        service.Configure(prefs);
        ext::Presentation popup(request, prefs, L"", L"", service);
        std::vector<snowdesktop::modern_menu::Item> items;
        snowdesktop::modern_menu::Options options; options.dpi = sample.dpi; popup.Attach(items, options, 0);
        const auto poll = [&] { if (auto next = options.pollItems(items, true)) items = std::move(*next); };
        const auto verify = [&](const auto& rows, unsigned char blue) {
            const auto provider = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.label == L"Provider"; });
            const auto unchanged = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.label == L"Normal"; });
            BITMAP a{}, b{};
            Check(provider != rows.end() && unchanged != rows.end() &&
                GetObjectW(provider->image, sizeof(a), &a) && GetObjectW(unchanged->image, sizeof(b), &b) &&
                a.bmWidth == sample.expected && a.bmHeight == sample.expected && b.bmWidth == 20 && b.bmHeight == 20,
                "preview/final/cache use one logical DPI size while normal native artwork retains its dimensions");
            Check(a.bmBits && static_cast<unsigned char*>(a.bmBits)[0] == blue,
                "final valid artwork corrects interim pixels instead of freezing the first image");
            return provider->command;
        };
        Until([&] { poll(); return items.size() == 2; }, "both provisional roots arrive");
        const auto id = verify(items, 40);
        finish = true;
        Until([&] { poll(); return options.pollItemsFinished(); }, "complete icon result arrives");
        Check(verify(items, 80) == id, "updating artwork retains the visible command identity");
        ext::Presentation warm(request, prefs, L"", L"", service);
        std::vector<snowdesktop::modern_menu::Item> cached;
        snowdesktop::modern_menu::Options warmOptions; warmOptions.dpi = sample.dpi; warm.Attach(cached, warmOptions, 0);
        verify(cached, 80);
        service.Shutdown();
    }
}
// Manual installed-provider observation only. Never opens a popup or invokes
// an extension command; all three lanes use the same private selected file.
inline void Run(const std::filesystem::path& selected)
{
    struct Lane { const char* name; const wchar_t* clsid; };
    const Lane lanes[] = {
        {"aggregate", L""},
        {"source-sevenzip", L"{23170f69-40c1-278a-1000-000100020000}"},
        {"source-winrar", L"{B41DB860-64E4-11D2-9906-E49FADC173CA}"},
    };
    for (const auto& lane : lanes)
    {
        ext::Request request; request.paths = {selected.wstring()}; request.context = ext::Context::File;
        request.visibleMenuProviders = std::vector<std::string>{"*"};
        request.sourceClsid = lane.clsid;
        if (*lane.clsid) request.sourceKey = L"*";
        ext::Session session(request, 8000, false);
        std::optional<ext::Reply> complete;
        const auto deadline = GetTickCount64() + 10000;
        while (!complete && GetTickCount64() < deadline)
        {
            MSG event{};
            while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&event); DispatchMessageW(&event); }
            if (auto roots = session.PollRoots()) Report("aggregate-preview", *roots);
            complete = session.Poll();
            if (!complete) MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        if (auto roots = session.PollRoots()) Report("aggregate-preview", *roots);
        if (!complete || !complete->ok) throw std::runtime_error("installed icon stage lane did not complete");
        Report(lane.name, *complete);
    }
}
}
