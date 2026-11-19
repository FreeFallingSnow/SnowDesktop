#pragma once

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1helper.h>
#include <wrl/client.h>

namespace snowdesktop::icon_shadow
{
struct Drawing
{
    Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
    Microsoft::WRL::ComPtr<ID2D1Effect> shadow;
    Microsoft::WRL::ComPtr<ID2D1Image> image;
    D2D1_RECT_F bounds{};
};

template<class DrawIcon>
bool Record(ID2D1DeviceContext* target, ID2D1DeviceContext* recorder,
    RECT icon, float scale, DrawIcon&& drawIcon, Drawing& drawing, bool cache = false)
{
    if (FAILED(recorder->CreateCommandList(&drawing.commands)) ||
        FAILED(target->CreateEffect(CLSID_D2D1Shadow, &drawing.shadow)))
        return false;

    recorder->SetDpi(96, 96);
    recorder->SetTarget(drawing.commands.Get());
    recorder->SetTransform(D2D1::Matrix3x2F::Translation(
        -static_cast<float>(icon.left), -static_cast<float>(icon.top)));
    recorder->BeginDraw();
    drawIcon(recorder);
    const HRESULT recorded = recorder->EndDraw();
    recorder->SetTarget(nullptr);
    if (FAILED(recorded) || FAILED(drawing.commands->Close()))
        return false;

    drawing.shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 2.5f * scale);
    drawing.shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1_VECTOR_4F{0, 0, 0, 0.65f});
    drawing.shadow->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_BALANCED);
    if (cache)
        drawing.shadow->SetValue(D2D1_PROPERTY_CACHED, static_cast<BOOL>(TRUE));
    drawing.shadow->SetInput(0, drawing.commands.Get());
    drawing.shadow->GetOutput(&drawing.image);
    return SUCCEEDED(target->GetImageLocalBounds(drawing.image.Get(), &drawing.bounds));
}

inline void Draw(ID2D1DeviceContext* target, const Drawing& drawing, RECT icon, float scale)
{
    const auto offset = D2D1::Point2F(icon.left + drawing.bounds.left,
        icon.top + drawing.bounds.top + 3.0f * scale);
    target->DrawImage(drawing.image.Get(), &offset, &drawing.bounds);
}
} // namespace snowdesktop::icon_shadow
