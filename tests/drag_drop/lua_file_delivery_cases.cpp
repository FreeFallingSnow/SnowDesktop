#include "widget/runtime/widget_file_drop_delivery.h"
#include "widget/runtime/widget_lua_lifecycle.h"

extern "C" {
#include <lualib.h>
}

#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace
{
using namespace snowdesktop::widget_runtime;

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

std::string Encode(const std::wstring& text)
{
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string value(static_cast<size_t>(count), '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), count, nullptr, nullptr);
    return value;
}

std::map<std::filesystem::path, std::string> Tree(const std::filesystem::path& root)
{
    std::map<std::filesystem::path, std::string> result;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root))
    {
        std::ifstream input(item.path(), std::ios::binary);
        result[item.path().lexically_relative(root)] = item.is_directory() ? "<directory>" :
            std::string(std::istreambuf_iterator<char>(input), {});
    }
    return result;
}

void Context(lua_State* state) { lua_newtable(state); }

class Callback
{
public:
    lua_State* state = luaL_newstate();
    WidgetLuaLifecycle lifecycle;
    int definition = LUA_NOREF;
    int calls = 0;
    std::vector<std::string> handles;

    Callback()
    {
        Require(state != nullptr, "create isolated Lua file-drop callback");
        luaL_openlibs(state);
        constexpr const char* program = R"lua(
            return { event = function(ctx, model, e)
                assert(e.kind == "action" and e.action == "fileDrop")
                assert(e.id == "import" and e.value == true and e.targetKey == "album")
                assert(e.source == "host.drop" and e.surface == "desktop" and e.trustedGesture == true)
                for _, item in ipairs(e.items) do
                    assert(type(item.handle) == "string" and item.access == "read")
                    assert(item.kind == "file" or item.kind == "folder")
                    assert(item.path == nil and item.target == nil)
                end
                if failDelivery then error("controlled callback failure") end
            end }
        )lua";
        Require(luaL_dostring(state, program) == LUA_OK, "load real Lua callback definition");
        definition = luaL_ref(state, LUA_REGISTRYINDEX);
        std::string error;
        Require(lifecycle.Setup(state, definition, Context, error), "set up real Lua lifecycle");
    }
    ~Callback()
    {
        lifecycle.Release(state);
        luaL_unref(state, LUA_REGISTRYINDEX, definition);
        lua_close(state);
    }

    template<class Target>
    bool Invoke(const Target& target, const std::vector<WidgetFilesystemHandleEntry>& items,
        const std::vector<std::filesystem::path>& expected, bool fail, WidgetTrustedGestureState& gesture)
    {
        ++calls;
        Require(gesture.Active(), "Lua file callback runs inside the real trusted-gesture scope");
        Require(items.size() == expected.size(), "callback receives one read reference per distinct captured source");
        for (size_t index = 0; index < items.size(); ++index)
        {
            Require(items[index].path == expected[index] && items[index].access == WidgetFilesystemHandleAccess::Read,
                "real grants preserve selected order and point to the original read-only sources");
            handles.push_back(items[index].handle);
        }
        lua_pushboolean(state, fail); lua_setglobal(state, "failDelivery");
        const int top = lua_gettop(state);
        lua_createtable(state, 0, 10);
        lua_pushliteral(state, "action"); lua_setfield(state, -2, "kind");
        PushWidgetFileDropFields(state, target, items,
            [](lua_State* inner, const InteractionValue& value) { lua_pushboolean(inner, value.boolean); }, Encode);
        lua_getfield(state, -1, "items");
        Require(lua_rawlen(state, -1) == expected.size(), "shared event payload keeps the exact distinct source count");
        for (size_t index = 0; index < expected.size(); ++index)
        {
            lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
            lua_getfield(state, -1, "name");
            Require(std::string(lua_tostring(state, -1)) == Encode(expected[index].filename().wstring()),
                "Lua receives the selected basename rather than the full filesystem path");
            lua_pop(state, 1);
            lua_getfield(state, -1, "kind");
            Require(std::string(lua_tostring(state, -1)) == (std::filesystem::is_directory(expected[index]) ? "folder" : "file"),
                "Lua payload distinguishes the original file and folder grants");
            lua_pop(state, 1);
            lua_getfield(state, -1, "handle");
            Require(std::string(lua_tostring(state, -1)) == items[index].handle &&
                WidgetFilesystemHandleStore::IsOpaqueHandle(items[index].handle),
                "shared Lua payload contains the real opaque store handle");
            lua_pop(state, 2);
        }
        lua_pop(state, 1);
        bool invoked = false;
        std::string error;
        const bool delivered = lifecycle.Event(state, definition, Context, -1, invoked, error);
        lua_settop(state, top);
        Require(invoked && (delivered ? error.empty() : error.find("controlled callback failure") != std::string::npos),
            "delivery result comes from the actual Lua lifecycle callback");
        return delivered;
    }
};
}

// B14 / public Desktop file and folder drops contract: final delivery must
// recheck permission/runtime/action, preserve originals and revoke only grants
// created by a rejected batch. Engine lookup and its generic value serializer
// remain outside this boundary; transaction, payload, Lua and store are real.
void RunLuaFileDeliveryMatrix(const std::filesystem::path& root)
{
    using namespace snowdesktop::widget_runtime;
    const auto source = root / L"sources";
    std::filesystem::create_directories(source / L"folder");
    const auto file = source / L"a.txt", folder = source / L"folder";
    std::ofstream(file) << "alpha"; std::ofstream(folder / L"b.txt") << "beta";
    std::vector<std::filesystem::path> boundary;
    for (size_t index = 0; index < 128; ++index)
    {
        boundary.push_back(source / (L"limit-" + std::to_wstring(index) + L".txt"));
        std::ofstream(boundary.back()) << "limit-" << index;
    }
    const auto original = Tree(source);
    struct Target { std::string packageId, targetKey; std::uint64_t runtimeToken; InteractionAction action; };
    Target captured{"package-a", "album", 7, {}};
    captured.action.id = "import";
    captured.action.value.type = InteractionValue::Type::Boolean;
    captured.action.value.boolean = true;
    const WidgetFilesystemHandleOwner owner{"instance-a", "package-a"}, stranger{"instance-b", "package-a"};
    for (int scenario = 0; scenario < 18; ++scenario)
    {
        WidgetInteractionRegions regions;
        InteractionRegion album;
        album.key = "album"; album.shape.width = 100; album.shape.height = 100;
        album.events["fileDrop"] = captured.action;
        std::unordered_set<std::string> permissions{"filesystem.userSelected.read"};
        bool active = true, preview = false, fail = scenario == 4;
        std::uint64_t runtimeToken = 7;
        auto actualOwner = owner;
        std::vector<std::filesystem::path> expected{file, folder};
        std::vector<std::wstring> paths{file.wstring(), folder.wstring()};
        if (scenario == 0) { paths = {file.wstring()}; expected = {file}; }
        if (scenario == 1) { paths = {folder.wstring()}; expected = {folder}; }
        if (scenario == 3) paths.push_back(file.wstring());
        if (scenario == 5) paths.push_back((source / L"missing").wstring());
        if (scenario == 6) permissions.clear();
        if (scenario == 7) runtimeToken = 8;
        if (scenario == 8) actualOwner.packageId = "replacement-package";
        if (scenario == 9) album.key = "replacement-target";
        if (scenario == 10) album.enabled = false;
        if (scenario == 11) album.events["fileDrop"].id = "replacement-action";
        if (scenario == 12) album.events["fileDrop"].value.boolean = false;
        if (scenario == 13) preview = true;
        if (scenario == 14) active = false;
        if (scenario == 15) { paths.clear(); expected.clear(); }
        if (scenario >= 16)
        {
            paths.clear(); expected = boundary;
            for (const auto& path : boundary) paths.push_back(path.wstring());
            if (scenario == 17) paths.push_back(file.wstring());
        }
        album.shape.x = 200; // Movement does not change the captured target.
        regions.BeginFrame();
        std::string error;
        Require(regions.Submit(album, error), "stage actual current file-drop region");
        regions.CommitFrame();
        const auto registry = root / (L"handles-" + std::to_wstring(scenario) + L".json");
        WidgetFilesystemHandleStore store(registry);
        const auto prior = store.Grant(owner, file, WidgetFilesystemHandleKind::File, WidgetFilesystemHandleAccess::Read);
        Require(static_cast<bool>(prior), "seed a real existing grant which failed drops must retain");
        Callback callback;
        WidgetTrustedGestureState gesture;
        const bool success = scenario <= 3 || scenario == 16;
        const bool dispatched = success || scenario == 4;
        const WidgetFileDropDeliveryState current{active, preview, runtimeToken, permissions, regions};
        const bool delivered = DeliverWidgetFileDrop(captured, current, store, actualOwner, gesture, paths,
            [&](const auto& items) { return callback.Invoke(captured, items, expected, fail, gesture); });
        Require(delivered == success && callback.calls == (dispatched ? 1 : 0),
            "Lua final delivery rechecks permission, package, runtime, target, action and selection limits before granting or invoking");
        Require(!gesture.Active() && Tree(source) == original,
            "Lua reference delivery and rejection leave original files, directories and gesture state unchanged");
        Require(store.Resolve(owner, prior.entry->handle).has_value() &&
            store.Size() == (success ? (scenario == 1 ? 2 : expected.size() + (scenario == 16 ? 1 : 0)) : 1),
            "failed grant or Lua callback revokes only newly created handles and retains every preexisting grant");
        WidgetFilesystemHandleStore reloaded(registry);
        Require(reloaded.Load(error) && reloaded.Size() == store.Size(),
            "delivered or rolled-back grants retain their real persisted ownership after reload");
        if (success)
            for (const auto& handle : callback.handles)
                Require(reloaded.Resolve(owner, handle).has_value() && !reloaded.Resolve(stranger, handle),
                    "successful callback retains usable references scoped to the original component owner");
    }
    std::cout << "Lua final file delivery, permission, stable target, callback failure and grant storage cases passed\n";
}
