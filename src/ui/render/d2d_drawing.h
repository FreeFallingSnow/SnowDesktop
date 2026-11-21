#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace snowdesktop
{
D2D1_RECT_F ToD2DRect(const RECT& rect);

// One drawing session on the UI thread. Call Reset when a temporary target
// stops drawing or its device resources are lost.
class D2DDrawing
{
public:
    void PrepareTarget(ID2D1RenderTarget* target);
    void Reset();
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> GetSolidBrush(
        ID2D1RenderTarget* target, const D2D1_COLOR_F& color);

    void DrawRoundedRectangle(ID2D1RenderTarget* target, RECT rect,
        float radius, D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth);
    void DrawRoundedRectangle(ID2D1RenderTarget* target, D2D1_RECT_F rect,
        float radius, D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth);
    void DrawFilledRectangle(ID2D1RenderTarget* target, RECT rect,
        D2D1_COLOR_F fill, D2D1_COLOR_F stroke);
    void DrawSeparator(ID2D1RenderTarget* target, RECT rect, const D2D1_COLOR_F& color);
    void DrawBasicText(ID2D1RenderTarget* target, IDWriteFactory* factory, const std::wstring& text,
        RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color,
        DWRITE_WORD_WRAPPING wordWrapping);
    void DrawTextEllipsis(ID2D1RenderTarget* target, IDWriteFactory* factory,
        const std::wstring& text, RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color,
        DWRITE_TEXT_ALIGNMENT hAlign, DWRITE_PARAGRAPH_ALIGNMENT vAlign, bool ellipsis,
        IDWriteTextLayout* cachedLayout);

private:
    ID2D1SolidColorBrush* FindOrCreateBrush(const D2D1_COLOR_F& color);

    ID2D1RenderTarget* target_ = nullptr;
    std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> solidBrushes_;
};
}
