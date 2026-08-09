#pragma once

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include "modern_menu.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace snowdesktop::component_preview
{

struct Bitmap
{
    int width = 0;
    int height = 0;
    /// Premultiplied BGRA pixels, stored top-down.
    std::vector<std::uint32_t> pixels;
};

enum class ApplyKind
{
    None,
    Collection,
    CollectionGroup,
    FileGroup,
    FileCategories,
    FolderMapping,
    LuaScript,
};

/** Settings represented by one preview page and applied by its explicit button. */
struct ApplySettings
{
    ApplyKind kind = ApplyKind::None;
    std::wstring packageId;
    int columns = 1;
    int rows = 1;
    bool listMode = false;
    bool scrollContainerMode = false;
    bool dateHeaders = false;
    bool showFileCategories = false;
    bool showSearchBox = false;
};

enum class OptionSetting
{
    ListMode,
    ScrollContainerMode,
    DateHeaders,
    ShowFileCategories,
    ShowSearchBox,
};

/** A same-size setting rendered as a two-choice control under the preview. */
struct Option
{
    OptionSetting setting = OptionSetting::ListMode;
    std::wstring label;
    std::wstring offLabel;
    std::wstring onLabel;
};

struct Card
{
    std::wstring title;
    std::wstring description;
    std::wstring sizeLabel;
    int columns = 2;
    int rows = 2;
    /// Exact physical pixel size of the component frame on the target grid.
    int previewWidth = 0;
    int previewHeight = 0;
    /// Includes component/mode/DPI/menu theme/personalization identity.
    std::wstring cacheKey;
    /// Called only after the exact final card viewport is known.
    std::function<Bitmap(int width, int height, UINT dpi,
        const ApplySettings& settings, bool hovered)> render;
    ApplySettings applySettings;
    std::vector<Option> options;
};

struct Model
{
    std::wstring title;
    std::wstring introduction;
    std::wstring resizeHint;
    std::wstring applyLabel;
    std::vector<Card> cards;

    bool Empty() const { return title.empty() || cards.empty(); }
};

/** Non-activating companion window shown next to the component menu. */
class Window
{
public:
    using ApplyHandler = std::function<void(const ApplySettings&)>;

    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Show(const Model& model, const RECT& menuBounds,
        HWND owner, UINT dpi, bool lightTheme,
        ApplyHandler onApply = {}, const RECT& itemBounds = {},
        modern_menu::Appearance appearance =
            modern_menu::Appearance::FollowSystem);
    /** Use the same dwell delay as opening a real menu submenu. */
    bool ScheduleShow(const Model& model, const RECT& menuBounds,
        HWND owner, UINT dpi, bool lightTheme,
        ApplyHandler onApply = {}, const RECT& itemBounds = {},
        modern_menu::Appearance appearance =
            modern_menu::Appearance::FollowSystem);
    /** Delay dismissal long enough for the pointer to cross the menu gap. */
    void ScheduleHide();
    void Hide();
    void Close();
    HWND Handle() const { return hwnd_; }
    const std::vector<POINT>& CommittedPositionsForTesting() const
    {
        return committedPositions_;
    }
    RECT OptionBoundsForTesting(
        OptionSetting setting, bool value) const;
    RECT ApplyBoundsForTesting() const { return applyRect_; }
    RECT CloseBoundsForTesting() const { return closeRect_; }
    RECT PreviousBoundsForTesting() const { return previousButton_; }
    RECT PreviousGlyphBoundsForTesting() const
    {
        return previousGlyphRect_;
    }
    RECT NextBoundsForTesting() const { return nextButton_; }
    RECT NextGlyphBoundsForTesting() const { return nextGlyphRect_; }

private:
    bool EnsureCreated(HWND owner);
    bool InitializeGraphics();
    void CreateFormats();
    bool RenderCurrent();
    void SelectRelative(int delta);
    void SetOption(OptionSetting setting, bool value);
    void ApplyCurrent();
    bool PointerInsideMenuOrPreview() const;
    std::wstring ModelIdentity(const Model& model) const;
    void ApplyWindowAppearance(bool lightTheme);
    POINT ResolvePosition(const RECT& menuBounds, UINT dpi) const;
    static LRESULT CALLBACK WindowProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcrTarget_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> cardTitleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> glyphFormat_;
    Model model_;
    size_t currentCard_ = 0;
    std::wstring modelIdentity_;
    RECT menuBounds_{};
    RECT itemBounds_{};
    RECT previousButton_{};
    RECT nextButton_{};
    RECT pagerRect_{};
    RECT previewRect_{};
    RECT applyRect_{};
    RECT closeRect_{};
    RECT previousGlyphRect_{};
    RECT nextGlyphRect_{};
    struct OptionHit
    {
        RECT bounds{};
        OptionSetting setting = OptionSetting::ListMode;
        bool value = false;
    };
    std::vector<OptionHit> optionHits_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool lightTheme_ = true;
    bool pointerTracking_ = false;
    bool componentHovered_ = false;
    ApplyHandler onApply_;
    Model pendingModel_;
    RECT pendingMenuBounds_{};
    RECT pendingItemBounds_{};
    HWND pendingOwner_ = nullptr;
    UINT pendingDpi_ = USER_DEFAULT_SCREEN_DPI;
    bool pendingLightTheme_ = true;
    ApplyHandler pendingOnApply_;
    std::unordered_map<std::wstring, Bitmap> cardFrameCache_;
    std::vector<POINT> committedPositions_;
};

} // namespace snowdesktop::component_preview
