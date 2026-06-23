/**
 * @file guide_widget.cpp
 * @brief GuideWidget —— 分页使用指南组件的实现
 */

#include "widget.h"
#include "app.h"
#include "constants.h"
#include "utils.h"

#include <sstream>
#include <algorithm>

/**
 * @brief 根据当前分页状态动态生成指南文本（UTF-16）。
 *
 * 文本包含：当前页 / 总页数 / 显示器数、双锚点锁定状态、
 * 翻页/跳转/新增页/首末屏锁定 等操作说明，以及空页清理规则。
 */
std::wstring GuideWidget::BuildGuideText(const DesktopApp* app)
{
    const auto& savedPageIds = app->savedPageIds_;
    const auto& gridPages = app->gridPages_;
    const int pageOffset = app->pageOffset_;
    const size_t N = gridPages.size();
    const size_t total = savedPageIds.size();
    // 末屏当前显示页 index = (N-1) + pageOffset
    const int currentLastIdx = (N >= 1)
        ? static_cast<int>(N) - 1 + pageOffset
        : 0;

    auto pageLabel = [&](size_t i) -> std::wstring {
        return L"第" + std::to_wstring(i + 1) + L"页";
    };

    std::wostringstream ss;
    ss << L"SnowDesktop 多屏幕分页系统\n\n";

    // ── 状态信息 ──
    ss << L"━━━ 当前状态 ━━━\n";
    ss << L"显示器数：" << N << L"\n";
    ss << L"总页数：" << total << L"\n";
    ss << L"末屏当前显示：" << pageLabel(static_cast<size_t>(
        std::max(0, std::min(currentLastIdx, static_cast<int>(total) - 1)))) << L"\n";

    // 首末屏锁定状态
    const std::wstring& firstPin = app->firstPageMonitorId_;
    const std::wstring& lastPin = app->lastPageMonitorId_;
    auto monitorShortName = [&](const std::wstring& id) -> std::wstring {
        if (id.empty()) return L"（未设置）";
        // 截取友好显示名：去掉 \\.\ 前缀
        std::wstring s = id;
        if (s.starts_with(L"\\.\\")) s = s.substr(4);
        return s;
    };
    ss << L"首屏锁定：" << monitorShortName(firstPin)
       << (firstPin.empty() ? L"（默认主屏）\n" : L"\n");
    ss << L"末屏锁定：" << monitorShortName(lastPin)
       << (lastPin.empty() ? L"（默认最右屏）\n" : L"\n");
    ss << L"\n";

    // ── 核心概念 ──
    ss << L"━━━ 核心概念 ━━━\n";
    ss << L"• 首屏 — 默认在系统主屏，可锁定到任意显示器\n";
    ss << L"• 末屏 — 默认在最右显示器，可锁定到任意显示器\n";
    ss << L"• 前 N-1 个显示器各显示一个固定槽位页\n";
    ss << L"• 末屏可翻页浏览所有溢出页\n";
    ss << L"• 单屏时该屏同时担首屏与末屏\n\n";

    // ── 基本操作 ──
    ss << L"━━━ 基本操作 ━━━\n";
    ss << L"翻页\n";
    ss << L"  点击末屏左右两侧箭头，或\n";
    ss << L"  右键菜单「上一页」/「下一页」\n\n";
    ss << L"跳转\n";
    ss << L"  右键菜单「跳转到」快速定位\n\n";
    ss << L"新增页\n";
    ss << L"  右键菜单「新增页」创建空白页\n";
    ss << L"  自动放置指南组件占位防清理\n\n";
    ss << L"锁定首屏/末屏\n";
    ss << L"  右键菜单「固定此显示器显示首屏」\n";
    ss << L"  右键菜单「固定此显示器显示末屏」\n";
    ss << L"  两锁互斥，再次点击可取消\n";
    ss << L"  锁定持久化，显示器离线不清锁\n\n";

    // ── 提示 ──
    ss << L"━━━ 提示 ━━━\n";
    ss << L"• 溢出区空页会被自动清理\n";
    ss << L"• 槽位页即使空也保留作显示填充\n";
    ss << L"• 首屏永远保留\n";
    ss << L"• 可将文件拖放到任意页面\n";
    ss << L"• 右键菜单「添加组件」放置集合、分类等\n";
    ss << L"• 右键菜单「行列调整」调整每页网格\n";
    ss << L"• 本指南组件可手动右键删除\n";

    return ss.str();
}

void GuideWidget::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!context || !app_ || !data_)
        return;

    auto* dwrite = app_->GetDWriteFactory();
    if (!dwrite)
        return;

    const LONG bodyWidth = (std::max)(1L, body.right - body.left);
    const LONG bodyHeight = (std::max)(1L, body.bottom - body.top);

    const std::wstring wtext = BuildGuideText(app_);

    const float bodyFontSize = FontCu(16.0f);
    const float titleFontSize = FontCu(20.0f);
    const float padX = static_cast<float>(Cu(14.0f));
    const float padY = static_cast<float>(Cu(12.0f));

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, bodyFontSize, L"", &fmt)) || !fmt)
        return;

    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    const float maxWidth = static_cast<float>(bodyWidth) - padX * 2.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size()),
        fmt.Get(), maxWidth, 10000.0f, &layout)) || !layout)
        return;

    // 第一行（标题）加粗放大：通过 text range 设置
    if (wtext.find(L'\n') != std::wstring::npos)
    {
        DWRITE_TEXT_RANGE titleRange{ 0, static_cast<UINT32>(wtext.find(L'\n')) };
        layout->SetFontSize(titleFontSize, titleRange);
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, titleRange);
    }
    // 分隔线行（━━━ 开头）用稍小灰色：保持默认即可

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    totalTextHeight_ = metrics.height + padY * 2.0f;
    lastBodyHeight_ = bodyHeight;

    int maxScroll = (std::max)(0, static_cast<int>(totalTextHeight_ - static_cast<float>(bodyHeight)));
    if (maxScroll > 0)
        data_->scrollOffset = std::clamp(data_->scrollOffset, 0, maxScroll);
    else
        data_->scrollOffset = 0;

    float textX = static_cast<float>(body.left) + padX;
    float textY = static_cast<float>(body.top) + padY - static_cast<float>(data_->scrollOffset);

    ComPtr<ID2D1SolidColorBrush> textBrush;
    context->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.93f, 0.93f, 0.96f), &textBrush);
    if (textBrush)
    {
        ComPtr<ID2D1SolidColorBrush> shadowBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &shadowBrush);
        if (shadowBrush)
            context->DrawTextLayout(D2D1::Point2F(textX + 1.0f, textY + 1.0f),
                layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        context->DrawTextLayout(D2D1::Point2F(textX, textY),
            layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void GuideWidget::DrawScrollbar(ID2D1DeviceContext* context, bool hovered) const
{
    if (!context || !data_) return;

    RECT body = GetBodyRect();
    RECT frame = GetFrameRect();
    LONG bodyHeight = (std::max)(1L, body.bottom - body.top);

    int contentHeight = (std::max)(static_cast<int>(bodyHeight), static_cast<int>(totalTextHeight_));
    int scrollOffset = GetMaxScrollOffset() > 0 ? data_->scrollOffset : 0;

    const LONG sbWidth = Cu(6.0f);
    RECT sbRect = {
        frame.right - sbWidth - Cu(2.0f),
        body.top,
        frame.right - Cu(2.0f),
        body.bottom
    };

    if (sbRect.right <= sbRect.left || contentHeight <= static_cast<int>(bodyHeight))
        return;

    DrawScrollbarAt(context, sbRect, contentHeight, static_cast<int>(bodyHeight),
        scrollOffset, hovered, GetCellScale());
}
