#include "widget_permission_broker.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include <vector>

namespace snowdesktop::widget
{
namespace
{
std::vector<std::string> EffectiveScopes(
    PermissionDecisionState state,
    std::span<const std::string> declared,
    std::span<const std::string> stored)
{
    const std::vector<std::string> resolved = ResolveGrantedScopes(
        state, declared, stored);
    const std::set<std::string> declaredSet(
        declared.begin(), declared.end());
    std::vector<std::string> effective;
    effective.reserve(resolved.size());
    for (const auto& scope : resolved)
    {
        if (declaredSet.contains(scope) &&
            std::find(effective.begin(), effective.end(), scope) ==
                effective.end())
        {
            effective.push_back(scope);
        }
    }
    return effective;
}

void AppendCanonicalScopes(std::ostringstream& out, char kind,
    std::span<const std::string> scopes)
{
    std::set<std::string> ordered(scopes.begin(), scopes.end());
    for (const auto& scope : ordered)
        out << kind << ':' << scope.size() << ':' << scope << '\n';
}

std::string Sha256(std::string_view text)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    DWORD hashSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm,
            BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &resultSize, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
            &resultSize, 0) < 0)
    {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<UCHAR> object(objectSize);
    std::vector<UCHAR> digest(hashSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
            nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    const bool hashed = text.empty() ||
        BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(
                const_cast<char*>(text.data())),
            static_cast<ULONG>(text.size()), 0) >= 0;
    const bool finished = hashed &&
        BCryptFinishHash(hash, digest.data(), hashSize, 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!finished) return {};

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const UCHAR byte : digest)
        out << std::setw(2) << static_cast<int>(byte);
    return out.str();
}
}

PermissionGrantSnapshot WidgetPermissionBroker::Evaluate(
    PermissionDecisionState state,
    std::span<const std::string> declaredPermissions,
    std::span<const std::string> declaredNetworkDomains,
    std::span<const std::string> storedGrantedPermissions,
    std::span<const std::string> storedGrantedNetworkDomains)
{
    return Evaluate(state, declaredPermissions,
        std::span<const std::string>{}, declaredNetworkDomains,
        storedGrantedPermissions, storedGrantedNetworkDomains);
}

PermissionGrantSnapshot WidgetPermissionBroker::Evaluate(
    PermissionDecisionState state,
    std::span<const std::string> requiredPermissions,
    std::span<const std::string> optionalPermissions,
    std::span<const std::string> declaredNetworkDomains,
    std::span<const std::string> storedGrantedPermissions,
    std::span<const std::string> storedGrantedNetworkDomains)
{
    std::vector<std::string> declaredPermissions(
        requiredPermissions.begin(), requiredPermissions.end());
    for (const auto& permission : optionalPermissions)
        if (std::find(declaredPermissions.begin(),
                declaredPermissions.end(), permission) ==
            declaredPermissions.end())
            declaredPermissions.push_back(permission);

    PermissionGrantSnapshot result;
    // A stale decision from an older package version must not keep blocking a
    // package after the author removes every permission and domain. There is
    // nothing the user can authorize in that state.
    const bool hasDeclaredScopes = !declaredPermissions.empty() ||
        !declaredNetworkDomains.empty();
    result.runtimeBlock = hasDeclaredScopes
        ? ActivationBlock(state) : PermissionRuntimeBlock::None;
    result.permissions = EffectiveScopes(state,
        declaredPermissions, storedGrantedPermissions);
    result.networkDomains = EffectiveScopes(state,
        declaredNetworkDomains, storedGrantedNetworkDomains);
    result.requestedScopeFingerprint = ScopeFingerprint(
        requiredPermissions, optionalPermissions,
        declaredNetworkDomains);
    if (result.runtimeBlock == PermissionRuntimeBlock::None)
    {
        for (const auto& required : requiredPermissions)
        {
            if (!AllowsPermission(result.permissions, required))
            {
                result.runtimeBlock =
                    PermissionRuntimeBlock::MissingRequired;
                break;
            }
        }
    }
    return result;
}

PermissionGrantSnapshot WidgetPermissionBroker::GrantForIsolatedExecution(
    std::span<const std::string> requiredPermissions,
    std::span<const std::string> optionalPermissions,
    std::span<const std::string> declaredNetworkDomains)
{
    PermissionGrantSnapshot result;
    result.runtimeBlock = PermissionRuntimeBlock::None;
    const auto appendUnique = [](std::vector<std::string>& destination,
        std::span<const std::string> scopes)
    {
        for (const auto& scope : scopes)
        {
            if (std::find(destination.begin(), destination.end(), scope) ==
                destination.end())
                destination.push_back(scope);
        }
    };
    appendUnique(result.permissions, requiredPermissions);
    appendUnique(result.permissions, optionalPermissions);
    appendUnique(result.networkDomains, declaredNetworkDomains);
    result.requestedScopeFingerprint = ScopeFingerprint(
        requiredPermissions, optionalPermissions,
        declaredNetworkDomains);
    return result;
}

PermissionRuntimeBlock WidgetPermissionBroker::ActivationBlock(
    PermissionDecisionState state) noexcept
{
    return PermissionRuntimeBlockFor(state);
}

bool WidgetPermissionBroker::AllowsPermission(
    std::span<const std::string> grantedPermissions,
    std::string_view permission) noexcept
{
    return std::find(grantedPermissions.begin(),
        grantedPermissions.end(), permission) != grantedPermissions.end();
}

bool WidgetPermissionBroker::AllowsPermission(
    const std::unordered_set<std::string>& grantedPermissions,
    std::string_view permission)
{
    return grantedPermissions.contains(std::string(permission));
}

bool WidgetPermissionBroker::AllowsNetworkDomain(
    std::span<const std::string> grantedNetworkDomains,
    std::string_view domain) noexcept
{
    return std::find(grantedNetworkDomains.begin(),
        grantedNetworkDomains.end(), domain) !=
        grantedNetworkDomains.end();
}

std::string WidgetPermissionBroker::ScopeFingerprint(
    std::span<const std::string> declaredPermissions,
    std::span<const std::string> declaredNetworkDomains)
{
    return ScopeFingerprint(declaredPermissions,
        std::span<const std::string>{}, declaredNetworkDomains);
}

std::string WidgetPermissionBroker::ScopeFingerprint(
    std::span<const std::string> requiredPermissions,
    std::span<const std::string> optionalPermissions,
    std::span<const std::string> declaredNetworkDomains)
{
    std::ostringstream canonical;
    if (optionalPermissions.empty())
    {
        canonical << "snowdesktop-widget-permission-scope-v1\n";
        AppendCanonicalScopes(canonical, 'P', requiredPermissions);
        AppendCanonicalScopes(canonical, 'D', declaredNetworkDomains);
        return Sha256(canonical.str());
    }
    canonical << "snowdesktop-widget-permission-scope-v2\n";
    AppendCanonicalScopes(canonical, 'R', requiredPermissions);
    AppendCanonicalScopes(canonical, 'O', optionalPermissions);
    AppendCanonicalScopes(canonical, 'D', declaredNetworkDomains);
    return Sha256(canonical.str());
}
}
