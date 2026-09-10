#include "core/slot_contract.h"
#include "slot_drop_expectations.h"

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
    namespace expected = slot_drop_expectations;
    std::size_t cases = 0;
    for (std::size_t source = 0; source < expected::surfaces.size(); ++source)
    {
        for (std::size_t payload = 0; payload < expected::payloads.size(); ++payload)
        {
            Check(contract::SurfaceEmits(expected::surfaces[source],
                    expected::payloads[payload]) == expected::emits[source][payload],
                "source payload policy: " + std::to_string(source) + "/" +
                    std::to_string(payload));
            for (std::size_t target = 0; target < expected::surfaces.size(); ++target)
            {
                for (const auto relation : expected::relations)
                {
                    const auto wanted = expected::ExpectedRoute(
                        source, payload, target, relation);
                    const auto actual = contract::EvaluateSlotDrop(
                        expected::surfaces[source], expected::payloads[payload],
                        expected::surfaces[target], relation);
                    const std::string context =
                        PairName(expected::surfaces[source], expected::surfaces[target]) +
                        " payload=" + std::to_string(payload) +
                        " relation=" + std::to_string(static_cast<int>(relation));
                    Check(actual == wanted, context +
                        " expected route=" + std::to_string(static_cast<int>(wanted)) +
                        " actual=" + std::to_string(static_cast<int>(actual)));
                    Check(contract::AcceptsSlotDrop(expected::surfaces[source],
                            expected::payloads[payload], expected::surfaces[target],
                            relation) == (wanted != contract::DropRoute::Reject),
                        context + ": acceptance must agree with the expected operation");
                    if (wanted != contract::DropRoute::Reject)
                        Check(contract::SurfaceSupportsRoute(
                                expected::surfaces[target], wanted),
                            context + ": the expected operation needs all target phases");
                    ++cases;
                }
            }
        }
    }
    std::cout << cases << " explicit route expectations checked\n";
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

}

int main()
{
    TestPayloadClassificationIsExclusive();
    TestEveryDirectedPairAndPayload();
    TestSurfaceGeometryMatrix();
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
