#pragma once
#include <commoncontrols.h>

namespace native_system_icon_refresh_cases
{
inline void Run(DesktopApp& app, ID2D1DeviceContext* context)
{
    SHSTOCKICONINFO stock{};
    stock.cbSize = sizeof(stock);
    native_drag_commit_cases::Require(SUCCEEDED(SHGetStockIconInfo(SIID_DOCNOASSOC,
        SHGSI_SYSICONINDEX, &stock)), "resolve a stable real system image-list index");
    const auto fullReference = [index = stock.iSysImageIndex](bool quick) -> HBITMAP {
        ComPtr<IImageList> list;
        if (FAILED(SHGetImageList(quick ? SHIL_EXTRALARGE : SHIL_JUMBO, IID_PPV_ARGS(&list))) || !list)
            SHGetImageList(SHIL_LARGE, IID_PPV_ARGS(&list));
        HICON icon = nullptr;
        if (!list || FAILED(list->GetIcon(index, ILD_TRANSPARENT | ILD_PRESERVEALPHA, &icon)) || !icon)
            return nullptr;
        SIZE size{};
        const auto bitmap = CreateAlphaBitmapFromIcon(icon, 64, 64, size);
        DestroyIcon(icon);
        return bitmap;
    };
    NativeDragIntegrationAccess::RunSystemIconRefreshProbe(app, context, stock.iSysImageIndex, fullReference);
}
}
