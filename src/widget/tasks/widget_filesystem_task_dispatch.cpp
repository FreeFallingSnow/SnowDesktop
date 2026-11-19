#include "widget_filesystem_task_dispatch.h"

#include "common/utils.h"
#include "widget/data/widget_resource_lua.h"
#include "widget_task_broker.h"

#include <algorithm>
#include <utility>

namespace snowdesktop::widget_runtime
{
bool IsPreviewFilesystemHandle(std::string_view handle)
{
    return handle == "filesystem:00000000000000000000000000000000" ||
        handle == "filesystem:11111111111111111111111111111111";
}

namespace
{
void RevokeNewGrants(WidgetFilesystemHandleStore& store,
    const WidgetFilesystemHandleOwner& owner, const std::vector<std::string>& handles)
{
    for (const auto& handle : handles)
    {
        std::string error;
        (void)store.Revoke(owner, handle, error);
    }
}

std::string ValidateSuccessfulMessage(WidgetFilesystemTaskCompletion& message,
    const WidgetTaskBroker& broker, WidgetFilesystemHandleStore* store,
    const WidgetFilesystemTaskDispatch::ResolveOwner& resolveOwner)
{
    const auto snapshot = broker.Snapshot(message.id);
    if (!snapshot || snapshot->cancelRequested) return "canceled";
    const auto owner = resolveOwner(snapshot->ownerToken);
    if (!owner || !store) return "instanceDisposed";

    const WidgetFilesystemHandleOwner scope{snapshot->instanceId, owner->packageId};
    const auto source = store->Resolve(scope, message.metadata.handle);
    if (!source) return "invalidReference";
    if (message.action == "filesystem.image" && !owner->canRead)
        return "permissionRevoked";
    if (message.action != "filesystem.list" || !message.grantHandles)
        return {};

    std::vector<std::string> created;
    for (auto& item : message.items)
    {
        auto grant = store->Grant(scope, item.path, item.kind, source->access);
        if (!grant)
        {
            RevokeNewGrants(*store, scope, created);
            return grant.error.empty() ? "handleGrantFailed" : std::move(grant.error);
        }
        item.handle = grant.entry->handle;
        if (grant.created) created.push_back(item.handle);
    }
    return {};
}

void MarkResultUnavailable(TaskCompletion& completion)
{
    completion.ok = false;
    completion.error = "taskResultUnavailable";
}

void SetString(lua_State* state, const char* key, std::string_view value)
{
    lua_pushlstring(state, value.data(), value.size());
    lua_setfield(state, -2, key);
}

void SetInteger(lua_State* state, const char* key, lua_Integer value)
{
    lua_pushinteger(state, value);
    lua_setfield(state, -2, key);
}

void PushSelection(lua_State* state, const WidgetFilesystemHandleEntry& entry,
    const std::string& name)
{
    lua_createtable(state, 0, 4);
    SetString(state, "handle", entry.handle);
    SetString(state, "kind", WidgetFilesystemHandleStore::KindName(entry.kind));
    SetString(state, "access", WidgetFilesystemHandleStore::AccessName(entry.access));
    SetString(state, "name", name);
}

void PushMetadata(lua_State* state, const WidgetFilesystemMetadata& metadata)
{
    lua_createtable(state, 0, 7);
    if (!metadata.handle.empty()) SetString(state, "handle", metadata.handle);
    SetString(state, "kind", WidgetFilesystemHandleStore::KindName(metadata.kind));
    SetString(state, "name", metadata.name);
    if (metadata.kind == WidgetFilesystemHandleKind::File)
        SetInteger(state, "size", static_cast<lua_Integer>(metadata.size));
    SetInteger(state, "modifiedMs", static_cast<lua_Integer>(metadata.modifiedMs));
    lua_pushboolean(state, metadata.readOnly);
    lua_setfield(state, -2, "readOnly");
    SetString(state, "revision", metadata.revision);
}
} // namespace

bool FilesystemTaskPayload::HasValue() const noexcept
{
    return result_.has_value() || !selections_.empty();
}

void FilesystemTaskPayload::PrepareValue(TaskCompletion& completion,
    const ImageRegistrar& registerImage)
{
    if (!completion.ok) return;
    if (result_ && completion.name == "filesystem.image")
    {
        imageToken_ = registerImage(result_->resourceToken, result_->image);
        if (imageToken_.empty()) MarkResultUnavailable(completion);
    }
    selectionNames_.reserve(selections_.size());
    for (const auto& selection : selections_)
        selectionNames_.push_back(WideToUtf8(selection.path.filename().wstring()));
}

void FilesystemTaskPayload::PushValue(lua_State* state, std::string_view taskName) const
{
    if (!selections_.empty())
    {
        // Keep the original first-selection fields as well as the items list.
        PushSelection(state, selections_.front(), selectionNames_.front());
        lua_createtable(state, static_cast<int>(selections_.size()), 0);
        for (std::size_t index = 0; index < selections_.size(); ++index)
        {
            PushSelection(state, selections_[index], selectionNames_[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        lua_setfield(state, -2, "items");
        return;
    }

    const auto& result = *result_;
    const auto& metadata = result.metadata;
    if (taskName == "filesystem.stat")
        PushMetadata(state, metadata);
    else if (taskName == "filesystem.list")
    {
        lua_createtable(state, 0, 3);
        lua_createtable(state, static_cast<int>(result.items.size()), 0);
        int index = 1;
        for (const auto& item : result.items)
        {
            PushMetadata(state, item);
            lua_rawseti(state, -2, index++);
        }
        lua_setfield(state, -2, "items");
        SetInteger(state, "nextOffset", static_cast<lua_Integer>(result.nextOffset));
        lua_pushboolean(state, result.hasMore);
        lua_setfield(state, -2, "hasMore");
    }
    else if (taskName == "filesystem.image")
    {
        PushMetadata(state, metadata);
        PushResourceHandle(state, LuaResourceType::Image, imageToken_);
        lua_setfield(state, -2, "image");
        SetInteger(state, "width", result.image->width);
        SetInteger(state, "height", result.image->height);
    }
    else if (taskName == "filesystem.read")
    {
        lua_createtable(state, 0, 4);
        SetString(state, "encoding", result.encoding);
        SetString(state, result.encoding == "binary" ? "data" : "text", result.text);
        SetInteger(state, "size", static_cast<lua_Integer>(metadata.size));
        SetString(state, "revision", metadata.revision);
    }
    else
    {
        lua_createtable(state, 0, 4);
        lua_pushboolean(state, 1);
        lua_setfield(state, -2, "accepted");
        SetInteger(state, "size", static_cast<lua_Integer>(metadata.size));
        SetInteger(state, "modifiedMs", static_cast<lua_Integer>(metadata.modifiedMs));
        SetString(state, "revision", metadata.revision);
    }
}

void WidgetFilesystemTaskDispatch::Consume(
    std::vector<WidgetFilesystemTaskCompletion> messages, WidgetTaskBroker& broker,
    WidgetFilesystemHandleStore* store, const ResolveOwner& resolveOwner)
{
    for (auto& message : messages)
    {
        const auto id = message.id;
        if (message.ok)
        {
            std::string error = ValidateSuccessfulMessage(message, broker, store, resolveOwner);
            if (!error.empty())
            {
                message.ok = false;
                message.error = std::move(error);
            }
        }
        const bool ok = message.ok;
        std::string error = message.error;
        if (ok)
            results_.insert_or_assign(id, std::move(message));
        else
            results_.erase(id);
        activeHandles_.erase(id);
        (void)broker.Complete(id, ok, std::move(error));
    }
}

void WidgetFilesystemTaskDispatch::StorePreviewResult(
    std::uint64_t id, const WidgetFilesystemTaskRequest& request)
{
    const auto& action = request.action;
    WidgetFilesystemTaskCompletion result;
    result.id = id;
    result.action = action;
    result.ok = true;
    result.metadata.handle = request.handle;
    const bool list = action == "filesystem.list";
    result.metadata.name = list ? "Preview Folder" : "Preview.txt";
    result.metadata.kind = list ? WidgetFilesystemHandleKind::Folder : WidgetFilesystemHandleKind::File;
    result.metadata.size = request.text.size();
    result.metadata.modifiedMs = 1786752000000LL;
    result.metadata.revision = "r1-0000000000000001-0000000000000001-f";
    if (action == "filesystem.image")
    {
        auto pixels = std::make_shared<WidgetRuntimeImagePixels>();
        pixels->width = pixels->height = 1;
        pixels->stride = 4;
        pixels->bgraPremultiplied = {160, 120, 80, 255};
        result.resourceToken = MakeWidgetRuntimeImageToken("filesystem", *pixels);
        result.image = std::move(pixels);
    }
    else if (action == "filesystem.read")
    {
        result.encoding = request.encoding;
        result.text = request.encoding == "binary"
            ? std::string("\x00\x01\x7f\xff", 4) : "Preview file content";
        result.metadata.size = result.text.size();
    }
    else if (list)
    {
        WidgetFilesystemMetadata item;
        if (request.grantHandles)
            item.handle = "filesystem:11111111111111111111111111111111";
        item.name = "Preview.txt";
        item.kind = WidgetFilesystemHandleKind::File;
        item.size = 20;
        item.modifiedMs = 1786752000000LL;
        item.revision = "r1-0000000000000001-0000000000000001-f";
        result.items.push_back(std::move(item));
        result.nextOffset = 1;
    }
    results_.insert_or_assign(id, std::move(result));
}

void WidgetFilesystemTaskDispatch::StorePreviewSelection(std::uint64_t id,
    WidgetFilesystemHandleOwner owner, WidgetFilesystemHandleKind kind,
    WidgetFilesystemHandleAccess access)
{
    WidgetFilesystemHandleEntry entry;
    entry.handle = "filesystem:00000000000000000000000000000000";
    entry.owner = std::move(owner);
    entry.path = kind == WidgetFilesystemHandleKind::Folder ? L"Preview Folder" : L"Preview.txt";
    entry.kind = kind;
    entry.access = access;
    selections_.insert_or_assign(id, std::vector<WidgetFilesystemHandleEntry>{std::move(entry)});
}

std::string WidgetFilesystemTaskDispatch::GrantSelection(std::uint64_t id,
    WidgetFilesystemHandleStore& store, const WidgetFilesystemHandleOwner& owner,
    const std::vector<std::filesystem::path>& paths,
    WidgetFilesystemHandleKind kind, WidgetFilesystemHandleAccess access)
{
    std::vector<WidgetFilesystemHandleEntry> entries;
    std::vector<std::string> created;
    for (const auto& path : paths)
    {
        auto grant = store.Grant(owner, path, kind, access);
        if (!grant)
        {
            RevokeNewGrants(store, owner, created);
            return grant.error.empty() ? "handleGrantFailed" : std::move(grant.error);
        }
        if (grant.created) created.push_back(grant.entry->handle);
        entries.push_back(std::move(*grant.entry));
    }
    selections_.insert_or_assign(id, std::move(entries));
    return {};
}

FilesystemTaskPayload WidgetFilesystemTaskDispatch::TakeCompletion(TaskCompletion& completion)
{
    // Keep the payload alive independently of its cache during the Lua callback.
    auto selection = selections_.extract(completion.id);
    auto result = results_.extract(completion.id);
    FilesystemTaskPayload payload;
    if (!completion.ok) return payload;
    if (IsFilesystemPickerTask(completion.name))
    {
        if (selection && !selection.mapped().empty())
            payload.selections_ = std::move(selection.mapped());
        else
            MarkResultUnavailable(completion);
    }
    else if (WidgetFilesystemTaskExecutor::SupportsAction(completion.name))
    {
        if (result)
            payload.result_.emplace(std::move(result.mapped()));
        else
            MarkResultUnavailable(completion);
    }
    return payload;
}

void WidgetFilesystemTaskDispatch::DiscardCompletion(std::uint64_t id)
{
    selections_.erase(id);
    results_.erase(id);
}

void WidgetFilesystemTaskDispatch::TrackHandle(std::uint64_t id, const std::string& handle)
{
    activeHandles_.insert_or_assign(id, handle);
}

bool WidgetFilesystemTaskDispatch::HasActiveHandle(std::string_view handle) const
{
    return std::any_of(activeHandles_.begin(), activeHandles_.end(),
        [handle](const auto& entry) { return entry.second == handle; });
}

std::vector<std::uint64_t> WidgetFilesystemTaskDispatch::ActiveTasksForHandle(
    std::string_view handle) const
{
    std::vector<std::uint64_t> tasks;
    for (const auto& [id, activeHandle] : activeHandles_)
        if (activeHandle == handle) tasks.push_back(id);
    return tasks;
}

void WidgetFilesystemTaskDispatch::Clear()
{
    selections_.clear();
    results_.clear();
    activeHandles_.clear();
}
}
