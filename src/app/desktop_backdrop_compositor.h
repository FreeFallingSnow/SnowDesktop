/**
 * @file desktop_backdrop_compositor.h
 * @brief 使用 Windows Visual Layer 为桌面面板提供系统原生 backdrop 模糊。
 */
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct ID2D1Device;

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
    bool Initialize(HWND contentWindow);
    /**
     * @brief 为指定顶层弹出窗口创建原生 backdrop 合成窗口。
     * @param topmost 辅助窗口是否加入 TOPMOST 带；必须与内容窗口策略一致。
     */
    bool InitializePopup(
        HWND contentWindow, bool topmost = true,
        bool initiallyVisible = true);
    /**
     * @brief 在一个延迟窗口事务中同步 popup 内容与 backdrop 的 Z 序。
     * @param contentInsertAfter 内容窗口应插入到其后的窗口或特殊 HWND 值。
     * @param topmost 事务完成后窗口对是否位于 TOPMOST 带。
     */
    void SetPopupWindowPairZOrder(
        HWND contentWindow, HWND contentInsertAfter,
        bool topmost);
    /** @brief 临时切换顶层 popup backdrop 所在的 Z 序带。 */
    void SetPopupTopmost(bool topmost);
    /** @brief 内容窗口更换桌面宿主后，同步 backdrop 窗口的 parent 和层级。 */
    void Reattach(HWND contentWindow);
    /** @brief 显示或隐藏独立的 backdrop 辅助窗口。 */
    void SetVisible(bool visible);
    /** @brief 在一个延迟窗口事务中同时显示 popup 内容与 backdrop。 */
    void ShowPopupWindowPair(HWND contentWindow);
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
    /** @brief 由 Windows Composition 自驱根视觉缩放，避免 UI 线程逐帧提交。 */
    [[nodiscard]] bool StartVisualScaleAnimation(
        float fromScale, float toScale, float opacity,
        float anchorX, float anchorY,
        std::uint32_t durationMilliseconds,
        float normalizedStartSlope = 0.0f);
    /** @brief 由 Windows Composition 自驱根视觉缩放和透明度。 */
    [[nodiscard]] bool StartVisualTransformAnimation(
        float fromScale, float toScale,
        float fromOpacity, float toOpacity,
        float anchorX, float anchorY,
        std::uint32_t durationMilliseconds,
        float normalizedStartSlope = 0.0f);
    /** @brief 开始收集一帧的玻璃区域。完整帧会在 EndFrame 清理未再次出现的视觉。 */
    void BeginFrame(bool completeCollection);
    /**
     * @brief 注册或更新一个圆角玻璃面板。
     * @param ownerKey 非零时作为跨几何变化保持稳定的面板身份；零值保留
     *        旧的矩形身份语义，供静态面板使用。
     */
    bool AddPanel(
        const RECT& frame, float cornerRadius, float blurRadius,
        std::uintptr_t ownerKey = 0);
    /** @brief 立即移除指定矩形对应的玻璃面板并同步辅助窗口区域。 */
    bool RemovePanel(const RECT& frame);
    /** @brief 在完整收集帧中保留一个由交接事务临时拥有的面板。 */
    bool KeepPanel(const RECT& frame);
    /** @brief 修改指定面板透明度；由 CommitVisualChanges 统一提交。 */
    bool SetPanelOpacity(const RECT& frame, float opacity);
    /** @brief 修改根视觉透明度；由 CommitVisualChanges 统一提交。 */
    bool SetVisualOpacity(float opacity);
    /** @brief 提交同线程所有 backdrop 目标的共享视觉事务。 */
    void CommitVisualChanges();
    /**
     * @brief 提交共享视觉事务，并在该批次真正完成后投递窗口消息。
     *
     * 用于跨 HWND 的视觉交接；接收方可在通知后安全回收旧目标，避免
     * RequestCommitAsync 尚未落屏时提前隐藏旧玻璃层。
     */
    bool CommitVisualChangesAndNotify(
        HWND notifyWindow, UINT message, WPARAM token);
    /**
     * @brief 结束本帧面板集合并同步辅助窗口区域。
     * @param requestCommit 是否立即请求非阻塞合成提交；跨目标交接可延后到
     *        两侧属性全部设置后，通过 CommitVisualChangesAndNotify 一次提交。
     */
    void EndFrame(bool requestCommit = true);
    /** @brief 销毁合成目标和辅助窗口。 */
    void Reset();

    bool IsAvailable() const;
    /** @brief 判断窗口是否为该实例创建的 backdrop 辅助窗口。 */
    bool IsBackdropWindow(HWND window) const;
    std::size_t PanelCount() const;
    const std::wstring& LastError() const;

private:
    bool InitializeInternal(
        HWND contentWindow, bool popupMode,
        bool popupTopmost = false,
        bool initiallyVisible = true);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
