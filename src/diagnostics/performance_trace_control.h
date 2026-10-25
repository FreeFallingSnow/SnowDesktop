#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace snowdesktop::performance
{
struct TraceQuality
{
    std::uint64_t droppedOwners = 0, droppedCounters = 0, droppedLinks = 0, probeErrors = 0, droppedTracks = 0;
    // Intentional filtering/sampling, never counted as data loss or probe errors.
    std::uint64_t filteredProbeCalls = 0, inputOperationsObserved = 0, inputOperationsSelected = 0, ownerFilteredProbeCalls = 0;
};
struct TraceOwnerDescription
{
    std::uint64_t opaqueId = 0;
    // In-memory local-control identity, never a trace/debug annotation.
    std::wstring instanceId;
};
void ActivateTrace(std::uint64_t generation, std::uint64_t salt,
    const std::vector<std::wstring>& selectedInstances = {}, unsigned inputSampleEvery = 1);
TraceQuality DeactivateTrace() noexcept;
std::vector<TraceOwnerDescription> DescribeTraceOwners();
// Session-local correlation identity; do not export a HWND/address verbatim.
std::uint64_t OperationId(std::uint64_t correlation) noexcept;
// Single atomic load for allocation/task probes. Zero means recording is off.
std::uint64_t CaptureGeneration() noexcept;
std::uint64_t ReserveAnimationTrack() noexcept;
}
