#include "widget_clipboard_task_executor.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using snowdesktop::widget_runtime::WidgetClipboardTaskExecutor;
using snowdesktop::widget_runtime::WidgetClipboardTaskRequest;
using snowdesktop::widget_runtime::WidgetClipboardTaskRunResult;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

template <typename Predicate>
bool WaitUntil(Predicate predicate)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

void TestValidationResultsAndRateLimit()
{
    Check(WidgetClipboardTaskExecutor::SupportsAction("clipboard.read") &&
            WidgetClipboardTaskExecutor::SupportsAction(
                "clipboard.write") &&
            WidgetClipboardTaskExecutor::SupportsAction(
                "clipboard.clear") &&
            !WidgetClipboardTaskExecutor::SupportsAction(
                "clipboard.history"),
        "the executor must expose only bounded clipboard tasks");
    Check(WidgetClipboardTaskExecutor::ValidateRequest(
                { .action = "clipboard.read", .format = "text" }) &&
            WidgetClipboardTaskExecutor::ValidateRequest(
                { .action = "clipboard.read", .format = "image" }) &&
            WidgetClipboardTaskExecutor::ValidateRequest(
                { .action = "clipboard.read",
                    .format = "file-reference" }) &&
            WidgetClipboardTaskExecutor::ValidateRequest(
                { .action = "clipboard.write", .format = "text",
                    .text = "hello" }) &&
            WidgetClipboardTaskExecutor::ValidateRequest(
                { .action = "clipboard.clear" }) &&
            !WidgetClipboardTaskExecutor::ValidateRequest(
                { .action = "clipboard.read", .format = "binary" }) &&
            !WidgetClipboardTaskExecutor::ValidateRequest(
                { .action = "clipboard.write", .format = "image" }) &&
            !WidgetClipboardTaskExecutor::ValidateRequest(
                { .action = "clipboard.clear", .format = "text" }) &&
            !WidgetClipboardTaskExecutor::ValidateRequest(
                { .action = "clipboard.write", .format = "text",
                    .text = std::string(
                        WidgetClipboardTaskExecutor::MaximumTextBytes + 1,
                        'x') }),
        "clipboard task payloads must be format-specific and bounded");

    auto now = WidgetClipboardTaskExecutor::Clock::time_point{};
    WidgetClipboardTaskExecutor executor(
        [](const WidgetClipboardTaskRequest& request)
            -> WidgetClipboardTaskRunResult {
            if (request.action == "clipboard.read")
            {
                if (request.format == "image")
                {
                    auto pixels = std::make_shared<snowdesktop::
                        widget_runtime::WidgetRuntimeImagePixels>();
                    pixels->width = 2;
                    pixels->height = 2;
                    pixels->stride = 8;
                    pixels->bgraPremultiplied.assign(16, 255);
                    WidgetClipboardTaskRunResult result;
                    result.ok = true;
                    result.format = "image";
                    result.resourceToken = snowdesktop::widget_runtime::
                        MakeWidgetRuntimeImageToken(
                            "clipboard", *pixels);
                    result.image = std::move(pixels);
                    return result;
                }
                if (request.format == "file-reference")
                {
                    WidgetClipboardTaskRunResult result;
                    result.ok = true;
                    result.format = "file-reference";
                    result.files.push_back({
                        std::filesystem::path(
                            L"C:\\Users\\Maya\\Document.txt"), false });
                    return result;
                }
                return { true, "text", "sample", {} };
            }
            return { true, {}, {}, {} };
        },
        [&] { return now; });
    Check(static_cast<bool>(executor.Start(1, "widget-a",
            { .action = "clipboard.read", .format = "text" })),
        "the first valid clipboard action must start");
    const auto limited = executor.Start(2, "widget-a",
        { .action = "clipboard.clear" });
    Check(!limited && limited.error == "rateLimited",
        "one widget must not churn the clipboard faster than the limit");
    Check(static_cast<bool>(executor.Start(3, "widget-b",
            { .action = "clipboard.write", .format = "text",
                .text = "new" })),
        "clipboard rate limiting must be isolated by instance");
    Check(static_cast<bool>(executor.Start(5, "widget-image",
            { .action = "clipboard.read", .format = "image" })) &&
            static_cast<bool>(executor.Start(6, "widget-files",
                { .action = "clipboard.read",
                    .format = "file-reference" })),
        "image and file-reference reads must enter the asynchronous worker");

    std::vector<snowdesktop::widget_runtime::
        WidgetClipboardTaskCompletion> completions;
    Check(WaitUntil([&] {
            completions = executor.DrainCompletions();
            return completions.size() == 4;
        }),
        "started clipboard tasks must complete asynchronously");
    Check(completions[0].id == 1 && completions[0].ok &&
            completions[0].format == "text" &&
            completions[0].text == "sample" &&
            completions[1].id == 3 && completions[1].ok &&
            completions[2].id == 5 && completions[2].ok &&
            completions[2].format == "image" &&
            completions[2].image &&
            completions[2].resourceToken.starts_with("@clipboard:") &&
            completions[3].id == 6 && completions[3].ok &&
            completions[3].format == "file-reference" &&
            completions[3].files.size() == 1 &&
            completions[3].files[0].path.is_absolute(),
        "bounded text, image, and file-reference payloads must be preserved");

    executor.ForgetInstance("widget-a");
    Check(static_cast<bool>(executor.Start(4, "widget-a",
            { .action = "clipboard.clear" })),
        "disposing an instance must release clipboard rate-limit state");
}

void TestInvalidRunnerPayloadIsRejected()
{
    WidgetClipboardTaskExecutor executor(
        [](const WidgetClipboardTaskRequest&)
            -> WidgetClipboardTaskRunResult {
            auto pixels = std::make_shared<snowdesktop::widget_runtime::
                WidgetRuntimeImagePixels>();
            pixels->width = 513;
            pixels->height = 1;
            pixels->stride = pixels->width * 4;
            pixels->bgraPremultiplied.resize(pixels->stride);
            WidgetClipboardTaskRunResult result;
            result.ok = true;
            result.format = "image";
            result.resourceToken = "@clipboard:invalid";
            result.image = std::move(pixels);
            return result;
        });
    Check(static_cast<bool>(executor.Start(12, "widget-invalid",
            { .action = "clipboard.read", .format = "image" })),
        "a syntactically valid image read must start");
    std::vector<snowdesktop::widget_runtime::
        WidgetClipboardTaskCompletion> completions;
    Check(WaitUntil([&] {
            completions = executor.DrainCompletions();
            return !completions.empty();
        }) && completions.size() == 1 && !completions[0].ok &&
            completions[0].image == nullptr &&
            completions[0].error == "clipboardTaskFailed",
        "the executor must reject oversized or malformed runner images");
}

void TestCancellationOverridesLateResult()
{
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    WidgetClipboardTaskExecutor executor(
        [&](const WidgetClipboardTaskRequest&)
            -> WidgetClipboardTaskRunResult {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
            return { true, "text", "late", {} };
        });
    Check(static_cast<bool>(executor.Start(9, "widget-c",
            { .action = "clipboard.read", .format = "text" })),
        "a valid clipboard task must enter the worker queue");
    {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, 1s, [&] { return entered; });
    }
    Check(entered && executor.Cancel(9),
        "an active clipboard task must accept cancellation");
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    condition.notify_all();

    std::vector<snowdesktop::widget_runtime::
        WidgetClipboardTaskCompletion> completions;
    Check(WaitUntil([&] {
            completions = executor.DrainCompletions();
            return !completions.empty();
        }),
        "a canceled clipboard task must complete");
    Check(completions.size() == 1 && completions[0].id == 9 &&
            !completions[0].ok && completions[0].text.empty() &&
            completions[0].error == "canceled",
        "cancellation must suppress a late clipboard payload");
}
}

int main()
{
    TestValidationResultsAndRateLimit();
    TestCancellationOverridesLateResult();
    TestInvalidRunnerPayloadIsRejected();
    std::cout << "widget clipboard task executor tests passed\n";
    return 0;
}
