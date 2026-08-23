/**
 * @file wallpaper_engine_capture.h
 * @brief On-demand Wallpaper Engine back-buffer capture for preview cards.
 */
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace snowdesktop::wallpaper_engine_capture
{

/** A fully opaque, top-down BGRA desktop region. */
struct Backdrop
{
    RECT desktopBounds{};
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;

    bool Empty() const
    {
        return width <= 0 || height <= 0 ||
            pixels.size() != static_cast<std::size_t>(width) * height;
    }
};

struct Result
{
    Backdrop backdrop;
    bool wallpaperEngineDetected = false;
    std::wstring error;
};

/**
 * Capture one Wallpaper Engine Present for the requested physical monitor.
 * The injected producer is enabled with a zero periodic interval, so only an
 * explicit request serial copies a frame. The hook is shut down before this
 * function returns. This function is blocking and must run off the UI thread.
 */
Result CaptureOneShotForMonitor(const RECT& monitorBounds,
    DWORD timeoutMs, const std::atomic_bool* cancelled = nullptr);

/** Pure crop helper used by the receiver and contract tests. */
Backdrop CropFrameToDesktopRegion(const std::uint32_t* sourcePixels,
    int sourceWidth, int sourceHeight, const RECT& sourceDesktopBounds,
    const RECT& requestedDesktopBounds);

} // namespace snowdesktop::wallpaper_engine_capture
