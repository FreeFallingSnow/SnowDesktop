#pragma once

#include "item_text_layout_cache.h"

#include <d2d1_1.h>
#include <functional>
#include <unordered_map>

namespace snowdesktop
{
class D2DDrawing;

class ItemTitleRenderer
{
public:
    struct GridStyle
    {
        float fontSize;
        float fontScale;
        int titleHeight;
        int lines;
        float opacity;
        bool selected;
        bool lightTheme;
        bool componentPanel;
        bool ellipsis;
    };

    struct LayoutCacheUsage
    {
        size_t entries = 0;
        size_t inputBytes = 0;
    };

    using ScrollOffset = std::function<std::optional<float>(float naturalWidth, float viewportWidth)>;

    void RecreateGridFormat(IDWriteFactory* factory, float fontSize, DWRITE_FONT_WEIGHT weight);
    void RecreateListFormat(IDWriteFactory* factory, float fontSize, DWRITE_FONT_WEIGHT weight);
    void InvalidateTitles();
    void ReleaseDeviceResources();

    IDWriteTextFormat* GridFormat() const { return gridFormat_.Get(); }
    IDWriteTextFormat* ListFormat() const { return listFormat_.Get(); }
    LayoutCacheUsage GetLayoutCacheUsage(bool componentList) const;
    void VisitShadowBitmaps(bool componentList, const std::function<void(ID2D1Bitmap*)>& visit) const;

    void DrawGrid(ID2D1RenderTarget* context, D2DDrawing& drawing, ID2D1Device* device,
        IDWriteFactory* factory, RECT textRect, const std::wstring& text,
        const GridStyle& style, DWRITE_FONT_WEIGHT weight, bool glassEnabled);
    void DrawList(ID2D1DeviceContext* context, D2DDrawing& drawing, ID2D1Device* device,
        IDWriteFactory* factory, RECT textRect, const std::wstring& text,
        float scale, float fontSize, DWRITE_FONT_WEIGHT weight, bool lightTheme,
        bool glassEnabled, const ScrollOffset& scrollOffset);

private:
    void DrawStyledLayout(ID2D1RenderTarget* context, D2DDrawing& drawing, ID2D1Device* device,
        IDWriteTextLayout* layout, const std::wstring& shadowKey,
        D2D1_POINT_2F origin, D2D1_SIZE_F layoutSize, float layoutScale, float opacity,
        bool lightTheme, bool lightPanelShadow, bool componentList);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> gridFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> listFormat_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> effectContext_;
    std::unordered_map<std::wstring, ItemTextLayoutCacheEntry> gridLayouts_;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IDWriteTextLayout>> listLayouts_;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> gridShadows_;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> listShadows_;
};
}
