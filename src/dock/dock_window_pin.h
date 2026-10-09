#pragma once

#include <windows.h>

#include <memory>

// A session owns only the pins it creates. Hiding the thumbnail panel does not
// end the session; destroying its owner restores those windows to normal.
class DockWindowPin
{
public:
    DockWindowPin();
    ~DockWindowPin();
    DockWindowPin(const DockWindowPin&) = delete;
    DockWindowPin& operator=(const DockWindowPin&) = delete;

    static bool IsPinned(HWND window);
    bool Toggle(HWND window);
    void Refresh();
    void Clear();

private:
    struct State;
    std::unique_ptr<State> state_;
};
