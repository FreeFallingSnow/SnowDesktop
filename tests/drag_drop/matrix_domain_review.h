#pragma once

#include "core/drop_model.h"
#include "core/slot.h"

// Matrix-schema gate, not an acceptance oracle. A new value must be reviewed
// against README factors, constraints, independent expectations and executable
// coverage before listing it here. No default silently classifies new values.
// Missing enum values are build errors only within these exhaustive reviews.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(error: 4062)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wswitch"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#endif

namespace matrix_domain_review
{
constexpr bool Reviewed(DropAction value) // G02, B01-B04; contextual modifiers stay separate.
{
    switch (value)
    {
    case DropAction::Move: case DropAction::Copy: case DropAction::Link: return true;
    }
    return false;
}
constexpr bool Reviewed(DropSourceKind value) // P01 and source identity; Unknown is a negative input.
{
    switch (value)
    {
    case DropSourceKind::DesktopIcon: case DropSourceKind::FolderEntry:
    case DropSourceKind::SearchResult: case DropSourceKind::ExternalFile: case DropSourceKind::Widget:
    case DropSourceKind::CollectionGroupEntry: case DropSourceKind::FileGroupEntry:
    case DropSourceKind::Unknown: return true;
    }
    return false;
}
constexpr bool Reviewed(DropTargetKind value) // Target E01/E02; each route needs its own results.
{
    switch (value)
    {
    case DropTargetKind::Desktop: case DropTargetKind::KeyedWidget:
    case DropTargetKind::FolderMapping: case DropTargetKind::External:
    case DropTargetKind::Handoff: case DropTargetKind::Unknown: return true;
    }
    return false;
}
constexpr bool Reviewed(DropLandingKind value) // H04/H05, O05/O06; None is not successful placement.
{
    switch (value)
    {
    case DropLandingKind::None: case DropLandingKind::DesktopCell:
    case DropLandingKind::WidgetIndex: case DropLandingKind::Folder:
    case DropLandingKind::External: return true;
    }
    return false;
}
constexpr bool Reviewed(DockEntryType value) // E03/E04 and B06/B18/B19; source/target are independent.
{
    switch (value)
    {
    case DockEntryType::DesktopItem: case DockEntryType::Collection:
    case DockEntryType::FolderMapping: case DockEntryType::DesktopFiles:
    case DockEntryType::CollectionGroup: case DockEntryType::FileGroup: return true;
    }
    return false;
}
constexpr bool Reviewed(HitRegion value) // H01-H03, including explicit rejection and handoff.
{
    switch (value)
    {
    case HitRegion::None: case HitRegion::Empty: case HitRegion::SortBefore:
    case HitRegion::SortAfter: case HitRegion::Handoff: case HitRegion::Blocked: return true;
    }
    return false;
}
constexpr bool Reviewed(snowdesktop::slot_contract::DragRelation value) // R01; see independent directed-pair table.
{
    using Relation = snowdesktop::slot_contract::DragRelation;
    switch (value)
    {
    case Relation::SameInstance: case Relation::SameSurface: case Relation::CrossSurface:
    case Relation::ExternalIngress: case Relation::ExternalEgress: return true;
    }
    return false;
}
constexpr bool Reviewed(snowdesktop::slot_contract::DropRoute value) // B01-B20; a route is not an execution result.
{
    using Route = snowdesktop::slot_contract::DropRoute;
    switch (value)
    {
    case Route::Reject: case Route::ReorderWithinContainer: case Route::PlaceOnDesktop:
    case Route::AddToDock: case Route::InsertLogicalItem: case Route::TransferFile:
    case Route::RouteThroughCollectionGroup: case Route::RouteThroughFileGroup:
    case Route::MoveCollectionIntoGroup: case Route::MoveFileSourceIntoGroup:
    case Route::ReleaseGroupedChild: case Route::TransferGroupedLabel:
    case Route::ExportToExternal: case Route::BindLogicalReference: return true;
    }
    return false;
}
}

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
