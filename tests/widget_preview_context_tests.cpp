#include "widget_preview_context.h"

#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
using snowdesktop::widget_runtime::StorageMap;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestStorageOverlayAndNestedRestoration()
{
    StorageMap persistent = { { "value", "persistent" } };
    StorageMap outer = { { "value", "outer" } };
    StorageMap inner = { { "value", "inner" } };

    Check(
        &snowdesktop::widget_runtime::ActiveStorage(persistent) ==
            &persistent,
        "persistent storage must be active without an overlay");
    {
        snowdesktop::widget_runtime::StorageOverlayScope outerScope(&outer);
        Check(
            &snowdesktop::widget_runtime::ActiveStorage(persistent) ==
                &outer,
            "outer storage overlay must become active");
        {
            snowdesktop::widget_runtime::StorageOverlayScope innerScope(
                &inner);
            Check(
                &snowdesktop::widget_runtime::ActiveStorage(persistent) ==
                    &inner,
                "inner storage overlay must replace the outer overlay");
        }
        Check(
            &snowdesktop::widget_runtime::ActiveStorage(persistent) ==
                &outer,
            "leaving an inner scope must restore the outer overlay");
    }
    Check(
        !snowdesktop::widget_runtime::HasStorageOverlay() &&
            &snowdesktop::widget_runtime::ActiveStorage(persistent) ==
                &persistent,
        "leaving all scopes must restore persistent storage");
}

void TestDryLoadAndPreviewScope()
{
    StorageMap preview;
    Check(!snowdesktop::widget_runtime::IsDryLoad() &&
            !snowdesktop::widget_runtime::IsPreviewExecution(),
        "dry-load and preview modes must be disabled by default");
    {
        snowdesktop::widget_runtime::DryLoadScope dryLoad;
        Check(snowdesktop::widget_runtime::IsDryLoad() &&
                !snowdesktop::widget_runtime::IsPreviewExecution(),
            "dry-load scope must not impersonate a preview");
        {
            snowdesktop::widget_runtime::DryLoadScope liveScope(false);
            Check(!snowdesktop::widget_runtime::IsDryLoad(),
                "nested dry-load scope must support temporary overrides");
        }
        Check(snowdesktop::widget_runtime::IsDryLoad(),
            "nested dry-load scope must restore the outer state");
    }
    {
        snowdesktop::widget_runtime::PreviewExecutionScope previewScope(
            &preview);
        Check(snowdesktop::widget_runtime::IsDryLoad() &&
                snowdesktop::widget_runtime::IsPreviewExecution() &&
                snowdesktop::widget_runtime::CurrentStorageOverlay() ==
                    &preview,
            "preview scope must combine preview, dry-load, and storage overlay state");
        Check(snowdesktop::widget_runtime::
                    CurrentWallClockMilliseconds() ==
                snowdesktop::widget_runtime::
                    PreviewWallClockMilliseconds &&
                snowdesktop::widget_runtime::
                    CurrentMonotonicMilliseconds() ==
                snowdesktop::widget_runtime::
                    PreviewMonotonicMilliseconds,
            "preview scope must expose a deterministic virtual clock");
    }
    Check(!snowdesktop::widget_runtime::IsDryLoad() &&
            !snowdesktop::widget_runtime::IsPreviewExecution() &&
            !snowdesktop::widget_runtime::HasStorageOverlay(),
        "preview scope must restore every execution-context field");

    snowdesktop::widget_runtime::PreviewExecutionScope inactive(nullptr);
    Check(!snowdesktop::widget_runtime::IsDryLoad() &&
            !snowdesktop::widget_runtime::HasStorageOverlay(),
        "null preview storage must leave the context unchanged");
}

void TestThreadIsolation()
{
    StorageMap overlay;
    snowdesktop::widget_runtime::PreviewExecutionScope scope(&overlay);
    bool workerWasIsolated = false;
    std::thread worker([&workerWasIsolated]
    {
        workerWasIsolated =
            !snowdesktop::widget_runtime::IsDryLoad() &&
            !snowdesktop::widget_runtime::IsPreviewExecution() &&
            !snowdesktop::widget_runtime::HasStorageOverlay();
    });
    worker.join();
    Check(workerWasIsolated,
        "preview execution context must be isolated per thread");
    Check(snowdesktop::widget_runtime::IsDryLoad(),
        "worker context must not modify the caller context");
}
}

int main()
{
    TestStorageOverlayAndNestedRestoration();
    TestDryLoadAndPreviewScope();
    TestThreadIsolation();
    std::cout << "widget preview context tests passed\n";
    return 0;
}
