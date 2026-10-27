#pragma once

#include "dock/dock_window_preview.h"
#include "dock/dock_window_preview_layout.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

// Replace only the owned test process's DWM/monitor-DPI imports. Show, layout,
// native popup creation/geometry, messages and cleanup remain production code.
// Existing real-DWM and host opening-order regressions still run separately.
namespace dock_preview_thumbnail_cases
{
struct ImportPatch
{
    ULONG_PTR* slot = nullptr;
    ULONG_PTR original = 0;
    ImportPatch(const char* name, ULONG_PTR replacement)
    {
        auto* module = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
        const auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(module +
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for (auto* entry = imports; entry->Name && !slot; ++entry)
        {
            if (!entry->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(module + entry->OriginalFirstThunk);
            auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(module + entry->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++addresses)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* imported = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    module + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(imported->Name), name) == 0)
                {
                    slot = &addresses->u1.Function;
                    break;
                }
            }
        }
        if (!slot) throw std::runtime_error(std::string("missing thumbnail import: ") + name);
        DWORD protection = 0;
        if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
            throw std::runtime_error("cannot patch owned thumbnail import");
        original = *slot;
        *slot = replacement;
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    }
    ~ImportPatch()
    {
        DWORD protection = 0;
        if (slot && VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
        {
            *slot = original;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(*slot), protection, &ignored);
        }
    }
    ImportPatch(const ImportPatch&) = delete;
    ImportPatch& operator=(const ImportPatch&) = delete;
};

struct Relationship
{
    HWND destination = nullptr;
    HWND source = nullptr;
    unsigned queries = 0;
    DWM_THUMBNAIL_PROPERTIES properties{};
};
struct Boundary
{
    std::unordered_map<HTHUMBNAIL, Relationship> live;
    std::unordered_map<HWND, SIZE> sizes;
    std::unordered_map<HWND, unsigned> sourceQueries;
    std::vector<HTHUMBNAIL> keepDuringMeasurement;
    HWND failRegister = nullptr;
    HWND failQuery = nullptr;
    HWND failUpdate = nullptr;
    HWND resizeAtDisplay = nullptr;
    UINT dpi = 96;
    unsigned registrations = 0, queries = 0, updates = 0, retirements = 0, errors = 0;
    std::uintptr_t next = 0x4000;
    void ResetCounts() { registrations = queries = updates = retirements = 0; }
};
inline Boundary boundary;

inline HRESULT WINAPI Register(HWND destination, HWND source, HTHUMBNAIL* result)
{
    ++boundary.registrations;
    *result = nullptr;
    if (boundary.failRegister == source)
    {
        boundary.failRegister = nullptr;
        return E_FAIL;
    }
    if (!IsWindow(destination) || !IsWindow(source)) return E_INVALIDARG;
    *result = reinterpret_cast<HTHUMBNAIL>(boundary.next++);
    boundary.live.emplace(*result, Relationship{destination, source});
    return S_OK;
}
inline HRESULT WINAPI Query(HTHUMBNAIL handle, SIZE* result)
{
    ++boundary.queries;
    const auto found = boundary.live.find(handle);
    if (found == boundary.live.end()) { ++boundary.errors; return E_INVALIDARG; }
    auto& link = found->second;
    ++link.queries;
    if (link.queries == 1)
        for (const auto previous : boundary.keepDuringMeasurement)
            if (!boundary.live.contains(previous)) ++boundary.errors;
    const unsigned sourceQueries = ++boundary.sourceQueries[link.source];
    if (boundary.failQuery == link.source)
    {
        boundary.failQuery = nullptr;
        return E_FAIL;
    }
    if (!IsWindow(link.source)) return E_INVALIDARG;
    *result = boundary.sizes.at(link.source);
    if (link.source == boundary.resizeAtDisplay && sourceQueries > 1)
        *result = {500, 1000};
    return S_OK;
}
inline HRESULT WINAPI Update(HTHUMBNAIL handle, const DWM_THUMBNAIL_PROPERTIES* properties)
{
    ++boundary.updates;
    const auto found = boundary.live.find(handle);
    if (found == boundary.live.end()) { ++boundary.errors; return E_INVALIDARG; }
    if (boundary.failUpdate == found->second.source)
    {
        boundary.failUpdate = nullptr;
        return E_FAIL;
    }
    if (!IsWindow(found->second.source) || !IsWindow(found->second.destination)) return E_INVALIDARG;
    found->second.properties = *properties;
    return S_OK;
}
inline HRESULT WINAPI Unregister(HTHUMBNAIL handle)
{
    ++boundary.retirements;
    if (boundary.live.erase(handle) != 1) { ++boundary.errors; return E_INVALIDARG; }
    return S_OK;
}
inline HRESULT WINAPI MonitorDpi(HMONITOR, MONITOR_DPI_TYPE, UINT* x, UINT* y)
{
    *x = *y = boundary.dpi;
    return S_OK;
}

struct Source
{
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"thumbnail-boundary-fixture", WS_OVERLAPPEDWINDOW,
        -16000, -16000, 300, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Source() { if (window && IsWindow(window)) DestroyWindow(window); }
};

template<class Check>
void Run(Check check)
{
    boundary = {};
    try
    {
        ImportPatch registration("DwmRegisterThumbnail", reinterpret_cast<ULONG_PTR>(&Register));
        ImportPatch query("DwmQueryThumbnailSourceSize", reinterpret_cast<ULONG_PTR>(&Query));
        ImportPatch update("DwmUpdateThumbnailProperties", reinterpret_cast<ULONG_PTR>(&Update));
        ImportPatch retirement("DwmUnregisterThumbnail", reinterpret_cast<ULONG_PTR>(&Unregister));
        ImportPatch dpi("GetDpiForMonitor", reinterpret_cast<ULONG_PTR>(&MonitorDpi));
        Source first, second;
        check(first.window && second.window, "thumbnail source fixtures are real owned off-screen windows");
        if (!first.window || !second.window) return;
        boundary.sizes[first.window] = {1000, 500};
        boundary.sizes[second.window] = {500, 1000};
        const RECT anchor{0, 0, 20, 20};
        MONITORINFO monitor{sizeof(monitor)};
        check(GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE,
            "thumbnail layout fixture resolves its actual monitor work area");
        const auto checkGeometry = [&](const DockWindowPreview& preview, const std::vector<SIZE>& sizes) {
            const int width = static_cast<int>(std::floor((monitor.rcWork.right - monitor.rcWork.left) * 0.90));
            const int height = static_cast<int>(std::floor((monitor.rcWork.bottom - monitor.rcWork.top) * 0.78));
            const auto layout = CalculateDockWindowPreviewLayout(sizes, width, height, boundary.dpi);
            const auto expected = CalculateDockWindowPreviewLayoutCardRects(layout, boundary.dpi);
            check(preview.GetWindows().size() == expected.size(), "measurement preserves native card count");
            for (size_t i = 0; i < std::min(preview.GetWindows().size(), expected.size()); ++i)
            {
                RECT actual{};
                check(GetClientRect(preview.GetWindows()[i], &actual) &&
                        actual.right == expected[i].right - expected[i].left &&
                        actual.bottom == expected[i].bottom - expected[i].top,
                    "registered source sizes feed unchanged card geometry at the current DPI");
            }
        };
        {
            DockWindowPreview preview;
            HWND closed = nullptr;
            check(preview.Initialize(GetModuleHandleW(nullptr), [](HWND) {},
                [&](HWND window) { closed = window; }), "thumbnail lifecycle preview initializes");
            const auto show = [&](bool reverse = false) {
                preview.Show(reverse ? std::vector<DockWindowPreviewItem>{{second.window, L"portrait"}, {first.window, L"landscape"}}
                                     : std::vector<DockWindowPreviewItem>{{first.window, L"landscape"}, {second.window, L"portrait"}, {first.window, L"duplicate"}},
                    anchor, DockPosition::Bottom, false);
            };
            const auto checkLinks = [&](const std::vector<HWND>& sources) {
                check(boundary.live.size() == sources.size(), "only current source/card relationships remain registered");
                for (size_t i = 0; i < std::min(sources.size(), preview.GetWindows().size()); ++i)
                {
                    const auto found = std::find_if(boundary.live.begin(), boundary.live.end(), [&](const auto& entry) {
                        return entry.second.destination == preview.GetWindows()[i] && entry.second.source == sources[i];
                    });
                    check(found != boundary.live.end(), "each source remains bound to its own ordered popup");
                    if (found == boundary.live.end()) continue;
                    const auto& link = found->second;
                    const auto& props = link.properties;
                    RECT client{};
                    GetClientRect(link.destination, &client);
                    check(link.queries == 2 && props.dwFlags == (DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                            DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY) &&
                            props.fVisible && props.opacity == 255 && !props.fSourceClientAreaOnly &&
                            props.rcDestination.left >= 0 && props.rcDestination.top >= MulDiv(34, static_cast<int>(boundary.dpi), 96) &&
                            props.rcDestination.right <= client.right && props.rcDestination.bottom <= client.bottom,
                        "one retained handle observes measurement and fresh display size with unchanged visible properties");
                }
            };
            show();
            check(boundary.registrations == 2 && boundary.queries == 4 && boundary.updates == 2 && boundary.retirements == 0,
                "opening two cards registers twice without temporary measurement registrations");
            checkLinks({first.window, second.window});
            checkGeometry(preview, {{1000, 500}, {500, 1000}});
            boundary.ResetCounts();
            preview.UpdateAnchor({20, 20, 40, 40}, DockPosition::Bottom);
            check(boundary.registrations == 0 && boundary.queries == 0 && boundary.updates == 0,
                "same-DPI anchor movement does not refresh thumbnail relationships");
            boundary.dpi = 144;
            preview.UpdateAnchor(anchor, DockPosition::Bottom);
            check(boundary.registrations == 2 && boundary.retirements == 2 && boundary.updates == 2,
                "DPI relayout retires the old generation and registers each new card once");
            checkLinks({first.window, second.window});
            checkGeometry(preview, {{1000, 500}, {500, 1000}});
            boundary.ResetCounts();
            for (const auto& entry : boundary.live) boundary.keepDuringMeasurement.push_back(entry.first);
            show(true);
            boundary.keepDuringMeasurement.clear();
            check(boundary.registrations == 2 && boundary.retirements == 2, "target changes replace the previous thumbnail generation");
            check(boundary.errors == 0, "previous displayed thumbnails survive measurement until the original display boundary");
            checkLinks({second.window, first.window});
            boundary.sourceQueries.clear();
            boundary.resizeAtDisplay = first.window;
            show();
            checkGeometry(preview, {{1000, 500}, {500, 1000}});
            for (const auto& entry : boundary.live)
            {
                const auto& link = entry.second;
                if (link.source != first.window) continue;
                const RECT& destination = link.properties.rcDestination;
                check(std::abs((destination.bottom - destination.top) - 2 * (destination.right - destination.left)) <= 1,
                    "a source resized after measurement uses its fresh portrait ratio for display");
            }
            boundary.resizeAtDisplay = nullptr;
            for (const bool failRegistration : {true, false})
            {
                boundary.ResetCounts();
                if (failRegistration) boundary.failRegister = first.window;
                else boundary.failQuery = first.window;
                show();
                check(boundary.registrations == (failRegistration ? 3u : 2u) && boundary.live.size() == 2,
                    "failed measurement registration retries at display; a failed query keeps the valid relationship");
                RECT fallback{};
                GetWindowRect(first.window, &fallback);
                checkGeometry(preview, {{fallback.right - fallback.left, fallback.bottom - fallback.top}, {500, 1000}});
            }
            boundary.failUpdate = first.window;
            show();
            check(boundary.live.size() == 1 && boundary.live.begin()->second.source == second.window,
                "failed thumbnail updates unregister the failed relationship immediately");
            preview.Hide();
            check(boundary.live.empty() && preview.IsCleared(), "Hide clears every handle including an update-failure slot");
            show();
            checkLinks({first.window, second.window});
            if (preview.GetWindows().size() == 2) DestroyWindow(preview.GetWindows()[1]);
            boundary.ResetCounts();
            boundary.dpi = 192;
            preview.UpdateAnchor(anchor, DockPosition::Bottom);
            check(boundary.registrations == 3 && boundary.retirements == 3 && boundary.live.size() == 1 &&
                    boundary.live.begin()->second.source == first.window,
                "DPI updates still measure a source whose card was destroyed, without retaining a temporary relationship");
            boundary.ResetCounts();
            show();
            check(boundary.registrations == 2 && boundary.retirements == 1,
                "recreated destination popups do not reuse their former thumbnail handles");
            checkLinks({first.window, second.window});
            DestroyWindow(first.window);
            first.window = nullptr;
            boundary.ResetCounts();
            show();
            check(boundary.registrations == 1 && boundary.retirements == 2,
                "destroyed sources are filtered before a fresh surviving-source registration");
            checkLinks({second.window});
            Source replacement;
            check(replacement.window != nullptr, "a replacement source can be created after its predecessor is destroyed");
            if (replacement.window)
            {
                boundary.sizes[replacement.window] = {600, 400};
                boundary.ResetCounts();
                preview.Show({{second.window, L"survivor"}, {replacement.window, L"replacement"}},
                    anchor, DockPosition::Bottom, false);
                check(boundary.registrations == 2 && boundary.retirements == 1,
                    "a new or recycled source HWND gets a fresh relationship after the surviving source");
                checkLinks({second.window, replacement.window});
            }
            if (!preview.GetWindows().empty())
                SendMessageW(preview.GetWindow(), WM_MBUTTONUP, 0, MAKELPARAM(10, 10));
            check(closed == second.window && boundary.live.empty() && preview.IsCleared(),
                "production close routing hides the group and releases retained registrations");
            show();
            preview.Show({}, anchor, DockPosition::Bottom, false);
            check(boundary.live.empty() && preview.IsCleared(), "empty target groups release all registrations");
            show();
        }
        check(boundary.live.empty() && boundary.errors == 0,
            "destruction unregisters each remaining relationship once without stale or double-released handles");
        std::cout << "preview thumbnail lifetime, call-count, geometry and failure cases completed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "thumbnail fixture: " << error.what() << '\n';
        check(false, "thumbnail production-path fixture must complete without boundary setup failure");
    }
}
}
