#pragma once

#include <d2d1.h>

namespace snowdesktop::icon_loading_placeholder
{
// Borrowed premultiplied bitmap with a stable cache identity for the host's lifetime.
HBITMAP SourceBitmap(bool beautified);
void DrawUnstyled(ID2D1RenderTarget* target, RECT bounds, float opacity);
}
