#pragma once

#include "../core/widget.h"

// Project only geometry into the popup. Search, rules and selection continue
// to belong to the original component; never copy or prune its file list.
class CategorizedPopupScope
{
public:
    CategorizedPopupScope(ScrollingItemWidget* view, RECT frame)
        : view_(view)
    {
        if (!view_) return;
        wasHosted_ = view_->IsHosted();
        wasPopupHosted_ = view_->IsPopupHosted();
        savedFrame_ = view_->GetLayoutFrameRect();
        view_->SetPopupFrame(&frame);
    }
    ~CategorizedPopupScope()
    {
        if (!view_) return;
        view_->SetPopupFrame(nullptr);
        if (wasPopupHosted_) view_->SetPopupFrame(&savedFrame_);
        else if (wasHosted_) view_->SetHostedFrame(&savedFrame_);
    }
    CategorizedPopupScope(const CategorizedPopupScope&) = delete;
    CategorizedPopupScope& operator=(const CategorizedPopupScope&) = delete;
private:
    ScrollingItemWidget* view_ = nullptr;
    RECT savedFrame_{};
    bool wasHosted_ = false;
    bool wasPopupHosted_ = false;
};
