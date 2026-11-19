/**
 * @file desktop_backdrop_compositor.h
 * @brief 使用 Windows Visual Layer 为桌面面板提供系统原生 backdrop 模糊。
 */
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include "desktop/display_color_preferences.h"
#include "icons/icon_beautify.h"
#include "ui/render/icon_backdrop_clip.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>

struct ID2D1Device;
struct IDCompositionDevice2;
struct IDCompositionVisual;
namespace snowdesktop::popup_motion_curve { struct Timeline; }
namespace snowdesktop
{
struct PanelBackdropIcon
{
    RECT frame{};
    IconBeautifyShape shape{};
    float blurRadius = 0;
    std::optional<IconBackdropClip> clip;

    void Offset(LONG x, LONG y)
    {
        OffsetRect(&frame, x, y);
        if (!clip) return;
        OffsetRect(&clip->bounds, x, y);
        clip->geometryOffset.x += x;
        clip->geometryOffset.y += y;
    }
};
}

/**
 * @brief 独立桌面玻璃合成窗口。
 *
 * 该窗口作为 SnowDesktop 内容窗口的下一个同级窗口，位于壁纸之上、
 * SnowDesktop 的透明 DComp 内容之下。每个面板由一个 SpriteVisual 表示，
 * 其画刷直接采样 DWM backdrop，避免应用自行捕获和模糊整幅桌面。
 */
class DesktopBackdropCompositor
{
public:
    DesktopBackdropCompositor();
    ~DesktopBackdropCompositor();

    DesktopBackdropCompositor(const DesktopBackdropCompositor&) = delete;
    DesktopBackdropCompositor& operator=(const DesktopBackdropCompositor&) = delete;

    /** @brief 为指定 SnowDesktop 内容窗口创建原生 backdrop 合成窗口。 */
    bool Initialize(HWND contentWindow, bool initiallyVisible = true);
    /**
     * @brief 为指定顶层弹出窗口创建原生 backdrop 合成窗口。
     * @param topmost 辅助窗口是否加入 TOPMOST 带；必须与内容窗口策略一致。
     */
    bool InitializePopup(
        HWND contentWindow, bool topmost = true,
        bool initiallyVisible = true);
    /** @brief 从窗口线程发起后台效果工厂准备；future 仅用于观察，打开弹窗不等待。 */
    std::shared_future<bool> PrepareBlurFactoryInBackground(float blurRadius);
    /**
     * @brief 安全同步 popup 内容与 backdrop 的 Z 序及 TOPMOST 状态。
     * @param contentInsertAfter 内容窗口应插入到其后的窗口或特殊 HWND 值。
     * @param topmost 操作完成后窗口对是否位于 TOPMOST 带。
     * @param preserveAboveWindow 已在内容窗口上方且不可被本次刷新越过的窗口。
     * @return 请求的配对定位是否成功；失败后的置顶带回退不计为定位成功。
     */
    bool SetPopupWindowPairZOrder(
        HWND contentWindow, HWND contentInsertAfter,
        bool topmost, HWND preserveAboveWindow = nullptr);
    /** @brief 临时切换顶层 popup backdrop 所在的 Z 序带。 */
    void SetPopupTopmost(bool topmost);
    /** @brief 内容窗口更换桌面宿主后，同步 backdrop 窗口的 parent 和层级。 */
    void Reattach(HWND contentWindow);
    /** @brief 显示或隐藏独立的 backdrop 辅助窗口。 */
    void SetVisible(bool visible);
    /** @brief 在一个延迟窗口事务中同时显示 popup 内容与 backdrop。 */
    void ShowPopupWindowPair(HWND contentWindow);
    bool SetExcludedFromPeek(bool excluded);
    HWND PreparePeekInsertBefore(HWND contentWindow);
    /**
     * @brief 在一个延迟窗口事务中同时隐藏 popup 内容窗口和 backdrop。
     *
     * 合成资源会保留，供下次弹窗打开时复用。若 popup backdrop 尚未
     * 初始化或批处理失败，仍会在返回前完成两个窗口的降级隐藏。
     */
    void HidePopupWindowPair(HWND contentWindow);
    /** @brief 同步根视觉围绕指定内容坐标的缩放与透明度。 */
    void SetVisualTransform(
        float scale, float opacity,
        float anchorX, float anchorY);
    // A popup panel keeps its own pose when other panels join the same host.
    // The anchor is in content-window coordinates.
    void SetPopupPanelTransform(std::uintptr_t ownerKey, float scale,
        float opacity, POINT anchor, bool preserveAnimation);
    /** @brief 由 Windows Composition 自驱根视觉缩放，避免 UI 线程逐帧提交。 */
    [[nodiscard]] bool StartVisualScaleAnimation(
        float fromScale, float toScale, float opacity,
        float anchorX, float anchorY,
        std::uint32_t durationMilliseconds,
        float normalizedStartSlope = 0.0f);
    // Animate the root, or the popup panel identified by panelOwnerKey.
    // The anchor is in content-window coordinates in both cases.
    [[nodiscard]] bool StartVisualTransformAnimation(
        float fromScale, float toScale,
        float fromOpacity, float toOpacity,
        float anchorX, float anchorY,
        std::uint32_t durationMilliseconds,
        float normalizedStartSlope = 0.0f,
        const snowdesktop::popup_motion_curve::Timeline* springTimeline = nullptr,
        const snowdesktop::popup_motion_curve::Timeline* opacityTimeline = nullptr,
        std::uintptr_t panelOwnerKey = 0, std::uintptr_t contentKey = 0);
    /**
     * @brief 使用与 Dock 内容相同的分片规则变形一个玻璃面板；矩形均为宿主坐标。
     *        由 CommitVisualChanges 或 EndFrame 统一提交，稳定几何不逐帧分配资源。
     */
    [[nodiscard]] bool SetGenieTransform(
        const RECT& panelFrame, const RECT& dockFrame,
        int edge, float collapsed, float opacity);
    /** @brief 使用与快捷导航内容相同的吸引网格变形玻璃面板。 */
    [[nodiscard]] bool SetAttractionTransform(
        const RECT& panelFrame, const RECT& dockFrame,
        float collapsed, float opacity);
    /** @brief 恢复普通面板及根视觉；与后续正常属性更新一起提交。 */
    void ClearGenieTransform();
    /** @brief 开始收集一帧的玻璃区域。完整帧会在 EndFrame 清理未再次出现的视觉。 */
    void BeginFrame(bool completeCollection);
    // Dock full redraws collect plain panel data until their content surface
    // is ready. No live visual, native region, or seen flag changes here, so
    // another target's shared-controller commit cannot publish this candidate.
    // Applied Dock frames retain the host-sized HWND fence; Composition clips
    // own moving panel contours without prematurely clipping the old frame.
    bool BeginStagedFrame();
    bool ApplyStagedFrame(ID2D1Device* drawDevice = nullptr);
    void DiscardStagedFrame();
    // Dock's content pass contributes silhouettes to the already staged chrome.
    bool CollectStagedPanelIcon(std::uintptr_t ownerKey, snowdesktop::PanelBackdropIcon icon);
    // Apply regions to the existing rounded panel, sampling its backdrop once.
    bool SetPanelIconRegions(std::uintptr_t ownerKey, ID2D1Device* drawDevice,
        std::span<const snowdesktop::PanelBackdropIcon> icons, bool extendOutline);
    // Extra popup allocation for shared Dock icons, with a fixed screen-space display clip.
    bool SetPopupWindowPadding(const RECT& padding, const RECT& displayBounds);
    /**
     * @brief 注册或更新一个圆角玻璃面板。
     * @param ownerKey 非零时作为跨几何变化保持稳定的面板身份；零值保留
     *        旧的矩形身份语义，供静态面板使用。
     */
    bool AddPanel(
        const RECT& frame, float cornerRadius, float blurRadius,
        std::uintptr_t ownerKey = 0, bool smoothCorners = true);
    // Replace the desktop's rectangular background blur regions in the
    // current frame, below the ordinary material panels.
    void SetBackgroundPanels(std::span<const RECT> frames, float blurRadius);
    // Host-private icon regions share the same native blur backend.
    bool AddIconPanel(const RECT& frame, snowdesktop::IconBeautifyShape shape,
        float blurRadius, std::uintptr_t ownerKey,
        const snowdesktop::IconBackdropClip* contentClip = nullptr);
    // The clip is local to a fixed sampling envelope and can animate independently.
    bool AddClippedPanel(const RECT& envelope, ID2D1Geometry* clip,
        float blurRadius, std::uintptr_t ownerKey);
    static snowdesktop::display_color::Compensation SrgbCompensationForFrame(
        HWND window, const RECT& clientFrame);
    static bool IsSrgbHighlightEligible(HWND window, const RECT& clientFrame);

    class SrgbSceneRecording
    {
    public:
        SrgbSceneRecording(ID2D1DeviceContext* context, bool enabled);
        ~SrgbSceneRecording();
        SrgbSceneRecording(const SrgbSceneRecording&) = delete;
        SrgbSceneRecording& operator=(const SrgbSceneRecording&) = delete;

        bool Started() const noexcept { return context_ != nullptr; }
        HRESULT Finish();
        ID2D1CommandList* Scene() const noexcept { return scene_.Get(); }

    private:
        ID2D1DeviceContext* context_ = nullptr;
        Microsoft::WRL::ComPtr<ID2D1Image> target_;
        Microsoft::WRL::ComPtr<ID2D1CommandList> scene_;
    };

    bool StageSrgbContent(HWND window, std::uintptr_t key, const RECT& clientFrame,
        ID2D1DeviceContext* context, POINT atlasOffset, ID2D1CommandList* scene = nullptr);
    // Draw the material and coverage through the same geometry before sampling
    // the backdrop. Coordinates refer to the frame's local pixel space.
    using SrgbMaterialDraw = std::function<HRESULT(ID2D1DeviceContext*, POINT, bool coverage)>;
    using SrgbSceneDraw = std::function<HRESULT(ID2D1DeviceContext*, POINT)>;
    bool StageSrgbMaterialFrame(HWND window, std::uintptr_t key, const RECT& clientFrame,
        ID2D1Device* device, const SrgbMaterialDraw& draw, const SrgbSceneDraw& drawScene = {});
    bool PrepareSrgbMaterials(HWND window, std::uintptr_t key, const RECT& clientFrame,
        ID2D1DeviceContext* context, POINT atlasOffset, float opacity = 1.f,
        ID2D1CommandList* scene = nullptr);
    bool PublishSrgbContent(std::uintptr_t key, float opacity = 1.0f);
    bool HasSrgbContent(std::uintptr_t key) const noexcept;
    // Keep the DirectComposition surface attached. Retire its old highlight
    // visual after DirectComposition accepts the ordinary highlight replacement.
    HRESULT SetDirectContentForSrgb(std::uintptr_t key, bool srgbPublished,
        IDCompositionDevice2* device, IDCompositionVisual* visual, IUnknown* surface);
    void RetireSrgbAfterDirectCommit(std::uintptr_t key);
    void OnDirectContentCommitted(IDCompositionDevice2* device);
    void RemoveSrgbContent(std::uintptr_t key);
    void SetSrgbContentOrder(std::span<const std::uintptr_t> bottomToTop);
    void SetSrgbContentOpacity(std::uintptr_t key, float opacity);
    bool SetSrgbContentPose(std::uintptr_t key, POINT anchor, float scale, float opacity);
    void SetSrgbRootPose(float opacity, float offsetX, float offsetY);
    bool CommitSrgbContent();
    // Host-private large-icon silhouettes use the same geometry as the card.
    bool AddLargeIconPanel(const RECT& frame, int shape, float cornerRadius,
        float blurRadius, std::uintptr_t ownerKey, int flagDirection = 0);
    bool HasPanelContaining(const RECT& frame) const;
    bool RemoveIconPanel(const RECT& frame, std::uintptr_t ownerKey);

    /** @brief Apply a local card matrix and its projected window region in this frame's transaction. */
    bool SetPanelTransform(std::uintptr_t ownerKey,
        const D2D1_MATRIX_4X4_F& matrix, const RECT& projectedFrame);
    // Reserve the full footprint of an animating desktop widget in the HWND
    // region. The panel's visual clip still owns the visible contour.
    // An empty frame restores the normal panel fence.
    void SetPanelAnimationFence(std::uintptr_t ownerKey, const RECT& frame);
    // Animate the clip inside a fixed, fully allocated widget backdrop.
    bool StartPanelHeightAnimation(std::uintptr_t ownerKey,
        float collapsedHeight, float radius,
        const snowdesktop::popup_motion_curve::Timeline& timeline);
    void SetSrgbContentRollClip(std::uintptr_t ownerKey, const RECT& bounds, float radius);
    /** @brief 移除玻璃面板并同步区域；非零 ownerKey 定位稳定身份，零值按矩形查找。 */
    bool RemovePanel(const RECT& frame, std::uintptr_t ownerKey = 0);
    /** @brief 在完整收集帧中保留一个由交接事务临时拥有的面板。 */
    bool KeepPanel(const RECT& frame, std::uintptr_t ownerKey = 0);
    /** @brief 修改指定面板透明度；由 CommitVisualChanges 统一提交。 */
    bool SetPanelOpacity(const RECT& frame, float opacity, std::uintptr_t ownerKey = 0);
    bool StartOwnerFadeOut(std::uintptr_t ownerKey, float fromOpacity,
        UINT durationMilliseconds, float normalizedStartSlope);
    /** @brief 修改根视觉透明度；由 CommitVisualChanges 统一提交。 */
    bool SetVisualOpacity(float opacity);
    // Translation stays inside the stable popup allocation; no AppBar movement.
    void SetVisualTranslation(float x, float y);
    /** @brief 立即提交同线程所有 backdrop 目标的共享视觉事务，不等待 GPU 完成。 */
    void CommitVisualChanges();
    // Submit a completed EndFrame(false) after the matching content commit.
    void CommitDeferredFrame();
    /**
     * @brief 结束本帧面板集合并同步辅助窗口区域。
     * @param requestCommit 是否立即执行合成提交。跨目标交接可在属性准备完成后一起提交。
     */
    void EndFrame(bool requestCommit = true);
    /** @brief 释放背景辅助窗口和面板，保留内容窗口的 sRGB 合成层。 */
    void ResetBackdrop();
    /** @brief 销毁合成目标和辅助窗口。 */
    void Reset();

    // Internal host-local exclusion for a desktop guide above a Dock HWND.
    void SetOcclusionRect(const RECT& bounds);

    bool IsAvailable() const;
    /** @brief 判断窗口是否为该实例创建的 backdrop 辅助窗口。 */
    bool IsBackdropWindow(HWND window) const;
    std::size_t PanelCount() const;
    std::size_t BlurFactoryCount() const;
    const std::wstring& LastError() const;

private:
    bool UpdatePanel(const RECT& frame, float cornerRadius, float blurRadius,
        std::uintptr_t ownerKey, bool smoothCorners, bool customClip, bool background = false);
    bool SetDeformationTransform(const RECT& panelFrame, const RECT& dockFrame,
        int edge, float collapsed, float opacity, bool attraction);
    bool InitializeInternal(
        HWND contentWindow, bool popupMode,
        bool popupTopmost = false,
        bool initiallyVisible = true);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool excludedFromPeek_ = false;
};
