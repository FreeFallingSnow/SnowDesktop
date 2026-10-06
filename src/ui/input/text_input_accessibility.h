#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct IRawElementProviderSimple;
struct ITextProvider;

namespace snowdesktop::text_input
{
// Keep SDK provider types in the implementation. In particular, UIAutomation's
// global DockPosition must not escape into application headers.
struct Point { double x=0,y=0; };
struct Rectangle { double left=0,top=0,width=0,height=0; };
enum class Unit { Character, Format, Word, Line, Paragraph, Page, Document };
struct AccessibleDocument
{
    std::wstring text;
    std::size_t cursor = 0, anchor = 0;
    bool readOnly = false, enabled = true, password = false;
    bool focused = false, offscreen = false;
    std::wstring name;
    RECT bounds{};
};
struct TextAccess
{
    const void* lifetimeIdentity = nullptr;
    std::wstring elementIdentity;
    std::function<std::optional<AccessibleDocument>()> document;
    std::function<bool(std::size_t,std::size_t)> select;
    std::function<std::vector<Rectangle>(std::size_t,std::size_t)> rectangles;
    std::function<std::optional<std::size_t>(Point)> hit;
    std::function<bool(std::size_t,bool)> reveal;
    std::function<std::vector<std::size_t>(Unit)> unitBoundaries;
    std::function<std::vector<std::pair<std::size_t,std::size_t>>()> visibleRanges;
};
// Ranges keep only offsets and access callbacks, never a cached text value.
// Password documents refuse both text and range reads.
HRESULT CreateTextProvider(std::shared_ptr<TextAccess>,IRawElementProviderSimple*,ITextProvider**);
}
