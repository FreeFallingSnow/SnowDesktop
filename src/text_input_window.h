#pragma once
#include <windows.h>
#include <d2d1.h>
#include <cstddef>
#include <string_view>
#include <functional>
#include "text_input_accessibility.h"

namespace snowdesktop::text_input
{
// A message host for the shared editor, never a Windows EDIT control. The
// ordinary desktop surface and the modal prompt both use the same renderer.
const wchar_t* WindowClass();
struct Colors
{
    COLORREF background = RGB(255,255,255);
    COLORREF foreground = RGB(28,34,44);
    COLORREF border = RGB(170,175,185);
    COLORREF accent = RGB(0,95,184);
    COLORREF selectionText = RGB(255,255,255);
    COLORREF secondary = RGB(100,105,115);
};
void SetColors(HWND, const Colors&, float radius = 6.f);
bool IsComposing(HWND);
void SetLogicalSingleLine(HWND, bool);
// Embedded hosts retain a tiny, offscreen HWND for focus and IMM. Their entire
// visible input is drawn in the owner's composition transaction. Geometry is
// in owner client pixels, including the clip and current presentation offset.
void SetEmbeddedPose(HWND, RECT frame, RECT clip, bool shown, bool interactive);
void Draw(HWND, ID2D1RenderTarget*, D2D1_RECT_F frame, float pixelsPerUnit = 1.f,
    bool drawFrame = true);
// Mouse messages use owner client pixels; WM_CONTEXTMENU retains Win32 screen coordinates.
bool RoutePointer(HWND, UINT message, WPARAM, POINT);
int DesiredHeight(HWND);
void CopySecret(HWND, wchar_t* destination, std::size_t capacity);
void SetAccessibleName(HWND, std::wstring_view);
std::shared_ptr<TextAccess> Accessibility(HWND);
HRESULT CreateWindowProvider(HWND, IRawElementProviderSimple**);
std::wstring DisplayText(HWND);
enum class MenuCommand : UINT { None, Undo, Redo, Cut, Copy, Paste, SelectAll };
struct MenuState { bool undo=false,redo=false,cut=false,copy=false,paste=false,selectAll=false; };
using MenuHandler=std::function<MenuCommand(HWND,POINT,const MenuState&)>;
void SetMenuHandler(MenuHandler);
bool HasEditingMenu(HWND);
void CompleteComposition(HWND, bool cancel = false);
void SetPadding(HWND, float horizontalPixels, float verticalPixels);
}
