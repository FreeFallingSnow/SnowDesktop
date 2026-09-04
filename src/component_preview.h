#pragma once

#include "modern_menu.h"
#include "widget_preview_stage.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace snowdesktop::component_preview
{

namespace detail
{

/** Pixel-center coverage for the antialiased rounded-rectangle rasterizer. */
float RoundedRectangleCoverage(float sampleX, float sampleY,
    float left, float top, float right, float bottom, float radius);

} // namespace detail

struct Bitmap
{
    int width = 0;
    int height = 0;
    /// Premultiplied BGRA pixels, stored top-down.
    std::vector<std::uint32_t> pixels;
};

struct WallpaperBackdropCaptureResult
{
    widget_preview::Wallpaper wallpaper;
    RECT desktopBounds{};
};

using WallpaperBackdropCaptureHandler = std::function<
    WallpaperBackdropCaptureResult(const RECT&, DWORD,
        const std::atomic_bool*)>;

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
    bool largeFolderTitleless = false;
    bool dateHeaders = false;
    bool showFileCategories = false;
    bool showSearchBox = false;
};

enum class OptionSetting
{
    ListMode,
    ScrollContainerMode,
    LargeFolderTitleless,
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

/** Location of the component viewport within the complete preview card stage. */
struct StagePlacement
{
    int canvasWidth = 0;
    int canvasHeight = 0;
    int offsetX = 0;
    int offsetY = 0;
    bool lightTheme = false;
    const widget_preview::Wallpaper* wallpaper = nullptr;
    bool transparent = false;
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
    bool lightStage = false;
    /// Optional fixed stage shared by the full card and component crop.
    std::shared_ptr<const widget_preview::Wallpaper> stageWallpaper;
    /// Recreate the current static Windows wallpaper behind the preview card.
    bool useDesktopWallpaperStage = false;
    /// Includes component/mode/DPI/menu theme/personalization identity.
    std::wstring cacheKey;
    /// Called only after the exact final card viewport is known.
    std::function<Bitmap(int width, int height, UINT dpi,
        const StagePlacement& stage,
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
    /// Page selected when a newly identified preview model is first shown.
    std::size_t initialCard = 0;

    bool Empty() const { return title.empty() || cards.empty(); }
};

/** Non-activating companion window shown next to the component menu. */
class Window
{
public:
    using ApplyHandler = std::function<void(const ApplySettings&)>;

    explicit Window(WallpaperBackdropCaptureHandler captureHandler = {});
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /** Start the menu-scoped Wallpaper Engine capture before a preview opens. */
    void PrefetchDesktopWallpaperBackdrop(HWND owner, POINT screenPoint);
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
    RECT CardBoundsForTesting() const { return cardRect_; }
    RECT PreviewBoundsForTesting() const { return previewRect_; }
    RECT MetadataBoundsForTesting() const { return metadataRect_; }
    RECT CloseBoundsForTesting() const { return closeRect_; }
    RECT PreviousBoundsForTesting() const { return previousButton_; }
    RECT PreviousGlyphBoundsForTesting() const
    {
        return previousGlyphRect_;
    }
    RECT NextBoundsForTesting() const { return nextButton_; }
    RECT NextGlyphBoundsForTesting() const { return nextGlyphRect_; }
    bool BlurEnabledForTesting() const { return blurEnabled_; }
    bool HasWallpaperEngineCacheForTesting() const
    {
        return wallpaperEngineCache_ &&
            !wallpaperEngineCache_->pixels.empty();
    }
    bool WaitingForWallpaperEngineFrameForTesting() const
    {
        return waitingForWallpaperEngineFrame_;
    }

private:
    enum class WallpaperBackdropLoadResult
    {
        Ready,
        WaitingForCapture,
    };

    bool EnsureCreated(HWND owner);
    bool RenderCurrent();
    WallpaperBackdropLoadResult LoadDesktopWallpaperBackdrop();
    void StartWallpaperEngineBackdropCapture(const RECT& monitorBounds);
    void FinishWallpaperEngineBackdropCapture(std::uint64_t generation);
    void CancelWallpaperEngineBackdropCapture(bool wait);
    void SelectRelative(int delta);
    void SetOption(OptionSetting setting, bool value);
    void ApplyCurrent();
    bool PointerInsideMenuOrPreview() const;
    std::wstring ModelIdentity(const Model& model) const;
    void ApplyWindowAppearance();
    POINT ResolvePosition(const RECT& menuBounds, UINT dpi) const;
    static LRESULT CALLBACK WindowProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    Model model_;
    size_t currentCard_ = 0;
    std::wstring modelIdentity_;
    RECT menuBounds_{};
    RECT itemBounds_{};
    RECT previousButton_{};
    RECT nextButton_{};
    RECT pagerRect_{};
    RECT cardRect_{};
    RECT previewRect_{};
    RECT metadataRect_{};
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
    bool blurEnabled_ = true;
    bool pointerTracking_ = false;
    bool componentHovered_ = false;
    ApplyHandler onApply_;
    Model pendingModel_;
    RECT pendingMenuBounds_{};
    RECT pendingItemBounds_{};
    HWND pendingOwner_ = nullptr;
    UINT pendingDpi_ = USER_DEFAULT_SCREEN_DPI;
    bool pendingLightTheme_ = true;
    modern_menu::Appearance pendingAppearance_ =
        modern_menu::Appearance::FollowSystem;
    ApplyHandler pendingOnApply_;
    std::unordered_map<std::wstring, Bitmap> cardFrameCache_;
    widget_preview::Wallpaper desktopWallpaper_;
    RECT desktopWallpaperBounds_{};
    struct WallpaperEngineCaptureState
    {
        std::atomic_bool cancelled = false;
        std::mutex mutex;
        widget_preview::Wallpaper wallpaper;
        RECT requestedBounds{};
        RECT desktopBounds{};
        std::uint64_t generation = 0;
        bool completed = false;
    };
    std::shared_ptr<WallpaperEngineCaptureState>
        wallpaperEngineCaptureState_;
    std::thread wallpaperEngineCaptureThread_;
    std::uint64_t wallpaperEngineCaptureGeneration_ = 0;
    WallpaperBackdropCaptureHandler wallpaperBackdropCaptureHandler_;
    std::shared_ptr<const widget_preview::Wallpaper>
        wallpaperEngineCache_;
    RECT wallpaperEngineCacheBounds_{};
    std::shared_ptr<const widget_preview::Wallpaper>
        desktopWallpaperEngineFrame_;
    bool waitingForWallpaperEngineFrame_ = false;
    std::vector<POINT> committedPositions_;
};

} // namespace snowdesktop::component_preview
