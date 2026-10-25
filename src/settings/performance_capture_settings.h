#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace snowdesktop
{
// Private same-executable settings IPC. Local widget labels never enter traces.
struct PerformanceCaptureRequest
{
    enum class Action : std::uint8_t { Query, Start, Stop, Analyze, OpenFolder, OpenReport };
    Action action = Action::Query;
    std::string session;
    std::uint64_t generation = 0;
    unsigned preset = 0, seconds = 120, bufferMiB = 32;
    bool threadCpu = false;
    std::vector<std::wstring> instances;
};
struct PerformanceCaptureInstance { std::wstring id, name; };
struct PerformanceCaptureStatus
{
    std::string session, phase = "idle", error, analysis = "none", quality = "unknown";
    std::uint64_t generation = 0;
    unsigned requestedSeconds = 0;
    unsigned preset = 0, bufferKiB = 0;
    double elapsedMs = 0;
    bool threadCpu = false;
    std::wstring trace, report;
    std::vector<PerformanceCaptureInstance> instances;
    std::vector<std::wstring> selectedInstances;
};
}
