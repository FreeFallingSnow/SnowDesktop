#pragma once

#include <windows.h>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace snowdesktop::status_bar_application_menu
{
struct Source
{
    HWND window = nullptr;
    HMENU root = nullptr; // UI Automation sources are identified by the window and have no native root.
    DWORD process = 0, thread = 0;

    bool Live() const;
    bool CanInteract() const;
    friend bool operator==(const Source&, const Source&) = default;
};

struct Heading
{
    UINT position = 0, command = 0;
    std::wstring label;
    bool submenu = false, enabled = false;
    friend bool operator==(const Heading&, const Heading&) = default;
};

struct Target
{
    Source source;
    Heading heading;
};

struct Snapshot
{
    Source source;
    std::vector<Heading> headings;
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

Snapshot Read(HWND window);

struct MenuDeleter { void operator()(HMENU menu) const noexcept; };
using OwnedMenu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;

// A popup owns a local menu copy and the verified routes into the target's
// menu tree. Its IDs are local to this session, including nested menus.
class Popup final
{
public:
    explicit Popup(Target target);
    ~Popup();
    OwnedMenu Open();
    OwnedMenu Expand(UINT command);
    bool InvokeDirect() const;
    bool Invoke(UINT command) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
