#include "widget_author_test.h"

#include "lua_runtime.h"
#include "widget_package.h"

#include <windows.h>

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string_view>
#include <system_error>

namespace snowdesktop::widget_authoring
{
namespace
{
constexpr std::size_t kMaximumTestBytes = 1u * 1024u * 1024u;
constexpr std::size_t kMaximumModuleCount = 64;
constexpr std::size_t kMaximumModuleBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaximumErrorBytes = 4096;
char kModuleLoadingMarker = 0;

struct TestFileContext
{
    std::filesystem::path root;
    std::filesystem::path canonicalRoot;
    std::size_t moduleCount = 0;
    std::size_t moduleBytes = 0;
};

void* QuotaAllocator(void* userData, void* pointer,
    std::size_t oldSize, std::size_t newSize)
{
    auto* quota = static_cast<LuaRuntimeQuota*>(userData);
    if (!quota) return nullptr;
    if (newSize == 0)
    {
        if (pointer)
            quota->memoryBytes = oldSize > quota->memoryBytes
                ? 0 : quota->memoryBytes - oldSize;
        std::free(pointer);
        return nullptr;
    }
    const std::size_t current = pointer ? oldSize : 0;
    const std::size_t withoutCurrent = current > quota->memoryBytes
        ? 0 : quota->memoryBytes - current;
    if (newSize > quota->memoryLimit ||
            withoutCurrent > quota->memoryLimit - newSize)
    {
        quota->memoryExceeded = true;
        return nullptr;
    }
    void* result = std::realloc(pointer, newSize);
    if (result) quota->memoryBytes = withoutCurrent + newSize;
    return result;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length) <= 0)
        return {};
    return result;
}

std::string PathUtf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
}

std::string JsonEscape(std::string_view value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20)
            {
                result += "\\u00";
                result.push_back(kHex[character >> 4]);
                result.push_back(kHex[character & 0x0f]);
            }
            else result.push_back(static_cast<char>(character));
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string BoundedError(const char* value)
{
    std::string result = value ? value : "unknown Lua test error";
    if (result.size() > kMaximumErrorBytes)
        result.resize(kMaximumErrorBytes);
    return result;
}

bool StartsWithPath(const std::filesystem::path& child,
    const std::filesystem::path& parent)
{
    auto childPart = child.begin();
    auto parentPart = parent.begin();
    for (; parentPart != parent.end(); ++parentPart, ++childPart)
    {
        if (childPart == child.end() ||
                _wcsicmp(childPart->c_str(), parentPart->c_str()) != 0)
            return false;
    }
    return true;
}

bool HasReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

TestFileContext* CurrentContext(lua_State* state)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__author_test_context");
    auto* context = static_cast<TestFileContext*>(
        lua_touserdata(state, -1));
    lua_pop(state, 1);
    return context;
}

void ClearModuleCache(lua_State* state, int cache,
    const std::string& key)
{
    lua_pushnil(state);
    lua_setfield(state, cache, key.c_str());
}

int LuaTestModuleRequire(lua_State* state)
{
    std::size_t rawLength = 0;
    const char* raw = luaL_checklstring(state, 1, &rawLength);
    auto* context = CurrentContext(state);
    if (!context || rawLength == 0 || rawLength > 512)
        return luaL_error(state, "module.require: invalid test context or path");
    const std::filesystem::path relative(
        Utf8ToWide(std::string_view(raw, rawLength)));
    if (relative.empty() || relative.is_absolute() ||
            relative.extension() != L".lua" ||
            relative.begin() == relative.end() ||
            _wcsicmp(relative.begin()->c_str(), L"modules") != 0)
        return luaL_error(state,
            "module.require: tests may load only modules/*.lua");
    for (const auto& part : relative)
        if (part == L".." || part == L".")
            return luaL_error(state, "module.require: unsafe path");

    std::error_code error;
    const auto full = std::filesystem::weakly_canonical(
        context->root / relative, error);
    if (error || !StartsWithPath(full, context->canonicalRoot) ||
            HasReparsePoint(full) ||
            !std::filesystem::is_regular_file(full, error))
        return luaL_error(state,
            "module.require: module is missing or outside the package");
    const auto fileSize = std::filesystem::file_size(full, error);
    if (error || fileSize > snowdesktop::widget::kMaxEntryLuaBytes ||
            context->moduleCount >= kMaximumModuleCount ||
            context->moduleBytes > kMaximumModuleBytes -
                static_cast<std::size_t>(fileSize))
        return luaL_error(state, "module.require: module quota exceeded");

    const std::string key = PathUtf8(full);
    lua_getfield(state, LUA_REGISTRYINDEX, "__author_test_modules");
    const int cache = lua_absindex(state, -1);
    lua_getfield(state, cache, key.c_str());
    if (lua_touserdata(state, -1) == &kModuleLoadingMarker)
        return luaL_error(state, "module.require: circular dependency");
    if (!lua_isnil(state, -1))
    {
        lua_remove(state, cache);
        return 1;
    }
    lua_pop(state, 1);
    lua_pushlightuserdata(state, &kModuleLoadingMarker);
    lua_setfield(state, cache, key.c_str());

    std::ifstream input(full, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (!input || source.size() != fileSize)
    {
        ClearModuleCache(state, cache, key);
        return luaL_error(state, "module.require: cannot read module");
    }
    const std::string chunkName = "@" + PathUtf8(relative);
    if (luaL_loadbuffer(state, source.data(), source.size(),
            chunkName.c_str()) != LUA_OK)
    {
        ClearModuleCache(state, cache, key);
        return lua_error(state);
    }
    lua_getfield(state, LUA_REGISTRYINDEX, "__author_test_environment");
    if (!lua_istable(state, -1) ||
            lua_setupvalue(state, -2, 1) == nullptr)
    {
        if (lua_isnil(state, -1)) lua_pop(state, 1);
        ClearModuleCache(state, cache, key);
        return luaL_error(state, "module.require: test sandbox is unavailable");
    }
    if (snowdesktop::lua_runtime::ProtectedCall(state, 0, 1) != LUA_OK)
    {
        ClearModuleCache(state, cache, key);
        return lua_error(state);
    }
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        lua_pushboolean(state, 1);
    }
    lua_pushvalue(state, -1);
    lua_setfield(state, cache, key.c_str());
    ++context->moduleCount;
    context->moduleBytes += static_cast<std::size_t>(fileSize);
    lua_remove(state, cache);
    return 1;
}

void CopyGlobal(lua_State* state, int environment, const char* name)
{
    lua_getglobal(state, name);
    lua_setfield(state, environment, name);
}

void PushSafeEnvironment(lua_State* state)
{
    lua_createtable(state, 0, 24);
    const int environment = lua_absindex(state, -1);
    static constexpr const char* kBaseFunctions[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall",
        "rawequal", "rawget", "rawlen", "rawset", "select", "tonumber",
        "tostring", "type", "xpcall",
    };
    for (const char* name : kBaseFunctions)
        CopyGlobal(state, environment, name);
    for (const char* name : { "string", "table", "math", "utf8" })
        CopyGlobal(state, environment, name);
    lua_pushvalue(state, environment);
    lua_setfield(state, environment, "_G");
    lua_createtable(state, 0, 1);
    lua_pushcfunction(state, LuaTestModuleRequire);
    lua_setfield(state, -2, "require");
    lua_setfield(state, environment, "module");
    lua_pushvalue(state, environment);
    lua_setfield(state, LUA_REGISTRYINDEX, "__author_test_environment");
    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, "__author_test_modules");
}

void AddIssue(TestRunReport& report, std::string code,
    const std::filesystem::path& path, std::string message)
{
    report.issues.push_back(
        { std::move(code), path, std::move(message) });
}

void RunFile(TestRunReport& report,
    const std::filesystem::path& packageRoot,
    const std::filesystem::path& canonicalRoot,
    const std::filesystem::path& file)
{
    const auto relative = file.lexically_relative(packageRoot);
    std::error_code error;
    const auto fileSize = std::filesystem::file_size(file, error);
    if (error || fileSize > kMaximumTestBytes)
    {
        AddIssue(report, "test.file-size", relative,
            "test file is unreadable or exceeds 1 MiB");
        return;
    }
    std::ifstream input(file, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (!input || source.size() != fileSize)
    {
        AddIssue(report, "test.read", relative,
            "cannot read test file");
        return;
    }

    LuaRuntimeQuota quota;
    lua_State* rawState = lua_newstate(QuotaAllocator, &quota);
    if (!rawState)
    {
        AddIssue(report, "test.memory", relative,
            "cannot allocate the isolated Lua test state");
        return;
    }
    const std::unique_ptr<lua_State, decltype(&lua_close)> state(
        rawState, lua_close);
    luaL_requiref(rawState, "_G", luaopen_base, 1); lua_pop(rawState, 1);
    luaL_requiref(rawState, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(rawState, 1);
    luaL_requiref(rawState, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(rawState, 1);
    luaL_requiref(rawState, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(rawState, 1);
    luaL_requiref(rawState, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(rawState, 1);
    lua_pushlightuserdata(rawState, &quota);
    lua_setfield(rawState, LUA_REGISTRYINDEX, "__quota_ptr");
    TestFileContext context{ packageRoot, canonicalRoot };
    lua_pushlightuserdata(rawState, &context);
    lua_setfield(rawState, LUA_REGISTRYINDEX, "__author_test_context");
    PushSafeEnvironment(rawState);
    const int environment = lua_absindex(rawState, -1);

    const std::string chunkName = "@" + PathUtf8(relative);
    if (luaL_loadbuffer(rawState, source.data(), source.size(),
            chunkName.c_str()) != LUA_OK)
    {
        AddIssue(report, "test.syntax", relative,
            BoundedError(lua_tostring(rawState, -1)));
        return;
    }
    lua_pushvalue(rawState, environment);
    if (lua_setupvalue(rawState, -2, 1) == nullptr)
    {
        AddIssue(report, "test.environment", relative,
            "test chunk does not accept an isolated environment");
        return;
    }
    if (snowdesktop::lua_runtime::ProtectedCall(rawState, 0, 1,
            1000000, std::chrono::milliseconds(100)) != LUA_OK)
    {
        AddIssue(report, quota.executionExceeded ? "test.quota" :
            quota.memoryExceeded ? "test.memory" : "test.load",
            relative, BoundedError(lua_tostring(rawState, -1)));
        return;
    }
    if (!lua_istable(rawState, -1))
    {
        AddIssue(report, "test.contract", relative,
            "test file must return a table of named functions");
        return;
    }

    std::vector<std::string> names;
    lua_pushnil(rawState);
    while (lua_next(rawState, -2) != 0)
    {
        if (!lua_isstring(rawState, -2) || !lua_isfunction(rawState, -1))
        {
            lua_pop(rawState, 2);
            AddIssue(report, "test.contract", relative,
                "every test table entry must map a string name to a function");
            return;
        }
        names.emplace_back(lua_tostring(rawState, -2));
        lua_pop(rawState, 1);
    }
    if (names.empty())
    {
        AddIssue(report, "test.empty", relative,
            "test file did not declare any cases");
        return;
    }
    std::sort(names.begin(), names.end());
    const int cases = lua_absindex(rawState, -1);
    for (const auto& name : names)
    {
        lua_getfield(rawState, cases, name.c_str());
        const int status = snowdesktop::lua_runtime::ProtectedCall(
            rawState, 0, 1);
        if (status != LUA_OK)
        {
            report.cases.push_back({ relative, name, false,
                BoundedError(lua_tostring(rawState, -1)) });
            lua_pop(rawState, 1);
            continue;
        }
        const bool explicitFailure = lua_isboolean(rawState, -1) &&
            lua_toboolean(rawState, -1) == 0;
        report.cases.push_back({ relative, name, !explicitFailure,
            explicitFailure ? "test returned false" : std::string() });
        lua_pop(rawState, 1);
    }
}
}

bool TestRunReport::Ok() const noexcept
{
    return issues.empty() && FailedCount() == 0 && !cases.empty();
}

std::size_t TestRunReport::PassedCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        cases.begin(), cases.end(), [](const auto& result) {
            return result.passed;
        }));
}

std::size_t TestRunReport::FailedCount() const noexcept
{
    return cases.size() - PassedCount();
}

std::string TestRunReport::ToJson() const
{
    std::ostringstream output;
    output << "{\"ok\":" << (Ok() ? "true" : "false")
           << ",\"fileCount\":" << fileCount
           << ",\"caseCount\":" << cases.size()
           << ",\"passedCount\":" << PassedCount()
           << ",\"failedCount\":" << FailedCount()
           << ",\"issues\":[";
    for (std::size_t index = 0; index < issues.size(); ++index)
    {
        if (index) output << ',';
        output << "{\"code\":" << JsonEscape(issues[index].code)
               << ",\"path\":" << JsonEscape(PathUtf8(issues[index].path))
               << ",\"message\":" << JsonEscape(issues[index].message)
               << '}';
    }
    output << "],\"cases\":[";
    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        if (index) output << ',';
        output << "{\"path\":" << JsonEscape(PathUtf8(cases[index].path))
               << ",\"name\":" << JsonEscape(cases[index].name)
               << ",\"passed\":" << (cases[index].passed ? "true" : "false")
               << ",\"error\":" << JsonEscape(cases[index].error) << '}';
    }
    output << "]}";
    return output.str();
}

TestRunReport RunWidgetTests(const std::filesystem::path& packageRoot)
{
    TestRunReport report;
    std::error_code error;
    const auto root = std::filesystem::absolute(packageRoot, error);
    const auto canonicalRoot = error
        ? std::filesystem::path()
        : std::filesystem::weakly_canonical(root, error);
    if (error || canonicalRoot.empty())
    {
        AddIssue(report, "test.root", {},
            "cannot resolve the package root");
        return report;
    }
    const auto testsRoot = root / L"tests";
    if (!std::filesystem::is_directory(testsRoot, error) || error ||
            HasReparsePoint(testsRoot))
    {
        AddIssue(report, "test.missing", L"tests",
            "package does not contain a safe tests directory");
        return report;
    }
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator iterator(
            testsRoot, std::filesystem::directory_options::none, error), end;
            !error && iterator != end; iterator.increment(error))
    {
        if (iterator->is_symlink(error) || HasReparsePoint(iterator->path()))
        {
            AddIssue(report, "test.reparse",
                iterator->path().lexically_relative(root),
                "test paths cannot contain links or reparse points");
            if (iterator->is_directory(error))
                iterator.disable_recursion_pending();
            continue;
        }
        if (iterator->is_regular_file(error) &&
                iterator->path().extension() == L".lua")
            files.push_back(iterator->path());
    }
    if (error)
    {
        AddIssue(report, "test.enumerate", L"tests",
            "cannot enumerate every test file");
        return report;
    }
    std::sort(files.begin(), files.end());
    if (files.empty())
    {
        AddIssue(report, "test.missing", L"tests",
            "tests directory does not contain any Lua test files");
        return report;
    }
    report.fileCount = files.size();
    for (const auto& file : files)
        RunFile(report, root, canonicalRoot, file);
    return report;
}
}
