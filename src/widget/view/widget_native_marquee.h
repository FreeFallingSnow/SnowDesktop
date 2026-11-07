#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

namespace snowdesktop::widget_runtime
{
struct NativeMarqueeText
{
    std::string key;
    D2D1_RECT_F viewport{};
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    float originX = 0.0f;
    float originY = 0.0f;
    float textWidth = 0.0f;
    float textHeight = 0.0f;
    float speed = 24.0f;
    float gap = 24.0f;
    float offset = 0.0f;
    int color = 0xFFFFFF;
    float alpha = 1.0f;
    bool scrolling = false;
};
}
