#pragma once
#include <algorithm>
#include <windows.h>

namespace snowdesktop::widget_title_layout
{
// Shared by Lua chrome painting and the rename overlay.
inline RECT LuaTitleRect(RECT handle, int leading, int vertical, int trailing)
{
    return {handle.left + leading, handle.top + vertical,
        std::max<LONG>(handle.left + leading + 1, handle.right - trailing),
        handle.bottom - vertical};
}
}
