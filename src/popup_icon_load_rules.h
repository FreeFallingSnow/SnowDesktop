#pragma once

#include <cstdint>

namespace snowdesktop::popup_icon_load_rules
{
constexpr std::uint64_t NextGeneration(std::uint64_t current)
{
    ++current;
    return current == 0 ? 1 : current;
}

constexpr bool ShouldRejectResult(
    bool popupRequest,
    std::uint64_t resultGeneration,
    std::uint64_t currentGeneration)
{
    return popupRequest && resultGeneration != currentGeneration;
}

}
