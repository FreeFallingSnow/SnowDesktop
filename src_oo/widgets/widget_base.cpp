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
    const int reserved = data_->type == DesktopWidgetType::FolderMapping ? 76 : 26;
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

// ── DrawChrome ────────────────────────────────────────────────

void WidgetContainer::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    if (!data_ || !context) return;

    RECT frame = GetFrameRect();
    RECT body = GetBodyRect();
    if (frame.right <= frame.left || body.bottom <= body.top) return;

    const bool selected = data_->selected;
    const bool hovered = PtInRect(&frame, mousePt) != FALSE;

    // Colors — TODO: read from personalization
    D2D1::ColorF fillColor(0.08f, 0.10f, 0.13f, 0.36f);
    D2D1::ColorF borderColor(1.0f, 1.0f, 1.0f, 0.40f);

    float radius = 12.0f;
    float strokeW = selected ? 1.6f : 1.0f;
    D2D1::ColorF selBorder(0.39f, 0.66f, 1.0f, 0.90f);

    // ── Background + border ────────────────────────────────
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
            context->DrawRoundedRectangle(rr, strokeBrush.Get(), strokeW,
                selected ? nullptr : nullptr);
    }

    // ── Gradient bottom bar (clipped to rounded frame) ────
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
                    // Clip gradient to rounded frame
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

    // ── Resize handle dot ──────────────────────────────────
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

    // ── Title text ─────────────────────────────────────────
    if (!data_->title.empty())
    {
        RECT titleRect = GetTitleRect();
        // TODO: use DWrite text rendering via app helper
        (void)titleRect;
    }

    // ── Subclass buttons ───────────────────────────────────
    {
        RECT handle = GetMoveHandleRect();
        DrawButtons(context, handle, hovered);
    }

    // ── Content ────────────────────────────────────────────
    {
        DrawContent(context, body);
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
