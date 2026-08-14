#include "widget_api_registry.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

namespace
{
using snowdesktop::widget_api::FunctionDescriptor;
using snowdesktop::widget_api::CatalogValidationError;
using snowdesktop::widget_api::LibraryDescriptor;
using snowdesktop::widget_api::LibraryValidationError;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int ReturnFortyTwo(lua_State* state)
{
    lua_pushinteger(state, 42);
    return 1;
}

int Add(lua_State* state)
{
    const lua_Integer left = luaL_checkinteger(state, 1);
    const lua_Integer right = luaL_checkinteger(state, 2);
    lua_pushinteger(state, left + right);
    return 1;
}

class LuaState
{
public:
    LuaState()
        : state_(luaL_newstate())
    {
        if (!state_)
            throw std::runtime_error("failed to create Lua state");
    }

    ~LuaState()
    {
        lua_close(state_);
    }

    operator lua_State*() const noexcept
    {
        return state_;
    }

private:
    lua_State* state_ = nullptr;
};

void TestValidation()
{
    constexpr FunctionDescriptor valid[] = {
        { "answer", ReturnFortyTwo },
        { "add", Add },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", valid) == LibraryValidationError::None,
        "valid library must pass validation");
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            nullptr, valid) ==
                LibraryValidationError::MissingLibraryName,
        "null library name must be rejected");
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "", valid) ==
                LibraryValidationError::MissingLibraryName,
        "empty library name must be rejected");

    constexpr FunctionDescriptor missingName[] = {
        { nullptr, ReturnFortyTwo },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", missingName) ==
                LibraryValidationError::MissingFunctionName,
        "missing function name must be rejected");

    constexpr FunctionDescriptor missingCallback[] = {
        { "answer", nullptr },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", missingCallback) ==
                LibraryValidationError::MissingCallback,
        "missing callback must be rejected");

    constexpr FunctionDescriptor duplicate[] = {
        { "answer", ReturnFortyTwo },
        { "answer", Add },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", duplicate) ==
                LibraryValidationError::DuplicateFunctionName,
        "duplicate function names must be rejected");
}

void TestRegistration()
{
    LuaState state;
    constexpr FunctionDescriptor functions[] = {
        { "answer", ReturnFortyTwo },
        { "add", Add },
    };

    lua_pushliteral(state, "sentinel");
    const int entryTop = lua_gettop(state);
    snowdesktop::widget_api::RegisterLibrary(
        state, "sample", functions);
    Check(
        lua_gettop(state) == entryTop,
        "successful registration must preserve stack height");

    lua_getglobal(state, "sample");
    Check(lua_istable(state, -1), "registered global must be a table");

    lua_getfield(state, -1, "answer");
    Check(lua_isfunction(state, -1), "answer must be registered");
    Check(
        lua_pcall(state, 0, 1, 0) == LUA_OK,
        "answer callback must execute");
    Check(
        lua_tointeger(state, -1) == 42,
        "answer callback must return its value");
    lua_pop(state, 1);

    lua_getfield(state, -1, "add");
    lua_pushinteger(state, 19);
    lua_pushinteger(state, 23);
    Check(
        lua_pcall(state, 2, 1, 0) == LUA_OK,
        "add callback must execute");
    Check(
        lua_tointeger(state, -1) == 42,
        "add callback must receive arguments");
    lua_pop(state, 2);

    Check(
        lua_gettop(state) == entryTop,
        "test cleanup must restore original stack height");
    Check(
        std::string(lua_tostring(state, -1)) == "sentinel",
        "registration must preserve existing stack values");
}

void TestCatalogValidation()
{
    static constexpr FunctionDescriptor firstFunctions[] = {
        { "answer", ReturnFortyTwo },
    };
    static constexpr FunctionDescriptor secondFunctions[] = {
        { "add", Add },
    };
    static constexpr LibraryDescriptor valid[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "first", firstFunctions),
        snowdesktop::widget_api::DescribeLibrary(
            "second", secondFunctions),
    };
    Check(
        snowdesktop::widget_api::ValidateCatalog(valid).error ==
            CatalogValidationError::None,
        "valid catalog must pass validation");

    static constexpr LibraryDescriptor duplicateLibraries[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "sample", firstFunctions),
        snowdesktop::widget_api::DescribeLibrary(
            "sample", secondFunctions),
    };
    const auto duplicateResult =
        snowdesktop::widget_api::ValidateCatalog(duplicateLibraries);
    Check(
        duplicateResult.error ==
            CatalogValidationError::DuplicateLibraryName &&
            duplicateResult.libraryIndex == 1,
        "duplicate library names must identify the later library");

    static constexpr FunctionDescriptor invalidFunctions[] = {
        { nullptr, ReturnFortyTwo },
    };
    static constexpr LibraryDescriptor invalidLibrary[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "sample", invalidFunctions),
    };
    const auto invalidResult =
        snowdesktop::widget_api::ValidateCatalog(invalidLibrary);
    Check(
        invalidResult.error == CatalogValidationError::InvalidLibrary &&
            invalidResult.libraryIndex == 0 &&
            invalidResult.libraryError ==
                LibraryValidationError::MissingFunctionName,
        "catalog validation must preserve library validation details");
}

void TestCatalogRegistration()
{
    LuaState state;
    static constexpr FunctionDescriptor firstFunctions[] = {
        { "answer", ReturnFortyTwo },
    };
    static constexpr FunctionDescriptor secondFunctions[] = {
        { "add", Add },
    };
    static constexpr LibraryDescriptor libraries[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "first", firstFunctions),
        snowdesktop::widget_api::DescribeLibrary(
            "second", secondFunctions),
    };

    lua_pushliteral(state, "sentinel");
    const int entryTop = lua_gettop(state);
    snowdesktop::widget_api::RegisterLibraries(state, libraries);
    Check(
        lua_gettop(state) == entryTop,
        "catalog registration must preserve stack height");
    lua_getglobal(state, "first");
    lua_getfield(state, -1, "answer");
    Check(
        lua_pcall(state, 0, 1, 0) == LUA_OK &&
            lua_tointeger(state, -1) == 42,
        "first catalog library must be callable");
    lua_pop(state, 2);
    lua_getglobal(state, "second");
    lua_getfield(state, -1, "add");
    lua_pushinteger(state, 20);
    lua_pushinteger(state, 22);
    Check(
        lua_pcall(state, 2, 1, 0) == LUA_OK &&
            lua_tointeger(state, -1) == 42,
        "second catalog library must be callable");
    lua_pop(state, 2);
    Check(
        lua_gettop(state) == entryTop,
        "catalog test cleanup must restore stack height");
}

void TestInvalidRegistrationIsAtomic()
{
    LuaState state;
    lua_pushinteger(state, 7);
    const int entryTop = lua_gettop(state);
    constexpr FunctionDescriptor duplicate[] = {
        { "answer", ReturnFortyTwo },
        { "answer", Add },
    };

    bool threw = false;
    try
    {
        snowdesktop::widget_api::RegisterLibrary(
            state, "invalid", duplicate);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "invalid registration must throw");
    Check(
        lua_gettop(state) == entryTop &&
            lua_tointeger(state, -1) == 7,
        "invalid registration must not modify the stack");
    lua_getglobal(state, "invalid");
    Check(lua_isnil(state, -1), "invalid library must not be published");
    lua_pop(state, 1);

    threw = false;
    try
    {
        constexpr FunctionDescriptor valid[] = {
            { "answer", ReturnFortyTwo },
        };
        snowdesktop::widget_api::RegisterLibrary(
            nullptr, "sample", valid);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "null Lua state must throw");
}


void TestInvalidCatalogRegistrationIsAtomic()
{
    LuaState state;
    static constexpr FunctionDescriptor validFunctions[] = {
        { "answer", ReturnFortyTwo },
    };
    static constexpr FunctionDescriptor invalidFunctions[] = {
        { nullptr, Add },
    };
    static constexpr LibraryDescriptor libraries[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "wouldPublish", validFunctions),
        snowdesktop::widget_api::DescribeLibrary(
            "invalid", invalidFunctions),
    };

    lua_pushinteger(state, 7);
    const int entryTop = lua_gettop(state);
    bool threw = false;
    try
    {
        snowdesktop::widget_api::RegisterLibraries(
            state, libraries);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "invalid catalog registration must throw");
    Check(
        lua_gettop(state) == entryTop &&
            lua_tointeger(state, -1) == 7,
        "invalid catalog registration must preserve the stack");
    lua_getglobal(state, "wouldPublish");
    Check(
        lua_isnil(state, -1),
        "catalog must be fully validated before publishing globals");
    lua_pop(state, 1);
}
}

int main()
{
    TestValidation();
    TestRegistration();
    TestCatalogValidation();
    TestCatalogRegistration();
    TestInvalidRegistrationIsAtomic();
    TestInvalidCatalogRegistrationIsAtomic();
    std::cout << "widget API registry tests passed\n";
    return 0;
}
