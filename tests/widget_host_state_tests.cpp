#include "widget_host_state.h"

#include <cstdlib>
#include <iostream>

namespace
{
using snowdesktop::widget_runtime::ClassifyWidgetRuntimeFailure;
using snowdesktop::widget_runtime::HostActionFor;
using snowdesktop::widget_runtime::ShowsHostPlaceholder;
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
}

int main()
{
    TestPlaceholderActions();
    TestRuntimeFailureClassification();
    std::cout << "widget host state tests passed\n";
    return 0;
}
