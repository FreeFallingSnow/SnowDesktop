/**
 * @file desktop_backdrop_compositor.h
 * @brief 使用 Windows Visual Layer 为桌面面板提供系统原生 backdrop 模糊。
 */
#pragma once

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>

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
    /** @brief 为指定顶层弹出窗口创建原生 backdrop 合成窗口。 */
    bool InitializePopup(HWND contentWindow);
    /** @brief 内容窗口更换桌面宿主后，同步 backdrop 窗口的 parent 和层级。 */
    void Reattach(HWND contentWindow);
    /** @brief 显示或隐藏独立的 backdrop 辅助窗口。 */
    void SetVisible(bool visible);
    /** @brief 开始收集一帧的玻璃区域。完整帧会在 EndFrame 清理未再次出现的视觉。 */
    void BeginFrame(bool completeCollection);
    /** @brief 注册或更新一个圆角玻璃面板。 */
    bool AddPanel(const RECT& frame, float cornerRadius, float blurRadius);
    /** @brief 立即移除指定矩形对应的玻璃面板。 */
    bool RemovePanel(const RECT& frame);
    /** @brief 提交本帧面板集合。 */
    void EndFrame();
    /** @brief 销毁合成目标和辅助窗口。 */
    void Reset();

    bool IsAvailable() const;
    /** @brief 判断窗口是否为该实例创建的 backdrop 辅助窗口。 */
    bool IsBackdropWindow(HWND window) const;
    std::size_t PanelCount() const;
    const std::wstring& LastError() const;

private:
    bool InitializeInternal(HWND contentWindow, bool popupMode);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
