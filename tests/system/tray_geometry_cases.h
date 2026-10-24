#pragma once
#include "system/tray/tray_geometry_table.h"
#include "system/status_bar/status_bar_geometry.h"
#include "system/status_bar/status_bar_interaction.h"
#include <iostream>
#include <map>
#include <memory>

namespace tray_geometry_tests
{
using namespace snowdesktop;
// Retain the old Service writer as a complete reference, including its
// unconditional publication after every registered key. This is an offscreen
// writer reference, not a live Service/Explorer or mutex scheduling fixture.
struct LegacyWriter
{
    std::map<std::string, tray::Geometry> geometries;
    void Set(const std::vector<tray::Icon>& icons, const std::string& key, RECT rect, tray::SharedState& state)
    {
        const auto found = std::find_if(icons.begin(), icons.end(), [&](const auto& icon) { return icon.key == key; });
        if (found == icons.end()) return;
        if (IsRectEmpty(&rect)) geometries.erase(key);
        else geometries[key] = {found->identity, rect};
        InterlockedIncrement(&state.geometrySequence);
        state.geometryCount = 0;
        for (const auto& [storedKey, geometry] : geometries)
        {
            (void)storedKey;
            if (state.geometryCount == tray::kGeometries) break;
            state.geometries[state.geometryCount++] = geometry;
        }
        MemoryBarrier(); InterlockedIncrement(&state.geometrySequence);
    }
};
inline bool EqualPublished(const tray::SharedState& a, const tray::SharedState& b)
{
    if (a.geometryCount != b.geometryCount) return false;
    for (DWORD index = 0; index < a.geometryCount; ++index)
        if (!tray::SameFocusIdentity(a.geometries[index].identity, b.geometries[index].identity) ||
            !EqualRect(&a.geometries[index].rect, &b.geometries[index].rect)) return false;
    return true;
}

template<class Check>
void Lifecycle(Check&& check)
{
    tray::Icon first, hidden, foreign;
    first.key = "first"; first.identity = {{}, 100, 1, 42}; first.identity.guid.Data1 = 0x525f0015;
    hidden.key = "hidden"; hidden.identity = {{}, 101, 2, 42};
    foreign.key = "other-presenter"; foreign.identity = {{}, 102, 3, 43};
    std::vector<tray::Icon> icons{first, hidden, foreign};
    const RECT firstRect{-1920, -80, -1888, -48}, secondRect{-1888, -80, -1856, -48}, otherRect{2600, 1080, 2632, 1112};
    const std::vector<tray::GeometryUpdate> initial{{first.key, firstRect}, {hidden.key, secondRect}, {foreign.key, otherRect}};
    tray::GeometryTable table;
    auto state = std::make_unique<tray::SharedState>();
    table.Set(icons, initial, nullptr);
    check(tray::Read(state->geometrySequence) == 0, "unavailable connection performs no shared-memory write");
    table.Set(icons, initial, state.get());
    check(state->geometryCount == 3 && tray::Read(state->geometrySequence) == 2,
        "resubmission publishes pending geometry once when the connection is available");
    const auto stable = tray::Read(state->geometrySequence);
    const std::vector<tray::GeometryUpdate> unknown{{"missing", otherRect}};
    table.Set(icons, unknown, state.get());
    const std::vector<tray::GeometryUpdate> reverted{{first.key, secondRect}, {first.key, firstRect}};
    table.Set(icons, reverted, state.get());
    check(tray::Read(state->geometrySequence) == stable, "unknown keys and a batch with no final change publish nothing");
    const std::vector<tray::GeometryUpdate> moved{{first.key, secondRect}, {hidden.key, firstRect}};
    table.Set(icons, moved, state.get());
    RECT read{};
    check(tray::Read(state->geometrySequence) == stable + 2 && tray::LookupGeometry(*state, foreign.identity, read) &&
        EqualRect(&read, &otherRect), "one batch updates related keys without deleting another presenter's geometry");
    icons[0].identity.window = 200; icons[0].identity.process = 44;
    const auto beforeRebind = tray::Read(state->geometrySequence);
    const std::vector<tray::GeometryUpdate> sameRectNewOwner{{first.key, secondRect}};
    table.Set(icons, sameRectNewOwner, state.get());
    check(tray::Read(state->geometrySequence) == beforeRebind + 2 &&
        !tray::LookupGeometry(*state, first.identity, read) && tray::LookupGeometry(*state, icons[0].identity, read) &&
        EqualRect(&read, &secondRect), "an unchanged rectangle still republishes when its registered GUID owner changes");
    icons[0].identity.window = 300; icons[0].identity.process = 45;
    icons[1].state = NIS_HIDDEN;
    table.Prune(icons, state.get());
    check(state->geometryCount == 1 && !tray::LookupGeometry(*state, first.identity, read),
        "hidden and replaced-owner entries are pruned together before a new registration supplies geometry");
    const std::vector<tray::GeometryUpdate> replaced{{first.key, firstRect}};
    table.Set(icons, replaced, state.get());
    tray::Identity guidOnly{}; guidOnly.guid = first.identity.guid;
    check(!tray::LookupGeometry(*state, first.identity, read) && tray::LookupGeometry(*state, icons[0].identity, read) &&
        EqualRect(&read, &firstRect) && tray::LookupGeometry(*state, guidOnly, read),
        "same GUID and rectangle retain the new owner and reject the retired HWND/process");
    const auto unchangedPrune = tray::Read(state->geometrySequence);
    table.Prune(icons, state.get());
    check(tray::Read(state->geometrySequence) == unchangedPrune, "unchanged pruning never republishes");
    icons.pop_back(); table.Prune(icons, state.get());
    check(!tray::LookupGeometry(*state, foreign.identity, read) && state->geometryCount == 1,
        "deleted keys are removed without clearing the remaining registered icon");
    const std::vector<tray::GeometryUpdate> removed{{first.key, {}}};
    table.Set(icons, removed, state.get());
    const auto cleared = tray::Read(state->geometrySequence);
    table.Set(icons, removed, state.get()); table.Clear(state.get());
    check(state->geometryCount == 0 && tray::Read(state->geometrySequence) == cleared,
        "empty rectangles remove keys and repeated removal/clear performs no write");
    table.Set(icons, replaced, state.get());
    table.Reset();
    auto reconnected = std::make_unique<tray::SharedState>();
    check(!table.Find(first.key) && !tray::LookupGeometry(*reconnected, icons[0].identity, read),
        "a new collector connection starts without geometry from a retired generation");
    table.Set(icons, replaced, reconnected.get());
    check(tray::Read(reconnected->geometrySequence) == 2 && tray::LookupGeometry(*reconnected, icons[0].identity, read),
        "the first layout after reconnection publishes even when its rectangle equals the retired generation");
}

template<class Check>
void Run(Check&& check, ID2D1DeviceContext* context, IDWriteFactory* text,
    StatusBarSnapshot data, const std::vector<std::string>& pinned, bool legacyNoopControl = false)
{
    Lifecycle(check);
    for (std::size_t index = 0; index < data.tray.size(); ++index)
    {
        data.tray[index].identity = {{}, 1000 + index, static_cast<DWORD>(index), 42};
        if (!index) data.tray[index].identity.guid.Data1 = 0x525f0015;
    }
    unsigned frames = 0, restoredCallbacks = 0;
    for (const bool merged : {false, true}) for (const float scale : {.75f, 1.f, 1.5f, 2.f})
    for (const UINT dipWidth : {640u, 1920u})
    {
        const UINT width = static_cast<UINT>(static_cast<float>(dipWidth) * scale);
        const UINT height = static_cast<UINT>((merged ? 64.f : 32.f) * scale);
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
        check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &target)),
            "tray geometry offscreen target initializes");
        StatusBarSettings settings; settings.pinnedTrayItems = pinned;
        auto items = BuildStatusBarItems(settings, data);
        StatusBarTrayScroll scroll;
        const auto draw = [&] {
            context->SetTarget(target.Get()); context->SetDpi(96, 96);
            context->SetTransform(D2D1::Matrix3x2F::Identity());
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
            const auto result = DrawStatusBarContent(context, text, items, width, height, scale,
                PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, merged, false, &scroll);
            const auto ended = context->EndDraw(); context->SetTarget(nullptr);
            check(SUCCEEDED(result) && SUCCEEDED(ended), "production full tray layout renders before geometry publication");
        };
        auto state = std::make_unique<tray::SharedState>();
        auto reference = std::make_unique<tray::SharedState>();
        tray::GeometryTable table; LegacyWriter legacy;
        const POINT origin{-1920, -90};
        const auto publish = [&] {
            const auto updates = StatusBarTrayGeometries(items, [&](RECT& rect) { OffsetRect(&rect, origin.x, origin.y); });
            table.Set(data.tray, updates, state.get());
            // Use the original per-item mapping loop independently of the
            // candidate collection helper, including clipped empty cells.
            for (const auto& item : items) if (item.icon)
            {
                RECT screen = item.bounds;
                if (!IsRectEmpty(&screen)) OffsetRect(&screen, origin.x, origin.y);
                legacy.Set(data.tray, item.icon->key, screen, *reference);
            }
            check(EqualPublished(*state, *reference), "batched Shell table equals the old complete per-item reference");
            for (const auto& icon : data.tray)
            {
                RECT actual{}, expected{};
                const bool a = tray::LookupGeometry(*state, icon.identity, actual);
                const bool b = tray::LookupGeometry(*reference, icon.identity, expected);
                check(a == b && (!a || EqualRect(&actual, &expected)),
                    "native LookupGeometry observes the same clipped signed rectangle and identity");
            }
        };
        draw(); publish();
        check(tray::Read(state->geometrySequence) == 2, "first arranged frame publishes its geometry table once");
        const auto before = tray::Read(state->geometrySequence);
        const auto oldBefore = tray::Read(reference->geometrySequence);
        publish();
        if (legacyNoopControl)
        {
            std::cout << "Legacy stable-frame publications=" << (tray::Read(reference->geometrySequence) - oldBefore) / 2
                << "; candidate=" << (tray::Read(state->geometrySequence) - before) / 2 << std::endl;
            check(tray::Read(reference->geometrySequence) == oldBefore,
                "old writer failure signal: unchanged arranged frame must not publish again");
        }
        check(tray::Read(state->geometrySequence) == before,
            "an unchanged complete frame performs no geometry publication");
        check(tray::Read(reference->geometrySequence) - oldBefore == static_cast<LONG>(2 * data.tray.size()),
            "the retained old writer executes its unconditional per-item publication path");
        ++frames;
        if (scroll.Maximum())
        {
            scroll.Move(scroll.Maximum() / 2 + 1); draw();
            const auto movedBefore = tray::Read(state->geometrySequence); publish();
            check(tray::Read(state->geometrySequence) == movedBefore + 2,
                "all scroll hit targets become visible to Shell in one publication");
        }
        // Execute the exact production focus-return dispatcher. Only native
        // RestoreFocus is substituted; preparation uses real arrangement and
        // the same collector/table writer as the status-bar host. This checks
        // the Shell read at the synchronous native-action boundary, not merely
        // the geometry after the final presentation.
        for (const bool granted : {false, true})
        {
            scroll.Move(-scroll.Maximum()); draw(); publish();
            const auto focusStart = tray::Read(state->geometrySequence);
            const auto previousOffset = scroll.offset;
            StatusBarInteraction interaction; bool keyboard = false;
            StatusBarPresentationPreparation preparation;
            StatusBarDrawResources resources;
            unsigned callbacks = 0, presentations = 0;
            auto key = data.tray.back().key;
            if (scroll.Maximum())
            {
                const auto clipped = std::find_if(items.begin(), items.end(), [&](const auto& item) {
                    return item.icon && (item.contentBounds.left < scroll.viewport.left ||
                        item.contentBounds.right > scroll.viewport.right);
                });
                check(clipped != items.end(), "overflow fixture has a tray target requiring reveal");
                key = clipped->icon->key;
            }
            const auto returned = DispatchStatusBarTrayFocusReturn(key, true, items, scroll, interaction, keyboard,
                preparation, [&] {
                    auto result = resources.PrepareText(text, scale);
                    if (SUCCEEDED(result)) result = ArrangeStatusBarContent(text, resources.format.Get(), items,
                        width, height, scale, merged, false, scroll, true);
                    check(SUCCEEDED(result), "focus preparation uses the production full arrangement");
                    publish();
                }, [&] {
                    ++callbacks; ++restoredCallbacks;
                    const auto item = std::find_if(items.begin(), items.end(), [&](const auto& candidate) {
                        return candidate.icon && candidate.icon->key == key;
                    });
                    check(item != items.end(), "focus target remains registered");
                    RECT expected = item->bounds, actual{}; OffsetRect(&expected, origin.x, origin.y);
                    check(!IsRectEmpty(&item->bounds) && tray::LookupGeometry(*state, item->icon->identity, actual) &&
                        EqualRect(&actual, &expected), "RestoreFocus observes the newly revealed signed Shell rectangle synchronously");
                    return granted;
                }, [&] { interaction.hovered.reset(); }, [&] {
                    ++presentations; draw(); publish();
                });
            check(returned == granted && callbacks == 1 && presentations == 1,
                "geometry publication neither delays nor repeats native restoration or final presentation");
            check(!granted || (keyboard && interaction.focused && items[*interaction.focused].icon &&
                items[*interaction.focused].icon->key == key), "successful restoration keeps keyboard focus on the same registered icon");
            check(tray::Read(state->geometrySequence) - focusStart == (scroll.offset != previousOffset ? 2 : 0),
                "reveal publishes one coherent table and unchanged final preparation/presentation publishes nothing");
        }
        table.Clear(state.get());
        check(state->geometryCount == 0, "clear publishes an empty Shell geometry table");
        const auto cleared = tray::Read(state->geometrySequence);
        table.Clear(state.get());
        check(tray::Read(state->geometrySequence) == cleared, "repeated clear performs no publication");
    }
    std::cout << "Tray geometry: " << frames << " arranged stable-frame references; " << restoredCallbacks
        << " production restore callbacks read current Shell geometry; legacy per stable frame=" << data.tray.size()
        << " publications, candidate=0; changed layout=1 publication\n";
}
}
