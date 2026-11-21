#include "widget/runtime/widget_engine.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_host_input_support.h"
#include "widget/view/widget_surface_host.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/permissions/widget_permission_broker.h"
#include "common/utils.h"
#include "data/data_paths.h"
#include "layout/logical_slot_picker_rules.h"
#include "layout/logical_slot_pointer_rules.h"
#include "widget/view/widget_interaction_lua_value.h"
#include "widget/settings/widget_settings_declarations.h"
#include "widget/storage/widget_storage_runtime.h"

#include <algorithm>
#include <cmath>
#include <utility>

using snowdesktop::widget_runtime::ActiveStorage;
using snowdesktop::widget_runtime::SaveStorageFile;
using snowdesktop::widget_runtime::EntityReferenceSettingTypeFilter;

namespace
{
using snowdesktop::widget_runtime::LogicalSlotKind;
using snowdesktop::widget_runtime::ViewNode;
using snowdesktop::widget_runtime::ViewNodeType;
using snowdesktop::widget_runtime::ViewRect;

std::optional<ViewRect> IntersectLogicalSlotFrames(
    const std::optional<ViewRect>& first, const ViewRect& second)
{
    if (!first) return second;
    const float left = std::max(first->x, second.x);
    const float top = std::max(first->y, second.y);
    const float right = std::min(
        first->x + first->width, second.x + second.width);
    const float bottom = std::min(
        first->y + first->height, second.y + second.height);
    if (right <= left || bottom <= top) return std::nullopt;
    return ViewRect{ left, top, right - left, bottom - top };
}

bool LogicalSlotScrollContainer(ViewNodeType type)
{
    return type == ViewNodeType::Scroll ||
        type == ViewNodeType::VirtualList ||
        type == ViewNodeType::VirtualGrid;
}

RECT LogicalSlotClientRect(const LuaWidget& widget, const ViewRect& frame)
{
    RECT result{
        widget.lastBounds.left +
            static_cast<LONG>(std::lround(frame.x)),
        widget.lastBounds.top +
            static_cast<LONG>(std::lround(frame.y)),
        widget.lastBounds.left +
            static_cast<LONG>(std::lround(frame.x + frame.width)),
        widget.lastBounds.top +
            static_cast<LONG>(std::lround(frame.y + frame.height)),
    };
    result.left = std::max(result.left, widget.lastBounds.left);
    result.top = std::max(result.top, widget.lastBounds.top);
    result.right = std::min(result.right, widget.lastBounds.right);
    result.bottom = std::min(result.bottom, widget.lastBounds.bottom);
    return result;
}

void RebuildLogicalSlotReferences(LuaWidget& widget)
{
    std::erase_if(widget.applicationReferences, [](const auto& entry) {
        return entry.second.persistent;
    });
    std::erase_if(widget.itemReferences, [](const auto& entry) {
        return entry.second.persistent;
    });
    for (const auto& snapshot : widget.logicalSlots.Snapshots())
    {
        for (const auto& item : snapshot.items)
        {
            if (item.kind == "app.reference")
            {
                widget.applicationReferences.insert_or_assign(item.reference,
                    LuaWidget::ApplicationReference{ {}, item.target, 0,
                        item.title, item.source, item.type, true,
                        item.available });
            }
            else
            {
                widget.itemReferences.insert_or_assign(item.reference,
                    LuaWidget::ItemReference{ item.target, "logical.slot",
                        0, item.title, item.source, item.type,
                        item.kind, true, item.available });
            }
        }
    }
}

bool CommitLogicalSlotMutation(WidgetEngine& engine, LuaWidget& widget,
    snowdesktop::widget_runtime::LogicalSlotModel previousModel,
    std::unordered_map<std::string, std::string> previousStorage,
    const snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    auto& storage = ActiveStorage();
    widget.logicalSlots.Export(storage, WideToUtf8(widget.widgetId));
    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        storage = std::move(previousStorage);
        widget.logicalSlots = std::move(previousModel);
        error = "logical slot persistence failed";
        return false;
    }
    widget.logicalSlotHistory.Record(std::move(previousModel), change);
    RebuildLogicalSlotReferences(widget);
    engine.RuntimeInvalidateHost(widget.widgetId);
    return true;
}
}

std::optional<LogicalSlotHostSurface>
WidgetEngine::RuntimeLogicalSlotSurface(const std::wstring& widgetId,
    std::string_view slotId) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid ||
        !widget->viewTree)
        return std::nullopt;

    const auto declaration = widget->logicalSlots.Declarations().find(slotId);
    const auto* snapshot = widget->logicalSlots.Find(slotId);
    if (declaration == widget->logicalSlots.Declarations().end() ||
        !snapshot)
        return std::nullopt;

    LogicalSlotHostSurface result;
    result.widgetId = widgetId;
    result.slotId = std::string(slotId);
    result.kind = snapshot->kind;
    result.revision = snapshot->revision;
    result.capacity = snapshot->capacity;
    result.itemCount = snapshot->items.size();
    result.allowClear = declaration->second.allowClear;
    result.accepts = declaration->second.accepts;
    result.replacePolicy = declaration->second.replacePolicy;

    const auto collect = [&](const auto& self,
        const ViewNode& node) -> bool {
        if (!node.visible || node.visibility ==
                snowdesktop::widget_runtime::ViewVisibility::Hidden)
            return false;
        if (node.type == ViewNodeType::SlotSurface &&
            node.logicalSlotId == slotId)
        {
            if (node.logicalSlotKind != snapshot->kind ||
                (node.logicalSlotRevision != 0 &&
                    node.logicalSlotRevision != snapshot->revision))
                return false;
            std::vector<const ViewNode*> sceneItems;
            for (const auto& child : node.children)
                if (child.type == ViewNodeType::SlotItem)
                    sceneItems.push_back(&child);
            if (snapshot->kind == LogicalSlotKind::Binding)
            {
                if ((!snapshot->items.empty() &&
                        (sceneItems.size() != 1 ||
                         node.children.size() != 1 ||
                         sceneItems.front()->key !=
                            snapshot->items.front().id ||
                         sceneItems.front()->logicalSlotReference !=
                            snapshot->items.front().reference)) ||
                    (snapshot->items.empty() && !sceneItems.empty()))
                    return false;
            }
            else
            {
                const bool declaredEmptyContent = snapshot->items.empty() &&
                    node.collectionContent ==
                        snowdesktop::widget_runtime::
                            ViewCollectionContent::Empty;
                if ((declaredEmptyContent &&
                        (!sceneItems.empty() || node.children.size() != 1)) ||
                    (!declaredEmptyContent &&
                        (sceneItems.size() != snapshot->items.size() ||
                         node.children.size() != snapshot->items.size())))
                    return false;
                for (std::size_t index = 0;
                    index < sceneItems.size(); ++index)
                {
                    if (sceneItems[index]->key != snapshot->items[index].id ||
                        sceneItems[index]->logicalSlotReference !=
                            snapshot->items[index].reference)
                        return false;
                }
            }
            const ViewRect transformedSurface =
                snowdesktop::widget_runtime::ApplyViewTransform(node.frame,
                    snowdesktop::widget_runtime::ResolveViewTransformForKey(
                        *widget->viewTree, node.key));
            const auto visibleSurface = IntersectLogicalSlotFrames(
                snowdesktop::widget_runtime::ResolveViewClipForKey(
                    *widget->viewTree, node.key, false),
                transformedSurface);
            if (!visibleSurface) return false;
            result.bounds = LogicalSlotClientRect(*widget, *visibleSurface);
            if (result.bounds.right <= result.bounds.left ||
                result.bounds.bottom <= result.bounds.top)
                return false;
            result.dropStyle =
                snowdesktop::widget_runtime::ResolveViewThemeStyle(
                    node.dropStyle,
                    BuildWidgetViewThemePalette(d2dState_, widgetId));

            for (const auto& child : node.children)
            {
                if (child.type != ViewNodeType::SlotItem || !child.visible)
                    continue;
                const ViewRect transformedItem =
                    snowdesktop::widget_runtime::ApplyViewTransform(
                        child.frame,
                        snowdesktop::widget_runtime::
                            ResolveViewTransformForKey(
                                *widget->viewTree, child.key));
                auto visibleItem = IntersectLogicalSlotFrames(
                    visibleSurface, transformedItem);
                if (visibleItem)
                    visibleItem = IntersectLogicalSlotFrames(
                        snowdesktop::widget_runtime::ResolveViewClipForKey(
                            *widget->viewTree, child.key, false),
                        *visibleItem);
                if (!visibleItem) continue;
                RECT bounds = LogicalSlotClientRect(*widget, *visibleItem);
                if (bounds.right <= bounds.left ||
                    bounds.bottom <= bounds.top)
                    continue;
                result.items.push_back({ child.key, bounds });
            }
            return true;
        }

        for (const auto& child : node.children)
            if (self(self, child)) return true;
        return false;
    };

    if (!collect(collect, *widget->viewTree))
        return std::nullopt;
    return result;
}

bool WidgetEngine::RuntimeBindHostLogicalSlot(
    const std::wstring& widgetId, std::string_view slotId,
    snowdesktop::widget_runtime::LogicalSlotItem candidate,
    std::size_t targetIndex,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error, std::string_view source)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidateWidget) {
            return candidateWidget.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }

    candidate.available = true;
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Bind(slotId, std::move(candidate),
            change, error, targetIndex))
        return false;
    if (change.operation == "unchanged") return true;
    if (!CommitLogicalSlotMutation(*this, *widget,
            std::move(previousModel), std::move(previousStorage),
            change, error))
        return false;

    DispatchHostLogicalSlotChange(*widget, change, source);
    return true;
}

bool WidgetEngine::RuntimeOpenHostLogicalSlotPicker(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    std::string_view slotId, std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    const auto declaration = widget->logicalSlots.Declarations().find(slotId);
    const auto* snapshot = widget->logicalSlots.Find(slotId);
    if (declaration == widget->logicalSlots.Declarations().end() ||
        !snapshot)
    {
        error = "logical slot is not declared";
        return false;
    }
    if (snapshot->kind == LogicalSlotKind::Collection &&
        snapshot->items.size() >= snapshot->capacity)
    {
        error = "logical slot is full";
        return false;
    }
    if (!logicalSlotPickerCallback_)
    {
        error = "hostPickerUnavailable";
        return false;
    }

    LogicalSlotPickerRequest request;
    request.widgetId = widgetId;
    request.ownerToken = ownerToken;
    request.slotId = std::string(slotId);
    request.kind = snapshot->kind;
    request.accepts = declaration->second.accepts;
    request.targetIndex = snapshot->kind == LogicalSlotKind::Collection
        ? snapshot->items.size() : 0;
    if (!logicalSlotPickerCallback_(request))
    {
        error = "hostPickerUnavailable";
        return false;
    }
    return true;
}

void WidgetEngine::DispatchHostLogicalSlotChange(LuaWidget& widget,
    const snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string_view source)
{
    const auto eventChange = change;
    const std::string eventSource(source);
    (void)InvokeLifecycleEvent(widget, "slot.changed",
        [eventChange, eventSource](lua_State* state) {
            lua_pushlstring(state, eventChange.slotId.data(),
                eventChange.slotId.size());
            lua_setfield(state, -2, "slotId");
            lua_pushstring(state,
                snowdesktop::widget_runtime::LogicalSlotKindName(
                    eventChange.kind));
            lua_setfield(state, -2, "slotKind");
            lua_pushinteger(state,
                static_cast<lua_Integer>(eventChange.revision));
            lua_setfield(state, -2, "revision");
            lua_pushlstring(state, eventChange.operation.data(),
                eventChange.operation.size());
            lua_setfield(state, -2, "operation");
            lua_createtable(state,
                static_cast<int>(eventChange.itemIds.size()), 0);
            for (std::size_t index = 0;
                index < eventChange.itemIds.size(); ++index)
            {
                lua_pushlstring(state, eventChange.itemIds[index].data(),
                    eventChange.itemIds[index].size());
                lua_rawseti(state, -2,
                    static_cast<lua_Integer>(index + 1));
            }
            lua_setfield(state, -2, "itemIds");
            if (!eventChange.relatedSlotId.empty())
            {
                lua_pushlstring(state, eventChange.relatedSlotId.data(),
                    eventChange.relatedSlotId.size());
                lua_setfield(state, -2, "relatedSlotId");
                lua_pushinteger(state,
                    static_cast<lua_Integer>(
                        eventChange.relatedRevision));
                lua_setfield(state, -2, "relatedRevision");
            }
            lua_pushlstring(state, eventSource.data(), eventSource.size());
            lua_setfield(state, -2, "source");
        });
}

void WidgetEngine::RefreshLogicalSlotAvailability()
{
    LuaApplicationCatalogSnapshot catalog;
    bool catalogReady = false;
    if (applicationCatalogProvider_)
    {
        try
        {
            catalog = applicationCatalogProvider_();
            catalogReady = catalog.state == "ready";
        }
        catch (...)
        {
            catalogReady = false;
        }
    }

    std::unordered_set<std::wstring> availableTargets;
    if (catalogReady)
    {
        availableTargets.reserve(catalog.entries.size());
        for (const auto& entry : catalog.entries)
        {
            std::wstring target = ToUpperInvariant(
                Utf8ToWide(entry.launchTarget));
            if (!target.empty())
                availableTargets.insert(std::move(target));
        }
    }

    struct AvailabilityChange
    {
        snowdesktop::widget_runtime::LogicalSlotChange change;
        std::string source;
    };
    struct ChangedWidget
    {
        std::size_t index = 0;
        snowdesktop::widget_runtime::LogicalSlotModel previous;
        std::vector<AvailabilityChange> changes;
    };
    std::vector<ChangedWidget> changedWidgets;
    const auto previousStorage = ActiveStorage();
    for (std::size_t widgetIndex = 0;
        widgetIndex < widgets_.size(); ++widgetIndex)
    {
        auto& widget = widgets_[widgetIndex];
        if (!widget.valid || widget.preview)
            continue;

        ChangedWidget changed;
        changed.index = widgetIndex;
        changed.previous = widget.logicalSlots;
        const auto expectedTypeForSlot = [&](std::string_view slotId) {
            const auto find = [&](const auto& settings) -> std::string_view {
                for (const auto& setting : settings)
                    if (setting.binding == slotId)
                        return EntityReferenceSettingTypeFilter(
                            setting.type);
                return {};
            };
            std::string_view result = find(widget.manifest.settings);
            return result.empty() ? find(widget.scriptSettings) : result;
        };
        for (const auto& snapshot : widget.logicalSlots.Snapshots())
        {
            for (const auto& item : snapshot.items)
            {
                bool available = item.available;
                std::string source;
                if (item.kind == "app.reference")
                {
                    const std::wstring target = snowdesktop::
                        logical_slot_picker_rules::
                            NormalizeApplicationLaunchTarget(
                                Utf8ToWide(item.target));
                    if (snowdesktop::logical_slot_picker_rules::
                            IsFilesystemApplicationLaunchTarget(target))
                    {
                        const DWORD attributes = target.empty()
                            ? INVALID_FILE_ATTRIBUTES
                            : GetFileAttributesW(target.c_str());
                        available = attributes != INVALID_FILE_ATTRIBUTES &&
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
                        source = "host.filesystem";
                    }
                    else
                    {
                        if (!catalogReady) continue;
                        const std::wstring catalogTarget =
                            ToUpperInvariant(target);
                        available = !catalogTarget.empty() &&
                            availableTargets.contains(catalogTarget);
                        source = "host.catalog";
                    }
                }
                else if (item.kind == "filesystem.reference")
                {
                    const std::string_view expectedType =
                        expectedTypeForSlot(snapshot.id);
                    const std::wstring target =
                        Utf8ToWide(item.target);
                    const DWORD attributes = target.empty()
                        ? INVALID_FILE_ATTRIBUTES
                        : GetFileAttributesW(target.c_str());
                    available = attributes != INVALID_FILE_ATTRIBUTES;
                    if (available && !expectedType.empty() &&
                        item.type != expectedType)
                        available = false;
                    const std::string_view effectiveType =
                        expectedType.empty()
                        ? std::string_view(item.type) : expectedType;
                    if (available && effectiveType == "folder")
                        available =
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    else if (available && effectiveType == "file")
                        available =
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
                    source = "host.filesystem";
                }
                else continue;
                snowdesktop::widget_runtime::LogicalSlotChange change;
                std::string error;
                if (!widget.logicalSlots.SetAvailability(snapshot.id,
                        item.id, available, change, error))
                {
                    OutputDebugStringA((
                        "SnowDesktop logical slot availability: " +
                        error + "\n").c_str());
                    continue;
                }
                if (change.operation != "unchanged")
                    changed.changes.push_back({
                        std::move(change), std::move(source) });
            }
        }
        if (!changed.changes.empty())
        {
            widget.logicalSlots.Export(ActiveStorage(),
                WideToUtf8(widget.widgetId));
            changedWidgets.push_back(std::move(changed));
        }
    }
    if (changedWidgets.empty()) return;

    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        ActiveStorage() = previousStorage;
        for (auto& changed : changedWidgets)
            widgets_[changed.index].logicalSlots =
                std::move(changed.previous);
        return;
    }

    for (const auto& changed : changedWidgets)
    {
        auto& widget = widgets_[changed.index];
        RebuildLogicalSlotReferences(widget);
        RuntimeInvalidateHost(widget.widgetId);
        for (const auto& change : changed.changes)
            DispatchHostLogicalSlotChange(
                widget, change.change, change.source);
    }
}

bool WidgetEngine::RuntimeRemoveHostLogicalSlotItem(
    const std::wstring& widgetId, std::string_view slotId,
    std::string_view itemId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error, std::string_view source)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    const auto* snapshot = widget->logicalSlots.Find(slotId);
    if (!snapshot)
    {
        error = "logical slot is not declared";
        return false;
    }
    const auto item = std::find_if(snapshot->items.begin(),
        snapshot->items.end(), [itemId](const auto& candidate) {
            return candidate.id == itemId;
        });
    if (item == snapshot->items.end())
    {
        error = "logical slot item does not exist";
        return false;
    }

    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    const bool changed = snapshot->kind == LogicalSlotKind::Binding
        ? widget->logicalSlots.Clear(slotId, change, error)
        : widget->logicalSlots.Remove(slotId, itemId, change, error);
    if (!changed) return false;
    if (change.operation == "unchanged") return true;
    if (!CommitLogicalSlotMutation(*this, *widget,
            std::move(previousModel), std::move(previousStorage),
            change, error))
        return false;
    DispatchHostLogicalSlotChange(*widget, change, source);
    return true;
}

bool WidgetEngine::RuntimeMoveHostLogicalSlotItem(
    const std::wstring& widgetId, std::string_view slotId,
    std::string_view itemId, std::size_t targetIndex,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error, std::string_view source)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Move(slotId, itemId, targetIndex,
            change, error))
        return false;
    if (change.operation == "unchanged") return true;
    if (!CommitLogicalSlotMutation(*this, *widget,
            std::move(previousModel), std::move(previousStorage),
            change, error))
        return false;
    DispatchHostLogicalSlotChange(*widget, change, source);
    return true;
}

bool WidgetEngine::RuntimeTransferHostLogicalSlotItem(
    const std::wstring& widgetId, std::string_view sourceSlotId,
    std::string_view itemId, std::string_view targetSlotId,
    std::size_t targetIndex,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error, std::string_view source)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Transfer(sourceSlotId, itemId,
            targetSlotId, targetIndex, change, error))
        return false;
    if (!CommitLogicalSlotMutation(*this, *widget,
            std::move(previousModel), std::move(previousStorage),
            change, error))
        return false;
    if (widget->logicalSlotFocus &&
        widget->logicalSlotFocus->slotId == sourceSlotId &&
        widget->logicalSlotFocus->itemId == itemId)
        widget->logicalSlotFocus->slotId = std::string(targetSlotId);
    DispatchHostLogicalSlotChange(*widget, change, source);
    return true;
}

std::optional<snowdesktop::widget_runtime::LogicalSlotSnapshot>
WidgetEngine::RuntimeLogicalSlotSnapshot(const std::wstring& widgetId,
    std::uint64_t ownerToken, std::string_view slotId,
    snowdesktop::widget_runtime::LogicalSlotKind kind) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end()) return std::nullopt;
    const auto* snapshot = widget->logicalSlots.Find(slotId);
    if (!snapshot || snapshot->kind != kind) return std::nullopt;
    return *snapshot;
}

bool WidgetEngine::RuntimeBindLogicalSlot(const std::wstring& widgetId,
    std::uint64_t ownerToken, std::string_view slotId,
    std::string_view reference,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview)
    {
        error = "previewReadOnly";
        return false;
    }
    if (!trustedGestureState_.Active())
    {
        error = "userGestureRequired";
        return false;
    }

    snowdesktop::widget_runtime::LogicalSlotItem candidate;
    if (const auto app = widget->applicationReferences.find(
            std::string(reference));
        app != widget->applicationReferences.end())
    {
        if (!app->second.available)
        {
            error = "unavailableReference";
            return false;
        }
        if (!app->second.persistent &&
            app->second.catalogRevision != appIndexRevision_)
        {
            error = "staleReference";
            return false;
        }
        candidate.kind = "app.reference";
        candidate.title = app->second.title.empty()
            ? app->second.launchTarget : app->second.title;
        candidate.source = app->second.source;
        candidate.type = app->second.type;
        candidate.target = app->second.launchTarget;
    }
    else if (const auto item = widget->itemReferences.find(
            std::string(reference));
        item != widget->itemReferences.end())
    {
        if (!item->second.available)
        {
            error = "unavailableReference";
            return false;
        }
        if (!item->second.persistent &&
            item->second.sourceTask == "desktop.search" &&
            item->second.revision != desktopDataRevision_)
        {
            error = "staleReference";
            return false;
        }
        candidate.kind = item->second.referenceKind;
        candidate.title = item->second.title.empty()
            ? item->second.target : item->second.title;
        candidate.source = item->second.source;
        candidate.type = item->second.type;
        candidate.target = item->second.target;
    }
    else
    {
        error = "invalidReference";
        return false;
    }
    candidate.available = true;

    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Bind(slotId, std::move(candidate),
            change, error))
        return false;
    if (change.operation == "unchanged") return true;
    return CommitLogicalSlotMutation(*this, *widget,
        std::move(previousModel), std::move(previousStorage), change, error);
}

bool WidgetEngine::RuntimeClearLogicalSlot(const std::wstring& widgetId,
    std::uint64_t ownerToken, std::string_view slotId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Clear(slotId, change, error)) return false;
    if (change.operation == "unchanged") return true;
    return CommitLogicalSlotMutation(*this, *widget,
        std::move(previousModel), std::move(previousStorage), change, error);
}

bool WidgetEngine::RuntimeRemoveLogicalSlotItem(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    std::string_view slotId, std::string_view itemId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Remove(slotId, itemId, change, error))
        return false;
    return CommitLogicalSlotMutation(*this, *widget,
        std::move(previousModel), std::move(previousStorage), change, error);
}

bool WidgetEngine::RuntimeMoveLogicalSlotItem(const std::wstring& widgetId,
    std::uint64_t ownerToken, std::string_view slotId,
    std::string_view itemId, std::size_t targetIndex,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Move(slotId, itemId, targetIndex,
            change, error))
        return false;
    if (change.operation == "unchanged") return true;
    return CommitLogicalSlotMutation(*this, *widget,
        std::move(previousModel), std::move(previousStorage), change, error);
}

bool WidgetEngine::RuntimeCanUndoLogicalSlot(
    const std::wstring& widgetId, std::uint64_t ownerToken) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    return widget != widgets_.end() && !widget->preview &&
        widget->logicalSlotHistory.CanUndo();
}

bool WidgetEngine::RuntimeCanRedoLogicalSlot(
    const std::wstring& widgetId, std::uint64_t ownerToken) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    return widget != widgets_.end() && !widget->preview &&
        widget->logicalSlotHistory.CanRedo();
}

bool WidgetEngine::RuntimeCanUndoHostLogicalSlot(
    const std::wstring& widgetId) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    return widget != widgets_.end() && !widget->preview && widget->valid &&
        widget->logicalSlotHistory.CanUndo();
}

bool WidgetEngine::RuntimeCanRedoHostLogicalSlot(
    const std::wstring& widgetId) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    return widget != widgets_.end() && !widget->preview && widget->valid &&
        widget->logicalSlotHistory.CanRedo();
}

namespace
{
bool RestoreLogicalSlotHistory(WidgetEngine& engine, LuaWidget& widget,
    bool redo, snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    auto previousHistory = widget.logicalSlotHistory;
    auto currentModel = widget.logicalSlots;
    auto previousStorage = ActiveStorage();
    const bool restored = redo
        ? widget.logicalSlotHistory.Redo(widget.logicalSlots, change, error)
        : widget.logicalSlotHistory.Undo(widget.logicalSlots, change, error);
    if (!restored) return false;
    auto& storage = ActiveStorage();
    widget.logicalSlots.Export(storage, WideToUtf8(widget.widgetId));
    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        storage = std::move(previousStorage);
        widget.logicalSlots = std::move(currentModel);
        widget.logicalSlotHistory = std::move(previousHistory);
        error = "logical slot persistence failed";
        return false;
    }
    RebuildLogicalSlotReferences(widget);
    engine.RuntimeInvalidateHost(widget.widgetId);
    return true;
}
}

bool WidgetEngine::RuntimeUndoLogicalSlot(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    return RestoreLogicalSlotHistory(*this, *widget, false, change, error);
}

bool WidgetEngine::RuntimeRedoLogicalSlot(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    return RestoreLogicalSlotHistory(*this, *widget, true, change, error);
}

bool WidgetEngine::RuntimeUndoHostLogicalSlot(
    const std::wstring& widgetId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    if (!RestoreLogicalSlotHistory(*this, *widget, false, change, error))
        return false;
    DispatchHostLogicalSlotChange(*widget, change, "host.keyboard");
    return true;
}

bool WidgetEngine::RuntimeRedoHostLogicalSlot(
    const std::wstring& widgetId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    if (!RestoreLogicalSlotHistory(*this, *widget, true, change, error))
        return false;
    DispatchHostLogicalSlotChange(*widget, change, "host.keyboard");
    return true;
}

void WidgetEngine::RestoreLogicalSlotReferences(LuaWidget& widget)
{
    RebuildLogicalSlotReferences(widget);
}
