#pragma once

#include "../src/core/slot_contract.h"

#include <array>
#include <optional>

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

struct WidgetExpectation
{
    DesktopWidgetType type;
    contract::WidgetContainerRole role;
    std::optional<Surface> surface;
    Payload nativeBodyPayload;
};
// Independent native-type classification. Lua file zones/logical slots are
// declarative surfaces within a component, not the native component body.
// No surface is prescribed for a body which is not a native slot container.
inline constexpr std::array widgets{
    WidgetExpectation{DesktopWidgetType::Collection, contract::WidgetContainerRole::SlotContainer, Surface::Collection, Payload::CollectionWidget},
    WidgetExpectation{DesktopWidgetType::CollectionGroup, contract::WidgetContainerRole::SlotContainer, Surface::CollectionGroup, Payload::GroupWidget},
    WidgetExpectation{DesktopWidgetType::FileGroup, contract::WidgetContainerRole::SlotContainer, Surface::FileGroup, Payload::GroupWidget},
    WidgetExpectation{DesktopWidgetType::FileCategories, contract::WidgetContainerRole::SlotContainer, Surface::FileCategories, Payload::FileSourceWidget},
    WidgetExpectation{DesktopWidgetType::FolderMapping, contract::WidgetContainerRole::SlotContainer, Surface::FolderMapping, Payload::FolderMappingWidget},
    WidgetExpectation{DesktopWidgetType::LuaScript, contract::WidgetContainerRole::NonContainer, std::nullopt, Payload::OtherWidget},
    WidgetExpectation{DesktopWidgetType::Guide, contract::WidgetContainerRole::NonSlotContainer, std::nullopt, Payload::OtherWidget},
};
static_assert(widgets.size() == contract::ToIndex(DesktopWidgetType::Count),
    "New widget types need independent matrix expectations, not only a production registry entry");

inline constexpr std::array surfaces{
    Surface::Desktop, Surface::Dock, Surface::Collection,
    Surface::FileCategories, Surface::FolderMapping, Surface::CollectionGroup,
    Surface::FileGroup, Surface::LuaLogicalSlot, Surface::Guide, Surface::External, Surface::SearchResults,
};
inline constexpr std::array payloads{
    Payload::DesktopItem, Payload::FolderEntry, Payload::ExternalFile,
    Payload::CollectionWidget, Payload::FolderMappingWidget,
    Payload::FileSourceWidget, Payload::OtherWidget,
    Payload::CollectionGroupLabel, Payload::FileGroupLabel,
    Payload::GroupWidget, Payload::SearchResult,
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
inline constexpr bool emits[11][11]{
    {true, false, false, true, true, true, true, false, false, true},
    {true, true, false, true, true, true, false, false, false, true},
    {true, false, false, false, false, false, false, false, false},
    {true, false, false, false, false, false, false, false, false},
    {false, true, false, false, false, false, false, false, false},
    {true, false, false, false, false, false, false, true, false},
    {true, true, false, false, true, false, false, false, true},
    {true, false, false, false, false, false, false, false, false},
    {false, false, false, false, false, false, false, false, false},
    {false, false, true, false, false, false, false, false, false},
    {false, false, false, false, false, false, false, false, false, false, true},
};

// Different-instance destinations, including external ingress/egress.
// Each row names an entire payload's destination policy, not implementation
// branches. Same-instance exceptions below preserve local reorder semantics.
inline constexpr Route destinationRoutes[11][11]{
    {Route::PlaceOnDesktop, Route::AddToDock, Route::InsertLogicalItem,
     Route::InsertLogicalItem, Route::TransferFile,
     Route::RouteThroughCollectionGroup, Route::RouteThroughFileGroup,
     Route::BindLogicalReference, Route::Reject, Route::ExportToExternal, Route::Reject},
    {Route::TransferFile, Route::AddToDock, Route::InsertLogicalItem,
     Route::InsertLogicalItem, Route::TransferFile,
     Route::RouteThroughCollectionGroup, Route::RouteThroughFileGroup,
     Route::BindLogicalReference, Route::Reject, Route::ExportToExternal, Route::Reject},
    {Route::PlaceOnDesktop, Route::AddToDock, Route::InsertLogicalItem,
     Route::InsertLogicalItem, Route::TransferFile,
     Route::RouteThroughCollectionGroup, Route::RouteThroughFileGroup,
     Route::BindLogicalReference, Route::Reject, Route::Reject, Route::Reject},
    {Route::PlaceOnDesktop, Route::AddToDock, Route::Reject, Route::Reject,
     Route::Reject, Route::MoveCollectionIntoGroup, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject, Route::Reject},
    {Route::PlaceOnDesktop, Route::AddToDock, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::MoveFileSourceIntoGroup,
     Route::Reject, Route::Reject, Route::Reject, Route::Reject},
    {Route::PlaceOnDesktop, Route::AddToDock, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::MoveFileSourceIntoGroup,
     Route::Reject, Route::Reject, Route::Reject, Route::Reject},
    {Route::PlaceOnDesktop, Route::Reject, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject, Route::Reject},
    {Route::ReleaseGroupedChild, Route::AddToDock, Route::Reject, Route::Reject,
     Route::Reject, Route::TransferGroupedLabel, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject, Route::Reject},
    {Route::ReleaseGroupedChild, Route::AddToDock, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::TransferGroupedLabel,
     Route::Reject, Route::Reject, Route::Reject, Route::Reject},
    {Route::PlaceOnDesktop, Route::AddToDock, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject,
     Route::Reject, Route::Reject, Route::Reject, Route::Reject},
    {Route::TransferFile, Route::AddToDock, Route::InsertLogicalItem,
     Route::InsertLogicalItem, Route::TransferFile, Route::RouteThroughCollectionGroup,
     Route::RouteThroughFileGroup, Route::BindLogicalReference, Route::Reject,
     Route::ExportToExternal, Route::Reject},
};

inline constexpr bool reordersInSameInstance[11][11]{
    {true, false, false, false, false, false, false, false, false},
    {true, false, false, true, true, true, false, false, false, true},
    {true, false, false, false, false, false, false, false, false},
    {true, false, false, false, false, false, false, false, false},
    {false, true, false, false, false, false, false, false, false},
    {false, false, false, false, false, false, false, true, false},
    {false, false, false, false, false, false, false, false, true},
    {true, false, false, false, false, false, false, false, false},
    {}, {}, {},
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
