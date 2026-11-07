#pragma once

#include "shell_icon_request.h"
#include "common/types.h"
#include "icons/icon_styled_pixels.h"

#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>

using IconLoadPhase = snowdesktop::shell_icon_request::Phase;

struct IconLoadTask {
    std::wstring sourceStamp;
    uint64_t serial = 0;
    uint64_t popupGeneration = 0;
    std::wstring requestKey;
    std::wstring layoutKey;
    std::wstring widgetId;
    Pidl absolutePidl;
    int sysIconIndex = -1;
    std::wstring parsingName;
    bool isDesktopItem = true;
    std::wstring folderPath;
    IconLoadPhase phase = IconLoadPhase::Phase1;
    int requestedSize = 0;
};

struct IconLoadResult {
    int sysIconIndex = -1;
    std::wstring typeName;
    uint64_t serial = 0;
    uint64_t popupGeneration = 0;
    std::wstring requestKey;
    std::wstring layoutKey;
    std::wstring widgetId;
    HBITMAP bitmap = nullptr;
    std::shared_ptr<const snowdesktop::icon_styled_pixels::Prepared> preparedPixels;
    SIZE bitmapSize{};
    bool shortcutArrow = false;
    bool isShortcut = false;
    bool isApplicationShortcut = false;
    snowdesktop::category_collection_rules::ShortcutTarget shortcutTarget;
    bool iconIsMediaThumbnail = false;
    IconLoadPhase phase = IconLoadPhase::Phase1;
    bool isDesktopItem = true;
    std::wstring folderPath;
};
