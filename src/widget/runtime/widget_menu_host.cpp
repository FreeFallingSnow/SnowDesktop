#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_surface_host.h"
#include "widget/view/widget_host_input_support.h"
#include "widget/preview/widget_preview_context.h"
#include "common/utils.h"
#include "common/l10n.h"
#include "widget/runtime/widget_menu_catalogue.h"
#include "widget/packages/widget_package_host.h"
#include "widget/data/widget_resource_lua.h"
#include "widget/api/widget_api_registry.h"
#include "widget/view/widget_interaction_lua_value.h"

#include <algorithm>
#include <utility>

using snowdesktop::widget_runtime::TestResourceHandle;
using snowdesktop::widget_runtime::LuaResourceType;
using snowdesktop::widget_runtime::PushInteractionValue;
using snowdesktop::widget_runtime::GetWidgetPackageManager;

static bool PushProtectedMenuField(lua_State* state, int tableIndex,
    const char* field, std::chrono::steady_clock::time_point deadline,
    std::string& error)
{
    // Menu descriptors are user-owned tables. Preserve __index/string
    // conversion semantics, but keep their errors and execution budget inside
    // Lua while native item/vector owners remain on the calling C++ stack.
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
    {
        error = "Widget menu descriptor execution quota exceeded";
        return false;
    }
    struct FieldArguments
    {
        const char* name;
        std::chrono::milliseconds remaining;
    } arguments{field, remaining};
    return snowdesktop::lua_runtime::ProtectedHostCall(state,
        +[](lua_State* inner) {
            const auto* input = static_cast<FieldArguments*>(lua_touserdata(inner, 1));
            lua_pushcfunction(inner, +[](lua_State* fieldState) {
                const auto* fieldInput = static_cast<FieldArguments*>(lua_touserdata(fieldState, 2));
                lua_getfield(fieldState, 1, fieldInput->name);
                if (lua_type(fieldState, -1) == LUA_TNUMBER)
                    (void)lua_tolstring(fieldState, -1, nullptr);
                return 1;
            });
            lua_pushvalue(inner, 2);
            lua_pushvalue(inner, 1);
            if (snowdesktop::lua_runtime::ProtectedCall(inner, 2, 1,
                    snowdesktop::lua_runtime::kDefaultInstructionBudget, input->remaining) != LUA_OK)
                return lua_error(inner);
            return 1;
        }, &arguments, 1, error, tableIndex);
}

std::vector<LuaWidgetMenuItem> WidgetEngine::GetContextMenu(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface, bool componentScopeOnly,
    const LuaWidgetMenuItem* refreshTarget)
{
    std::vector<LuaWidgetMenuItem> result;
    int idx = FindWidget(widgetId);
    if (idx < 0) return result;
    auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return result;
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);

        auto& interactionRegions =
            InteractionRegionsForSurface(w, surface);
        std::string targetKey;
        const snowdesktop::widget_runtime::InteractionAction* requestActionPointer;
        if (refreshTarget)
        {
            if (w.runtimeToken != refreshTarget->runtimeToken) return result;
            targetKey = refreshTarget->targetKey;
            const auto* region = interactionRegions.Find(targetKey);
            if (!region || !region->enabled) return result;
            requestActionPointer = interactionRegions.FindAction(targetKey, "contextMenu");
            if (!requestActionPointer || requestActionPointer->id != refreshTarget->bindingId ||
                requestActionPointer->value != refreshTarget->contextValue ||
                (requestActionPointer->contextMenuScope ==
                    snowdesktop::widget_runtime::InteractionAction::ContextMenuScope::Element) !=
                    refreshTarget->elementContext)
                return result;
        }
        else
        {
            requestActionPointer = interactionRegions.ContextMenuActionAt(
                static_cast<float>(x), static_cast<float>(y),
                componentScopeOnly, &targetKey);
        }
        if (!requestActionPointer || targetKey.empty()) return result;
        const auto requestAction = *requestActionPointer;
        WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
        WidgetSurfaceScope surfaceScope(d2dState_,
            normalizedSurface.c_str());
        snowdesktop::lua_runtime::StackGuard stackGuard(state);
        SetWidgetRectContext(d2dState_, BoundsForSurface(w, surface));
        bool invoked = false;
        std::string error;
        struct MenuRequestArguments
        {
            const snowdesktop::widget_runtime::InteractionAction* action;
            const std::string* targetKey;
            std::string_view surface;
        } arguments{&requestAction, &targetKey, surface};
        if (!snowdesktop::lua_runtime::ProtectedHostCall(state,
                +[](lua_State* inner) {
                    const auto& input = *static_cast<MenuRequestArguments*>(lua_touserdata(inner, 1));
                    lua_createtable(inner, 0, 6);
                    lua_pushlstring(inner, input.action->id.data(), input.action->id.size());
                    lua_setfield(inner, -2, "id");
                    PushInteractionValue(inner, input.action->value);
                    lua_setfield(inner, -2, "value");
                    lua_pushlstring(inner, input.targetKey->data(), input.targetKey->size());
                    lua_setfield(inner, -2, "targetKey");
                    lua_pushlstring(inner, input.surface.data(), input.surface.size());
                    lua_setfield(inner, -2, "surface");
                    lua_pushliteral(inner, "pointer");
                    lua_setfield(inner, -2, "source");
                    lua_pushstring(inner, input.action->contextMenuScope ==
                        snowdesktop::widget_runtime::InteractionAction::ContextMenuScope::Component
                            ? "component" : "element");
                    lua_setfield(inner, -2, "scope");
                    return 1;
                }, &arguments, 1, error))
        {
            RuntimeRecordError(widgetId, error);
            return result;
        }
        const auto pushContext = +[](lua_State* lifecycleState) {
            (void)lua_WidgetContext(lifecycleState);
        };
        if (!w.lifecycle.Menu(state, w.ref, pushContext, -1,
                invoked, error))
        {
            if (!error.empty()) RuntimeRecordError(widgetId, error);
            return result;
        }
        if (!invoked) return result;
        if (!lua_istable(state, -1))
        {
            if (!lua_isnil(state, -1))
                RuntimeRecordError(widgetId,
                    "Widget menu callback must return ui.menu(...) or nil");
            return result;
        }

        constexpr std::size_t kMaxMenuDescriptors = 64;
        constexpr int kMaxSubmenuDepth = 3;
        std::unordered_set<std::string> ids;
        std::size_t descriptorCount = 0;
        std::string menuError;
        const auto descriptorDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        const auto pushField = [&](int index, const char* field) {
            return PushProtectedMenuField(state, index, field, descriptorDeadline, menuError);
        };
        std::function<bool(int, int, std::vector<LuaWidgetMenuItem>&)>
            parseMenuItems;
        parseMenuItems = [&](int tableIndex, int depth,
                             std::vector<LuaWidgetMenuItem>& output) {
            tableIndex = lua_absindex(state, tableIndex);
            const std::size_t count = lua_rawlen(state, tableIndex);
            if (count > kMaxMenuDescriptors - descriptorCount)
            {
                menuError = "Widget menu exceeds the 64 descriptor limit";
                return false;
            }
            for (std::size_t itemIndex = 1; itemIndex <= count; ++itemIndex)
            {
                lua_rawgeti(state, tableIndex,
                    static_cast<lua_Integer>(itemIndex));
                if (!lua_istable(state, -1))
                {
                    menuError = "Widget menu items must be descriptor tables";
                    return false;
                }
                ++descriptorCount;

                LuaWidgetMenuItem item;
                if (!pushField(-1, "type")) return false;
                item.separator = lua_isstring(state, -1) &&
                    std::strcmp(lua_tostring(state, -1), "separator") == 0;
                lua_pop(state, 1);
                if (item.separator)
                {
                    output.push_back(std::move(item));
                    lua_pop(state, 1);
                    continue;
                }

                if (!pushField(-1, "id")) return false;
                if (lua_isstring(state, -1))
                    item.actionId = lua_tostring(state, -1);
                lua_pop(state, 1);
                if (!pushField(-1, "label")) return false;
                if (lua_isstring(state, -1))
                    item.label = lua_tostring(state, -1);
                lua_pop(state, 1);
                if (!pushField(-1, "icon")) return false;
                if (lua_isstring(state, -1))
                    item.icon = lua_tostring(state, -1);
                lua_pop(state, 1);
                if (!pushField(-1, "iconFont")) return false;
                if (lua_isstring(state, -1))
                    item.iconFont = lua_tostring(state, -1);
                lua_pop(state, 1);
                if (!pushField(-1, "image")) return false;
                if (!lua_isnil(state, -1))
                {
                    if (!snowdesktop::lua_runtime::ProtectedHostCall(state,
                            +[](lua_State* inner) {
                                auto* resource = TestResourceHandle(inner, 2);
                                lua_pushlightuserdata(inner, resource);
                                return 1;
                            }, nullptr, 1, menuError, -1)) return false;
                    const auto* handle = static_cast<const snowdesktop::widget_runtime::LuaResourceHandle*>(
                        lua_touserdata(state, -1));
                    lua_pop(state, 1);
                    if (!handle || handle->type != LuaResourceType::Image ||
                        snowdesktop::widget_runtime::
                            IsWidgetRuntimeImageToken(handle->name))
                    {
                        menuError =
                            "Widget menu image must be a package resource.image handle";
                        return false;
                    }
                    item.imageResourceName = handle->name;
                }
                lua_pop(state, 1);
                if (!item.icon.empty() && !item.imageResourceName.empty())
                {
                    menuError =
                        "Widget menu items cannot combine icon and image";
                    return false;
                }
                if (!pushField(-1, "enabled")) return false;
                item.enabled = lua_isnil(state, -1) ||
                    lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                if (!pushField(-1, "checked")) return false;
                item.checked = lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                if (!pushField(-1, "keepOpen")) return false;
                item.keepOpen = lua_isboolean(state, -1) && lua_toboolean(state, -1);
                lua_pop(state, 1);
                if (item.label.empty() || item.label.size() > 512)
                {
                    menuError =
                        "Widget menu item labels must contain 1..512 UTF-8 bytes";
                    return false;
                }

                if (!pushField(-1, "children")) return false;
                const bool hasChildren = lua_istable(state, -1);
                if (!hasChildren && !lua_isnil(state, -1))
                {
                    menuError = "Widget menu children must be an array table";
                    return false;
                }
                if (hasChildren)
                {
                    if (!item.actionId.empty() || item.keepOpen)
                    {
                        menuError =
                            "Widget submenu parents cannot declare id or keepOpen";
                        return false;
                    }
                    if (depth >= kMaxSubmenuDepth)
                    {
                        menuError = "Widget menu exceeds three submenu levels";
                        return false;
                    }
                    if (lua_rawlen(state, -1) == 0)
                    {
                        menuError = "Widget submenu children cannot be empty";
                        return false;
                    }
                    if (!parseMenuItems(-1, depth + 1, item.children))
                        return false;
                    lua_pop(state, 1);
                }
                else
                {
                    lua_pop(state, 1);
                    if (item.actionId.empty() ||
                        item.actionId.size() > 128 ||
                        !ids.insert(item.actionId).second)
                    {
                        menuError =
                            "Widget menu leaf IDs must be unique and contain 1..128 UTF-8 bytes";
                        return false;
                    }
                    item.elementContext =
                        requestAction.contextMenuScope ==
                        snowdesktop::widget_runtime::InteractionAction::
                            ContextMenuScope::Element;
                    item.targetKey = targetKey;
                    item.bindingId = requestAction.id;
                    item.surface = normalizedSurface;
                    item.contextValue = requestAction.value;
                    item.runtimeToken = w.runtimeToken;
                }
                output.push_back(std::move(item));
                lua_pop(state, 1);
            }
            return true;
        };
        if (!parseMenuItems(-1, 0, result))
        {
            result.clear();
            RuntimeRecordError(widgetId, menuError);
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
    return result;
}

bool WidgetEngine::InvokeMenu(const std::wstring& widgetId,
    const LuaWidgetMenuItem& menuItem)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    auto& widget = widgets_[index];
    auto& interactionRegions = InteractionRegionsForSurface(
        widget, menuItem.surface);
    if (menuItem.actionId.empty() ||
        !snowdesktop::widget_runtime::IsWidgetMenuSelectionCurrent(
            interactionRegions, menuItem.targetKey,
            menuItem.runtimeToken, widget.runtimeToken))
        return false;
    if (menuItem.keepOpen)
    {
        const auto* region = interactionRegions.Find(menuItem.targetKey);
        const auto* action = interactionRegions.FindAction(menuItem.targetKey, "contextMenu");
        if (!region || !region->enabled || !action || action->id != menuItem.bindingId ||
            action->value != menuItem.contextValue ||
            (action->contextMenuScope ==
                snowdesktop::widget_runtime::InteractionAction::ContextMenuScope::Element) !=
                menuItem.elementContext)
            return false;
    }
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, true);
    WidgetSurfaceScope surfaceScope(d2dState_, menuItem.surface.c_str());
    return InvokeLifecycleEvent(widget, "action",
        [&menuItem](lua_State* eventState) {
            lua_pushlstring(eventState, menuItem.actionId.data(),
                menuItem.actionId.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, menuItem.contextValue);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, menuItem.targetKey.data(),
                menuItem.targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushliteral(eventState, "contextMenu");
            lua_setfield(eventState, -2, "source");
            lua_pushlstring(eventState, menuItem.surface.data(),
                menuItem.surface.size());
            lua_setfield(eventState, -2, "surface");
            lua_pushboolean(eventState, 1);
            lua_setfield(eventState, -2, "trustedGesture");
        });
}

// Menu metadata cache is separate from runtime manifest/permission loading.
std::vector<snowdesktop::widget_menu::Entry> WidgetEngine::ListAvailableMenuEntries()
{
    static thread_local snowdesktop::widget_menu::Catalogue catalogue;
    std::string refreshError;
    if (!RefreshWidgetPackages(refreshError))
        OutputDebugStringW((L"Widget catalog refresh failed: " +
            Utf8ToWide(refreshError) + L"\n").c_str());
    const auto packages = GetWidgetPackageManager().ListPackages();
    return catalogue.Build(packages, Locale::Instance().GetEffectiveLanguage(),
        [](const snowdesktop::widget::InstalledPackage& package) {
            const auto manifest = GetWidgetManifest((package.root /
                Utf8ToWide(package.manifest.entry)).wstring());
            return snowdesktop::widget_menu::Metadata{
                Utf8ToWide(package.manifest.id),
                Utf8ToWide(manifest.name),
                Utf8ToWide(manifest.description),
                Utf8ToWide(manifest.publisher),
                snowdesktop::widget::MeetsWidgetMinimumHostVersion(
                    SNOWDESKTOP_VERSION, manifest.minHostVersion),
                manifest.hasManifest};
        });
}
