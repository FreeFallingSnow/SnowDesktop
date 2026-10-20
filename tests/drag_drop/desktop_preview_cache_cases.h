#pragma once

#include "desktop_request_cases.h"
#include "preview_cache_state.h"

namespace desktop_preview_cache_cases
{
using namespace selected_desktop_cases;

struct Scene
{
    World world;
    desktop_request_cases::Origin origin;
    DragSession session;
    preview_cache_test::Storage cache;
    desktop_drop_cache::BestCellEntry best;
    size_t builds = 0;
    explicit Scene(const std::filesystem::path& root, int column = 1, int row = 1) : world(root)
    {
        internal_desktop_cases::Seed(world, 1, 0, false);
        world.items[0].selected = true; world.items[1].selected = false;
        world.items[1].gridCell = {L"target", 0, 0};
        world.records.at(world.items[1].layoutKey).cell = world.items[1].gridCell;
        world.Apply(); world.Legal(); pointer_geometry::Configure(world.pages);
        auto sources = internal_desktop_cases::Sources(world, 1); sources.BindRuntimeOrigin(&origin);
        session.Begin(&origin, {}, std::move(sources), {}, pointer_geometry::Center(world.pages.front(), column, row));
        session.AnchorToPointer({});
    }
    DropPreviewList Fresh()
    {
        const auto point = session.CurrentPoint();
        const auto requested = desktop_request::Resolve(world.pages, session.SourceList(), session, {}, {}, point, &world.pages.front());
        return Preview(world, session.SourceList(), requested, &best,
            {true, session.IsActive(), point, session.MouseDownPoint(), session.StaticSceneRevision()});
    }
    DropPreviewList Read()
    {
        const auto point = session.CurrentPoint();
        // Copy the published list so a later rebuild cannot silently alter the
        // earlier UI observation through an aliased cache reference.
        return desktop_preview_cache::Read(cache.Ref(),
            {true, point, DropActionToMods(session.Action()), &origin, nullptr, HitRegion::Empty,
                session.SourceList().entries.size(), session.StaticSceneRevision(), 0},
            [&] { ++builds; return Fresh(); }, []() -> DropPreviewList { throw std::runtime_error("internal cache cannot select external planner"); });
    }
    void Warm()
    {
        const auto before = world.Snapshot(); const auto first = Read(); const auto second = Read();
        Require(!first.Empty() && Equivalent(first, second) && builds == 1 && world.Snapshot() == before,
            "an unchanged desktop preview reuses one real build without mutating the model");
    }
    void Finish(const char* signal)
    {
        const auto before = world.Snapshot(); const auto shown = Read();
        Require(world.Snapshot() == before, "desktop cache refresh does not itself commit layout state");
        session.DeactivateForDrop(); const auto committed = Fresh();
        const bool agrees = Equivalent(shown, committed);
        if (!committed.Empty()) CheckCommit(world, session.SourceList(), committed);
        Require(agrees, signal);
    }
};

inline void Run(const std::filesystem::path& root, std::wstring_view section = L"all")
{
    Require(section == L"all" || section == L"occupancy" || section == L"footprint" || section == L"page", "known nonempty preview cache section");
    size_t scenes = 0;
    if (section == L"all" || section == L"occupancy")
    {
        Scene scene(root / L"occupancy"); scene.Warm();
        scene.world.items[1].gridCell = {scene.world.Named(L"Destination"), 1, 1};
        scene.world.records.at(scene.world.items[1].layoutKey).cell = scene.world.items[1].gridCell;
        scene.session.InvalidateStaticScene(); scene.world.Legal();
        scene.Finish("desktop preview cache refreshes revised occupancy before actual drop commits"); ++scenes;
    }
    if (section == L"all" || section == L"footprint")
    {
        Scene scene(root / L"footprint"); scene.Warm();
        auto& item = scene.world.items[0]; item.gridSpan = {2, 2}; item.largeIcon.emplace();
        item.largeIcon->columns = item.largeIcon->rows = 2;
        scene.world.records.at(item.layoutKey) = {item.gridCell, item.gridSpan, item.largeIcon, true, item.slot};
        scene.session.InvalidateStaticScene(); scene.world.Legal();
        scene.Finish("desktop preview cache represents the current complete source footprint after scene revision"); ++scenes;
    }
    if (section == L"all" || section == L"page")
    {
        Scene scene(root / L"page", 5, 4); scene.Warm();
        const auto page = scene.world.Named(L"Destination"); scene.world.columns[page] = scene.world.rows[page] = 3;
        scene.world.Apply(); scene.world.Legal(); scene.session.InvalidateStaticScene();
        scene.Finish("desktop preview cache follows revised page geometry at an unchanged pointer position"); ++scenes;
    }
    Require(scenes != 0, "desktop preview cache executes a nonempty scene selection");
    std::cout << scenes << " actual desktop preview cache, commit and storage scenes passed\n";
}
}
