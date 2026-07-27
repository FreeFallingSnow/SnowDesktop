/**
 * @file slot_contract.h
 * @brief 槽位组件与拖拽载荷之间的集中式行为契约。
 *
 * 所有可构建 Slot 的具体 Container 都必须声明唯一的 SlotSurfaceKind。
 * 新增槽位面时必须：
 *  1. 在 SlotSurfaceKind 中增加枚举；
 *  2. 在 kSurfaceDescriptors 中登记其可发出的载荷；
 *  3. 在 EvaluateSlotDrop 中声明接受规则；
 *  4. 由 slot_contract_tests 的全有向矩阵覆盖。
 *
 * 运行时命中测试和自动化测试共同调用本文件中的规则，避免测试复制实现。
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../types.h"

namespace snowdesktop::slot_contract
{
enum class SlotSurfaceKind : std::uint8_t
{
    Desktop,
    Dock,
    Collection,
    FileCategories,
    FolderMapping,
    CollectionGroup,
    FileGroup,
    Guide,
    External,
    Count,
};

enum class DragPayloadKind : std::uint8_t
{
    DesktopItem,
    FolderEntry,
    ExternalFile,
    CollectionWidget,
    FolderMappingWidget,
    FileSourceWidget,
    OtherWidget,
    CollectionGroupLabel,
    FileGroupLabel,
    Count,
};

enum class DragRelation : std::uint8_t
{
    SameInstance,
    SameSurface,
    CrossSurface,
    ExternalIngress,
    ExternalEgress,
};

enum class DropRoute : std::uint8_t
{
    Reject,
    ReorderWithinContainer,
    PlaceOnDesktop,
    AddToDock,
    InsertLogicalItem,
    TransferFile,
    RouteThroughCollectionGroup,
    RouteThroughFileGroup,
    MoveCollectionIntoGroup,
    MoveFileSourceIntoGroup,
    ReleaseGroupedChild,
    TransferGroupedLabel,
    ExportToExternal,
};

struct DragPayloadFlags
{
    bool desktopItems = false;
    bool folderEntries = false;
    bool externalFiles = false;
    bool widgets = false;
    bool collectionWidgetsOnly = false;
    bool collectionGroupLabels = false;
    bool fileGroupLabels = false;
    bool folderMappingWidgetsOnly = false;
};

constexpr DragPayloadKind ClassifyPayload(
    const DragPayloadFlags& flags)
{
    const unsigned activeFamilies =
        static_cast<unsigned>(flags.desktopItems) +
        static_cast<unsigned>(flags.folderEntries) +
        static_cast<unsigned>(flags.externalFiles) +
        static_cast<unsigned>(flags.widgets) +
        static_cast<unsigned>(
            flags.collectionGroupLabels) +
        static_cast<unsigned>(
            flags.fileGroupLabels);
    if (activeFamilies != 1)
        return DragPayloadKind::Count;
    if (flags.collectionGroupLabels)
        return DragPayloadKind::CollectionGroupLabel;
    if (flags.fileGroupLabels)
        return DragPayloadKind::FileGroupLabel;
    if (flags.widgets)
    {
        if (flags.collectionWidgetsOnly)
            return DragPayloadKind::CollectionWidget;
        if (flags.folderMappingWidgetsOnly)
            return DragPayloadKind::FolderMappingWidget;
        return DragPayloadKind::OtherWidget;
    }
    if (flags.folderEntries)
        return DragPayloadKind::FolderEntry;
    if (flags.externalFiles)
        return DragPayloadKind::ExternalFile;
    return DragPayloadKind::DesktopItem;
}

constexpr std::size_t ToIndex(SlotSurfaceKind value)
{
    return static_cast<std::size_t>(value);
}

constexpr std::size_t ToIndex(DragPayloadKind value)
{
    return static_cast<std::size_t>(value);
}

constexpr std::uint32_t PayloadBit(DragPayloadKind payload)
{
    return std::uint32_t{1} << ToIndex(payload);
}

struct SurfaceDescriptor
{
    SlotSurfaceKind kind;
    std::string_view name;
    bool buildsSlots;
    bool externalBoundary;
    std::uint32_t emittedPayloads;
};

inline constexpr std::array<
    SurfaceDescriptor,
    ToIndex(SlotSurfaceKind::Count)> kSurfaceDescriptors{{
    {
        SlotSurfaceKind::Desktop,
        "desktop",
        true,
        false,
        PayloadBit(DragPayloadKind::DesktopItem) |
            PayloadBit(DragPayloadKind::CollectionWidget) |
            PayloadBit(DragPayloadKind::FolderMappingWidget) |
            PayloadBit(DragPayloadKind::FileSourceWidget) |
            PayloadBit(DragPayloadKind::OtherWidget),
    },
        {
            SlotSurfaceKind::Dock,
            "dock",
            true,
            false,
            PayloadBit(DragPayloadKind::DesktopItem) |
                PayloadBit(DragPayloadKind::CollectionWidget) |
                PayloadBit(DragPayloadKind::FolderMappingWidget) |
                PayloadBit(DragPayloadKind::FolderEntry),
    },
    {
        SlotSurfaceKind::Collection,
        "collection",
        true,
        false,
        PayloadBit(DragPayloadKind::DesktopItem),
    },
    {
        SlotSurfaceKind::FileCategories,
        "file_categories",
        true,
        false,
        PayloadBit(DragPayloadKind::DesktopItem),
    },
    {
        SlotSurfaceKind::FolderMapping,
        "folder_mapping",
        true,
        false,
        PayloadBit(DragPayloadKind::FolderEntry),
    },
    {
        SlotSurfaceKind::CollectionGroup,
        "collection_group",
        true,
        false,
        PayloadBit(DragPayloadKind::DesktopItem) |
            PayloadBit(DragPayloadKind::CollectionGroupLabel),
    },
    {
        SlotSurfaceKind::FileGroup,
        "file_group",
        true,
        false,
        PayloadBit(DragPayloadKind::DesktopItem) |
            PayloadBit(DragPayloadKind::FolderMappingWidget) |
            PayloadBit(DragPayloadKind::FolderEntry) |
            PayloadBit(DragPayloadKind::FileGroupLabel),
    },
    {
        SlotSurfaceKind::Guide,
        "guide",
        false,
        false,
        0,
    },
    {
        SlotSurfaceKind::External,
        "external",
        false,
        true,
        PayloadBit(DragPayloadKind::ExternalFile),
    },
}};

enum class WidgetContainerRole : std::uint8_t
{
    SlotContainer,
    NonSlotContainer,
    NonContainer,
};

struct WidgetContractDescriptor
{
    DesktopWidgetType type;
    std::string_view name;
    WidgetContainerRole role;
    SlotSurfaceKind surface;
    DragPayloadKind widgetPayload;
};

constexpr std::size_t ToIndex(DesktopWidgetType value)
{
    return static_cast<std::size_t>(value);
}

inline constexpr std::array<
    WidgetContractDescriptor,
    ToIndex(DesktopWidgetType::Count)>
    kWidgetContractDescriptors{{
        {
            DesktopWidgetType::Collection,
            "collection",
            WidgetContainerRole::SlotContainer,
            SlotSurfaceKind::Collection,
            DragPayloadKind::CollectionWidget,
        },
        {
            DesktopWidgetType::CollectionGroup,
            "collection_group",
            WidgetContainerRole::SlotContainer,
            SlotSurfaceKind::CollectionGroup,
            DragPayloadKind::OtherWidget,
        },
        {
            DesktopWidgetType::FileGroup,
            "file_group",
            WidgetContainerRole::SlotContainer,
            SlotSurfaceKind::FileGroup,
            DragPayloadKind::OtherWidget,
        },
        {
            DesktopWidgetType::FileCategories,
            "file_categories",
            WidgetContainerRole::SlotContainer,
            SlotSurfaceKind::FileCategories,
            DragPayloadKind::FileSourceWidget,
        },
        {
            DesktopWidgetType::FolderMapping,
            "folder_mapping",
            WidgetContainerRole::SlotContainer,
            SlotSurfaceKind::FolderMapping,
            DragPayloadKind::FolderMappingWidget,
        },
        {
            DesktopWidgetType::LuaScript,
            "lua_script",
            WidgetContainerRole::NonContainer,
            SlotSurfaceKind::Guide,
            DragPayloadKind::OtherWidget,
        },
        {
            DesktopWidgetType::Guide,
            "guide",
            WidgetContainerRole::NonSlotContainer,
            SlotSurfaceKind::Guide,
            DragPayloadKind::OtherWidget,
        },
    }};

constexpr const WidgetContractDescriptor&
DescribeWidget(DesktopWidgetType type)
{
    return kWidgetContractDescriptors[
        ToIndex(type)];
}

constexpr SlotSurfaceKind SurfaceForWidgetType(
    DesktopWidgetType type)
{
    return DescribeWidget(type).surface;
}

constexpr DragPayloadKind PayloadForWidgetType(
    DesktopWidgetType type)
{
    return DescribeWidget(type).widgetPayload;
}

constexpr const SurfaceDescriptor& Describe(SlotSurfaceKind kind)
{
    return kSurfaceDescriptors[ToIndex(kind)];
}

constexpr bool IsKnownSurface(SlotSurfaceKind kind)
{
    return ToIndex(kind) < kSurfaceDescriptors.size() &&
        Describe(kind).kind == kind &&
        !Describe(kind).name.empty();
}

constexpr bool SurfaceBuildsSlots(SlotSurfaceKind kind)
{
    return IsKnownSurface(kind) && Describe(kind).buildsSlots;
}

constexpr bool SurfaceEmits(
    SlotSurfaceKind surface,
    DragPayloadKind payload)
{
    return IsKnownSurface(surface) &&
        (Describe(surface).emittedPayloads &
            PayloadBit(payload)) != 0;
}

constexpr DragRelation ClassifyRelation(
    SlotSurfaceKind source,
    SlotSurfaceKind target,
    bool sameInstance)
{
    if (source == SlotSurfaceKind::External)
        return DragRelation::ExternalIngress;
    if (target == SlotSurfaceKind::External)
        return DragRelation::ExternalEgress;
    if (sameInstance)
        return DragRelation::SameInstance;
    if (source == target)
        return DragRelation::SameSurface;
    return DragRelation::CrossSurface;
}

constexpr bool RelationMatches(
    SlotSurfaceKind source,
    SlotSurfaceKind target,
    DragRelation relation)
{
    switch (relation)
    {
    case DragRelation::SameInstance:
        return source == target &&
            source != SlotSurfaceKind::External;
    case DragRelation::SameSurface:
        return source == target &&
            source != SlotSurfaceKind::External;
    case DragRelation::CrossSurface:
        return source != target &&
            source != SlotSurfaceKind::External &&
            target != SlotSurfaceKind::External;
    case DragRelation::ExternalIngress:
        return source == SlotSurfaceKind::External &&
            target != SlotSurfaceKind::External;
    case DragRelation::ExternalEgress:
        return source != SlotSurfaceKind::External &&
            target == SlotSurfaceKind::External;
    }
    return false;
}

constexpr DropRoute EvaluateSlotDrop(
    SlotSurfaceKind source,
    DragPayloadKind payload,
    SlotSurfaceKind target,
    DragRelation relation)
{
    if (!IsKnownSurface(source) ||
        !IsKnownSurface(target) ||
        !SurfaceEmits(source, payload) ||
        !RelationMatches(source, target, relation) ||
        target == SlotSurfaceKind::Guide)
        return DropRoute::Reject;

    if (relation == DragRelation::ExternalEgress)
    {
        return payload == DragPayloadKind::DesktopItem ||
                payload == DragPayloadKind::FolderEntry
            ? DropRoute::ExportToExternal
            : DropRoute::Reject;
    }

    if (relation == DragRelation::ExternalIngress)
    {
        if (payload != DragPayloadKind::ExternalFile)
            return DropRoute::Reject;
        if (target == SlotSurfaceKind::Dock)
            return DropRoute::AddToDock;
        if (target == SlotSurfaceKind::Desktop)
            return DropRoute::PlaceOnDesktop;
        if (target == SlotSurfaceKind::FolderMapping)
            return DropRoute::TransferFile;
        if (target == SlotSurfaceKind::CollectionGroup)
            return DropRoute::RouteThroughCollectionGroup;
        if (target == SlotSurfaceKind::FileGroup)
            return DropRoute::RouteThroughFileGroup;
        return SurfaceBuildsSlots(target)
            ? DropRoute::InsertLogicalItem
            : DropRoute::Reject;
    }

    if (payload == DragPayloadKind::CollectionGroupLabel)
    {
        if (target == SlotSurfaceKind::Desktop)
            return DropRoute::ReleaseGroupedChild;
        if (target == SlotSurfaceKind::CollectionGroup)
            return relation == DragRelation::SameInstance
                ? DropRoute::ReorderWithinContainer
                : DropRoute::TransferGroupedLabel;
        return DropRoute::Reject;
    }

    if (payload == DragPayloadKind::FileGroupLabel)
    {
        if (target == SlotSurfaceKind::Desktop)
            return DropRoute::ReleaseGroupedChild;
        if (target == SlotSurfaceKind::FileGroup)
            return relation == DragRelation::SameInstance
                ? DropRoute::ReorderWithinContainer
                : DropRoute::TransferGroupedLabel;
        return DropRoute::Reject;
    }

    if (payload == DragPayloadKind::CollectionWidget)
    {
        if (relation == DragRelation::SameInstance &&
            target == SlotSurfaceKind::Dock)
            return DropRoute::ReorderWithinContainer;
        if (target == SlotSurfaceKind::Desktop)
            return DropRoute::PlaceOnDesktop;
        if (target == SlotSurfaceKind::Dock)
            return DropRoute::AddToDock;
        if (target == SlotSurfaceKind::CollectionGroup)
            return DropRoute::MoveCollectionIntoGroup;
        return DropRoute::Reject;
    }

    if (payload == DragPayloadKind::FolderMappingWidget)
    {
        if (relation == DragRelation::SameInstance &&
            target == SlotSurfaceKind::Dock)
            return DropRoute::ReorderWithinContainer;
        if (target == SlotSurfaceKind::Desktop)
            return DropRoute::PlaceOnDesktop;
        if (target == SlotSurfaceKind::Dock)
            return DropRoute::AddToDock;
        if (target == SlotSurfaceKind::FileGroup)
            return DropRoute::MoveFileSourceIntoGroup;
        return DropRoute::Reject;
    }

    if (payload == DragPayloadKind::FileSourceWidget)
    {
        if (target == SlotSurfaceKind::Desktop)
            return DropRoute::PlaceOnDesktop;
        if (target == SlotSurfaceKind::FileGroup)
            return DropRoute::MoveFileSourceIntoGroup;
        return DropRoute::Reject;
    }

    if (payload == DragPayloadKind::OtherWidget)
        return target == SlotSurfaceKind::Desktop
            ? DropRoute::PlaceOnDesktop
            : DropRoute::Reject;

    if (payload == DragPayloadKind::DesktopItem)
    {
        if (target == SlotSurfaceKind::Desktop)
            return relation == DragRelation::SameInstance
                ? DropRoute::ReorderWithinContainer
                : DropRoute::PlaceOnDesktop;
        if (target == SlotSurfaceKind::Dock)
            return relation == DragRelation::SameInstance
                ? DropRoute::ReorderWithinContainer
                : DropRoute::AddToDock;
        if (target == SlotSurfaceKind::FolderMapping)
            return DropRoute::TransferFile;
        if (target == SlotSurfaceKind::CollectionGroup)
            return DropRoute::RouteThroughCollectionGroup;
        if (target == SlotSurfaceKind::FileGroup)
            return DropRoute::RouteThroughFileGroup;
        if (target == SlotSurfaceKind::Collection ||
            target == SlotSurfaceKind::FileCategories)
            return relation == DragRelation::SameInstance
                ? DropRoute::ReorderWithinContainer
                : DropRoute::InsertLogicalItem;
        return DropRoute::Reject;
    }

    if (payload == DragPayloadKind::FolderEntry)
    {
        if (target == SlotSurfaceKind::Desktop)
            return DropRoute::TransferFile;
        if (target == SlotSurfaceKind::Dock)
            return DropRoute::AddToDock;
        if (target == SlotSurfaceKind::FolderMapping)
            return relation == DragRelation::SameInstance
                ? DropRoute::ReorderWithinContainer
                : DropRoute::TransferFile;
        if (target == SlotSurfaceKind::CollectionGroup)
            return DropRoute::RouteThroughCollectionGroup;
        if (target == SlotSurfaceKind::FileGroup)
            return DropRoute::RouteThroughFileGroup;
        if (target == SlotSurfaceKind::Collection ||
            target == SlotSurfaceKind::FileCategories)
            return DropRoute::InsertLogicalItem;
        return DropRoute::Reject;
    }

    return DropRoute::Reject;
}

constexpr bool AcceptsSlotDrop(
    SlotSurfaceKind source,
    DragPayloadKind payload,
    SlotSurfaceKind target,
    DragRelation relation)
{
    return EvaluateSlotDrop(
        source, payload, target, relation) !=
        DropRoute::Reject;
}

consteval bool RegistryIsComplete()
{
    for (std::size_t i = 0;
        i < kSurfaceDescriptors.size(); ++i)
    {
        const auto& descriptor =
            kSurfaceDescriptors[i];
        if (ToIndex(descriptor.kind) != i ||
            descriptor.name.empty())
            return false;
        for (std::size_t j = i + 1;
            j < kSurfaceDescriptors.size(); ++j)
            if (descriptor.name ==
                kSurfaceDescriptors[j].name)
                return false;
    }
    return true;
}

static_assert(
    RegistryIsComplete(),
    "Every SlotSurfaceKind must have one unique registry entry");

consteval bool WidgetRegistryIsComplete()
{
    for (std::size_t i = 0;
        i < kWidgetContractDescriptors.size(); ++i)
    {
        const auto& descriptor =
            kWidgetContractDescriptors[i];
        if (ToIndex(descriptor.type) != i ||
            descriptor.name.empty())
            return false;
        if (descriptor.role ==
                WidgetContainerRole::SlotContainer &&
            !SurfaceBuildsSlots(descriptor.surface))
            return false;
        if (descriptor.role !=
                WidgetContainerRole::SlotContainer &&
            SurfaceBuildsSlots(descriptor.surface))
            return false;
    }
    return true;
}

static_assert(
    WidgetRegistryIsComplete(),
    "Every DesktopWidgetType must declare its slot role and drag payload");
}
