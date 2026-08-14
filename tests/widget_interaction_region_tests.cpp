#include "widget_interaction_region.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using snowdesktop::widget_runtime::InteractionAction;
using snowdesktop::widget_runtime::InteractionRegion;
using snowdesktop::widget_runtime::InteractionShapeType;
using snowdesktop::widget_runtime::WidgetInteractionRegions;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

InteractionRegion Rect(std::string key, float x, float y,
    float width, float height)
{
    InteractionRegion region;
    region.key = std::move(key);
    region.shape.type = InteractionShapeType::Rect;
    region.shape.x = x;
    region.shape.y = y;
    region.shape.width = width;
    region.shape.height = height;
    return region;
}

void TestFrameTransactionAndStableState()
{
    WidgetInteractionRegions regions;
    std::string error;
    regions.BeginFrame();
    Check(regions.Submit(Rect("first", 0, 0, 50, 50), error),
        "valid region must stage");
    Check(regions.Submit(Rect("second", 25, 25, 50, 50), error),
        "second valid region must stage");
    regions.CommitFrame();

    Check(regions.TargetAt(30, 30) == "second",
        "last submitted overlapping region must be topmost");
    const auto entered = regions.UpdateHover(30, 30);
    Check(entered.leftKey.empty() && entered.enteredKey == "second" &&
            regions.IsHovered("second"),
        "hover must enter the topmost region");
    const std::uint64_t firstGeneration = regions.Generation();

    regions.BeginFrame();
    Check(regions.Submit(Rect("first", 0, 0, 50, 50), error) &&
            regions.Submit(Rect("second", 25, 25, 50, 50), error),
        "identical semantic frame must stage");
    regions.CommitFrame();
    Check(regions.Generation() == firstGeneration,
        "identical successful renders must retain the menu generation");

    regions.BeginFrame();
    Check(regions.Submit(Rect("first", 0, 0, 50, 50), error),
        "replacement frame must accept stable keys");
    regions.AbortFrame();
    Check(regions.IsHovered("second") && regions.TargetAt(30, 30) == "second",
        "aborted render must retain the last successful region set");

    regions.BeginFrame();
    Check(regions.Submit(Rect("first", 0, 0, 50, 50), error),
        "successful replacement must stage");
    const auto replaced = regions.CommitFrame();
    Check(replaced.leftKey == "second" && replaced.enteredKey == "first" &&
            regions.IsHovered("first"),
        "commit must reconcile hover against new geometry atomically");
    Check(regions.Generation() != firstGeneration,
        "semantic region changes must invalidate the prior menu generation");
}

void TestPointerPairingAndActions()
{
    WidgetInteractionRegions regions;
    std::string error;
    auto left = Rect("left", 0, 0, 40, 40);
    left.events.emplace("click", InteractionAction{ "left.open", {} });
    left.events.emplace("pointerLeave",
        InteractionAction{ "left.leave", {} });
    regions.BeginFrame();
    Check(regions.Submit(std::move(left), error),
        "action region must stage");
    Check(regions.Submit(Rect("right", 50, 0, 40, 40), error),
        "sibling region must stage");
    regions.CommitFrame();

    Check(regions.PointerDown(10, 10, 1).targetKey == "left" &&
            regions.IsPressed("left"),
        "pointer down must capture the stable target");
    Check(regions.PointerUp(60, 10, 1).clickTargetKey.empty(),
        "release on another target must not synthesize click");
    Check(regions.ConsumeClickTarget(60, 10).empty(),
        "cancelled click must not leak to a later callback");

    regions.PointerDown(10, 10, 1);
    Check(regions.PointerUp(10, 10, 1).clickTargetKey == "left" &&
            regions.ConsumeClickTarget(10, 10) == "left" &&
            regions.ConsumeClickTarget(10, 10).empty(),
        "matching down/up must produce exactly one click target");
    const auto* action = regions.FindAction("left", "click");
    Check(action && action->id == "left.open",
        "serialized action must remain attached to the stable region");

    regions.UpdateHover(10, 10);
    regions.BeginFrame();
    Check(regions.Submit(Rect("right", 50, 0, 40, 40), error),
        "replacement without hovered key must stage");
    const auto removed = regions.CommitFrame();
    const auto* leave = regions.FindTransitionAction(
        removed.leftKey, "pointerLeave");
    Check(removed.leftKey == "left" && removed.enteredKey.empty() &&
            leave && leave->id == "left.leave",
        "removed hovered region must retain its leave action for dispatch");
}

void TestShapesAndValidation()
{
    WidgetInteractionRegions regions;
    std::string error;
    regions.BeginFrame();
    auto circle = Rect("circle", 0, 0, 1, 1);
    circle.shape.type = InteractionShapeType::Circle;
    circle.shape.x = 20;
    circle.shape.y = 20;
    circle.shape.radius = 10;
    Check(regions.Submit(std::move(circle), error),
        "circle must accept a positive radius");
    auto rounded = Rect("rounded", 40, 10, 20, 20);
    rounded.shape.type = InteractionShapeType::RoundedRect;
    rounded.shape.radius = 8;
    Check(regions.Submit(std::move(rounded), error),
        "rounded rect must stage");
    Check(!regions.Submit(Rect("rounded", 0, 0, 2, 2), error) &&
            error.find("duplicate") != std::string::npos,
        "duplicate stable keys must be rejected in one frame");
    regions.CommitFrame();
    Check(regions.TargetAt(20, 20) == "circle" &&
            regions.TargetAt(30, 30).empty(),
        "circle hit testing must use the radius");
    Check(regions.TargetAt(41, 11).empty() &&
            regions.TargetAt(50, 20) == "rounded",
        "rounded corners must not behave as a plain bounding box");

    regions.BeginFrame();
    auto invalid = Rect("invalid", 0, 0, 0, 10);
    Check(!regions.Submit(std::move(invalid), error) &&
            !error.empty(),
        "non-positive geometry must be rejected");
    regions.AbortFrame();
}
}

int main()
{
    TestFrameTransactionAndStableState();
    TestPointerPairingAndActions();
    TestShapesAndValidation();
    std::cout << "widget interaction region tests passed\n";
    return 0;
}
