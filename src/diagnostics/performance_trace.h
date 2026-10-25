#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace snowdesktop::performance
{
namespace detail { struct TraceContext; }
bool Enabled() noexcept;
std::uint64_t OwnerId(std::wstring_view instance) noexcept;

// A business activity whose explicit SDK BEGIN/END enclose this object's real
// lifetime. Only owner/operation context is nested here, never timing state.
class Scope final
{
public:
    Scope(std::string_view module, std::string_view phase,
        std::wstring_view owner = {}, std::uint64_t correlation = 0) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    std::shared_ptr<detail::TraceContext> context_;
    std::uint64_t previousOwner_ = 0, previousOperation_ = 0, previousGeneration_ = 0;
    std::uint8_t category_ = 0;
    bool open_ = false;
};

// Numeric observations are gauges unless a metric explicitly documents
// cumulative semantics. A value of one is not silently accumulated.
void Value(std::string_view module, std::string_view name,
    std::wstring_view owner, double value, std::uint64_t correlation = 0) noexcept;
void DrawLink(std::string_view surface, std::wstring_view owner, bool consume) noexcept;
}
