#include "core/slot_contract.h"

#include <array>
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

void TestRegistryContract()
{
    std::array<bool,
        contract::ToIndex(
            contract::SlotSurfaceKind::Count)> seen{};
    for (const auto& descriptor :
        contract::kSurfaceDescriptors)
    {
        const auto index =
            contract::ToIndex(descriptor.kind);
        Check(index < seen.size(),
            "registered surface index must be in range");
        if (index < seen.size())
        {
            Check(!seen[index],
                "each surface must be registered once");
            seen[index] = true;
        }
        Check(!descriptor.name.empty(),
            "each surface must have a diagnostic name");
        if (descriptor.buildsSlots)
            Check(!descriptor.externalBoundary,
                "an in-process slot surface cannot be an external boundary");
    }
    for (bool registered : seen)
        Check(registered,
            "every SlotSurfaceKind must be registered");

    std::array<bool,
        contract::ToIndex(
            DesktopWidgetType::Count)> widgetSeen{};
    for (const auto& descriptor :
        contract::kWidgetContractDescriptors)
    {
        const auto index =
            contract::ToIndex(descriptor.type);
        Check(index < widgetSeen.size(),
            "registered widget type index must be in range");
        if (index < widgetSeen.size())
        {
            Check(!widgetSeen[index],
                "each DesktopWidgetType must be registered once");
            widgetSeen[index] = true;
        }
        const bool shouldBuildSlots =
            descriptor.role ==
                contract::WidgetContainerRole::
                    SlotContainer;
        Check(
            contract::SurfaceBuildsSlots(
                descriptor.surface) ==
                shouldBuildSlots,
            std::string(descriptor.name) +
            ": widget role and slot surface must agree");
    }
    for (bool registered : widgetSeen)
        Check(registered,
            "every DesktopWidgetType must declare a slot contract");
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
        surfaceCount> pairCovered{};
    std::array<bool, payloadCount> payloadCovered{};

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

            for (std::size_t payloadIndex = 0;
                payloadIndex < payloadCount;
                ++payloadIndex)
            {
                const auto payload =
                    static_cast<Payload>(payloadIndex);
                for (const auto relation : relations)
                {
                    const Route route =
                        contract::EvaluateSlotDrop(
                            source, payload,
                            target, relation);
                    pairCovered[sourceIndex][targetIndex] =
                        true;
                    payloadCovered[payloadIndex] = true;

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
                    Check(
                        contract::AcceptsSlotDrop(
                            source, payload,
                            target, relation) ==
                            (route != Route::Reject),
                        PairName(source, target) +
                        ": acceptance and route must agree");
                }
            }
        }
    }

    for (const auto& row : pairCovered)
        for (bool covered : row)
            Check(covered,
                "every directed source/target pair must be exercised");
    for (bool covered : payloadCovered)
        Check(covered,
            "every payload kind must be exercised");
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
        !contract::AcceptsSlotDrop(
            Surface::FolderMapping,
            Payload::FolderEntry,
            Surface::Dock,
            Relation::CrossSurface),
        "folder entries must not reuse Dock desktop-entry slots");
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
    TestRegistryContract();
    TestPayloadClassificationIsExclusive();
    TestEveryDirectedPairAndPayload();
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
        << "All directed slot contract tests passed\n";
    return 0;
}
