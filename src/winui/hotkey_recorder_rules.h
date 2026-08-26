#pragma once

#include <cstdint>

namespace snowdesktop::winui
{

struct HotkeyChord
{
    std::uint32_t modifiers = 0;
    std::uint32_t virtualKey = 0;

    [[nodiscard]] bool Empty() const noexcept { return virtualKey == 0; }
    friend bool operator==(const HotkeyChord&, const HotkeyChord&) = default;
};

enum class HotkeyAvailability : std::uint8_t
{
    Unknown,
    Checking,
    Available,
    Conflict,
};

enum class HotkeyRecorderAction : std::uint8_t
{
    None,
    ProbeAvailability,
    Commit,
    Clear,
    Cancel,
};

struct HotkeyRecorderTransition
{
    HotkeyRecorderAction action = HotkeyRecorderAction::None;
    HotkeyChord chord;
    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;
};

/** XAML-independent state machine used by the WinUI hotkey recorder. */
class HotkeyRecorderRules
{
public:
    static constexpr std::uint32_t ModifierAlt = 0x0001;
    static constexpr std::uint32_t ModifierControl = 0x0002;
    static constexpr std::uint32_t ModifierShift = 0x0004;
    static constexpr std::uint32_t ModifierWindows = 0x0008;

    static constexpr std::uint32_t KeyBack = 0x08;
    static constexpr std::uint32_t KeyTab = 0x09;
    static constexpr std::uint32_t KeyEnter = 0x0D;
    static constexpr std::uint32_t KeyEscape = 0x1B;
    static constexpr std::uint32_t KeyDelete = 0x2E;
    static constexpr std::uint32_t KeyShift = 0x10;
    static constexpr std::uint32_t KeyControl = 0x11;
    static constexpr std::uint32_t KeyAlt = 0x12;
    static constexpr std::uint32_t KeyLeftWindows = 0x5B;
    static constexpr std::uint32_t KeyRightWindows = 0x5C;
    static constexpr std::uint32_t KeyLeftShift = 0xA0;
    static constexpr std::uint32_t KeyRightShift = 0xA1;
    static constexpr std::uint32_t KeyLeftControl = 0xA2;
    static constexpr std::uint32_t KeyRightControl = 0xA3;
    static constexpr std::uint32_t KeyLeftAlt = 0xA4;
    static constexpr std::uint32_t KeyRightAlt = 0xA5;
    static constexpr std::uint32_t KeyProcess = 0xE5;

    [[nodiscard]] static std::uint32_t ResolveCapturedVirtualKey(
        std::uint32_t routedVirtualKey,
        std::uint32_t scanCodeVirtualKey) noexcept
    {
        if (routedVirtualKey != KeyProcess)
            return routedVirtualKey;
        return scanCodeVirtualKey == KeyProcess ? 0 : scanCodeVirtualKey;
    }

    void Reset(HotkeyChord committed, std::uint64_t generation) noexcept
    {
        committed_ = committed;
        candidate_ = committed;
        generation_ = generation;
        active_ = false;
        availability_ = HotkeyAvailability::Unknown;
        ++requestId_;
    }

    [[nodiscard]] HotkeyRecorderTransition BeginCapture() noexcept
    {
        candidate_ = committed_;
        active_ = true;
        availability_ = HotkeyAvailability::Unknown;
        ++requestId_;
        if (candidate_.Empty())
            return {};
        availability_ = HotkeyAvailability::Checking;
        return {
            HotkeyRecorderAction::ProbeAvailability,
            candidate_, generation_, requestId_
        };
    }

    void Close() noexcept
    {
        active_ = false;
        availability_ = HotkeyAvailability::Unknown;
        ++generation_;
        ++requestId_;
    }

    [[nodiscard]] HotkeyRecorderTransition KeyDown(
        std::uint32_t virtualKey,
        std::uint32_t physicalModifiers) noexcept
    {
        if (!active_)
            return {};
        // ContentDialog owns navigation and commands. Enter applies through
        // its default primary button, Escape cancels, and Tab changes focus.
        if (virtualKey == KeyEnter || virtualKey == KeyEscape ||
            virtualKey == KeyTab)
            return {};
        if (IsModifierKey(virtualKey))
            return {};

        HotkeyChord next{
            physicalModifiers & (ModifierAlt | ModifierControl |
                ModifierShift | ModifierWindows),
            virtualKey
        };
        if (next == candidate_ &&
            availability_ != HotkeyAvailability::Unknown)
            return {};

        candidate_ = next;
        availability_ = HotkeyAvailability::Checking;
        ++requestId_;
        return {
            HotkeyRecorderAction::ProbeAvailability,
            candidate_, generation_, requestId_
        };
    }

    [[nodiscard]] HotkeyRecorderTransition KeyUp(
        std::uint32_t) const noexcept
    {
        // The chord is sampled from physical state on KeyDown. KeyUp does not
        // erase it, so the user can inspect the combination before Enter.
        return {};
    }

    [[nodiscard]] HotkeyRecorderTransition LoseFocus() noexcept
    {
        // Focus is expected to move between the recorder surface and the
        // dialog buttons. Only an explicit dialog action ends editing.
        return {};
    }

    [[nodiscard]] HotkeyRecorderTransition Commit() noexcept
    {
        if (!active_ || candidate_.Empty() ||
            availability_ != HotkeyAvailability::Available)
            return {};
        committed_ = candidate_;
        active_ = false;
        return {
            HotkeyRecorderAction::Commit,
            committed_, generation_, requestId_
        };
    }

    [[nodiscard]] HotkeyRecorderTransition Clear() noexcept
    {
        if (!active_)
            return {};
        candidate_ = {};
        committed_ = {};
        active_ = false;
        availability_ = HotkeyAvailability::Available;
        ++requestId_;
        return { HotkeyRecorderAction::Clear, {}, generation_, requestId_ };
    }

    [[nodiscard]] HotkeyRecorderTransition Restore(
        HotkeyChord value) noexcept
    {
        if (!active_)
            return {};
        candidate_ = value;
        committed_ = value;
        active_ = false;
        availability_ = HotkeyAvailability::Unknown;
        ++requestId_;
        return {
            HotkeyRecorderAction::Commit,
            committed_, generation_, requestId_
        };
    }

    [[nodiscard]] HotkeyRecorderTransition CancelCapture() noexcept
    {
        if (!active_)
            return {};
        candidate_ = committed_;
        active_ = false;
        availability_ = HotkeyAvailability::Unknown;
        ++requestId_;
        return {
            HotkeyRecorderAction::Cancel,
            committed_, generation_, requestId_
        };
    }

    [[nodiscard]] bool ApplyAvailability(
        std::uint64_t generation,
        std::uint64_t requestId,
        bool available) noexcept
    {
        if (!active_ || generation != generation_ ||
            requestId != requestId_ || candidate_.Empty())
            return false;
        availability_ = available
            ? HotkeyAvailability::Available
            : HotkeyAvailability::Conflict;
        return true;
    }

    [[nodiscard]] bool Active() const noexcept { return active_; }
    [[nodiscard]] HotkeyChord Committed() const noexcept { return committed_; }
    [[nodiscard]] HotkeyChord Candidate() const noexcept { return candidate_; }
    [[nodiscard]] HotkeyAvailability Availability() const noexcept
    { return availability_; }
    [[nodiscard]] std::uint64_t Generation() const noexcept
    { return generation_; }
    [[nodiscard]] std::uint64_t RequestId() const noexcept
    { return requestId_; }

    [[nodiscard]] static bool IsModifierKey(
        std::uint32_t virtualKey) noexcept
    {
        return virtualKey == KeyShift || virtualKey == KeyControl ||
            virtualKey == KeyAlt || virtualKey == KeyLeftWindows ||
            virtualKey == KeyRightWindows ||
            virtualKey == KeyLeftShift || virtualKey == KeyRightShift ||
            virtualKey == KeyLeftControl || virtualKey == KeyRightControl ||
            virtualKey == KeyLeftAlt || virtualKey == KeyRightAlt;
    }

private:
    HotkeyChord committed_;
    HotkeyChord candidate_;
    std::uint64_t generation_ = 0;
    std::uint64_t requestId_ = 0;
    bool active_ = false;
    HotkeyAvailability availability_ = HotkeyAvailability::Unknown;
};

} // namespace snowdesktop::winui
