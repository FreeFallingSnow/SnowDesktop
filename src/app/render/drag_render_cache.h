#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>

/**
 * @brief 拖拽渲染缓存，用于缓存拖拽操作期间的静态场景位图。
 *
 * 在拖拽过程中，桌面背景的静态部分只需绘制一次并缓存为位图，
 * 后续帧只需要将动态内容（如拖拽图标、放置预览）叠加绘制到
 * 缓存的静态位图上，从而大幅减少每帧的绘制开销。
 */
class DragRenderCache
{
public:
    /** @brief 重置缓存，释放所有 GPU 资源。在窗口尺寸变化或设备丢失时调用。 */
    void Reset();

    /** @brief 丢弃旧内容，保留有界的同尺寸存储供下一次绘制复用。 */
    void Invalidate();
    [[nodiscard]] std::uint64_t BitmapAllocationCount() const { return bitmapAllocationCount_; }

    /**
     * @brief 确保缓存已创建且为最新版本。
     * @param device     D2D 设备对象
     * @param pixelSize  所需的像素尺寸
     * @param revision   场景修订号，修订号未变时跳过重绘以提高性能
     * @param drawStatic 用于绘制静态场景的回调函数
     * @return true 表示缓存已就绪；false 表示创建失败
     */
    bool Ensure(ID2D1Device* device, D2D1_SIZE_U pixelSize, std::uint64_t revision,
        const std::function<void(ID2D1DeviceContext*)>& drawStatic);

    /** @brief 将缓存的静态场景绘制到指定设备上下文中。 @param ctx D2D 设备上下文 */
    void Draw(ID2D1DeviceContext* ctx) const;
    /**
     * @brief 将缓存绘制到指定坐标。
     * @return 缓存存在并完成绘制时返回 true。
     */
    bool DrawAt(
        ID2D1DeviceContext* ctx,
        D2D1_POINT_2F destination,
        D2D1_INTERPOLATION_MODE interpolation =
            D2D1_INTERPOLATION_MODE_LINEAR) const;

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;      /**< 缓存的静态场景位图 */
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_; /**< 离屏渲染设备上下文 */
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    bool contentValid_ = false;
    std::uint64_t bitmapAllocationCount_ = 0;
    UINT width_ = 0;                   /**< 缓存位图宽度 */
    UINT height_ = 0;                  /**< 缓存位图高度 */
    std::uint64_t revision_ = 0;       /**< 当前缓存的修订号，用于判断是否需要重绘 */
};
