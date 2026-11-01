#pragma once

#include "platform/control/control_elements.h"
#include <memory>

namespace snowdesktop::settings_automation
{
struct State
{
    std::string id, status = "idle", kind, error, elementId, action;
    std::uint64_t startedUnixMs = 0, finishedUnixMs = 0;
    std::size_t elementCount = 0;
    bool truncated = false;
};

// A windowless MTA client. The application supplies only its owned settings
// facade HWND; clients cannot supply an HWND or enumerate the desktop.
class Service final
{
public:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    bool Refresh(HWND window, std::string& error);
    bool Perform(HWND window, const control::Element& element, std::string action,
        std::wstring value, double number, std::string& error);
    State Current() const;
    std::vector<control::Element> Elements(HWND window) const;
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool Start(HWND window, const control::Element* element, std::string action,
        std::wstring value, double number, std::string& error);
};
}
