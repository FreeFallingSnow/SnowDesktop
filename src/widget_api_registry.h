#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_api
{
struct FunctionDescriptor
{
    const char* name = nullptr;
    lua_CFunction callback = nullptr;
    std::uint32_t sinceApi = 1;
    const char* requiredPermission = nullptr;
    std::uint32_t untilApi = 0;
};

struct LibraryDescriptor
{
    const char* name = nullptr;
    std::span<const FunctionDescriptor> functions;
};

enum class SystemCapabilityKind
{
    Function,
    DataTopic,
    Task,
};

enum class SystemCapabilityPreview
{
    Deterministic,
    NoSideEffects,
};

struct SystemFunctionParameterContract
{
    const char* name = nullptr;
    const char* type = nullptr;
    bool optional = false;
};

struct SystemFunctionContract
{
    const char* name = nullptr;
    const char* feature = nullptr;
    std::span<const SystemFunctionParameterContract> parameters;
    const char* resultType = nullptr;
};

struct SystemDataTopicContract
{
    const char* name = nullptr;
    const char* feature = nullptr;
    const char* requiredPermission = nullptr;
    std::uint32_t minimumIntervalMs = 1000;
    std::uint32_t hiddenIntervalMs = 5000;
    std::uint32_t idleGraceMs = 2000;
    bool highRisk = false;
    bool supportsHiddenContinue = false;
    const char* optionsType = "SnowDataSubscribeOptions";
    const char* valueType = nullptr;
    SystemCapabilityPreview preview =
        SystemCapabilityPreview::Deterministic;
};

struct SystemTaskContract
{
    const char* name = nullptr;
    const char* feature = nullptr;
    const char* requiredPermission = nullptr;
    bool requiresTrustedGesture = false;
    std::size_t maximumPerInstance = 4;
    const char* argumentsType = nullptr;
    const char* resultType = nullptr;
    SystemCapabilityPreview preview =
        SystemCapabilityPreview::NoSideEffects;
};

enum class LibraryValidationError
{
    None,
    MissingLibraryName,
    MissingFunctionName,
    MissingCallback,
    InvalidApiVersion,
    EmptyRequiredPermission,
    DuplicateFunctionName,
};

enum class CatalogValidationError
{
    None,
    InvalidLibrary,
    DuplicateLibraryName,
};

struct CatalogValidationResult
{
    CatalogValidationError error = CatalogValidationError::None;
    std::size_t libraryIndex = 0;
    LibraryValidationError libraryError = LibraryValidationError::None;
};

LibraryValidationError ValidateLibrary(
    const char* libraryName,
    std::span<const FunctionDescriptor> functions) noexcept;

const char* DescribeValidationError(
    LibraryValidationError error) noexcept;

CatalogValidationResult ValidateCatalog(
    std::span<const LibraryDescriptor> libraries) noexcept;

const char* DescribeValidationError(
    CatalogValidationError error) noexcept;

const FunctionDescriptor* FindFunction(
    std::span<const LibraryDescriptor> libraries,
    std::string_view libraryName,
    std::string_view functionName) noexcept;

std::span<const std::string_view> HostFeatures() noexcept;
bool SupportsFeature(std::string_view feature) noexcept;
std::span<const SystemFunctionContract>
SystemFunctionContracts() noexcept;
std::span<const SystemDataTopicContract>
SystemDataTopicContracts() noexcept;
std::span<const SystemTaskContract> SystemTaskContracts() noexcept;
std::span<const std::string_view> SandboxLibraries() noexcept;
std::vector<std::string> MissingFeatures(
    std::span<const std::string> requiredFeatures);

int LuaDefineWidget(lua_State* state);
int LuaApiInfo(lua_State* state);
int LuaHasFeature(lua_State* state);
int LuaSystemCapabilities(lua_State* state);
bool IsDefinedWidget(lua_State* state, int index) noexcept;

int LuaTransientStateGet(lua_State* state);
int LuaTransientStateSet(lua_State* state);
int LuaTransientStateRemove(lua_State* state);
int LuaTransientStateHas(lua_State* state);
int LuaTransientStateKeys(lua_State* state);
int LuaTransientStateClear(lua_State* state);
bool ConsumeTransientStateDirty(lua_State* state) noexcept;

/**
 * Registers a table of C callbacks as one Lua global library.
 *
 * Validation happens before the Lua stack is modified. Invalid descriptors
 * throw std::invalid_argument; a null lua_State also throws. Successful
 * registration preserves the caller's stack height.
 */
void RegisterLibrary(
    lua_State* state,
    const char* libraryName,
    std::span<const FunctionDescriptor> functions);

void RegisterLibrary(
    lua_State* state,
    const char* libraryName,
    std::span<const FunctionDescriptor> functions,
    std::uint32_t apiVersion);

template<std::size_t N>
void RegisterLibrary(
    lua_State* state,
    const char* libraryName,
    const FunctionDescriptor (&functions)[N])
{
    RegisterLibrary(
        state, libraryName,
        std::span<const FunctionDescriptor>(functions));
}

template<std::size_t N>
constexpr LibraryDescriptor DescribeLibrary(
    const char* name,
    const FunctionDescriptor (&functions)[N]) noexcept
{
    return { name, std::span<const FunctionDescriptor>(functions) };
}

/**
 * Validates the complete catalog before publishing any global library.
 * Successful registration preserves the caller's Lua stack height.
 */
void RegisterLibraries(
    lua_State* state,
    std::span<const LibraryDescriptor> libraries);

void RegisterLibraries(
    lua_State* state,
    std::span<const LibraryDescriptor> libraries,
    std::uint32_t apiVersion);

template<std::size_t N>
void RegisterLibraries(
    lua_State* state,
    const LibraryDescriptor (&libraries)[N])
{
    RegisterLibraries(
        state, std::span<const LibraryDescriptor>(libraries));
}
}
