#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace snowdesktop::performance
{
inline constexpr wchar_t ControlMessageName[] =
    L"FreeFallingSnow.SnowDesktop.Performance.v1";
inline constexpr ULONG_PTR CopyDataTag = 0x53445031;
inline constexpr UINT_PTR SampleTimer = 0x53445046;
inline constexpr LRESULT ProtocolIdle = 0x53445010;
inline constexpr LRESULT ProtocolRecording = 0x53445011;
inline constexpr LRESULT ProtocolFinishing = 0x53445012;
inline constexpr LRESULT ProtocolFailed = 0x53445013;

enum class CaptureMode { Trace, Summary };

struct CaptureOptions
{
    std::string session;
    std::filesystem::path output;
    unsigned seconds = 60;
    std::size_t maximumEvents = 32768;
    // Preserve v1/backend callers; v2 CLI explicitly defaults to summary.
    CaptureMode mode = CaptureMode::Trace;
    bool scopeCpu = true;
};

// Backend entry points are also exercised by the runtime-diagnostics tests.
bool Start(const CaptureOptions& options, std::string& error);
bool RequestStop(std::string_view session) noexcept;
void Shutdown() noexcept;
LRESULT Status() noexcept;
bool IsControlMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;
LRESULT HandleControlMessage(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, void (*sampleWidgets)(void*), void* context) noexcept;
}
