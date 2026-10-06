#pragma once
#include "theme/personalization.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "ui/input/text_input_accessibility.h"

namespace snowdesktop
{
struct SystemCalendarInputField;

// Offscreen focus/IME hosts; visible fields are drawn in the panel's surface.
// The model owns draft values and all actions.
class SystemCalendarInputs
{
public:
    using Change=std::function<void(std::string,std::wstring)>;
    using Key=std::function<void(std::string,UINT,bool,bool)>;
    using PointerChanged=std::function<void()>;
    SystemCalendarInputs(HWND parent,Change,Key,PointerChanged={});
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
    // Offline GDI adapter for the same production renderer.
    void Print(HDC) const;
    void Draw(ID2D1RenderTarget*) const;
    bool HandlePointer(UINT,WPARAM,POINT);
    std::shared_ptr<text_input::TextAccess> Accessibility(std::string_view) const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Internal offline helpers. Only their own hidden parent/focus hosts exist;
// they never show a window, read a live draft, or write the calendar service.
void OverlaySystemCalendarInputs(const std::vector<SystemCalendarInputField>&,
    const PersonalizationSettings&,UINT dpi,int width,int height,std::vector<std::uint32_t>& pixels);
void CheckSystemCalendarInputs();
void CheckSystemControlPasswordInput();
float MeasureSystemCalendarNotesHeight(std::wstring_view text,float width);
}
