#pragma once
#include <optional>

namespace snowdesktop::dock_process_snapshot
{
// The caller owns this value only for one enumeration. Empty snapshots are
// valid results for that pass; a new pass must query again. No handles persist.
template<class Snapshot, class Query>
const Snapshot& Read(std::optional<Snapshot>& snapshot, Query query)
{
    if (!snapshot) snapshot.emplace(query());
    return *snapshot;
}
}
