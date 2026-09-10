#pragma once

#include "core/slot_contract.h"

#include <array>

// Independent, reviewable expected behavior. Do not derive these tables from
// EvaluateSlotDrop, SurfaceEmits, or kSurfaceDescriptors: doing so lets a broken
// production route redefine its own expected result.
namespace slot_drop_expectations
{
namespace contract = snowdesktop::slot_contract;
using Surface = contract::SlotSurfaceKind;
using Payload = contract::DragPayloadKind;
using Relation = contract::DragRelation;
using Route = contract::DropRoute;

inline constexpr std::array surfaces{
    Surface::Desktop, Surface::Dock, Surface::Collection,
    Surface::FileCategories, Surface::FolderMapping, Surface::CollectionGroup,
    Surface::FileGroup, Surface::LuaLogicalSlot, Surface::Guide, Surface::External,
};
inline constexpr std::array payloads{
    Payload::DesktopItem, Payload::FolderEntry, Payload::ExternalFile,
    Payload::CollectionWidget, Payload::FolderMappingWidget,
    Payload::FileSourceWidget, Payload::OtherWidget,
    Payload::CollectionGroupLabel, Payload::FileGroupLabel,
};
inline constexpr std::array relations{
    Relation::SameInstance, Relation::SameSurface, Relation::CrossSurface,
    Relation::ExternalIngress, Relation::ExternalEgress,
};
static_assert(surfaces.size() == contract::ToIndex(Surface::Count),
    "New surfaces need explicit drag expectations");
static_assert(payloads.size() == contract::ToIndex(Payload::Count),
    "New payloads need explicit drag expectations");

// Columns follow payloads above; rows follow surfaces above.
inline constexpr bool emits[10][9]{
    {true, false, false, true, true, true, true, false, false},
    {true, true, false, true, true, false, false, false, false},
    {true, false, false, false, false, false, false, false, false},
    {true, false, false, false, false, false, false, false, false},
    {false, true, false, false, false, false, false, false, false},
    {true, false, false, false, false, false, false, true, false},
    {true, true, false, false, true, false, false, false, true},
    {true, false, false, false, false, false, false, false, false},
    {false, false, false, false, false, false, false, false, false},
    {false, false, true, false, false, false, false, false, false},
};

// Different-instance destinations, including external ingress/egress.
// Each row names an entire payload's destination policy, not implementation
// branches. Same-instance exceptions below preserve local reorder semantics.
inline constexpr Route destinationRoutes[9][10]{
    {Route::PlaceOnDesktop, Route::AddToDock, Route::InsertLogicalItem,
     Route::InsertLogicalItem, Route::TransferFile,
     Route::RouteThroughCollectionGroup, Route::RouteThroughFileGroup,
     Route::BindLogicalReference, Route::Reject, Route::ExportToExternal},
    {Route::TransferFile, Route::AddToDock, Route::InsertLogicalItem,
     Route::InsertLogicalItem, Route::TransferFile,
     Route::RouteThroughCollectionGroup, Route::RouteThroughFileGroup,
     Route::BindLogicalReference, Route::Reject, Route::ExportToExternal},
    {Route::PlaceOnDesktop, Route::AddToDock, Route::InsertLogicalItem,
     Route::InsertLogicalItem, Route::TransferFile,
     Route::RouteThroughCollectionGroup, Route::RouteThroughFileGroup,
     Route::BindLogicalReference, Route::Reject, Route::Reject},
    {Route::PlaceOnDesktop, Route::AddToDock, Route::Reject, Route::Reject,
     Route::Reject, Route::MoveCollectionIntoGroup, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject},
    {Route::PlaceOnDesktop, Route::AddToDock, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::MoveFileSourceIntoGroup,
     Route::Reject, Route::Reject, Route::Reject},
    {Route::PlaceOnDesktop, Route::Reject, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::MoveFileSourceIntoGroup,
     Route::Reject, Route::Reject, Route::Reject},
    {Route::PlaceOnDesktop, Route::Reject, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject},
    {Route::ReleaseGroupedChild, Route::Reject, Route::Reject, Route::Reject,
     Route::Reject, Route::TransferGroupedLabel, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject},
    {Route::ReleaseGroupedChild, Route::Reject, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::TransferGroupedLabel,
     Route::Reject, Route::Reject, Route::Reject},
};

inline constexpr bool reordersInSameInstance[10][9]{
    {true, false, false, false, false, false, false, false, false},
    {true, false, false, true, true, false, false, false, false},
    {true, false, false, false, false, false, false, false, false},
    {true, false, false, false, false, false, false, false, false},
    {false, true, false, false, false, false, false, false, false},
    {false, false, false, false, false, false, false, true, false},
    {false, false, false, false, false, false, false, false, true},
    {true, false, false, false, false, false, false, false, false},
    {}, {},
};

constexpr Route ExpectedRoute(std::size_t source, std::size_t payload,
    std::size_t target, Relation relation)
{
    if (!emits[source][payload]) return Route::Reject;
    const auto from = surfaces[source];
    const auto to = surfaces[target];
    const bool internal = from != Surface::External && to != Surface::External;
    const bool validRelation =
        ((relation == Relation::SameInstance || relation == Relation::SameSurface) &&
            internal && from == to) ||
        (relation == Relation::CrossSurface && internal && from != to) ||
        (relation == Relation::ExternalIngress &&
            from == Surface::External && to != Surface::External) ||
        (relation == Relation::ExternalEgress &&
            from != Surface::External && to == Surface::External);
    if (!validRelation) return Route::Reject;
    if (relation == Relation::SameInstance &&
        reordersInSameInstance[target][payload])
        return Route::ReorderWithinContainer;
    return destinationRoutes[payload][target];
}
}
