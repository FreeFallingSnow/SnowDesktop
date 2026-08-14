#include "widget_permission_state.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using snowdesktop::widget::PermissionDecisionState;
using snowdesktop::widget::PermissionRiskClass;
using snowdesktop::widget::PermissionRuntimeBlock;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestStableNamesAndParsing()
{
    constexpr PermissionDecisionState states[] = {
        PermissionDecisionState::LegacyImplicit,
        PermissionDecisionState::Pending,
        PermissionDecisionState::Granted,
        PermissionDecisionState::Denied,
    };
    for (const PermissionDecisionState state : states)
    {
        const char* name =
            snowdesktop::widget::PermissionDecisionStateName(state);
        const auto parsed =
            snowdesktop::widget::ParsePermissionDecisionState(name);
        Check(parsed && *parsed == state,
            "permission decision state names must round-trip");
    }
    Check(!snowdesktop::widget::ParsePermissionDecisionState("") &&
            !snowdesktop::widget::ParsePermissionDecisionState("unknown"),
        "unknown permission decision states must be rejected");
}

void TestExplicitEmptyGrantDoesNotFallBack()
{
    const std::vector<std::string> declared = {
        "network.http", "system.read"
    };
    const std::vector<std::string> empty;
    const auto legacy = snowdesktop::widget::ResolveGrantedScopes(
        PermissionDecisionState::LegacyImplicit, declared, empty);
    Check(legacy == declared,
        "legacy records without a snapshot must retain compatibility");

    const auto grantedEmpty = snowdesktop::widget::ResolveGrantedScopes(
        PermissionDecisionState::Granted, declared, empty);
    Check(grantedEmpty.empty(),
        "an explicit empty grant must not fall back to declared scopes");
    Check(snowdesktop::widget::ResolveGrantedScopes(
            PermissionDecisionState::Pending, declared, declared).empty() &&
            snowdesktop::widget::ResolveGrantedScopes(
                PermissionDecisionState::Denied,
                declared, declared).empty(),
        "pending and denied states must expose no effective scopes");
}

void TestRuntimeEligibility()
{
    Check(snowdesktop::widget::PermissionStateAllowsRuntime(
            PermissionDecisionState::LegacyImplicit) &&
            snowdesktop::widget::PermissionStateAllowsRuntime(
                PermissionDecisionState::Granted),
        "legacy and granted records must remain runtime-eligible");
    Check(!snowdesktop::widget::PermissionStateAllowsRuntime(
            PermissionDecisionState::Pending) &&
            !snowdesktop::widget::PermissionStateAllowsRuntime(
                PermissionDecisionState::Denied),
        "pending and denied records must not be runtime-eligible");
    Check(snowdesktop::widget::PermissionRuntimeBlockFor(
            PermissionDecisionState::LegacyImplicit) ==
                PermissionRuntimeBlock::None &&
            snowdesktop::widget::PermissionRuntimeBlockFor(
                PermissionDecisionState::Granted) ==
                PermissionRuntimeBlock::None,
        "runtime-eligible states must have no activation block");
    Check(snowdesktop::widget::PermissionRuntimeBlockFor(
            PermissionDecisionState::Pending) ==
                PermissionRuntimeBlock::PendingConsent &&
            snowdesktop::widget::PermissionRuntimeBlockFor(
                PermissionDecisionState::Denied) ==
                PermissionRuntimeBlock::Denied,
        "pending and denied activation blocks must remain distinguishable");
}

void TestPermissionRiskClassification()
{
    using snowdesktop::widget::ClassifyPermissionRisk;
    Check(ClassifyPermissionRisk("ui.input") == PermissionRiskClass::Basic &&
            ClassifyPermissionRisk("ui.contextMenu") ==
                PermissionRiskClass::Basic,
        "surface input permissions must remain basic capabilities");
    Check(ClassifyPermissionRisk("system.read") ==
                PermissionRiskClass::SystemStatus &&
            ClassifyPermissionRisk("system.performance.read") ==
                PermissionRiskClass::SystemStatus,
        "legacy and v2 system reads must require system-status consent");
    Check(ClassifyPermissionRisk("desktop.read") ==
                PermissionRiskClass::PersonalData &&
            ClassifyPermissionRisk("network.internet") ==
                PermissionRiskClass::ExternalCommunication &&
            ClassifyPermissionRisk("everything.search") ==
                PermissionRiskClass::ElevatedRead &&
            ClassifyPermissionRisk("calendar.write") ==
                PermissionRiskClass::Modification,
        "sensitive permissions must retain their consent categories");
    Check(ClassifyPermissionRisk("future.unregistered") ==
            PermissionRiskClass::Unknown,
        "unknown permissions must remain distinguishable and fail closed");
}

void TestConsentSelection()
{
    const std::vector<std::string> declared = {
        "ui.input", "desktop.read", "ui.contextMenu",
        "calendar.write", "future.unregistered"
    };
    const std::vector<std::string> expected = {
        "desktop.read", "calendar.write", "future.unregistered"
    };
    Check(snowdesktop::widget::PermissionsRequiringConsent(declared) ==
            expected,
        "consent selection must preserve manifest order and omit only basic permissions");
    Check(!snowdesktop::widget::PermissionRequiresConsent("ui.input") &&
            snowdesktop::widget::PermissionRequiresConsent("network.http") &&
            snowdesktop::widget::PermissionRequiresConsent(
                "future.unregistered"),
        "basic capabilities may activate silently while sensitive and unknown capabilities require consent");
}
}

int main()
{
    TestStableNamesAndParsing();
    TestExplicitEmptyGrantDoesNotFallBack();
    TestRuntimeEligibility();
    TestPermissionRiskClassification();
    TestConsentSelection();
    std::cout << "widget permission state tests passed\n";
    return 0;
}
