#pragma once

#include <d2d1.h>
#include <windows.h>
#include "navigation/navigation_settings.h"
#include "ui/render/rgba_color.h"
#include <array>
#include <cstdlib>
#include <utility>

// Colors shared by quick-navigation rendering and desktop label rendering.

struct QuickNavTheme {
    snowdesktop::RgbaColor windowBg, windowBorder;
    snowdesktop::RgbaColor searchBg, searchBorder, searchEditBg;
    snowdesktop::RgbaColor tabActiveFill, tabActiveStroke;
    snowdesktop::RgbaColor tabHoverFill, tabHoverStroke;
    snowdesktop::RgbaColor tabDefaultFill, tabDefaultStroke;
    snowdesktop::RgbaColor tabText, tabSeparator;
    snowdesktop::RgbaColor tabDragFill, tabDragStroke;
    snowdesktop::RgbaColor tabDragFloatFill, tabDragFloatStroke, tabDragFloatText;
    snowdesktop::RgbaColor tabDragIndicator;
    snowdesktop::RgbaColor itemHoverFill, itemHoverStroke;
    snowdesktop::RgbaColor itemText;
    snowdesktop::RgbaColor headerText, headerSeparator;
    snowdesktop::RgbaColor appRowHoverFill, appRowHoverStroke;
    snowdesktop::RgbaColor appNameText, appTypeText;
    snowdesktop::RgbaColor expandHoverText, expandDefaultText;
    snowdesktop::RgbaColor emptyText, emptyHeaderText;
    snowdesktop::RgbaColor scrollTrack, scrollThumbDefault, scrollThumbHover;

    D2D1_COLOR_F popupBg, popupBorder, popupTitle;
    D2D1_COLOR_F iconHoverBgFill, iconHoverBgStroke;
    D2D1_COLOR_F iconSelectBgFill, iconSelectBgStroke;
    D2D1_COLOR_F iconTextColor;
    D2D1_COLOR_F iconShadowFallback;
    snowdesktop::RgbaColor searchText = RGB(28,34,44), searchPlaceholder = RGB(100,105,115), searchFocus = RGB(75,120,205);
    snowdesktop::RgbaColor typeFill = RGB(224,232,246), typeText = RGB(45,80,145);
    snowdesktop::RgbaColor resultFill = RGB(246,248,252), resultBorder = RGB(246,248,252);
    snowdesktop::RgbaColor tabActiveText = RGB(30,60,110), tabHoverText = RGB(28,34,44);
    snowdesktop::RgbaColor iconPlateFill = RGB(255,255,255), iconPlateBorder = RGB(255,255,255);
    snowdesktop::RgbaColor selectedFill = RGB(222,234,252), selectedBorder = RGB(185,210,246), selectedText = RGB(28,34,44);
};

inline const QuickNavTheme kQuickNavDark = {
    // GDI
    RGB(18, 22, 30),    // windowBg
    RGB(120, 130, 150),  // windowBorder
    RGB(35, 40, 51),    // searchBg
    RGB(92, 105, 128),   // searchBorder
    RGB(35, 40, 51),     // searchEditBg
    RGB(48, 112, 215),   // tabActiveFill
    RGB(82, 140, 235),   // tabActiveStroke
    RGB(66, 72, 84),     // tabHoverFill
    RGB(68, 76, 92),     // tabHoverStroke
    RGB(42, 47, 58),     // tabDefaultFill
    RGB(68, 76, 92),     // tabDefaultStroke
    RGB(245, 248, 252),  // tabText
    RGB(90, 96, 110),    // tabSeparator
    RGB(80, 92, 112),    // tabDragFill
    RGB(120, 140, 180),  // tabDragStroke
    RGB(60, 80, 110),    // tabDragFloatFill
    RGB(100, 130, 200),  // tabDragFloatStroke
    RGB(245, 248, 252),  // tabDragFloatText
    RGB(82, 140, 235),   // tabDragIndicator
    RGB(58, 68, 86),     // itemHoverFill
    RGB(78, 92, 118),    // itemHoverStroke
    RGB(245, 248, 252),  // itemText
    RGB(182, 194, 212),  // headerText
    RGB(48, 56, 70),     // headerSeparator
    RGB(46, 56, 72),     // appRowHoverFill
    RGB(68, 82, 106),    // appRowHoverStroke
    RGB(245, 248, 252),  // appNameText
    RGB(146, 156, 174),  // appTypeText
    RGB(226, 236, 252),  // expandHoverText
    RGB(170, 184, 208),  // expandDefaultText
    RGB(160, 168, 182),  // emptyText
    RGB(182, 194, 212),  // emptyHeaderText
    RGB(55, 62, 76),     // scrollTrack
    RGB(136, 146, 166),  // scrollThumbDefault
    RGB(167, 178, 199),  // scrollThumbHover
    // D2D
    {0.08f, 0.10f, 0.13f, 1.0f},     // popupBg
    {1.0f, 1.0f, 1.0f, 0.50f},       // popupBorder
    {1.0f, 1.0f, 1.0f, 1.0f},        // popupTitle
    {1.0f, 1.0f, 1.0f, 0.08f},       // iconHoverBgFill
    {1.0f, 1.0f, 1.0f, 0.20f},       // iconHoverBgStroke
    {0.55f, 0.55f, 0.55f, 0.34f},    // iconSelectBgFill
    {0.78f, 0.78f, 0.78f, 0.55f},    // iconSelectBgStroke
    {1.0f, 1.0f, 1.0f, 1.0f},        // iconTextColor
    {0.0f, 0.0f, 0.0f, 0.80f},       // iconShadowFallback
};

inline const QuickNavTheme kQuickNavLight = {
    // GDI
    RGB(246, 248, 252),  // windowBg
    RGB(180, 190, 200),  // windowBorder
    RGB(255, 255, 255),  // searchBg
    RGB(170, 182, 198),  // searchBorder
    RGB(255, 255, 255),  // searchEditBg
    RGB(48, 112, 215),   // tabActiveFill
    RGB(82, 140, 235),   // tabActiveStroke
    RGB(215, 222, 236),  // tabHoverFill
    RGB(190, 200, 215),  // tabHoverStroke
    RGB(230, 234, 242),  // tabDefaultFill
    RGB(190, 200, 215),  // tabDefaultStroke
    RGB(28, 34, 44),     // tabText
    RGB(200, 208, 220),  // tabSeparator
    RGB(195, 205, 220),  // tabDragFill
    RGB(140, 160, 190),  // tabDragStroke
    RGB(180, 195, 215),  // tabDragFloatFill
    RGB(120, 150, 200),  // tabDragFloatStroke
    RGB(28, 34, 44),     // tabDragFloatText
    RGB(82, 140, 235),   // tabDragIndicator
    RGB(220, 228, 240),  // itemHoverFill
    RGB(190, 200, 218),  // itemHoverStroke
    RGB(28, 34, 44),     // itemText
    RGB(110, 120, 138),  // headerText
    RGB(210, 218, 228),  // headerSeparator
    RGB(222, 228, 242),  // appRowHoverFill
    RGB(190, 200, 218),  // appRowHoverStroke
    RGB(28, 34, 44),     // appNameText
    RGB(125, 135, 152),  // appTypeText
    RGB(48, 112, 215),   // expandHoverText
    RGB(100, 112, 135),  // expandDefaultText
    RGB(135, 145, 162),  // emptyText
    RGB(110, 120, 138),  // emptyHeaderText
    RGB(215, 222, 232),  // scrollTrack
    RGB(168, 178, 198),  // scrollThumbDefault
    RGB(140, 155, 175),  // scrollThumbHover
    // D2D
    {0.96f, 0.97f, 0.98f, 1.0f},     // popupBg
    {0.50f, 0.55f, 0.60f, 0.50f},    // popupBorder
    {0.10f, 0.12f, 0.16f, 1.0f},     // popupTitle
    {0.0f, 0.0f, 0.0f, 0.06f},       // iconHoverBgFill
    {0.0f, 0.0f, 0.0f, 0.12f},       // iconHoverBgStroke
    {0.20f, 0.40f, 0.70f, 0.18f},    // iconSelectBgFill
    {0.25f, 0.50f, 0.80f, 0.40f},    // iconSelectBgStroke
    {0.10f, 0.12f, 0.16f, 1.0f},     // iconTextColor
    {0.0f, 0.0f, 0.0f, 0.12f},       // iconShadowFallback
};

// ── Graphics ─────────────────────────────────────────────────
inline constexpr std::pair<const char*, snowdesktop::RgbaColor QuickNavTheme::*> kQuickNavColorFields[] = {
    {"searchBg", &QuickNavTheme::searchBg}, {"searchBorder", &QuickNavTheme::searchBorder},
    {"searchText", &QuickNavTheme::searchText}, {"searchPlaceholder", &QuickNavTheme::searchPlaceholder},
    {"searchFocus", &QuickNavTheme::searchFocus}, {"typeFill", &QuickNavTheme::typeFill}, {"typeText", &QuickNavTheme::typeText},
    {"tabDefaultFill", &QuickNavTheme::tabDefaultFill}, {"tabDefaultStroke", &QuickNavTheme::tabDefaultStroke},
    {"tabText", &QuickNavTheme::tabText}, {"tabHoverFill", &QuickNavTheme::tabHoverFill}, {"tabHoverStroke", &QuickNavTheme::tabHoverStroke},
    {"tabActiveFill", &QuickNavTheme::tabActiveFill}, {"tabActiveStroke", &QuickNavTheme::tabActiveStroke},
    {"tabActiveText", &QuickNavTheme::tabActiveText}, {"tabHoverText", &QuickNavTheme::tabHoverText},
    {"iconPlateFill", &QuickNavTheme::iconPlateFill}, {"iconPlateBorder", &QuickNavTheme::iconPlateBorder},
    {"headerText", &QuickNavTheme::headerText}, {"headerSeparator", &QuickNavTheme::headerSeparator},
    {"resultFill", &QuickNavTheme::resultFill}, {"resultBorder", &QuickNavTheme::resultBorder},
    {"appNameText", &QuickNavTheme::appNameText}, {"appTypeText", &QuickNavTheme::appTypeText},
    {"appRowHoverFill", &QuickNavTheme::appRowHoverFill}, {"appRowHoverStroke", &QuickNavTheme::appRowHoverStroke},
    {"selectedFill", &QuickNavTheme::selectedFill}, {"selectedBorder", &QuickNavTheme::selectedBorder}, {"selectedText", &QuickNavTheme::selectedText},
    {"itemText", &QuickNavTheme::itemText}, {"itemHoverFill", &QuickNavTheme::itemHoverFill}, {"itemHoverStroke", &QuickNavTheme::itemHoverStroke},
    {"scrollTrack", &QuickNavTheme::scrollTrack}, {"scrollThumbDefault", &QuickNavTheme::scrollThumbDefault},
    {"scrollThumbHover", &QuickNavTheme::scrollThumbHover}, {"emptyText", &QuickNavTheme::emptyText}
};

inline QuickNavTheme ResolveQuickNavTheme(bool light, const NavigationSettings& settings, bool glass = false)
{
    QuickNavTheme t = light ? kQuickNavLight : kQuickNavDark;
    t.searchBg = light ? RGB(250,250,250) : RGB(44,44,44);
    t.searchEditBg = t.searchBg;
    t.searchBorder = light ? RGB(185,185,185) : RGB(86,86,86);
    t.searchText = light ? RGB(36,36,36) : RGB(240,240,240);
    t.searchPlaceholder = light ? RGB(104,104,104) : RGB(177,177,177);
    t.tabText = light ? RGB(69,69,69) : RGB(207,207,207);
    t.tabActiveText = t.searchText; t.tabHoverText = t.searchText;
    t.appTypeText = light ? RGB(96,96,96) : RGB(168,168,168);
    t.headerText = t.appTypeText; t.tabSeparator = light ? RGB(164,164,164) : RGB(103,103,103);
    t.emptyText = t.appTypeText;
    t.searchFocus = light ? RGB(68,112,199) : RGB(117,161,239);
    t.tabDefaultFill = light ? RGB(239,239,239) : RGB(38,38,38);
    t.tabDefaultStroke = t.tabDefaultFill;
    t.tabActiveFill = light ? RGB(230,230,230) : RGB(60,60,60);
    t.tabActiveStroke = t.tabActiveFill;
    t.tabHoverFill = light ? RGB(239,239,239) : RGB(47,47,47);
    t.tabHoverStroke = t.tabHoverFill;
    t.typeFill = t.tabActiveFill; t.typeText = t.searchText;
    t.resultFill = light ? RGB(248,248,248) : RGB(30,30,30);
    t.resultBorder = t.resultFill;
    t.selectedFill = t.tabActiveFill; t.selectedBorder = t.tabActiveStroke; t.selectedText = t.searchText;
    t.appRowHoverFill = t.tabHoverFill; t.appRowHoverStroke = t.tabHoverFill;
    t.itemHoverFill = t.tabHoverFill; t.itemHoverStroke = t.tabHoverStroke;
    t.headerSeparator = light ? RGB(205,205,205) : RGB(78,78,78);
    t.scrollTrack = light ? RGB(228,228,228) : RGB(45,45,45);
    t.scrollThumbDefault = light ? RGB(148,148,148) : RGB(116,116,116);
    t.scrollThumbHover = light ? RGB(120,120,120) : RGB(160,160,160);
    t.appNameText = light ? RGB(36,36,36) : RGB(245,245,245);
    // Transparent default colors replace the old presence-based paint gates.
    t.resultFill.alpha = t.resultBorder.alpha = 0.f;
    t.iconPlateFill.alpha = t.iconPlateBorder.alpha = 0.f;
    t.tabDefaultFill.alpha = t.tabDefaultStroke.alpha = 0.f;
    t.tabActiveStroke.alpha = t.tabHoverStroke.alpha = 0.f;
    t.tabActiveFill.alpha = .82f; t.tabHoverFill.alpha = .72f;
    t.searchBorder.alpha = .4f; t.headerSeparator.alpha = .45f;
    t.searchBg.alpha = glass ? .52f : 1.f;
    for (const auto& [name, field] : kQuickNavColorFields)
    {
        auto it = settings.colors.find(name);
        snowdesktop::RgbaColor color;
        if (it != settings.colors.end() && snowdesktop::DecodeRgbaColor(it->second, color)) t.*field = color;
    }
    t.searchEditBg = t.searchBg;
    return t;
}
