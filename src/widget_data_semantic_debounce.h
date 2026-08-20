#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace snowdesktop::widget_runtime
{
struct WidgetDataSemanticEnvelope
{
    bool available = false;
    bool warmingUp = false;
    std::string error;

    bool operator==(const WidgetDataSemanticEnvelope&) const = default;
};

class WidgetDataSemanticDebouncer
{
public:
    static constexpr std::size_t RequiredConfirmations = 2;

    bool Accept(WidgetDataSemanticEnvelope value)
    {
        if (!stable_ ||
            (value.available && !value.warmingUp && value.error.empty()))
        {
            stable_ = std::move(value);
            pending_.reset();
            pendingConfirmations_ = 0;
            return true;
        }

        if (*stable_ == value)
        {
            pending_.reset();
            pendingConfirmations_ = 0;
            return true;
        }

        if (pending_ && *pending_ == value)
        {
            ++pendingConfirmations_;
        }
        else
        {
            pending_ = std::move(value);
            pendingConfirmations_ = 1;
        }

        if (pendingConfirmations_ < RequiredConfirmations)
            return false;

        stable_ = *pending_;
        pending_.reset();
        pendingConfirmations_ = 0;
        return true;
    }

    void Reset() noexcept
    {
        stable_.reset();
        pending_.reset();
        pendingConfirmations_ = 0;
    }

private:
    std::optional<WidgetDataSemanticEnvelope> stable_;
    std::optional<WidgetDataSemanticEnvelope> pending_;
    std::size_t pendingConfirmations_ = 0;
};

template<typename Snapshot>
WidgetDataSemanticEnvelope WidgetDataEnvelopeOf(const Snapshot& snapshot)
{
    WidgetDataSemanticEnvelope result;
    result.available = snapshot.available;
    if constexpr (requires { snapshot.warmingUp; })
        result.warmingUp = snapshot.warmingUp;
    result.error = snapshot.error;
    return result;
}

template<typename Snapshot>
Snapshot StabilizeWidgetDataEnvelope(Snapshot snapshot,
    const std::optional<Snapshot>& previous,
    WidgetDataSemanticDebouncer& debouncer)
{
    if (debouncer.Accept(WidgetDataEnvelopeOf(snapshot)) || !previous)
        return snapshot;

    Snapshot held = *previous;
    held.timestampMs = snapshot.timestampMs;
    return held;
}
}
