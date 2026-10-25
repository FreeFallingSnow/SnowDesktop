#pragma once
#include <cstdint>
#include <string_view>
namespace snowdesktop::performance
{
std::uint64_t AnimationBegin(std::uint64_t key, std::string_view type, std::string_view surface,
    double targetMs, double effectiveMs, bool observed);
void AnimationUpdate(std::uint64_t animation, std::uint64_t frame, double effectiveMs,
    double lateMs, std::uint64_t missed) noexcept;
void AnimationEnd(std::uint64_t animation, bool canceled) noexcept;
void Presentation(std::uint64_t frame, std::uint64_t owner, bool shared) noexcept;
void CompositionCommit(std::uint64_t frame, std::uint64_t commit, double durationMs) noexcept;
}
