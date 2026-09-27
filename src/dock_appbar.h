#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace snowdesktop
{
struct DockAppBarReservation
{
    HMONITOR monitor = nullptr;
    RECT bounds{};
    UINT edge = 0;
    int thickness = 0;
    bool operator==(const DockAppBarReservation& other) const
    {
        return monitor == other.monitor && EqualRect(&bounds, &other.bounds) &&
            edge == other.edge && thickness == other.thickness;
    }
};

// Hidden Shell registration endpoints; visual/input ownership stays with Dock.
// All methods and callbacks run on the desktop UI thread.
class DockAppBars final
{
public:
    explicit DockAppBars(std::function<void()> changed);
    ~DockAppBars();
    void Configure(const std::vector<DockAppBarReservation>& reservations);
    RECT RestoreWorkArea(HMONITOR monitor, RECT work) const;
    std::optional<RECT> Approved(const DockAppBarReservation& request) const;
private:
    struct Window;
    std::function<void()> changed_;
    std::vector<std::unique_ptr<Window>> windows_;
};
}
