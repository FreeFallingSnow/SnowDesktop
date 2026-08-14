#include "widget_clipboard_task_executor.h"

#include <windows.h>

#include <cstring>
#include <limits>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
class ClipboardScope
{
public:
    ClipboardScope() : opened_(OpenClipboard(nullptr) != FALSE) {}
    ~ClipboardScope()
    {
        if (opened_) CloseClipboard();
    }
    explicit operator bool() const noexcept { return opened_; }

private:
    bool opened_ = false;
};

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            result.data(), length) != length)
        return {};
    return result;
}

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0 ||
        static_cast<std::size_t>(length) >
            WidgetClipboardTaskExecutor::MaximumTextBytes)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            result.data(), length, nullptr, nullptr) != length)
        return {};
    return result;
}

WidgetClipboardTaskRunResult ReadText()
{
    ClipboardScope clipboard;
    if (!clipboard) return { false, {}, {}, "clipboardBusy" };
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return { false, {}, {}, "formatUnavailable" };
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return { false, {}, {}, "clipboardReadFailed" };
    const SIZE_T bytes = GlobalSize(handle);
    const SIZE_T maximumBytes =
        (WidgetClipboardTaskExecutor::MaximumTextBytes + 1) *
        sizeof(wchar_t);
    if (bytes < sizeof(wchar_t) || bytes > maximumBytes)
        return { false, {}, {}, "clipboardTooLarge" };
    const auto* value = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!value) return { false, {}, {}, "clipboardReadFailed" };
    const std::size_t capacity = bytes / sizeof(wchar_t);
    const wchar_t* end = static_cast<const wchar_t*>(
        std::wmemchr(value, L'\0', capacity));
    if (!end)
    {
        GlobalUnlock(handle);
        return { false, {}, {}, "clipboardReadFailed" };
    }
    const std::wstring_view wide(
        value, static_cast<std::size_t>(end - value));
    std::string text = WideToUtf8(wide);
    GlobalUnlock(handle);
    if (!wide.empty() && text.empty())
        return { false, {}, {}, "clipboardTooLarge" };
    return { true, "text", std::move(text), {} };
}

WidgetClipboardTaskRunResult WriteText(std::string_view text)
{
    const std::wstring wide = Utf8ToWide(text);
    if (!text.empty() && wide.empty())
        return { false, {}, {}, "invalidArguments" };
    if (wide.size() >=
        std::numeric_limits<SIZE_T>::max() / sizeof(wchar_t))
        return { false, {}, {}, "clipboardTooLarge" };
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return { false, {}, {}, "clipboardWriteFailed" };
    void* target = GlobalLock(memory);
    if (!target)
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardWriteFailed" };
    }
    if (!wide.empty())
        std::memcpy(target, wide.data(), wide.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(target)[wide.size()] = L'\0';
    GlobalUnlock(memory);

    ClipboardScope clipboard;
    if (!clipboard)
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardBusy" };
    }
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardWriteFailed" };
    }
    return { true, {}, {}, {} };
}

WidgetClipboardTaskRunResult ClearClipboardData()
{
    ClipboardScope clipboard;
    if (!clipboard) return { false, {}, {}, "clipboardBusy" };
    return EmptyClipboard()
        ? WidgetClipboardTaskRunResult{ true, {}, {}, {} }
        : WidgetClipboardTaskRunResult{
            false, {}, {}, "clipboardWriteFailed" };
}
}

WidgetClipboardTaskExecutor::WidgetClipboardTaskExecutor(
    Runner runner, NowProvider nowProvider)
    : runner_(std::move(runner)), nowProvider_(std::move(nowProvider))
{
    if (!runner_) runner_ = RunSystemAction;
    if (!nowProvider_)
        nowProvider_ = [] { return Clock::now(); };
}

WidgetClipboardTaskExecutor::~WidgetClipboardTaskExecutor()
{
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    if (worker_.joinable())
    {
        worker_.request_stop();
        condition_.notify_all();
        worker_.join();
    }
}

WidgetClipboardTaskStartResult WidgetClipboardTaskExecutor::Start(
    std::uint64_t id, std::string instanceId,
    WidgetClipboardTaskRequest request)
{
    if (id == 0 || instanceId.empty() || !ValidateRequest(request))
        return { false, "invalidArguments" };
    const auto now = nowProvider_();
    std::scoped_lock lock(mutex_);
    if (stopping_ || active_.contains(id))
        return { false, "taskExecutorUnavailable" };
    if (const auto last = lastStarts_.find(instanceId);
        last != lastStarts_.end() && now >= last->second &&
        now - last->second < MinimumActionInterval)
        return { false, "rateLimited" };
    lastStarts_.insert_or_assign(instanceId, now);
    active_.insert(id);
    requests_.push_back(
        { id, std::move(instanceId), std::move(request) });
    if (!worker_.joinable())
    {
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                WorkerMain(stopToken);
            });
    }
    condition_.notify_one();
    return { true, {} };
}

bool WidgetClipboardTaskExecutor::Cancel(std::uint64_t id)
{
    std::scoped_lock lock(mutex_);
    if (!active_.contains(id)) return false;
    canceled_.insert(id);
    condition_.notify_all();
    return true;
}

void WidgetClipboardTaskExecutor::ForgetInstance(
    std::string_view instanceId)
{
    std::scoped_lock lock(mutex_);
    lastStarts_.erase(std::string(instanceId));
}

std::vector<WidgetClipboardTaskCompletion>
WidgetClipboardTaskExecutor::DrainCompletions()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(completions_, {});
}

std::size_t WidgetClipboardTaskExecutor::ActiveCount() const
{
    std::scoped_lock lock(mutex_);
    return active_.size();
}

bool WidgetClipboardTaskExecutor::SupportsAction(
    std::string_view action) noexcept
{
    return action == "clipboard.read" ||
        action == "clipboard.write" || action == "clipboard.clear";
}

bool WidgetClipboardTaskExecutor::ValidateRequest(
    const WidgetClipboardTaskRequest& request) noexcept
{
    if (!SupportsAction(request.action)) return false;
    if (request.action == "clipboard.read")
        return request.format == "text" && request.text.empty();
    if (request.action == "clipboard.write")
        return request.format == "text" &&
            request.text.size() <= MaximumTextBytes &&
            request.text.find('\0') == std::string::npos;
    return request.format.empty() && request.text.empty();
}

WidgetClipboardTaskRunResult
WidgetClipboardTaskExecutor::RunSystemAction(
    const WidgetClipboardTaskRequest& request)
{
    if (!ValidateRequest(request))
        return { false, {}, {}, "invalidArguments" };
    if (request.action == "clipboard.read") return ReadText();
    if (request.action == "clipboard.write")
        return WriteText(request.text);
    return ClearClipboardData();
}

void WidgetClipboardTaskExecutor::WorkerMain(
    std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        QueuedRequest request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] {
                return stopToken.stop_requested() || !requests_.empty();
            });
            if (stopToken.stop_requested()) break;
            request = std::move(requests_.front());
            requests_.pop_front();
            if (canceled_.contains(request.id))
            {
                active_.erase(request.id);
                canceled_.erase(request.id);
                completions_.push_back({ request.id,
                    request.request.action, false, {}, {}, "canceled" });
                continue;
            }
        }

        WidgetClipboardTaskRunResult result;
        try
        {
            result = runner_(request.request);
        }
        catch (...)
        {
            result = { false, {}, {}, "clipboardTaskFailed" };
        }
        {
            std::scoped_lock lock(mutex_);
            if (canceled_.erase(request.id) > 0)
                result = { false, {}, {}, "canceled" };
            active_.erase(request.id);
            completions_.push_back({ request.id,
                std::move(request.request.action), result.ok,
                std::move(result.format), std::move(result.text),
                std::move(result.error) });
        }
    }
}
}
