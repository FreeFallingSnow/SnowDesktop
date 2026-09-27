#pragma once
#include "personalization.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop
{
struct SystemCalendarInputField;

// Native EDIT children of the existing panel. There is no calendar top-level
// window or nested modal loop. The model owns draft values and all actions.
class SystemCalendarInputs
{
public:
    using Change=std::function<void(std::string,std::wstring)>;
    using Key=std::function<void(std::string,UINT,bool,bool)>;
    SystemCalendarInputs(HWND parent,Change,Key);
    ~SystemCalendarInputs();
    SystemCalendarInputs(const SystemCalendarInputs&)=delete;
    SystemCalendarInputs& operator=(const SystemCalendarInputs&)=delete;
    void Sync(const std::vector<SystemCalendarInputField>&,const PersonalizationSettings&,UINT dpi);
    void Pose(float offsetYPixels,bool interactive);
    bool Focus(std::string_view id,bool keyboard=true);
    bool SetValue(std::string_view id,const std::wstring&);
    bool Contains(HWND) const;
    std::string FieldId(HWND) const;
    bool HandleCommand(WPARAM,LPARAM);
    HBRUSH ControlColor(HWND,HDC) const;
    void Clear();
    // The HDC origin is the panel client origin. Includes each input's native
    // text and shared nonclient styling, clipped exactly like the live child.
    void Print(HDC) const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Internal offline helpers. Only their own hidden parent/EDIT children exist;
// they never show a window, read a live draft, or write the calendar service.
void OverlaySystemCalendarInputs(const std::vector<SystemCalendarInputField>&,
    const PersonalizationSettings&,UINT dpi,int width,int height,std::vector<std::uint32_t>& pixels);
void CheckSystemCalendarInputs();
void CheckSystemControlPasswordInput();
}
