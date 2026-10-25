#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::performance
{
inline constexpr wchar_t ControlMessageName[] = L"FreeFallingSnow.SnowDesktop.Performance.v3";
inline constexpr ULONG_PTR CopyDataTag = 0x53445031; // Recognize/reject deprecated unauthenticated writes.
inline constexpr UINT_PTR SampleTimer = 0x53445046;
inline constexpr LRESULT ProtocolIdle = 0x53445010, ProtocolRecording = 0x53445011,
    ProtocolFinishing = 0x53445012, ProtocolFailed = 0x53445013, ProtocolStarting = 0x53445014;

enum class CapturePreset { Resource, Interaction, Widget };
enum class CaptureProcessRole { Host, WidgetPreview };
enum class CapturePhase { Idle, Starting, Recording, Stopping, Finalizing, Completed, Failed };
struct CaptureOptions
{
    std::string session, requestId;
    std::filesystem::path output;
    unsigned seconds = 120, bufferKiB = 32768;
    std::uint64_t maximumFileBytes = 256ULL * 1024 * 1024;
    CapturePreset preset = CapturePreset::Resource;
    CaptureProcessRole processRole = CaptureProcessRole::Host;
    bool scopeCpu = false;
    std::vector<std::wstring> selectedInstances;
};
struct CaptureSnapshot
{
    CapturePhase phase = CapturePhase::Idle;
    std::string session, requestId, error, configHash;
    std::string analysis = "none", analysisError, quality = "unknown";
    std::uint64_t generation = 0;
    std::filesystem::path trace, manifest, report;
    unsigned requestedSeconds = 0;
    unsigned preset = 0, bufferKiB = 0;
    std::vector<std::wstring> selectedInstances;
    double elapsedMs = 0;
    bool scopeCpu = false, found = true;
};

// Start means accepted/Starting. Only the worker changes to Recording after
// the SDK acknowledges startup. Completion is a per-session result, not Idle.
bool Start(const CaptureOptions& options, std::string& error);
bool RequestStop(std::string_view session, std::uint64_t generation = 0) noexcept;
bool RequestAnalysis(std::string_view session, std::uint64_t generation, std::string& error);
CaptureSnapshot Snapshot(std::string_view session = {});
std::string SnapshotJson(const CaptureSnapshot& snapshot, std::string_view replyRequestId = {});
std::string ProcessControlRequest(std::string_view request) noexcept;
void ConfigureControl(HWND window, const std::filesystem::path& outputRoot);
void ConfigureGpuDemand(std::function<void(bool)> demand);
void Shutdown() noexcept;
bool IsStateChangeMessage(UINT message) noexcept;
LRESULT Status() noexcept;
bool IsControlMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;
LRESULT HandleControlMessage(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, void (*sampleWidgets)(void*), void* context) noexcept;
}
