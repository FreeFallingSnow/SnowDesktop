#include "widget_clipboard_observer.h"
#include "widget/runtime/widget_runtime_image.h"

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr wchar_t kWindowClass[] = L"SnowDesktop.WidgetClipboardObserver";
constexpr std::size_t kMaxCompletedChanges = 8;
}

WidgetClipboardObserver::~WidgetClipboardObserver()
{
    Stop();
}

bool WidgetClipboardObserver::Start(std::function<void()> wake)
{
    if (window_) return true;
    const HINSTANCE module = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = module;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    window_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, module, this);
    if (!window_) return false;
    if (!AddClipboardFormatListener(window_))
    {
        DestroyWindow(std::exchange(window_, nullptr));
        return false;
    }

    lastSequence_ = GetClipboardSequenceNumber();
    try
    {
        const std::lock_guard lock(mutex_);
        wake_ = std::move(wake);
        if (!worker_.joinable())
            worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
    }
    catch (...)
    {
        Stop();
        return false;
    }
    return true;
}

void WidgetClipboardObserver::Stop()
{
    if (window_)
    {
        RemoveClipboardFormatListener(window_);
        DestroyWindow(std::exchange(window_, nullptr));
    }
    const std::lock_guard lock(mutex_);
    ++session_;
    pending_.reset();
    completed_.clear();
    wake_ = {};
}

std::deque<WidgetClipboardChange> WidgetClipboardObserver::Drain()
{
    const std::lock_guard lock(mutex_);
    return std::exchange(completed_, {});
}

LRESULT CALLBACK WidgetClipboardObserver::WindowProc(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_CLIPBOARDUPDATE)
    {
        const auto observer = reinterpret_cast<WidgetClipboardObserver*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (observer) observer->OnClipboardUpdate();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void WidgetClipboardObserver::OnClipboardUpdate()
{
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == 0 || sequence == lastSequence_) return;
    lastSequence_ = sequence;
    {
        const std::lock_guard lock(mutex_);
        pending_ = Request{ sequence, ++revision_, session_ };
    }
    changed_.notify_one();
}

void WidgetClipboardObserver::Run(std::stop_token stop)
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!stop.stop_requested())
    {
        Request request{};
        {
            std::unique_lock lock(mutex_);
            if (!changed_.wait(lock, stop, [this] { return pending_.has_value(); }))
                break;
            request = *std::exchange(pending_, std::nullopt);
        }

        std::optional<WidgetClipboardChange> change;
        try
        {
            for (int attempt = 0; attempt < 4 && !stop.stop_requested(); ++attempt)
            {
                {
                    const std::lock_guard lock(mutex_);
                    if (request.session != session_) break;
                }
                change = ReadWidgetClipboardChange(request.sequence);
                if (!change || change->error != "clipboardBusy" || attempt == 3)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }
        catch (...)
        {
            change.emplace();
            change->error = "clipboardReadFailed";
        }
        if (!change || stop.stop_requested()) continue;

        std::function<void()> wake;
        {
            const std::lock_guard lock(mutex_);
            if (session_ != request.session) continue;
            change->revision = request.revision;
            if (change->image)
            {
                constexpr std::size_t kQueuedImageBudget = 64 * 1024 * 1024;
                const auto incomingBytes = change->image->bgraPremultiplied.size();
                std::size_t queuedBytes = 0;
                for (const auto& completed : completed_)
                    if (completed.image) queuedBytes += completed.image->bgraPremultiplied.size();
                while (queuedBytes > std::max(kQueuedImageBudget, incomingBytes) - incomingBytes)
                {
                    if (completed_.front().image)
                        queuedBytes -= completed_.front().image->bgraPremultiplied.size();
                    completed_.pop_front();
                }
            }
            if (completed_.size() == kMaxCompletedChanges) completed_.pop_front();
            completed_.push_back(std::move(*change));
            wake = wake_;
        }
        if (wake) wake();
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
}
}
