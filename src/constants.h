#pragma once
/**
 * @file constants.h
 * @brief 全局常量定义
 * @details 包含窗口类名、透明色键值、图标尺寸、网格布局参数、系统托盘消息、
 *          桌面图标CLSID、右键菜单命令、外壳变更通知、定时器ID、快捷导航、
 *          集合弹出面板布局等所有全局常量
 */
#include <windows.h>

// ── 窗口类名 ──────────────────────────────────
constexpr wchar_t kWindowClassName[] = L"SnowDesktopNativeProofWindow";
constexpr wchar_t kControlWindowClassName[] = L"SnowDesktopControlWindow";
constexpr wchar_t kHintWindowClassName[] = L"SnowDesktopDragHintWindow";
constexpr wchar_t kQuickNavigationWindowClassName[] = L"SnowDesktopQuickNavigationWindow";
constexpr wchar_t kHiddenBySnowDesktopProp[] = L"SnowDesktop.HiddenExplorerIconLayer";

// ── 透明色键值 ────────────────────────────────
constexpr COLORREF kTransparentKey = RGB(1, 2, 3);

// ── 图标与网格布局 ────────────────────────────
constexpr int kIconSize = 64;
constexpr int kIconBitmapSize = 64;
constexpr int kCellWidth = 92;
constexpr int kMinCellHeight = 116;
constexpr int kGridMarginX = 6;
constexpr int kGridMarginY = 6;
constexpr int kMarginX = kGridMarginX;
constexpr int kMarginY = 6;
constexpr int kTextTop = 70;
constexpr float kItemFontSize = 12.0f;
constexpr float kItemLineHeight = 14.0f;
constexpr float kItemBaseline = 10.0f;
constexpr int kTextCollapsedHeight = 28;
constexpr int kTextExpandedHeight = 48;
constexpr int kTextHeight = kTextCollapsedHeight;
constexpr float kGapPercentX = 0.16f;
constexpr float kGapPercentY = 0.14f;

// ── 重命名编辑框控件ID ────────────────────────
constexpr int kRenameEditId = 1001;

// ── 系统托盘通知 ──────────────────────────────
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;

// ── 托盘右键菜单命令 ──────────────────────────
constexpr UINT kTrayReloadCommand = 40001;
constexpr UINT kTraySortByNameCommand = 40002;
constexpr UINT kTraySwitchNativeCommand = 40003;
constexpr UINT kTraySwitchCustomCommand = 40004;
constexpr UINT kTrayExitCommand = 40005;
constexpr UINT kTraySortByTypeCommand = 40006;
constexpr UINT kTraySettingsCommand = 40012;
constexpr UINT kTrayDesktopIconThisPC = 40007;
constexpr UINT kTrayDesktopIconUserFiles = 40008;
constexpr UINT kTrayDesktopIconNetwork = 40009;
constexpr UINT kTrayDesktopIconControlPanel = 40010;
constexpr UINT kTrayDesktopIconRecycleBin = 40011;

// ── 桌面特殊图标CLSID ─────────────────────────
constexpr wchar_t kDesktopIconClsidThisPC[] = L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
constexpr wchar_t kDesktopIconClsidUserFiles[] = L"{59031A47-3F72-44A7-89C5-5595FE6B30EE}";
constexpr wchar_t kDesktopIconClsidNetwork[] = L"{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}";
constexpr wchar_t kDesktopIconClsidControlPanel[] = L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}";
constexpr wchar_t kDesktopIconClsidRecycleBin[] = L"{645FF040-5081-101B-9F08-00AA002F954E}";

// ── 右键菜单命令ID ────────────────────────────
constexpr UINT kContextOpenCommand = 41001;
constexpr UINT kContextRenameCommand = 41002;
constexpr UINT kContextCutCommand = 41003;
constexpr UINT kContextCopyCommand = 41004;
constexpr UINT kContextPasteCommand = 41005;
constexpr UINT kContextDeleteCommand = 41006;
constexpr UINT kContextRefreshCommand = 41007;
constexpr UINT kContextSortByNameCommand = 41008;
constexpr UINT kContextSortByTypeCommand = 41009;
constexpr UINT kContextMoreCommand = 41010;
constexpr UINT kContextThisDisplayFirstCommand = 41011;
constexpr UINT kContextGridAddRow = 41012;
constexpr UINT kContextGridRemoveRow = 41013;
constexpr UINT kContextGridAddColumn = 41014;
constexpr UINT kContextGridRemoveColumn = 41015;
constexpr UINT kContextZoomIncrease = 41016;
constexpr UINT kContextZoomDecrease = 41017;
constexpr UINT kContextAddCollectionWidget = 41018;
constexpr UINT kContextWidgetOpen = 41019;
constexpr UINT kContextWidgetRename = 41020;
constexpr UINT kContextWidgetDelete = 41021;
constexpr UINT kContextWidgetEdit = 41032;
constexpr UINT kContextAddFileCategoryWidget = 41022;
constexpr UINT kContextWidgetManualCollect = 41023;
constexpr UINT kContextWidgetToggleAutoCollect = 41024;
constexpr UINT kContextWidgetToggleListMode = 41025;
constexpr UINT kContextAddFolderMappingWidget = 41026;
constexpr UINT kContextAddLuaWidgetFirst = 41450;
constexpr UINT kContextLuaWidgetMenuFirst = 41600;
constexpr UINT kContextLuaWidgetMenuLast = 41799;
constexpr UINT kContextWidgetOpenFolder = 41027;
constexpr UINT kContextWidgetToggleFolderView = 41028;
constexpr UINT kContextWidgetSortByName = 41029;
constexpr UINT kContextWidgetSortByType = 41030;
constexpr UINT kContextWidgetSortByDate = 41031;
constexpr UINT kContextZoomPresetFirst = 41150;
constexpr UINT kContextNewMenu = 41400;
constexpr UINT kContextSettingsCommand = 41401;
constexpr UINT kContextGridAdjustmentMenu = 41402;
constexpr UINT kContextGridAdjustmentDone = 41403;

// ── 外壳变更通知 ──────────────────────────────
constexpr UINT kShellChangeMessage = WM_APP + 2;
constexpr UINT_PTR kShellChangeTimerId = 2;
constexpr UINT kShellChangeDebounceMs = 500;

// ── 定时器ID与间隔 ────────────────────────────
constexpr UINT_PTR kRecycleBinPollTimerId = 3;
constexpr UINT kRecycleBinPollIntervalMs = 2000;
constexpr UINT_PTR kDesktopHostWatchTimerId = 4;
constexpr UINT kDesktopHostWatchIntervalMs = 2000;
constexpr UINT_PTR kWidgetRefreshTimerId = 5;
constexpr UINT kWidgetRefreshIntervalMs = 1000;
constexpr UINT_PTR kCollectionPopupDwellTimerId = 6;
constexpr UINT kCollectionPopupDwellIntervalMs = 50;
constexpr DWORD kCollectionPopupDwellDelayMs = 600;

// ── 快捷导航 ──────────────────────────────────
constexpr int kQuickNavigationHotkeyId = 101;

// ── 集合弹出面板布局 ──────────────────────────
constexpr int kCollectionPopupPaddingX = 18;
constexpr int kCollectionPopupHeaderHeight = 54;
constexpr int kCollectionPopupBottomPadding = 18;

// ── 快捷导航单元格尺寸 ────────────────────────
constexpr int kQuickNavigationCellWidth = 72;
constexpr int kQuickNavigationCellHeight = 88;
constexpr int kQuickNavigationTextHeight = 36;
