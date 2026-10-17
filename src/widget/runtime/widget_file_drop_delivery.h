#pragma once

#include "widget/permissions/widget_permission_broker.h"
#include "widget/permissions/widget_trusted_gesture.h"
#include "widget/tasks/widget_filesystem_drop.h"
#include "widget/view/widget_interaction_region.h"

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_runtime
{
struct WidgetFileDropDeliveryState
{
    bool active;
    bool preview;
    std::uint64_t runtimeToken;
    const std::unordered_set<std::string>& permissions;
    const WidgetInteractionRegions& regions;
};

// Shared final delivery transaction. WidgetEngine resolves the captured widget
// identity; validity, grants, trusted gesture and rollback remain together here.
template<class Target, class Invoke>
bool DeliverWidgetFileDrop(const Target& target, const WidgetFileDropDeliveryState& current,
    WidgetFilesystemHandleStore& store, const WidgetFilesystemHandleOwner& owner,
    WidgetTrustedGestureState& gesture, const std::vector<std::wstring>& paths, Invoke invoke)
{
    if (paths.empty() || paths.size() > 128 || !current.active || current.preview ||
        owner.packageId != target.packageId ||
        !snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(
            current.permissions, "filesystem.userSelected.read") ||
        !IsWidgetFileDropTargetCurrent(current.regions, target.targetKey,
            target.action, target.runtimeToken, current.runtimeToken)) return false;
    WidgetFilesystemDropGrant batch(store, owner);
    if (!batch.Acquire(paths)) return false;
    WidgetTrustedGestureScope gestureScope(gesture, true);
    const bool delivered = invoke(batch.items);
    if (delivered) batch.Commit();
    return delivered;
}

// The lifecycle dispatcher provides the event table and its kind. Keep the
// exact fileDrop payload shared; text encoding and generic value encoding use
// the host's existing serializers.
template<class Target, class PushValue, class Encode>
void PushWidgetFileDropFields(lua_State* state, const Target& target,
    const std::vector<WidgetFilesystemHandleEntry>& items, PushValue pushValue, Encode encode)
{
    lua_pushlstring(state, target.action.id.data(), target.action.id.size());
    lua_setfield(state, -2, "id");
    pushValue(state, target.action.value); lua_setfield(state, -2, "value");
    lua_pushliteral(state, "fileDrop"); lua_setfield(state, -2, "action");
    lua_pushliteral(state, "host.drop"); lua_setfield(state, -2, "source");
    lua_pushliteral(state, "desktop"); lua_setfield(state, -2, "surface");
    lua_pushlstring(state, target.targetKey.data(), target.targetKey.size());
    lua_setfield(state, -2, "targetKey");
    lua_pushboolean(state, 1); lua_setfield(state, -2, "trustedGesture");
    lua_createtable(state, static_cast<int>(items.size()), 0);
    int itemIndex = 1;
    for (const auto& item : items)
    {
        lua_createtable(state, 0, 4);
        lua_pushlstring(state, item.handle.data(), item.handle.size());
        lua_setfield(state, -2, "handle");
        const auto name = encode(item.path.filename().wstring());
        lua_pushlstring(state, name.data(), name.size()); lua_setfield(state, -2, "name");
        const auto kind = WidgetFilesystemHandleStore::KindName(item.kind);
        lua_pushlstring(state, kind.data(), kind.size()); lua_setfield(state, -2, "kind");
        lua_pushliteral(state, "read"); lua_setfield(state, -2, "access");
        lua_rawseti(state, -2, itemIndex++);
    }
    lua_setfield(state, -2, "items");
}
}
