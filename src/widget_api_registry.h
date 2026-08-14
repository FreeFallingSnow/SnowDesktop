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

struct LibraryDescriptor
{
    const char* name = nullptr;
    std::span<const FunctionDescriptor> functions;
};

enum class LibraryValidationError
{
    None,
    MissingLibraryName,
    MissingFunctionName,
    MissingCallback,
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

template<std::size_t N>
void RegisterLibraries(
    lua_State* state,
    const LibraryDescriptor (&libraries)[N])
{
    RegisterLibraries(
        state, std::span<const LibraryDescriptor>(libraries));
}
}
