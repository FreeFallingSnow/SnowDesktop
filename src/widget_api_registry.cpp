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
    case LibraryValidationError::DuplicateFunctionName:
        return "duplicate function name";
    }
    return "unknown validation error";
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
}
