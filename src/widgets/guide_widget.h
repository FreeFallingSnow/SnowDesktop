#pragma once

#include "core/widget.h"

/**
 * @class GuideWidget
 * @brief 分页系统使用指南组件
 *
 * GuideWidget 继承 WidgetContainer，作为新页面的欢迎卡片和操作入口。
 * 无子项、不接受拖放；删除卡片后，现有空页清理流程会移除该页面。
 */
class GuideWidget : public WidgetContainer
{
protected:
    void CollectControlRegions(std::vector<WidgetControlRegion>& regions) const override;
public:
    using WidgetContainer::WidgetContainer;

    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SurfaceForWidgetType(
                DesktopWidgetType::Guide);
    }
    size_t GetSlotCount() const override { return 0; }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override { return {}; }
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    WidgetHit HitTestWidget(POINT pt) const override;
    int GetMaxScrollOffset() const override { return 0; }
    HitRegion HitTestDrag(POINT /*pt*/, Slot*& outSlot) override { outSlot = nullptr; return HitRegion::None; }
    std::wstring GetDragHint(Slot*, HitRegion, const std::vector<Item*>&, Container*, int) const override { return L""; }
    void OnItemsDropped(const std::vector<Item*>&, Container*, Slot*, HitRegion, int) override {}
    std::vector<Item*> GetSelectedItems() const override { return {}; }
    void ToggleDetails() { detailsExpanded_ = !detailsExpanded_; }
    // Shared drawing, hit testing and internal automation geometry.
    RECT GetPrimaryButtonRect(RECT body) const;
    RECT GetSecondaryButtonRect(RECT body) const;

private:
    bool detailsExpanded_ = false;
};
