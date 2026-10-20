#pragma once

#include <windows.h>

#include <memory>

struct PersonalizationSettings;

// A session owns only the pins it creates. Hiding the thumbnail panel does not
// end the session; destroying its owner restores those windows to normal.
// Application resets of TOPMOST retain the request until explicitly cancelled.
class DockWindowPin
{
public:
    DockWindowPin();
    ~DockWindowPin();
    DockWindowPin(const DockWindowPin&) = delete;
    DockWindowPin& operator=(const DockWindowPin&) = delete;

    // Reports a live pin request or an existing native TOPMOST window.
    static bool IsPinned(HWND window);
    bool Toggle(HWND window);
    void SetAppearance(const PersonalizationSettings& settings);
    void Refresh();
    void Clear();

private:
    struct State;
    std::unique_ptr<State> state_;
};
