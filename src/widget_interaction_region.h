#pragma once

#include <map>
#include <cstdint>
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
    std::string id;
    InteractionValue value;

    bool operator==(const InteractionAction&) const = default;
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

struct InteractionRegion
{
    std::string key;
    InteractionShape shape;
    std::string cursor;
    std::map<std::string, InteractionAction, std::less<>> events;
    std::string accessibilityRole;
    std::string accessibilityLabel;
    bool enabled = true;

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

    void BeginFrame();
    bool Submit(InteractionRegion region, std::string& error);
    InteractionHoverTransition CommitFrame();
    void AbortFrame() noexcept;

    InteractionHoverTransition UpdateHover(float x, float y);
    InteractionHoverTransition ClearHover();
    InteractionPointerResult PointerDown(float x, float y, int button);
    InteractionPointerResult PointerUp(float x, float y, int button);
    std::string ConsumeClickTarget(float x, float y);
    std::string TargetAt(float x, float y) const;

    const InteractionRegion* Find(std::string_view key) const noexcept;
    const InteractionAction* FindAction(
        std::string_view key, std::string_view eventName) const noexcept;
    const InteractionAction* FindTransitionAction(
        std::string_view key, std::string_view eventName) const noexcept;
    const InteractionAction* ActionAt(
        float x, float y, std::string_view eventName,
        std::string* targetKey = nullptr) const noexcept;

    bool IsHovered(std::string_view key) const noexcept;
    bool IsPressed(std::string_view key) const noexcept;
    const std::string& HoveredKey() const noexcept;
    const std::string& PressedKey() const noexcept;
    bool LastPointer(float& x, float& y) const noexcept;
    std::uint64_t Generation() const noexcept;
    std::string CursorAt(float x, float y) const;
    bool Empty() const noexcept;
    void Reset() noexcept;

private:
    const InteractionRegion* HitTest(float x, float y) const noexcept;
    bool ContainsKey(std::string_view key) const noexcept;

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
    std::uint64_t generation_ = 0;
};
}
