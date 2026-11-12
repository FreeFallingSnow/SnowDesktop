#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::screen_time
{

inline constexpr int kRetentionDays = 90;
inline constexpr int kIdlePauseMs = 180000;
inline constexpr int kMaximumSliceMs = 5000;
inline constexpr int kWidgetAppLimit = 8;

struct AppTotal
{
    std::string name;
    std::int64_t seconds = 0;
    std::string identity;
};

struct AppIcon
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bgraPremultiplied;
};

// Resolve existing history identities on a COM-initialized worker. The
// process-local cache is bounded and also remembers unavailable icons briefly.
[[nodiscard]] std::shared_ptr<const AppIcon> LoadAppIcon(std::string_view identity);

struct DayReport
{
    std::string date;
    std::int64_t totalSeconds = 0;
    std::array<std::int64_t, 24> hours{};
    std::vector<AppTotal> apps;
};

struct TodayView
{
    bool enabled = true;
    std::int64_t totalSeconds = 0;
    std::array<std::int64_t, 24> hours{};
    std::vector<AppTotal> apps;
};

// Host-owned collector. Observe() runs on the UI thread. Today() is safe
// from the widget data worker.
class Tracker
{
public:
    explicit Tracker(std::filesystem::path path);
    ~Tracker();

    void SetEnabled(bool enabled);
    [[nodiscard]] bool Enabled() const;

    void Observe();
    void SetSessionLocked(bool locked);
    void SetDisplayOn(bool on);
    void SetSuspended(bool suspended);
    void Flush();
    [[nodiscard]] bool Clear(std::string* error = nullptr);

    [[nodiscard]] TodayView Today() const;
    [[nodiscard]] std::vector<DayReport> Reports() const;

private:
    struct State;
    State* state_;
};

// Settings process reads the same file. A missing or unreadable file is an
// empty history, not a failure the page needs to surface.
[[nodiscard]] std::vector<DayReport> LoadReports(
    const std::filesystem::path& path);

} // namespace snowdesktop::screen_time
