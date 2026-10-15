#include "widget_calendar_query_options.h"
#include "widget/runtime/lua_runtime.h"

#include <utility>

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_api
{
namespace
{
struct ParseContext
{
    snowdesktop::widget_runtime::CalendarEventQueryOptions options;
    std::string error;
    bool specified = false;
    bool accepted = false;
};

bool ParseRaw(lua_State* state, ParseContext& context)
{
    // All owning C++ values live outside pcall. A Lua allocation failure while
    // interning a raw field key cannot jump over their destructors.
    const auto table = 2;
    const auto field = [&](const char* name) { lua_pushstring(state, name); lua_rawget(state, table); };
    auto& options = context.options;
    auto& error = context.error;
    field("query");
    if (!lua_isnil(state, -1))
    {
        context.specified = true;
        if (lua_type(state, -1) != LUA_TSTRING)
        { lua_pop(state, 1); error = "calendar query must be a string"; return false; }
        std::size_t length = 0;
        const auto* text = lua_tolstring(state, -1, &length);
        if (length > 512)
        { lua_pop(state, 1); error = "calendar query exceeds 512 bytes"; return false; }
        options.query.assign(text, length);
    }
    lua_pop(state, 1);
    for (const auto* name : {"offset", "limit"})
    {
        field(name);
        if (!lua_isnil(state, -1))
        {
            context.specified = true;
            int integer = 0;
            const auto number = lua_tointegerx(state, -1, &integer);
            if (lua_type(state, -1) != LUA_TNUMBER || !integer || number < 0 ||
                (name[0] == 'l' && (number < 1 || number > 512)))
            { lua_pop(state, 1); error = "calendar offset must be nonnegative and limit must be 1 to 512, both integers"; return false; }
            if (name[0] == 'o') options.offset = static_cast<std::uint64_t>(number);
            else options.limit = static_cast<std::size_t>(number);
        }
        lua_pop(state, 1);
    }
    if (!context.specified) return true;
    field("eventId");
    const bool hasId = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (hasId) { error = "eventId and query pagination are mutually exclusive"; return false; }
    if (!snowdesktop::widget_runtime::ValidateCalendarEventQuery(options, error)) return false;
    return true;
}
}

bool ParseCalendarEventQueryOptions(lua_State* state, int optionsIndex,
    std::optional<snowdesktop::widget_runtime::CalendarEventQueryOptions>& result,
    std::string& error)
{
    error.clear();
    result.reset();
    if (!state) { error = "invalid calendar Lua state"; return false; }
    if (lua_isnoneornil(state, optionsIndex)) return true;
    if (!lua_istable(state, optionsIndex)) { error = "calendar options must be a table"; return false; }
    ParseContext context;
    if (!snowdesktop::lua_runtime::ProtectedHostCall(state, +[](lua_State* inner) {
            auto& parsed = *static_cast<ParseContext*>(lua_touserdata(inner, 1));
            parsed.accepted = ParseRaw(inner, parsed);
            return 0;
        }, &context, 0, error, optionsIndex)) return false;
    if (!context.accepted) { error = std::move(context.error); return false; }
    if (context.specified) result = std::move(context.options);
    return true;
}
}
