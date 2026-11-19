#include "widget_l10n_lua.h"

#include "common/l10n.h"
#include "lua_runtime.h"
#include "widget_l10n_format.h"

#include <windows.h>
#include <array>
#include <cmath>
#include <exception>
#include <string_view>
#include <vector>

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::widget_l10n
{
using lua_runtime::DeferLuaError;
using lua_runtime::PushProtectedLuaString;

static const LocaleCatalog*
SelectManifestLocale(const LocaleCatalogs& catalogs)
{
    const std::string language = Locale::Instance().GetEffectiveLanguage();
    std::vector<std::string> available;
    available.reserve(catalogs.size());
    for (const auto& [code, catalog] : catalogs)
    {
        (void)catalog;
        available.push_back(code);
    }
    const std::string selected =
        snowdesktop::localization::ResolveBestLanguage(available, language);
    auto catalog = catalogs.find(selected);
    if (catalog != catalogs.end()) return &catalog->second;
    catalog = catalogs.find("en-US");
    if (catalog != catalogs.end())
        return &catalog->second;
    return catalogs.empty() ? nullptr : &catalogs.begin()->second;
}

std::string TranslateManifest(const LocaleCatalogs& catalogs,
    const std::string& key, const std::string& fallback)
{
    if (const auto* catalog = SelectManifestLocale(catalogs))
    {
        auto value = catalog->find(key);
        if (value != catalog->end())
            return value->second;
    }
    auto english = catalogs.find("en-US");
    if (english != catalogs.end())
    {
        auto value = english->second.find(key);
        if (value != english->second.end())
            return value->second;
    }
    return fallback.empty() ? key : fallback;
}

static int LuaL10nTrWithDeferredError(lua_State* L)
{
    constexpr std::size_t maximumBytes = 64 * 1024;
    constexpr int maximumArguments = 64;
    const int argumentCount = lua_gettop(L) - 1;
    std::size_t keyLength = 0;
    (void)luaL_checklstring(L, 1, &keyLength);
    if (keyLength > maximumBytes || argumentCount > maximumArguments)
        return luaL_error(L, "l10n.tr: template or argument quota exceeded");
    if (!lua_checkstack(L, argumentCount + 4))
        return luaL_error(L, "l10n.tr: not enough Lua stack space");
    lua_pushvalue(L, 1);
    lua_rawget(L, lua_upvalueindex(1));
    if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 1); lua_pushvalue(L, 1); }
    std::size_t templateLength = 0;
    const char* templateBytes = lua_tolstring(L, -1, &templateLength);
    if (templateLength > maximumBytes)
        return luaL_error(L, "l10n.tr: template quota exceeded");
    const std::string_view pattern(templateBytes, templateLength);
    std::array<std::string_view, maximumArguments> values{};
    std::size_t argumentBytes = 0;
    // Convert before constructing owning C++ values. __tostring and Lua
    // allocation errors may longjmp; converted strings stay pinned on-stack.
    for (int index = 0; index < argumentCount; ++index)
    {
        std::size_t length = 0;
        const char* value = luaL_tolstring(L, index + 2, &length);
        if (length > maximumBytes - argumentBytes)
            return luaL_error(L, "l10n.tr: argument byte quota exceeded");
        argumentBytes += length;
        values[static_cast<std::size_t>(index)] = {value, length};
    }
    try
    {
        std::string result;
        result.reserve(pattern.size());
        for (std::size_t position = 0; position < pattern.size();)
        {
            std::string_view next = pattern.substr(position, 1);
            std::size_t consumed = 1;
            if (pattern[position] == '{')
            {
                std::size_t end = position + 1;
                unsigned index = 0;
                while (end < pattern.size() && end - position <= 2 &&
                    pattern[end] >= '0' && pattern[end] <= '9')
                    index = index * 10 + static_cast<unsigned>(pattern[end++] - '0');
                if (end > position + 1 && end < pattern.size() && pattern[end] == '}' &&
                    (end == position + 2 || pattern[position + 1] != '0') &&
                    index < static_cast<unsigned>(argumentCount))
                {
                    next = values[index];
                    consumed = end - position + 1;
                }
            }
            if (next.size() > maximumBytes - result.size())
                return DeferLuaError(L, "l10n.tr: output exceeds 65536 bytes");
            result.append(next);
            position += consumed;
        }
        return PushProtectedLuaString(L, result) ? 1 : -1;
    }
    catch (const std::exception&)
    {
        return DeferLuaError(L, "l10n.tr: native formatting allocation failed");
    }
}

static int lua_L10nTr(lua_State* L)
{
    const int result = LuaL10nTrWithDeferredError(L);
    return result < 0 ? lua_error(L) : result;
}

static int lua_L10nLanguage(lua_State* L)
{
    const std::string language = Locale::Instance().GetEffectiveLanguage();
    lua_pushlstring(L, language.data(), language.size());
    return 1;
}

static int LuaL10nOptions(lua_State* L, int index)
{
    if (lua_isnoneornil(L, index)) return 0;
    luaL_checktype(L, index, LUA_TTABLE);
    return lua_absindex(L, index);
}

static std::string LuaL10nLocale(lua_State* L, int options)
{
    std::string locale = Locale::Instance().GetEffectiveLanguage();
    if (!options) return locale;
    lua_getfield(L, options, "locale");
    if (!lua_isnil(L, -1))
    {
        size_t length = 0;
        const char* value = luaL_checklstring(L, -1, &length);
        if (length == 0 || length >= LOCALE_NAME_MAX_LENGTH)
            luaL_error(L, "l10n: 'locale' must be a valid locale name");
        locale.assign(value, length);
    }
    lua_pop(L, 1);
    return locale;
}

static int LuaL10nIntegerOption(lua_State* L, int options,
    const char* name, int defaultValue, int minimum, int maximum)
{
    if (!options) return defaultValue;
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return defaultValue;
    }
    int isNumber = 0;
    const lua_Integer raw = lua_tointegerx(L, -1, &isNumber);
    if (!isNumber || raw < minimum || raw > maximum)
    {
        return luaL_error(L,
            "l10n: option '%s' must be an integer from %d to %d",
            name, minimum, maximum);
    }
    lua_pop(L, 1);
    return static_cast<int>(raw);
}

static bool LuaL10nBooleanOption(lua_State* L, int options,
    const char* name, bool defaultValue)
{
    if (!options) return defaultValue;
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return defaultValue;
    }
    if (!lua_isboolean(L, -1))
        luaL_error(L, "l10n: option '%s' must be a boolean", name);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return result;
}

static std::string LuaL10nStringOption(lua_State* L, int options,
    const char* name, const char* defaultValue)
{
    if (!options) return defaultValue;
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return defaultValue;
    }
    size_t length = 0;
    const char* value = luaL_checklstring(L, -1, &length);
    if (length == 0 || length > 32)
        luaL_error(L, "l10n: option '%s' has an invalid length", name);
    std::string result(value, length);
    lua_pop(L, 1);
    return result;
}

static int lua_L10nFormatNumber(lua_State* L)
{
    const double value = luaL_checknumber(L, 1);
    const int options = LuaL10nOptions(L, 2);
    snowdesktop::widget_l10n::NumberOptions format;
    format.minimumFractionDigits = LuaL10nIntegerOption(L, options,
        "minimumFractionDigits", 0, 0, 6);
    format.maximumFractionDigits = LuaL10nIntegerOption(L, options,
        "maximumFractionDigits", 2, 0, 6);
    if (format.minimumFractionDigits > format.maximumFractionDigits)
    {
        return luaL_error(L,
            "l10n.formatNumber: minimumFractionDigits exceeds maximumFractionDigits");
    }
    format.grouping = LuaL10nBooleanOption(
        L, options, "grouping", true);
    const std::string result = snowdesktop::widget_l10n::FormatNumber(
        value, LuaL10nLocale(L, options), format);
    if (result.empty())
        return luaL_error(L, "l10n.formatNumber: value must be finite");
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_L10nFormatBytes(lua_State* L)
{
    const double value = luaL_checknumber(L, 1);
    const int options = LuaL10nOptions(L, 2);
    const int base = LuaL10nIntegerOption(L, options,
        "base", 1024, 1000, 1024);
    if (base != 1000 && base != 1024)
        return luaL_error(L, "l10n.formatBytes: base must be 1000 or 1024");
    const int digits = LuaL10nIntegerOption(L, options,
        "maximumFractionDigits", 1, 0, 3);
    const std::string result = snowdesktop::widget_l10n::FormatBytes(
        value, LuaL10nLocale(L, options), base, digits);
    if (result.empty())
    {
        return luaL_error(L,
            "l10n.formatBytes: value must be finite and non-negative");
    }
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_L10nFormatDuration(lua_State* L)
{
    const lua_Integer value = luaL_checkinteger(L, 1);
    if (value < 0)
    {
        return luaL_error(L,
            "l10n.formatDuration: milliseconds must be non-negative");
    }
    const int options = LuaL10nOptions(L, 2);
    std::string style = "short";
    if (options)
    {
        lua_getfield(L, options, "style");
        if (!lua_isnil(L, -1))
        {
            size_t length = 0;
            const char* styleValue = luaL_checklstring(L, -1, &length);
            style.assign(styleValue, length);
        }
        lua_pop(L, 1);
    }
    if (style != "short" && style != "clock")
    {
        return luaL_error(L,
            "l10n.formatDuration: style must be 'short' or 'clock'");
    }
    const std::string result = snowdesktop::widget_l10n::FormatDuration(
        static_cast<std::int64_t>(value), LuaL10nLocale(L, options), style);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_L10nFormatRelativeTime(lua_State* L)
{
    const auto delta = static_cast<std::int64_t>(luaL_checkinteger(L, 1));
    const int options = LuaL10nOptions(L, 2);
    const std::string unit = LuaL10nStringOption(
        L, options, "unit", "auto");
    const std::string numeric = LuaL10nStringOption(
        L, options, "numeric", "auto");
    const std::string result = snowdesktop::widget_l10n::FormatRelativeTime(
        delta, LuaL10nLocale(L, options), unit, numeric);
    if (result.empty())
    {
        return luaL_error(L,
            "l10n.formatRelativeTime: unit or numeric option is invalid");
    }
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_L10nFormatList(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    const int valuesTable = lua_absindex(L, 1);
    const size_t count = lua_rawlen(L, valuesTable);
    if (count > 64)
        return luaL_error(L, "l10n.formatList: at most 64 values are allowed");
    size_t totalBytes = 0;
    for (size_t index = 1; index <= count; ++index)
    {
        lua_rawgeti(L, valuesTable, static_cast<lua_Integer>(index));
        if (lua_type(L, -1) != LUA_TSTRING)
        {
            return luaL_error(L,
                "l10n.formatList: value %llu must be a string",
                static_cast<unsigned long long>(index));
        }
        size_t length = 0;
        lua_tolstring(L, -1, &length);
        lua_pop(L, 1);
        if (length > 65536 - totalBytes)
        {
            return luaL_error(L,
                "l10n.formatList: values exceed the 64 KiB limit");
        }
        totalBytes += length;
    }

    const int options = LuaL10nOptions(L, 2);
    const std::string locale = LuaL10nLocale(L, options);
    std::vector<std::string> values;
    values.reserve(count);
    for (size_t index = 1; index <= count; ++index)
    {
        lua_rawgeti(L, valuesTable, static_cast<lua_Integer>(index));
        size_t length = 0;
        const char* value = lua_tolstring(L, -1, &length);
        values.emplace_back(value, length);
        lua_pop(L, 1);
    }
    const std::string result = snowdesktop::widget_l10n::FormatList(
        values, locale);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

void PushWidgetL10nAPI(lua_State* L, const LocaleCatalogs& catalogs)
{
    lua_newtable(L);

    lua_newtable(L);
    auto pushCatalog = [L](
        const LocaleCatalog& catalog) {
        for (const auto& [key, value] : catalog)
        {
            lua_pushlstring(L, value.data(), value.size());
            lua_setfield(L, -2, key.c_str());
        }
    };
    auto english = catalogs.find("en-US");
    if (english != catalogs.end())
        pushCatalog(english->second);

    const auto* selected = SelectManifestLocale(catalogs);
    if (selected && (english == catalogs.end() ||
        selected != &english->second))
    {
        pushCatalog(*selected);
    }
    lua_pushcclosure(L, lua_L10nTr, 1);
    lua_setfield(L, -2, "tr");

    lua_pushcfunction(L, lua_L10nLanguage);
    lua_setfield(L, -2, "language");

    lua_pushcfunction(L, lua_L10nFormatNumber);
    lua_setfield(L, -2, "formatNumber");
    lua_pushcfunction(L, lua_L10nFormatBytes);
    lua_setfield(L, -2, "formatBytes");
    lua_pushcfunction(L, lua_L10nFormatDuration);
    lua_setfield(L, -2, "formatDuration");
    lua_pushcfunction(L, lua_L10nFormatRelativeTime);
    lua_setfield(L, -2, "formatRelativeTime");
    lua_pushcfunction(L, lua_L10nFormatList);
    lua_setfield(L, -2, "formatList");
}

}
