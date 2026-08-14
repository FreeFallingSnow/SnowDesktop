#include "widget_host_state.h"

#include <cstdlib>
#include <iostream>

namespace
{
using snowdesktop::widget_runtime::ClassifyWidgetRuntimeFailure;
using snowdesktop::widget_runtime::CenterConsentDialogInWorkArea;
using snowdesktop::widget_runtime::ConsentSessionActionFor;
using snowdesktop::widget_runtime::HostActionFor;
using snowdesktop::widget_runtime::ShowsHostPlaceholder;
using snowdesktop::widget_runtime::WidgetConsentSessionAction;
using snowdesktop::widget_runtime::WidgetHostAction;
using snowdesktop::widget_runtime::WidgetHostStateKind;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestPlaceholderActions()
{
    Check(!ShowsHostPlaceholder(WidgetHostStateKind::Ready),
        "ready widgets must not show a host placeholder");
    Check(HostActionFor(WidgetHostStateKind::PermissionPending) ==
            WidgetHostAction::RequestPermission &&
        HostActionFor(WidgetHostStateKind::PermissionDenied) ==
            WidgetHostAction::RequestPermission,
        "permission blocks must expose the authorization action");
    Check(HostActionFor(WidgetHostStateKind::QuotaExceeded) ==
            WidgetHostAction::Reload &&
        HostActionFor(WidgetHostStateKind::RuntimeSuspended) ==
            WidgetHostAction::Reload &&
        HostActionFor(WidgetHostStateKind::LoadFailed) ==
            WidgetHostAction::Reload,
        "recoverable runtime failures must expose the reload action");
}

void TestRuntimeFailureClassification()
{
    Check(ClassifyWidgetRuntimeFailure(true, false, {}) ==
            WidgetHostStateKind::QuotaExceeded,
        "VM quota flags must take precedence");
    Check(ClassifyWidgetRuntimeFailure(false, true,
            "Widget notification quota exceeded") ==
            WidgetHostStateKind::QuotaExceeded,
        "quota errors must remain distinguishable after the circuit opens");
    Check(ClassifyWidgetRuntimeFailure(false, true,
            "callback failed") ==
            WidgetHostStateKind::RuntimeSuspended,
        "an open circuit without a quota error must be suspended");
    Check(ClassifyWidgetRuntimeFailure(false, false,
            "syntax error") == WidgetHostStateKind::LoadFailed,
        "ordinary failures must use the generic load failure state");
}

void TestConsentSessionRecovery()
{
    constexpr std::uint64_t timeout = 3000;
    Check(ConsentSessionActionFor(false, false, false, 0, timeout) ==
            WidgetConsentSessionAction::Start,
        "a consent request without a pending session must start");
    Check(ConsentSessionActionFor(true, false, false, 2999, timeout) ==
            WidgetConsentSessionAction::WaitForWindow,
        "a newly launched consent worker must get time to publish its window");
    Check(ConsentSessionActionFor(true, true, true, 60000, timeout) ==
            WidgetConsentSessionAction::ActivateWindow,
        "a repeated consent click must reactivate the live dialog");
    Check(ConsentSessionActionFor(true, false, false, timeout, timeout) ==
            WidgetConsentSessionAction::ReplaceStale &&
            ConsentSessionActionFor(true, true, false, 1, timeout) ==
                WidgetConsentSessionAction::ReplaceStale,
        "missing or destroyed consent windows must not swallow retry clicks");
}

void TestConsentDialogPlacement()
{
    const auto secondary = CenterConsentDialogInWorkArea(
        1920, 40, 4480, 1560, 640, 480);
    Check(secondary.x == 2880 && secondary.y == 560,
        "consent dialogs must center inside the selected monitor work area");

    const auto oversized = CenterConsentDialogInWorkArea(
        -1280, 0, 0, 720, 1600, 900);
    Check(oversized.x == -1280 && oversized.y == 0,
        "oversized consent dialogs must remain anchored to the work area");
}
}

int main()
{
    TestPlaceholderActions();
    TestRuntimeFailureClassification();
    TestConsentSessionRecovery();
    TestConsentDialogPlacement();
    std::cout << "widget host state tests passed\n";
    return 0;
}
