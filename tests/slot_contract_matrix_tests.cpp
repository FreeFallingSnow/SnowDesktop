#include "core/slot_contract.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string>

namespace contract = snowdesktop::slot_contract;

namespace
{
int failures = 0;

void Check(bool condition, const std::string& message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

std::string PairName(
    contract::SlotSurfaceKind source,
    contract::SlotSurfaceKind target)
{
    return std::string(contract::Describe(source).name) +
        " -> " +
        std::string(contract::Describe(target).name);
}

void TestEveryDirectedPairAndPayload()
{
    using Surface = contract::SlotSurfaceKind;
    using Payload = contract::DragPayloadKind;
    using Route = contract::DropRoute;

    constexpr std::size_t surfaceCount =
        contract::ToIndex(Surface::Count);
    constexpr std::size_t payloadCount =
        contract::ToIndex(Payload::Count);
    std::array<std::array<bool, surfaceCount>,
        surfaceCount> nativeRouteCovered{};

    for (std::size_t sourceIndex = 0;
        sourceIndex < surfaceCount;
        ++sourceIndex)
    {
        const auto source =
            static_cast<Surface>(sourceIndex);
        for (std::size_t targetIndex = 0;
            targetIndex < surfaceCount;
            ++targetIndex)
        {
            const auto target =
                static_cast<Surface>(targetIndex);
            const bool sameSurface =
                source == target &&
                source != Surface::External;
            const std::array relations{
                contract::ClassifyRelation(
                    source, target, sameSurface),
                contract::ClassifyRelation(
                    source, target, false),
            };
            const size_t relationCount =
                relations[0] == relations[1]
                ? 1
                : relations.size();

            for (std::size_t payloadIndex = 0;
                payloadIndex < payloadCount;
                ++payloadIndex)
            {
                const auto payload =
                    static_cast<Payload>(payloadIndex);
                for (size_t relationIndex = 0;
                    relationIndex < relationCount;
                    ++relationIndex)
                {
                    const auto relation =
                        relations[relationIndex];
                    const Route route =
                        contract::EvaluateSlotDrop(
                            source, payload,
                            target, relation);

                    if (contract::SurfaceEmits(
                            source, payload) &&
                        route != Route::Reject)
                        nativeRouteCovered
                            [sourceIndex][targetIndex] = true;

                    if (!contract::SurfaceEmits(
                            source, payload))
                    {
                        Check(route == Route::Reject,
                            PairName(source, target) +
                            ": a surface must not emit an unregistered payload");
                    }
                    if (target == Surface::Guide)
                    {
                        Check(route == Route::Reject,
                            PairName(source, target) +
                            ": the guide must never accept a drop");
                    }
                    if (route != Route::Reject)
                    {
                        Check(
                            contract::SurfaceSupportsRoute(
                                target, route),
                            PairName(source, target) +
                            ": every accepted route must provide all preview/commit/insertion phases");
                        Check(
                            contract::SurfaceSupports(
                                target,
                                contract::InteractionCapability::Commit),
                            PairName(source, target) +
                            ": every accepted target must implement commit");
                        if (contract::RouteRequiresInsertionIndicator(route))
                        {
                            Check(
                                contract::SurfaceSupports(
                                    target,
                                    contract::InteractionCapability::InsertionIndicator),
                                PairName(source, target) +
                                ": sortable routes must implement an insertion indicator");
                        }
                        if (relation == contract::DragRelation::CrossSurface ||
                            relation == contract::DragRelation::ExternalIngress ||
                            relation == contract::DragRelation::ExternalEgress)
                        {
                            Check(
                                contract::SurfaceSupports(
                                    source,
                                    contract::InteractionCapability::CrossDisplayCoordinates) &&
                                contract::SurfaceSupports(
                                    target,
                                    contract::InteractionCapability::CrossDisplayCoordinates),
                                PairName(source, target) +
                                ": cross-surface routes must own coordinate conversion");
                        }
                    }
                }
            }
        }
    }

    for (const auto& source :
         contract::kSurfaceDescriptors)
    {
        if (!source.buildsSlots) continue;
        for (const auto& target :
             contract::kSurfaceDescriptors)
        {
            if (!target.buildsSlots) continue;
            Check(
                nativeRouteCovered
                    [contract::ToIndex(source.kind)]
                    [contract::ToIndex(target.kind)],
                PairName(source.kind, target.kind) +
                ": every registered slot pair must declare at least one accepted native-payload route");
        }
    }
}

void TestSurfaceGeometryMatrix()
{
    using Capability = contract::InteractionCapability;
    using Surface = contract::SlotSurfaceKind;

    constexpr std::array origins{
        contract::SurfacePoint{-2560.0, -240.0},
        contract::SurfacePoint{0.0, 0.0},
        contract::SurfacePoint{3840.0, 360.0},
    };
    constexpr std::array scales{
        0.75, 1.0, 1.25, 1.5, 2.0,
    };
    constexpr contract::SurfacePoint localPoint{
        37.25, 61.5
    };

    for (const auto& target :
        contract::kSurfaceDescriptors)
    {
        if (!target.buildsSlots) continue;
        Check(
            contract::SurfaceSupports(
                target.kind,
                Capability::ParentVisualMetrics),
            std::string(target.name) +
            ": slot visuals must inherit their current parent metrics");
        Check(
            contract::SurfaceSupports(
                target.kind,
                Capability::CrossDisplayCoordinates),
            std::string(target.name) +
            ": slot coordinates must support monitor-origin changes");

        for (const auto& origin : origins)
        {
            for (double scale : scales)
            {
                const contract::SurfaceFrame frame{
                    origin.x, origin.y, scale
                };
                const auto screen =
                    contract::ParentToScreen(
                        localPoint, frame);
                const auto restored =
                    contract::ScreenToParent(
                        screen, frame);
                Check(
                    std::abs(restored.x - localPoint.x) < 0.0001 &&
                    std::abs(restored.y - localPoint.y) < 0.0001,
                    std::string(target.name) +
                    ": parent/screen conversion must round-trip for every monitor and DPI frame");
            }
        }

        for (const auto& source :
            contract::kSurfaceDescriptors)
        {
            for (double sourceScale : scales)
            {
                for (double targetScale : scales)
                {
                    Check(
                        contract::ResolvePreviewScale(
                            target.kind,
                            sourceScale,
                            targetScale) == targetScale,
                        PairName(source.kind, target.kind) +
                        ": drag preview size must follow the target parent rather than its old source");
                }
            }
        }
    }

    Check(
        contract::ResolvePreviewScale(
            Surface::External, 1.5, 2.0) == 1.5,
        "external egress keeps source metrics because it has no SnowDesktop parent");
}

void TestPayloadClassificationIsExclusive()
{
    using Payload = contract::DragPayloadKind;
    constexpr unsigned familyCount = 6;
    for (unsigned mask = 0;
        mask < (1u << familyCount);
        ++mask)
    {
        const bool desktop =
            (mask & (1u << 0)) != 0;
        const bool folder =
            (mask & (1u << 1)) != 0;
        const bool external =
            (mask & (1u << 2)) != 0;
        const bool widgets =
            (mask & (1u << 3)) != 0;
        const bool collectionLabel =
            (mask & (1u << 4)) != 0;
        const bool fileLabel =
            (mask & (1u << 5)) != 0;
        const unsigned active =
            static_cast<unsigned>(desktop) +
            static_cast<unsigned>(folder) +
            static_cast<unsigned>(external) +
            static_cast<unsigned>(widgets) +
            static_cast<unsigned>(collectionLabel) +
            static_cast<unsigned>(fileLabel);

        const auto ordinaryWidget =
            contract::ClassifyPayload({
                desktop, folder, external,
                widgets, false,
                collectionLabel, fileLabel,
            });
        Check(
            (ordinaryWidget != Payload::Count) ==
                (active == 1),
            "mixed drag families must be rejected instead of borrowing another payload type");

        const auto collectionWidget =
            contract::ClassifyPayload({
                desktop, folder, external,
                widgets, true,
                collectionLabel, fileLabel,
            });
        Check(
            (collectionWidget ==
                Payload::CollectionWidget) ==
                (active == 1 && widgets),
            "collection-widget classification must require an exclusive widget payload");

        const auto folderMappingWidget =
            contract::ClassifyPayload({
                desktop, folder, external,
                widgets, false,
                collectionLabel, fileLabel,
                true,
            });
        Check(
            (folderMappingWidget ==
                Payload::FolderMappingWidget) ==
                (active == 1 && widgets),
            "folder-mapping classification must require an exclusive widget payload");
    }
}

void TestSameComponentAndSameTypeRules()
{
    using Surface = contract::SlotSurfaceKind;
    using Payload = contract::DragPayloadKind;
    using Relation = contract::DragRelation;
    using Route = contract::DropRoute;

    for (const auto& descriptor :
        contract::kSurfaceDescriptors)
    {
        if (!descriptor.buildsSlots)
            continue;
        for (std::size_t payloadIndex = 0;
            payloadIndex <
                contract::ToIndex(Payload::Count);
            ++payloadIndex)
        {
            const auto payload =
                static_cast<Payload>(payloadIndex);
            if (!contract::SurfaceEmits(
                    descriptor.kind, payload))
                continue;

            const Route internal =
                contract::EvaluateSlotDrop(
                    descriptor.kind, payload,
                    descriptor.kind,
                    Relation::SameInstance);
            const Route peer =
                contract::EvaluateSlotDrop(
                    descriptor.kind, payload,
                    descriptor.kind,
                    Relation::SameSurface);

            Check(internal != Route::Reject,
                std::string(descriptor.name) +
                ": every native payload needs an explicit same-component route");
            Check(peer != Route::Reject,
                std::string(descriptor.name) +
                ": every native payload needs an explicit same-type peer route");
        }
    }
}

void TestTypeIsolationRules()
{
    using Surface = contract::SlotSurfaceKind;
    using Payload = contract::DragPayloadKind;
    using Relation = contract::DragRelation;

    for (const auto& target :
        contract::kSurfaceDescriptors)
    {
        const bool collectionLabelAccepted =
            contract::AcceptsSlotDrop(
                Surface::CollectionGroup,
                Payload::CollectionGroupLabel,
                target.kind,
                contract::ClassifyRelation(
                    Surface::CollectionGroup,
                    target.kind, false));
        Check(collectionLabelAccepted ==
                (target.kind == Surface::Desktop ||
                 target.kind == Surface::CollectionGroup),
            std::string(target.name) +
            ": collection labels must use only collection-label targets");

        const bool fileLabelAccepted =
            contract::AcceptsSlotDrop(
                Surface::FileGroup,
                Payload::FileGroupLabel,
                target.kind,
                contract::ClassifyRelation(
                    Surface::FileGroup,
                    target.kind, false));
        Check(fileLabelAccepted ==
                (target.kind == Surface::Desktop ||
                 target.kind == Surface::FileGroup),
            std::string(target.name) +
            ": file-source labels must use only file-label targets");
    }

    Check(
        contract::EvaluateSlotDrop(
            Surface::FolderMapping,
            Payload::FolderEntry,
            Surface::Dock,
            Relation::CrossSurface) ==
            contract::DropRoute::AddToDock,
        "folder entries dragged to Dock must be materialized as safe shortcut mappings");
    Check(
        contract::AcceptsSlotDrop(
            Surface::Desktop,
            Payload::CollectionWidget,
            Surface::CollectionGroup,
            Relation::CrossSurface) &&
        !contract::AcceptsSlotDrop(
            Surface::Desktop,
            Payload::CollectionWidget,
            Surface::FileGroup,
            Relation::CrossSurface),
        "collection widgets must only enter collection groups");
    Check(
        contract::AcceptsSlotDrop(
            Surface::Desktop,
            Payload::FileSourceWidget,
            Surface::FileGroup,
            Relation::CrossSurface) &&
        !contract::AcceptsSlotDrop(
            Surface::Desktop,
            Payload::FileSourceWidget,
            Surface::CollectionGroup,
            Relation::CrossSurface),
        "file-source widgets must only enter file groups");

    Check(
        contract::AcceptsSlotDrop(
            Surface::Desktop,
            Payload::FolderMappingWidget,
            Surface::Dock,
            Relation::CrossSurface) &&
        contract::AcceptsSlotDrop(
            Surface::FileGroup,
            Payload::FolderMappingWidget,
            Surface::Dock,
            Relation::CrossSurface) &&
        contract::EvaluateSlotDrop(
            Surface::Dock,
            Payload::FolderMappingWidget,
            Surface::Dock,
            Relation::SameInstance) ==
            contract::DropRoute::ReorderWithinContainer,
        "folder mappings must enter and reorder within the Dock");
    Check(
        contract::EvaluateSlotDrop(
            Surface::FileGroup,
            Payload::FolderMappingWidget,
            Surface::Desktop,
            Relation::CrossSurface) ==
            contract::DropRoute::PlaceOnDesktop &&
        contract::EvaluateSlotDrop(
            Surface::Dock,
            Payload::FolderMappingWidget,
            Surface::FileGroup,
            Relation::CrossSurface) ==
            contract::DropRoute::MoveFileSourceIntoGroup &&
        contract::EvaluateSlotDrop(
            Surface::FileGroup,
            Payload::FolderMappingWidget,
            Surface::FileGroup,
            Relation::SameInstance) ==
            contract::DropRoute::MoveFileSourceIntoGroup,
        "folder mappings must move between Desktop, Dock, and FileGroup without changing payload family");
    Check(
        !contract::AcceptsSlotDrop(
            Surface::Desktop,
            Payload::FolderMappingWidget,
            Surface::CollectionGroup,
            Relation::CrossSurface),
        "folder mappings must not enter collection groups");
}

void TestExternalIngressAndEgress()
{
    using Surface = contract::SlotSurfaceKind;
    using Payload = contract::DragPayloadKind;
    using Relation = contract::DragRelation;

    for (const auto& target :
        contract::kSurfaceDescriptors)
    {
        const bool accepted =
            contract::AcceptsSlotDrop(
                Surface::External,
                Payload::ExternalFile,
                target.kind,
                Relation::ExternalIngress);
        Check(accepted == target.buildsSlots,
            std::string(target.name) +
            ": external files must be accepted exactly by real slot surfaces");
    }

    for (const auto& source :
        contract::kSurfaceDescriptors)
    {
        for (std::size_t payloadIndex = 0;
            payloadIndex <
                contract::ToIndex(Payload::Count);
            ++payloadIndex)
        {
            const auto payload =
                static_cast<Payload>(payloadIndex);
            if (!contract::SurfaceEmits(
                    source.kind, payload))
                continue;
            const bool expected =
                payload == Payload::DesktopItem ||
                payload == Payload::FolderEntry;
            Check(
                contract::AcceptsSlotDrop(
                    source.kind, payload,
                    Surface::External,
                    Relation::ExternalEgress) ==
                    expected,
                std::string(source.name) +
                ": external drag-out must be limited to file-backed payloads");
        }
    }
}
}

int main()
{
    TestPayloadClassificationIsExclusive();
    TestEveryDirectedPairAndPayload();
    TestSurfaceGeometryMatrix();
    TestSameComponentAndSameTypeRules();
    TestTypeIsolationRules();
    TestExternalIngressAndEgress();
    if (failures != 0)
    {
        std::cerr << failures
            << " slot contract test(s) failed\n";
        return 1;
    }
    std::cout
        << "All directed slot contract matrix tests passed\n";
    return 0;
}
