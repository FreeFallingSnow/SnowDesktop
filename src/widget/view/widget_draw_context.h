#pragma once

#include "widget_image_sampling.h"
#include "widget_text_glow_cache.h"
#include "widget_text_layout_cache.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct IDWriteFontCollection1;

namespace snowdesktop::widget_runtime
{
struct WidgetImageSpin;
struct WidgetSurfacePaint;

// Shared render-thread resources for immediate drawing and the view renderer.
class WidgetDrawContext
{
public:
    ID2D1DeviceContext* ctx = nullptr;
    IDWriteFactory* dwrite = nullptr;
    D2D1_RECT_F widgetRect{};
    std::wstring currentWidgetId;
    const char* surfaceKind = "desktop";
    int gridCellW = 92;
    int gridCellH = 116;
    DWRITE_FONT_WEIGHT itemFontWeight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    int widgetClipDepth = 0;
    WidgetImageSpin* desktopImageSpin = nullptr;
    WidgetSurfacePaint* desktopSurfacePaint = nullptr;
    WidgetImageSamplingCache imageSamplingCache;
    WidgetTextLayoutCache textLayoutCache;
    WidgetTextGlowCache textGlowCache;

    WidgetDrawContext();
    ~WidgetDrawContext();
    WidgetDrawContext(const WidgetDrawContext&) = delete;
    WidgetDrawContext& operator=(const WidgetDrawContext&) = delete;

    ID2D1SolidColorBrush* Brush(int color, float alpha);
    IDWriteTextFormat* TextFormat(float size, DWRITE_FONT_WEIGHT weight,
        bool centered, DWRITE_WORD_WRAPPING wrapping, bool fontAwesome,
        bool verticallyCentered, bool fluent, const std::wstring* privateFontPath);
    bool PreloadPrivateFont(const std::wstring& path);
    bool HasPrivateFont(const std::wstring& path) const;
    void ResetBrushes();
    void ClearPrivateTextFormats();

    struct Counts
    {
        std::size_t brushes;
        std::size_t textFormats;
        std::size_t privateTextFormats;
        std::size_t privateFonts;
    };
    Counts ResourceCounts() const noexcept;

private:
    struct PrivateFontResource
    {
        Microsoft::WRL::ComPtr<IDWriteFontCollection1> collection;
        std::wstring familyName;
    };
    PrivateFontResource* LoadPrivateFont(const std::wstring& path);

    ID2D1DeviceContext* brushContext_ = nullptr;
    std::unordered_map<std::uint32_t, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushes_;
    std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<IDWriteTextFormat>> textFormats_;
    std::unordered_map<std::wstring, PrivateFontResource> privateFonts_;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IDWriteTextFormat>> privateTextFormats_;
};

std::string_view CurrentWidgetSurface(const WidgetDrawContext* state) noexcept;
ID2D1SolidColorBrush* GetCachedBrush(WidgetDrawContext* state, int color, float alpha = 1.0f);
IDWriteTextFormat* GetCachedTextFormat(WidgetDrawContext* state, float size,
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
    bool centered = false,
    DWRITE_WORD_WRAPPING wrapping = DWRITE_WORD_WRAPPING_WRAP,
    bool fontAwesome = false, bool verticallyCentered = false,
    bool fluent = false, const std::wstring* privateFontPath = nullptr);

void DrawHostRect(WidgetDrawContext* state, float x, float y, float width,
    float height, int color, float radius, float alpha);
void DrawHostText(WidgetDrawContext* state, const std::wstring& text,
    float x, float y, float width, float height, float size, int color);
void DrawHostStrokeRect(WidgetDrawContext* state, float x, float y, float width,
    float height, int color, float radius, float thickness, float alpha);
Microsoft::WRL::ComPtr<IDWriteTextLayout> CreateHostSingleLineTextLayout(
    WidgetDrawContext* state, const std::wstring& text, float size, float width, float height);
Microsoft::WRL::ComPtr<IDWriteTextLayout> CreateHostMultilineTextLayout(
    WidgetDrawContext* state, const std::wstring& text, float size, float width);

struct HostInputDisplayText
{
    std::wstring text;
    std::size_t cursor = 0;
    std::size_t compositionStart = 0;
    std::size_t compositionLength = 0;
};
HostInputDisplayText BuildHostInputDisplayText(const std::wstring& text,
    std::size_t cursor, std::size_t selectionAnchor,
    const std::wstring& compositionText, std::size_t compositionCursor);
void DrawHostTextSelection(WidgetDrawContext* state, IDWriteTextLayout* textLayout,
    std::size_t selectionStart, std::size_t selectionEnd, float originX,
    float originY, int color);
void DrawHostCompositionUnderline(WidgetDrawContext* state, IDWriteTextLayout* textLayout,
    std::size_t compositionStart, std::size_t compositionLength, float originX,
    float originY, int color);
void DrawSimulatedPreviewIcon(WidgetDrawContext* state, const std::wstring& identity,
    const std::wstring& title, float x, float y, float size, float alpha);

void PopWidgetClips(WidgetDrawContext& draw) noexcept;

// Scope of one immediate-mode recording. The capture keeps the recording context
// and command list alive until the display target is restored.
class WidgetCommandCapture
{
public:
    enum class BeginResult { Started, NoDevice, NoRecordingContext, NoCommandList };

    WidgetCommandCapture() = default;
    ~WidgetCommandCapture();
    WidgetCommandCapture(const WidgetCommandCapture&) = delete;
    WidgetCommandCapture& operator=(const WidgetCommandCapture&) = delete;

    BeginResult Begin(WidgetDrawContext& draw, std::vector<D2D1_RECT_F>& clipRects,
        ID2D1Device* device, Microsoft::WRL::ComPtr<ID2D1DeviceContext>& recordingContext);
    HRESULT FinishDraw() noexcept;
    HRESULT Close() noexcept;
    void Abort() noexcept;
    ID2D1CommandList* Commands() const noexcept { return commands_.Get(); }
    Microsoft::WRL::ComPtr<ID2D1CommandList> TakeCommands() noexcept;

private:
    WidgetDrawContext* draw_ = nullptr;
    std::vector<D2D1_RECT_F>* clipRects_ = nullptr;
    ID2D1DeviceContext* display_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> recording_;
    Microsoft::WRL::ComPtr<ID2D1CommandList> commands_;
};
}
