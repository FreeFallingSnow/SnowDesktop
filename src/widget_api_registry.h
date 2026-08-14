#pragma once

#include <cstddef>
#include <span>

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_api
{
struct FunctionDescriptor
{
    const char* name = nullptr;
    lua_CFunction callback = nullptr;
};

enum class LibraryValidationError
{
    None,
    MissingLibraryName,
    MissingFunctionName,
    MissingCallback,
    DuplicateFunctionName,
};

LibraryValidationError ValidateLibrary(
    const char* libraryName,
    std::span<const FunctionDescriptor> functions) noexcept;

const char* DescribeValidationError(
    LibraryValidationError error) noexcept;

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
}
