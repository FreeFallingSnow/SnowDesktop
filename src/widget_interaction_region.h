#pragma once

#include <map>
#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct InteractionValue
{
    enum class Type
    {
        Null,
        Boolean,
        Integer,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool boolean = false;
    long long integer = 0;
    double number = 0.0;
    std::string string;
    std::vector<InteractionValue> array;
    std::map<std::string, InteractionValue, std::less<>> object;

    bool operator==(const InteractionValue&) const = default;
};

struct InteractionAction
{
    enum class ContextMenuScope
    {
        Element,
        Component,
    };

    std::string id;
    InteractionValue value;
    ContextMenuScope contextMenuScope = ContextMenuScope::Element;

    bool operator==(const InteractionAction&) const = default;
};

enum class InteractionControlKind
{
    None,
    Toggle,
    Checkbox,
    Radio,
    SelectionSingle,
    SelectionMultiple,
    Slider,
};

struct InteractionResolvedAction
{
    InteractionAction action;
    std::string eventName;
    std::optional<bool> previousChecked;
    std::optional<bool> checked;
    std::optional<bool> previousIndeterminate;
    std::optional<bool> indeterminate;
    std::optional<float> previousControlValue;
    std::optional<float> controlValue;
    std::optional<std::string> previousSelection;
    std::optional<std::string> selection;
    std::optional<std::vector<std::string>> previousSelectedKeys;
    std::optional<std::vector<std::string>> selectedKeys;
    std::optional<bool> previousExpanded;
    std::optional<bool> expanded;
};

enum class InteractionShapeType
{
    Rect,
    RoundedRect,
    Circle,
};

struct InteractionShape
{
    InteractionShapeType type = InteractionShapeType::Rect;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float radius = 0.0f;

    bool Contains(float pointX, float pointY) const noexcept;
    bool operator==(const InteractionShape&) const = default;
};

struct InteractionClipRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool Contains(float pointX, float pointY) const noexcept
    {
        return pointX >= x && pointY >= y &&
            pointX <= x + width && pointY <= y + height;
    }

    bool operator==(const InteractionClipRect&) const = default;
};

struct InteractionAffineTransform
{
    // Direct2D-compatible row-vector affine matrix:
    // x' = x*m11 + y*m21 + dx; y' = x*m12 + y*m22 + dy.
    float m11 = 1.0f;
    float m12 = 0.0f;
    float m21 = 0.0f;
    float m22 = 1.0f;
    float dx = 0.0f;
    float dy = 0.0f;

    bool operator==(const InteractionAffineTransform&) const = default;
};

struct InteractionRegion
{
    std::string key;
    InteractionShape shape;
    // Optional exact hit fragments for wrapped inline content. `shape` remains
    // the stable union bounds used by accessibility and diagnostics.
    std::vector<InteractionShape> hitFragments;
    // Declarative affine transforms retain local geometry for exact inverse
    // hit testing while `shape`/`hitFragments` expose transformed AABBs.
    std::optional<InteractionShape> localHitShape;
    std::vector<InteractionShape> localHitFragments;
    std::optional<InteractionAffineTransform> hitTransform;
    std::optional<InteractionClipRect> clip;
    std::string cursor;
    std::string tooltipTitle;
    std::string tooltip;
    std::string accessKey;
    std::string acceleratorText;
    std::map<std::string, InteractionAction, std::less<>> events;
    std::string accessibilityRole;
    std::string accessibilityLabel;
    bool capturePointer = false;
    InteractionControlKind controlKind = InteractionControlKind::None;
    bool checked = false;
    bool indeterminate = false;
    float controlValue = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.01f;
    float controlStart = 0.0f;
    float controlLength = 0.0f;
    bool hasControlAxis = false;
    float controlStartX = 0.0f;
    float controlStartY = 0.0f;
    float controlEndX = 0.0f;
    float controlEndY = 0.0f;
    bool vertical = false;
    std::string currentSelection;
    std::string proposedSelection;
    std::vector<std::string> currentSelectedKeys;
    std::string proposedSelectedKey;
    bool hasExpandedProposal = false;
    bool expanded = false;
    bool enabled = true;
    std::optional<bool> focusable;
    int tabIndex = 0;

    bool operator==(const InteractionRegion&) const = default;
};

struct InteractionHoverTransition
{
    std::string leftKey;
    std::string enteredKey;

    bool Changed() const noexcept
    {
        return leftKey != enteredKey;
    }
};

struct InteractionPointerResult
{
    std::string targetKey;
    std::string clickTargetKey;
};

/**
 * Host-owned, frame-transactional hit regions for immediate-mode widgets.
 * A failed render can abort its staging set without disturbing the last
 * successfully rendered geometry or stable-key pointer state.
 */
class WidgetInteractionRegions
{
public:
    static constexpr std::size_t kMaximumRegions = 256;
    static constexpr std::size_t kMaximumHitFragments = 64;

    void BeginFrame();
    bool Submit(InteractionRegion region, std::string& error);
    InteractionHoverTransition CommitFrame();
    void AbortFrame() noexcept;

    InteractionHoverTransition UpdateHover(float x, float y);
    InteractionHoverTransition ClearHover();
    InteractionPointerResult PointerDown(float x, float y, int button);
    InteractionPointerResult PointerUp(float x, float y, int button);
    void CancelPointerPress() noexcept;
    bool HasPointerCapture() const noexcept;
    std::string ConsumeClickTarget(float x, float y);
    std::string TargetAt(float x, float y) const;
    std::string PointerMoveTarget(float x, float y);

    const InteractionRegion* Find(std::string_view key) const noexcept;
    const InteractionAction* FindAction(
        std::string_view key, std::string_view eventName) const noexcept;
    const InteractionAction* FindTransitionAction(
        std::string_view key, std::string_view eventName) const noexcept;
    bool HasTransitionAction(
        const InteractionHoverTransition& transition) const noexcept;
    std::optional<InteractionResolvedAction> ResolveAction(
        std::string_view key, std::string_view eventName,
        float x = 0.0f, float y = 0.0f, int button = 0) const;
    std::optional<InteractionResolvedAction> ResolveKeyboardStep(
        std::string_view key, int direction) const;
    std::optional<InteractionResolvedAction> ResolveRangeValue(
        std::string_view key, float value) const;
    const InteractionAction* ActionAt(
        float x, float y, std::string_view eventName,
        std::string* targetKey = nullptr) const noexcept;

    std::vector<std::string> KeyboardFocusableKeys() const;
    const InteractionRegion* FindAccessKey(
        char key) const noexcept;
    std::vector<InteractionRegion> AccessibilityRegions() const;
    bool IsKeyboardFocusable(std::string_view key) const noexcept;
    bool IsHovered(std::string_view key) const noexcept;
    bool IsPressed(std::string_view key) const noexcept;
    const std::string& HoveredKey() const noexcept;
    const std::string& PressedKey() const noexcept;
    bool LastPointer(float& x, float& y) const noexcept;
    std::uint64_t Generation() const noexcept;
    bool FrameOpen() const noexcept;
    std::string CursorAt(float x, float y) const;
    bool Empty() const noexcept;
    void Reset() noexcept;

private:
    const InteractionRegion* HitTest(float x, float y) const noexcept;
    bool ContainsKey(std::string_view key) const noexcept;
    bool PointerPressExpired() const noexcept;

    std::vector<InteractionRegion> active_;
    std::vector<InteractionRegion> staging_;
    bool frameOpen_ = false;
    bool pointerKnown_ = false;
    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;
    std::string hoveredKey_;
    std::string pressedKey_;
    std::string clickCandidateKey_;
    std::optional<InteractionRegion> retiredHoverRegion_;
    int pressedButton_ = -1;
    std::chrono::steady_clock::time_point pointerPressStarted_{};
    std::uint64_t generation_ = 0;
};
}
