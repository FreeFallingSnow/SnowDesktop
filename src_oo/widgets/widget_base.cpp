#include "widget.h"
#include "types.h"
#include "constants.h"
#include "utils.h"
#include "app.h"
#include <d2d1_1.h>
#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// ── Widget base (Item only) ─────────────────────────────────

Widget::Widget(DesktopWidget* data, DesktopApp* app)
    : data_(data), app_(app) {}

std::wstring Widget::GetTitle() const { return data_ ? data_->title : L""; }
std::wstring Widget::GetPath() const { return L""; }
HBITMAP Widget::GetIconBitmap() const { return nullptr; }
RECT Widget::GetBounds() const { return data_ ? data_->bounds : RECT{}; }
void Widget::SetBounds(RECT bounds) { if (data_) data_->bounds = bounds; }
bool Widget::IsSelected() const { return data_ && data_->selected; }
void Widget::SetSelected(bool selected) { if (data_) data_->selected = selected; }
Container* Widget::GetContainer() const { return nullptr; }

void Widget::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    (void)context;
    (void)rect;
    (void)state;
}

ComPtr<IDataObject> Widget::CreateDataObject()
{
    return nullptr;
}

// ── WidgetContainer geometry ──────────────────────────────────

std::vector<std::unique_ptr<Slot>> WidgetContainer::BuildSlots()
{
    slotItemCache_.clear();

    std::vector<std::unique_ptr<Slot>> slots;
    size_t count = GetSlotCount();
    if (count == 0 && !IncludeTrailingEmptySlot()) return slots;

    RECT body = GetBodyRect();
    int bodyW = std::max(1L, static_cast<LONG>(body.right - body.left));
    int itemW = GetItemWidth();
    int itemH = GetItemHeight();
    int cols = SingleColumn() ? 1 : std::max(1, bodyW / std::max(1, itemW));

    size_t total = IncludeTrailingEmptySlot() ? count + 1 : count;
    for (size_t idx = 0; idx < total; ++idx)
    {
        int col = static_cast<int>(idx) % cols;
        int row = static_cast<int>(idx) / cols;
        RECT cell = {
            body.left + col * itemW,
            body.top + row * itemH,
            body.left + std::min<LONG>(col * itemW + itemW, body.right),
            body.top + row * itemH + itemH
        };
        auto slot = std::make_unique<Slot>(this, cell, idx);
        Item* item = GetSlotItem(idx);
        if (item) item->SetBounds(cell);
        slot->SetItem(item);
        slots.push_back(std::move(slot));
    }
    return slots;
}

RECT WidgetContainer::GetFrameRect() const
{
    if (!data_) return {};
    RECT rect = data_->bounds;
    if (rect.right - rect.left > 16 && rect.bottom - rect.top > 16)
        InflateRect(&rect, -4, -4);
    return rect;
}

RECT WidgetContainer::GetBodyRect() const
{
    RECT frame = GetFrameRect();
    if (data_->type == DesktopWidgetType::Collection && data_->gridSpan.rows <= 1)
        return frame;
    frame.bottom = std::max<LONG>(frame.top + 24, frame.bottom - 24);
    return frame;
}

RECT WidgetContainer::GetMoveHandleRect() const
{
    RECT frame = GetFrameRect();
    constexpr int handleHeight = 24;
    return {
        frame.left + 4,
        std::max<LONG>(frame.top, frame.bottom - handleHeight - 2),
        frame.right - 4,
        frame.bottom - 2
    };
}

RECT WidgetContainer::GetResizeHandleRect() const
{
    RECT handle = GetMoveHandleRect();
    constexpr int handleWidth = 24;
    return {
        std::max<LONG>(handle.left, handle.right - handleWidth),
        handle.top,
        handle.right,
        handle.bottom
    };
}

RECT WidgetContainer::GetTitleRect() const
{
    RECT handle = GetMoveHandleRect();
    LONG left = handle.left + 4;
    const int reserved = data_->type == DesktopWidgetType::FolderMapping ? 60 : 26;
    LONG right = std::max<LONG>(left + 1, handle.right - reserved);
    return { left, handle.top + 2, right, handle.bottom - 2 };
}

// ── Hit testing ───────────────────────────────────────────────

bool WidgetContainer::HitResizeHandle(POINT pt) const
{
    RECT r = GetResizeHandleRect();
    return PtInRect(&r, pt) != FALSE;
}

WidgetHit WidgetContainer::HitTestWidget(POINT pt) const
{
    RECT frame = GetFrameRect();
    if (!PtInRect(&frame, pt)) return WidgetHit::None;

    if (HitResizeHandle(pt)) return WidgetHit::ResizeHandle;

    RECT move = GetMoveHandleRect();
    if (PtInRect(&move, pt)) return WidgetHit::MoveHandle;

    return WidgetHit::Content;
}

// ── Container drag virtuals ──────────────────────────────────────

HitRegion WidgetContainer::HitTestDrag(POINT pt, Slot*& outSlot)
{
    outSlot = nullptr;
    RECT frame = GetFrameRect();
    if (!PtInRect(&frame, pt)) return HitRegion::None;

    auto& slots = GetSlots();
    for (size_t i = 0; i < slots.size(); ++i)
    {
        HitRegion region = slots[i]->HitTest(pt);
        if (region != HitRegion::None)
        {
            outSlot = slots[i].get();
            if (region == HitRegion::Handoff)
            {
                Item* item = outSlot->GetItem();
                if (item && item->IsSelected())
                {
                    RECT r = outSlot->GetBounds();
                    region = (pt.y < r.top + (r.bottom - r.top) / 2)
                        ? HitRegion::SortBefore
                        : HitRegion::SortAfter;
                }
            }
            return region;
        }
    }
    // Mouse in frame but not on any slot — sort at end
    return HitRegion::SortAfter;
}

std::wstring WidgetContainer::GetDragHint(Slot* slot, HitRegion region,
    const std::vector<Item*>& sourceItems, Container* origin, int mods) const
{
    if (!slot) return L"释放：放置到此处";
    return slot->GetDropHint(region, sourceItems);
    (void)origin; (void)mods;
}

void WidgetContainer::DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region)
{
    if (!slot || !ctx) return;
    slot->DrawDropIndicator(ctx, region);
}

// ── DrawChrome ────────────────────────────────────────────────

void WidgetContainer::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    if (!data_ || !context) return;

    RECT frame = GetFrameRect();
    RECT body = GetBodyRect();
    if (frame.right <= frame.left || body.bottom <= body.top) return;

    const bool selected = data_->selected;
    const bool hovered = PtInRect(&frame, mousePt) != FALSE;

    D2D1::ColorF fillColor(0.08f, 0.10f, 0.13f, 0.36f);
    D2D1::ColorF borderColor(1.0f, 1.0f, 1.0f, 0.40f);

    float radius = 12.0f;
    float strokeW = selected ? 1.6f : 1.0f;
    D2D1::ColorF selBorder(0.39f, 0.66f, 1.0f, 0.90f);

    // ── 1. Background + border ────────────────────────────────
    {
        ComPtr<ID2D1SolidColorBrush> fillBrush;
        context->CreateSolidColorBrush(fillColor, &fillBrush);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF((float)frame.left, (float)frame.top, (float)frame.right, (float)frame.bottom),
            radius, radius);
        if (fillBrush)
            context->FillRoundedRectangle(rr, fillBrush.Get());

        ComPtr<ID2D1SolidColorBrush> strokeBrush;
        context->CreateSolidColorBrush(selected ? selBorder : borderColor, &strokeBrush);
        if (strokeBrush)
            context->DrawRoundedRectangle(rr, strokeBrush.Get(), strokeW);
    }

    // ── 2. Content (clipped to rounded frame, renders UNDER gradient) ──
    {
        auto* factory = app_->GetD2DFactory();
        ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        if (factory)
            factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(
                    D2D1::RectF((float)frame.left, (float)frame.top, (float)frame.right, (float)frame.bottom),
                    radius, radius), &clipGeo);
        if (clipGeo)
            context->PushLayer(D2D1::LayerParameters(
                D2D1::RectF((float)frame.left, (float)frame.top, (float)frame.right, (float)frame.bottom),
                clipGeo.Get()), nullptr);

        DrawContent(context, body);

        if (clipGeo) context->PopLayer();
    }

    const bool tinyCollection = data_->type == DesktopWidgetType::Collection &&
        data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    const bool persistentBottomBar = tinyCollection ||
        data_->type == DesktopWidgetType::FileCategories ||
        data_->type == DesktopWidgetType::FolderMapping;

    // ── 3. Gradient bottom bar (over content, clipped to frame) ──
    bool showGradient = persistentBottomBar || !data_->bottomBarHover || hovered;
    if (showGradient)
    {
        RECT gradRect = { frame.left, std::max<LONG>(body.top, frame.bottom - 36),
                          frame.right, frame.bottom };
        if (gradRect.bottom > gradRect.top && !IsRectEmptyRect(gradRect))
        {
            ComPtr<ID2D1GradientStopCollection> stops;
            D2D1_GRADIENT_STOP sd[] = {
                { 0.0f, D2D1::ColorF(fillColor.r, fillColor.g, fillColor.b, 0.0f) },
                { 1.0f, D2D1::ColorF(fillColor.r, fillColor.g, fillColor.b, 0.65f) },
            };
            if (SUCCEEDED(context->CreateGradientStopCollection(sd, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stops)) && stops)
            {
                ComPtr<ID2D1LinearGradientBrush> brush;
                if (SUCCEEDED(context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(0.0f, (float)gradRect.top),
                        D2D1::Point2F(0.0f, (float)gradRect.bottom)),
                    stops.Get(), &brush)) && brush)
                {
                    auto* factory = app_->GetD2DFactory();
                    ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
                    bool pushed = false;
                    if (factory && SUCCEEDED(factory->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(
                            D2D1::RectF((float)frame.left, (float)frame.top, (float)frame.right, (float)frame.bottom),
                            radius, radius), &clipGeo)) && clipGeo)
                    {
                        context->PushLayer(D2D1::LayerParameters(
                            D2D1::RectF((float)frame.left, (float)frame.top, (float)frame.right, (float)frame.bottom),
                            clipGeo.Get()), nullptr);
                        pushed = true;
                    }
                    context->FillRectangle(
                        D2D1::RectF((float)gradRect.left, (float)gradRect.top,
                                     (float)gradRect.right, (float)gradRect.bottom),
                        brush.Get());
                    if (pushed) context->PopLayer();
                }
            }
        }
    }

    // ── 4. Bottom-bar items (on top of gradient, visibility per bottomBarHover) ──
    bool showHandle = persistentBottomBar || (data_->bottomBarHover ? hovered : true);

    if (showHandle)
    {
        // Title text (with shadow)
        if (!data_->title.empty() && data_->showTitle)
        {
            RECT titleRect = GetTitleRect();
            LONG tw = titleRect.right - titleRect.left;
            LONG th = titleRect.bottom - titleRect.top;
            if (tw > 0 && th > 0 && app_ && app_->GetDWriteFactory())
            {
                auto* dwrite = app_->GetDWriteFactory();
                ComPtr<IDWriteTextFormat> fmt;
                dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    13.0f, L"", &fmt);
                if (fmt)
                {
                    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                    ComPtr<IDWriteTextLayout> layout;
                    dwrite->CreateTextLayout(data_->title.c_str(),
                        static_cast<UINT32>(data_->title.size()), fmt.Get(),
                        (float)tw, (float)th, &layout);
                    if (layout)
                    {
                        // Shadow
                        ComPtr<ID2D1SolidColorBrush> shadowBrush;
                        context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f), &shadowBrush);
                        if (shadowBrush)
                            context->DrawTextLayout(
                                D2D1::Point2F((float)titleRect.left + 1.0f, (float)titleRect.top + 1.0f),
                                layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

                        ComPtr<ID2D1SolidColorBrush> textBrush;
                        context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), &textBrush);
                        if (textBrush)
                            context->DrawTextLayout(
                                D2D1::Point2F((float)titleRect.left, (float)titleRect.top),
                                layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
            }
        }

        // Resize handle dot
        {
            RECT rh = GetResizeHandleRect();
            const int dot = 8;
            int cx = rh.left + (rh.right - rh.left) / 2;
            int cy = rh.top + (rh.bottom - rh.top) / 2;
            D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                D2D1::RectF((float)(cx - dot/2), (float)(cy - dot/2),
                             (float)(cx + dot/2), (float)(cy + dot/2)),
                4.0f, 4.0f);
            D2D1::ColorF dotFill = selected
                ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.62f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f);
            D2D1::ColorF dotStroke(1.0f, 1.0f, 1.0f, 0.50f);

            ComPtr<ID2D1SolidColorBrush> b;
            if (SUCCEEDED(context->CreateSolidColorBrush(dotFill, &b)) && b)
                context->FillRoundedRectangle(pill, b.Get());
            if (SUCCEEDED(context->CreateSolidColorBrush(dotStroke, &b)) && b)
                context->DrawRoundedRectangle(pill, b.Get(), 1.0f);
        }

        // Subclass buttons
        {
            RECT handle = GetMoveHandleRect();
            DrawButtons(context, handle, hovered);
        }
    }
}

// ── Factory ─────────────────────────────────────────────────

std::unique_ptr<Widget> CreateWidget(DesktopWidget* data, DesktopApp* app)
{
    if (!data) return nullptr;
    switch (data->type)
    {
    case DesktopWidgetType::Collection:
        return std::make_unique<Collection>(data, app);
    case DesktopWidgetType::FileCategories:
        return std::make_unique<FileCategories>(data, app);
    case DesktopWidgetType::FolderMapping:
        return std::make_unique<FolderMapping>(data, app);
    case DesktopWidgetType::LuaScript:
        return std::make_unique<LuaScript>(data, app);
    }
    return nullptr;
}
