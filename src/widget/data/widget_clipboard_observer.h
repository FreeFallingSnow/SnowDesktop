#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetRuntimeImagePixels;

inline constexpr char kClipboardChangesTopic[] = "clipboard.changes";
inline constexpr char kClipboardObservePermission[] = "clipboard.observe";

struct WidgetClipboardChange
{
    std::uint64_t revision = 0;
    std::string format;
    std::string text;
    std::string rgb;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::shared_ptr<const WidgetRuntimeImagePixels> image;
    std::string imageToken;
    std::vector<std::string> names;
    std::string error;
};

// Runs on the clipboard worker. A superseded sequence has no snapshot.
std::optional<WidgetClipboardChange> ReadWidgetClipboardChange(
    DWORD sequence);

class WidgetClipboardObserver
{
public:
    ~WidgetClipboardObserver();

    // Start, Stop, Revision and Drain run on the window's owner thread.
    bool Start(std::function<void()> wake);
    void Stop();
    std::uint64_t Revision() const noexcept { return revision_; }
    std::deque<WidgetClipboardChange> Drain();

private:
    struct Request
    {
        DWORD sequence;
        std::uint64_t revision;
        std::uint64_t session;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam);
    void OnClipboardUpdate();
    void Run(std::stop_token stop);

    HWND window_ = nullptr;
    std::uint64_t revision_ = 0;
    DWORD lastSequence_ = 0;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::uint64_t session_ = 0;
    std::optional<Request> pending_;
    std::deque<WidgetClipboardChange> completed_;
    std::function<void()> wake_;
    std::jthread worker_;
};
}
