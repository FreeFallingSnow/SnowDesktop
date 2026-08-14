#include "widget_api_registry.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace snowdesktop::widget_api
{
LibraryValidationError ValidateLibrary(
    const char* libraryName,
    std::span<const FunctionDescriptor> functions) noexcept
{
    if (!libraryName || libraryName[0] == '\0')
        return LibraryValidationError::MissingLibraryName;

    for (std::size_t index = 0; index < functions.size(); ++index)
    {
        const FunctionDescriptor& function = functions[index];
        if (!function.name || function.name[0] == '\0')
            return LibraryValidationError::MissingFunctionName;
        if (!function.callback)
            return LibraryValidationError::MissingCallback;
        if (function.sinceApi == 0)
            return LibraryValidationError::InvalidApiVersion;
        if (function.requiredPermission &&
            function.requiredPermission[0] == '\0')
        {
            return LibraryValidationError::EmptyRequiredPermission;
        }
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (std::string_view(functions[previous].name) ==
                function.name)
            {
                return LibraryValidationError::DuplicateFunctionName;
            }
        }
    }
    return LibraryValidationError::None;
}

const char* DescribeValidationError(
    LibraryValidationError error) noexcept
{
    switch (error)
    {
    case LibraryValidationError::None:
        return "none";
    case LibraryValidationError::MissingLibraryName:
        return "missing library name";
    case LibraryValidationError::MissingFunctionName:
        return "missing function name";
    case LibraryValidationError::MissingCallback:
        return "missing callback";
    case LibraryValidationError::InvalidApiVersion:
        return "invalid API version";
    case LibraryValidationError::EmptyRequiredPermission:
        return "empty required permission";
    case LibraryValidationError::DuplicateFunctionName:
        return "duplicate function name";
    }
    return "unknown validation error";
}

CatalogValidationResult ValidateCatalog(
    std::span<const LibraryDescriptor> libraries) noexcept
{
    for (std::size_t index = 0; index < libraries.size(); ++index)
    {
        const LibraryDescriptor& library = libraries[index];
        const LibraryValidationError libraryError =
            ValidateLibrary(library.name, library.functions);
        if (libraryError != LibraryValidationError::None)
        {
            return {
                CatalogValidationError::InvalidLibrary,
                index,
                libraryError,
            };
        }
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (std::string_view(libraries[previous].name) ==
                library.name)
            {
                return {
                    CatalogValidationError::DuplicateLibraryName,
                    index,
                    LibraryValidationError::None,
                };
            }
        }
    }
    return {};
}

const char* DescribeValidationError(
    CatalogValidationError error) noexcept
{
    switch (error)
    {
    case CatalogValidationError::None:
        return "none";
    case CatalogValidationError::InvalidLibrary:
        return "invalid library";
    case CatalogValidationError::DuplicateLibraryName:
        return "duplicate library name";
    }
    return "unknown validation error";
}

const FunctionDescriptor* FindFunction(
    std::span<const LibraryDescriptor> libraries,
    std::string_view libraryName,
    std::string_view functionName) noexcept
{
    if (libraryName.empty() || functionName.empty())
        return nullptr;

    for (const LibraryDescriptor& library : libraries)
    {
        if (!library.name || std::string_view(library.name) != libraryName)
            continue;
        for (const FunctionDescriptor& function : library.functions)
        {
            if (function.name &&
                std::string_view(function.name) == functionName)
            {
                return &function;
            }
        }
        return nullptr;
    }
    return nullptr;
}

void RegisterLibrary(
    lua_State* state,
    const char* libraryName,
    std::span<const FunctionDescriptor> functions)
{
    if (!state)
        throw std::invalid_argument(
            "cannot register a widget API library on a null Lua state");

    const LibraryValidationError validation =
        ValidateLibrary(libraryName, functions);
    if (validation != LibraryValidationError::None)
    {
        throw std::invalid_argument(
            std::string("invalid widget API library '") +
            (libraryName ? libraryName : "") + "': " +
            DescribeValidationError(validation));
    }

    const int entryTop = lua_gettop(state);
    lua_createtable(
        state, 0, static_cast<int>(functions.size()));
    for (const FunctionDescriptor& function : functions)
    {
        lua_pushcfunction(state, function.callback);
        lua_setfield(state, -2, function.name);
    }
    lua_setglobal(state, libraryName);

    if (lua_gettop(state) != entryTop)
    {
        lua_settop(state, entryTop);
        throw std::logic_error(
            "widget API registration did not preserve the Lua stack");
    }
}

void RegisterLibraries(
    lua_State* state,
    std::span<const LibraryDescriptor> libraries)
{
    if (!state)
        throw std::invalid_argument(
            "cannot register a widget API catalog on a null Lua state");

    const CatalogValidationResult validation =
        ValidateCatalog(libraries);
    if (validation.error != CatalogValidationError::None)
    {
        const LibraryDescriptor& library =
            libraries[validation.libraryIndex];
        std::string message =
            std::string("invalid widget API catalog at library '") +
            (library.name ? library.name : "") + "': " +
            DescribeValidationError(validation.error);
        if (validation.libraryError != LibraryValidationError::None)
        {
            message += ": ";
            message += DescribeValidationError(validation.libraryError);
        }
        throw std::invalid_argument(message);
    }

    const int entryTop = lua_gettop(state);
    for (const LibraryDescriptor& library : libraries)
        RegisterLibrary(state, library.name, library.functions);

    if (lua_gettop(state) != entryTop)
    {
        lua_settop(state, entryTop);
        throw std::logic_error(
            "widget API catalog registration did not preserve the Lua stack");
    }
}
}
