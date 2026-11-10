#pragma once

#include "status_bar_application_menu.h"

namespace snowdesktop::status_bar_application_menu
{
// All UI Automation calls and element references are confined to a worker
// apartment. The bar thread exchanges only window identities and menu data.
class AutomationMenu final
{
public:
    explicit AutomationMenu(UINT readyMessage);
    ~AutomationMenu();

    Snapshot Refresh(HWND window, HWND receiver = nullptr);
    void Activate(const Target& target);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
