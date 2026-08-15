#include "widget_interaction_region.h"

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>

namespace
{
using snowdesktop::widget_runtime::InteractionAction;
using snowdesktop::widget_runtime::InteractionControlKind;
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
    regions.PointerDown(10, 10, 1);
    regions.CancelPointerPress();
    Check(!regions.IsPressed("left") &&
            regions.PointerUp(10, 10, 1).clickTargetKey.empty() &&
            regions.ConsumeClickTarget(10, 10).empty(),
        "a host-owned drag takeover must cancel pressed and click state");
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

    regions.BeginFrame();
    auto oversizedTooltip = Rect("oversized-tooltip", 0, 0, 10, 10);
    oversizedTooltip.tooltip.assign(4097, 'x');
    Check(!regions.Submit(std::move(oversizedTooltip), error) &&
            error.find("tooltip") != std::string::npos,
        "interaction tooltips must enforce the bounded text quota");
    regions.AbortFrame();
}

void TestClippedHitTesting()
{
    WidgetInteractionRegions regions;
    std::string error;
    auto clipped = Rect("clipped", 0, 0, 100, 100);
    clipped.clip = snowdesktop::widget_runtime::InteractionClipRect{
        20, 20, 40, 40 };
    regions.BeginFrame();
    Check(regions.Submit(std::move(clipped), error),
        "a bounded interaction clip must stage");
    regions.CommitFrame();
    Check(regions.TargetAt(30, 30) == "clipped" &&
            regions.TargetAt(10, 10).empty() &&
            regions.TargetAt(80, 80).empty(),
        "hit testing must intersect a region shape with its ancestor clip");

    auto invalid = Rect("invalid-clip", 0, 0, 20, 20);
    invalid.clip = snowdesktop::widget_runtime::InteractionClipRect{
        0, 0, 0, 10 };
    regions.BeginFrame();
    Check(!regions.Submit(std::move(invalid), error) &&
            error.find("clip") != std::string::npos,
        "empty interaction clips must be rejected");
    regions.AbortFrame();
}

void TestControlledActionResolution()
{
    WidgetInteractionRegions regions;
    std::string error;
    auto toggle = Rect("enabled", 0, 0, 80, 32);
    toggle.controlKind = InteractionControlKind::Toggle;
    toggle.checked = false;
    toggle.events.emplace("change",
        InteractionAction{ "enabled.change", {} });
    regions.BeginFrame();
    Check(regions.Submit(std::move(toggle), error),
        "a controlled region with change must stage");
    regions.CommitFrame();

    const auto resolved = regions.ResolveAction("enabled", "click");
    Check(resolved && resolved->eventName == "change" &&
            resolved->action.id == "enabled.change" &&
            resolved->previousChecked == false &&
            resolved->checked == true,
        "a control click must resolve to change with a proposed value");
    Check(!regions.ResolveAction("enabled", "doubleClick"),
        "a control must not synthesize unbound pointer actions");

    regions.BeginFrame();
    auto mixed = Rect("mixed", 0, 40, 80, 32);
    mixed.controlKind = InteractionControlKind::Checkbox;
    mixed.indeterminate = true;
    mixed.events.emplace("change",
        InteractionAction{ "mixed.change", {} });
    Check(regions.Submit(std::move(mixed), error),
        "an indeterminate checkbox region must stage");
    regions.CommitFrame();
    const auto resolvedMixed = regions.ResolveAction("mixed", "click");
    Check(resolvedMixed && resolvedMixed->previousChecked == false &&
            resolvedMixed->checked == true &&
            resolvedMixed->previousIndeterminate == true &&
            resolvedMixed->indeterminate == false,
        "an indeterminate checkbox must propose a checked determinate state");

    regions.BeginFrame();
    auto invalidControl = Rect("bad-control", 0, 0, 80, 32);
    invalidControl.controlKind = InteractionControlKind::Checkbox;
    invalidControl.events.emplace("click",
        InteractionAction{ "bad.click", {} });
    Check(!regions.Submit(std::move(invalidControl), error) &&
            error.find("require change") != std::string::npos,
        "controlled regions must reject click bindings");
    auto invalidPlain = Rect("bad-plain", 0, 0, 80, 32);
    invalidPlain.events.emplace("change",
        InteractionAction{ "bad.change", {} });
    Check(!regions.Submit(std::move(invalidPlain), error) &&
            error.find("reserved") != std::string::npos,
        "plain regions must reject controlled change bindings");
    regions.AbortFrame();
}

void TestRadioAndSliderActionResolution()
{
    WidgetInteractionRegions regions;
    std::string error;
    auto radio = Rect("theme/dark", 0, 0, 100, 32);
    radio.controlKind = InteractionControlKind::Radio;
    radio.currentSelection = "light";
    radio.proposedSelection = "dark";
    radio.events.emplace("change",
        InteractionAction{ "theme.change", {} });
    auto slider = Rect("volume", 0, 40, 100, 24);
    slider.controlKind = InteractionControlKind::Slider;
    slider.controlValue = 4.0f;
    slider.minimum = 0.0f;
    slider.maximum = 10.0f;
    slider.step = 2.0f;
    slider.events.emplace("change",
        InteractionAction{ "volume.change", {} });
    regions.BeginFrame();
    Check(regions.Submit(std::move(radio), error) &&
            regions.Submit(std::move(slider), error),
        "radio and slider controlled regions must stage");
    regions.CommitFrame();

    const auto selection = regions.ResolveAction("theme/dark", "click");
    Check(selection && selection->eventName == "change" &&
            selection->previousSelection == "light" &&
            selection->selection == "dark",
        "radio clicks must propose the selected option value");

    regions.PointerDown(55.0f, 52.0f, 1);
    const auto value = regions.ResolveAction(
        "volume", "pointerDown", 55.0f, 52.0f, 1);
    Check(value && value->eventName == "change" &&
            value->previousControlValue &&
            std::abs(*value->previousControlValue - 4.0f) < 0.01f &&
            value->controlValue &&
            std::abs(*value->controlValue - 6.0f) < 0.01f,
        "slider pointer down must propose a step-rounded value");
    Check(regions.PointerMoveTarget(140.0f, 52.0f) == "volume",
        "an active slider drag must retain pointer capture outside bounds");
    const auto dragged = regions.ResolveAction(
        "volume", "pointerMove", 140.0f, 52.0f, 1);
    Check(dragged && dragged->controlValue &&
            std::abs(*dragged->controlValue - 10.0f) < 0.01f,
        "captured slider movement must clamp to the declared maximum");
    regions.PointerUp(140.0f, 52.0f, 1);
    Check(regions.PointerMoveTarget(140.0f, 52.0f).empty(),
        "slider capture must end on pointer up");

    const auto steppedUp = regions.ResolveKeyboardStep("volume", 1);
    const auto steppedDown = regions.ResolveKeyboardStep("volume", -1);
    Check(steppedUp && steppedUp->eventName == "change" &&
            steppedUp->previousControlValue && steppedUp->controlValue &&
            std::abs(*steppedUp->previousControlValue - 4.0f) < 0.01f &&
            std::abs(*steppedUp->controlValue - 6.0f) < 0.01f &&
            steppedDown && steppedDown->controlValue &&
            std::abs(*steppedDown->controlValue - 2.0f) < 0.01f,
        "slider keyboard steps must propose one bounded declared step");

    const auto accessibleValue =
        regions.ResolveRangeValue("volume", 7.1f);
    Check(accessibleValue && accessibleValue->eventName == "change" &&
            accessibleValue->previousControlValue == 4.0f &&
            accessibleValue->controlValue == 8.0f &&
            !regions.ResolveRangeValue("volume", 12.0f),
        "accessibility range values must snap to step and reject bounds violations");

    regions.PointerDown(20.0f, 52.0f, 2);
    Check(!regions.ResolveAction(
            "volume", "pointerDown", 20.0f, 52.0f, 2),
        "right button presses must not change slider values");
    regions.PointerUp(20.0f, 52.0f, 2);
}

void TestCollectionSelectionResolution()
{
    WidgetInteractionRegions regions;
    std::string error;
    auto single = Rect("mail:b", 0, 0, 100, 32);
    single.controlKind = InteractionControlKind::SelectionSingle;
    single.currentSelectedKeys = { "mail:a" };
    single.proposedSelectedKey = "mail:b";
    single.events.emplace("change",
        InteractionAction{ "mail.select", {} });
    auto multiple = Rect("tag:b", 0, 40, 100, 32);
    multiple.controlKind = InteractionControlKind::SelectionMultiple;
    multiple.currentSelectedKeys = { "tag:a", "tag:b" };
    multiple.proposedSelectedKey = "tag:b";
    multiple.events.emplace("change",
        InteractionAction{ "tag.select", {} });
    auto add = Rect("tag:c", 0, 80, 100, 32);
    add.controlKind = InteractionControlKind::SelectionMultiple;
    add.currentSelectedKeys = { "tag:a", "tag:b" };
    add.proposedSelectedKey = "tag:c";
    add.events.emplace("change",
        InteractionAction{ "tag.select", {} });
    regions.BeginFrame();
    Check(regions.Submit(std::move(single), error) &&
            regions.Submit(std::move(multiple), error) &&
            regions.Submit(std::move(add), error),
        "single and multiple collection regions must stage");
    regions.CommitFrame();

    const auto singleResult = regions.ResolveAction("mail:b", "click");
    Check(singleResult && singleResult->eventName == "change" &&
            singleResult->previousSelectedKeys ==
                std::vector<std::string>{ "mail:a" } &&
            singleResult->selectedKeys ==
                std::vector<std::string>{ "mail:b" },
        "single selection must propose replacing the controlled key");
    const auto removeResult = regions.ResolveAction("tag:b", "click");
    Check(removeResult && removeResult->previousSelectedKeys ==
                std::vector<std::string>{ "tag:a", "tag:b" } &&
            removeResult->selectedKeys ==
                std::vector<std::string>{ "tag:a" },
        "multiple selection must propose removing an active key");
    const auto addResult = regions.ResolveAction("tag:c", "click");
    Check(addResult && addResult->selectedKeys ==
                std::vector<std::string>{ "tag:a", "tag:b", "tag:c" },
        "multiple selection must append a newly selected key");
}

void TestKeyboardFocusableOrderAndFiltering()
{
    WidgetInteractionRegions regions;
    std::string error;
    auto button = Rect("button", 0, 0, 80, 32);
    button.events.emplace("click", InteractionAction{ "button.open", {} });
    button.tabIndex = 3;
    auto input = Rect("query", 0, 40, 120, 32);
    input.accessibilityRole = "searchbox";
    input.tabIndex = -1;
    auto slider = Rect("volume", 0, 80, 120, 24);
    slider.controlKind = InteractionControlKind::Slider;
    slider.accessibilityRole = "slider";
    slider.accessibilityLabel = "Volume";
    slider.events.emplace("change",
        InteractionAction{ "volume.change", {} });
    slider.tabIndex = 1;
    auto pointerOnly = Rect("pointer", 0, 112, 80, 32);
    pointerOnly.events.emplace("pointerMove",
        InteractionAction{ "pointer.move", {} });
    pointerOnly.focusable = true;
    pointerOnly.tabIndex = 2;
    auto disabled = Rect("disabled", 0, 152, 80, 32);
    disabled.events.emplace("click",
        InteractionAction{ "disabled.open", {} });
    disabled.enabled = false;

    regions.BeginFrame();
    Check(regions.Submit(std::move(button), error) &&
            regions.Submit(std::move(input), error) &&
            regions.Submit(std::move(slider), error) &&
            regions.Submit(std::move(pointerOnly), error) &&
            regions.Submit(std::move(disabled), error),
        "keyboard focus fixtures must stage");
    regions.CommitFrame();

    const auto keys = regions.KeyboardFocusableKeys();
    Check(keys.size() == 3 && keys[0] == "volume" &&
            keys[1] == "pointer" && keys[2] == "button",
        "positive tab indices must sort before source-order nodes and -1 must skip traversal");
    Check(regions.IsKeyboardFocusable("query") &&
            regions.IsKeyboardFocusable("pointer") &&
            !regions.IsKeyboardFocusable("disabled") &&
            !regions.IsKeyboardFocusable("missing"),
        "focusability must remain independent from sequential tab inclusion");

    const auto semantics = regions.AccessibilityRegions();
    Check(semantics.size() == 2 && semantics[0].key == "query" &&
            semantics[1].key == "volume",
        "accessibility snapshots must omit regions without declared semantics");
}
}

int main()
{
    TestFrameTransactionAndStableState();
    TestPointerPairingAndActions();
    TestShapesAndValidation();
    TestClippedHitTesting();
    TestControlledActionResolution();
    TestRadioAndSliderActionResolution();
    TestCollectionSelectionResolution();
    TestKeyboardFocusableOrderAndFiltering();
    std::cout << "widget interaction region tests passed\n";
    return 0;
}
