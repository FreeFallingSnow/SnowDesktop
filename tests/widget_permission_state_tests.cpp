#include "widget_permission_state.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using snowdesktop::widget::PermissionDecisionState;

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
}
}

int main()
{
    TestStableNamesAndParsing();
    TestExplicitEmptyGrantDoesNotFallBack();
    TestRuntimeEligibility();
    std::cout << "widget permission state tests passed\n";
    return 0;
}
