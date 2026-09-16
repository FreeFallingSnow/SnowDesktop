# 使用指南设置入口核对表

2026-09-16：从设置目录的 155 项静态入口出发，归类 124 个普通入口。动态显隐沿用原目录；以下是可见性过滤前的上限，不表示每台设备都会显示全部项目。

核对了各设置 Presenter 的 `FocusTarget` / `RegisterFocusTargets` 及 `SettingsShell` 页面登记。本文是本轮人工目录核对记录，不是一个自动依赖选择器或实机导航验收。

操作型内容独立于本表。相同控件的旧路由别名不重复列项；组合编辑器子项由父入口承载，组件实例字段由各自设置承载。关于、开发和调试页面不归为个性化。

## 通用（7）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 启动 | `General` / `general.autoStart` |
| 高级功能 | `General` / `general.advancedFeatures` |
| 软件桌面 | `Desktop` / `desktop.softwareDesktop` |
| 语言 | `General` / `general.language` |
| 双击隐藏 | `Desktop` / `desktop.doubleClickHide` |
| 桌面点击穿透 | `Desktop` / `desktop.passthrough` |
| 快捷键 | `Desktop` / `desktop.passthrough.hotkey` |

## 页面与网格（7）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 桌面翻页 | `DesktopPages` / `general.pageNavigation` |
| 上一页 | `DesktopPages` / `general.pageNavigation.previous` |
| 下一页 | `DesktopPages` / `general.pageNavigation.next` |
| 桌面页面 | `DesktopPages` / `pages.order` |
| 新增页 | `DesktopPages` / `pages.add` |
| 页面网格 | `DesktopPages` / `pages.grid` |
| 行数 | `DesktopPages` / `pages.rows` |

## 桌面图标（6）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 图标间距 | `AppearanceDesktopIcons` / `desktop.spacing` |
| 图标大小 | `AppearanceDesktopIcons` / `desktop.iconSize` |
| 图标文字 | `AppearanceDesktopIcons` / `desktop.itemFontSize` |
| 列表字号 | `AppearanceDesktopIcons` / `desktop.listFontSize` |
| 标题粗细 | `AppearanceDesktopIcons` / `desktop.fontWeight` |
| 快捷方式箭头 | `AppearanceDesktopIcons` / `desktop.shortcutArrow` |

## 图标美化（22）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 完整图标美化 | `AppearanceIconBeautification` / `desktop.iconBeautify` |
| 美化模式 | `AppearanceIconBeautification` / `desktop.iconBeautify.mode` |
| 默认底色 | `AppearanceIconBeautification` / `desktop.iconBeautify.backgroundColor` |
| 底色不透明度 | `AppearanceIconBeautification` / `desktop.iconBeautify.backgroundOpacity` |
| 启用渐变底色 | `AppearanceIconBeautification` / `desktop.iconBeautify.gradient` |
| 渐变结束色 | `AppearanceIconBeautification` / `desktop.iconBeautify.gradientEndColor` |
| 渐变方向 | `AppearanceIconBeautification` / `desktop.iconBeautify.gradientDirection` |
| 形状 | `AppearanceIconBeautification` / `desktop.iconBeautify.shape` |
| 内容缩放 | `AppearanceIconBeautification` / `desktop.iconBeautify.contentScale` |
| 高光强度 | `AppearanceIconBeautification` / `desktop.iconBeautify.highlightStrength` |
| 高光范围 | `AppearanceIconBeautification` / `desktop.iconBeautify.highlightSize` |
| 高光角度 | `AppearanceIconBeautification` / `desktop.iconBeautify.highlightAngle` |
| 暗部强度 | `AppearanceIconBeautification` / `desktop.iconBeautify.shadeStrength` |
| 边缘高光 | `AppearanceIconBeautification` / `desktop.iconBeautify.edgeHighlight` |
| 统一颜色滤镜 | `AppearanceIconBeautification` / `desktop.iconBeautify.filter` |
| 统一颜色 | `AppearanceIconBeautification` / `desktop.iconBeautify.filterColor` |
| 统一强度 | `AppearanceIconBeautification` / `desktop.iconBeautify.filterStrength` |
| 阴影强度 | `AppearanceIconBeautification` / `desktop.iconBeautify.shadowStrength` |
| 描边 | `AppearanceIconBeautification` / `desktop.iconBeautify.outline` |
| 描边宽度 | `AppearanceIconBeautification` / `desktop.iconBeautify.outlineWidth` |
| 描边不透明度 | `AppearanceIconBeautification` / `desktop.iconBeautify.outlineOpacity` |
| 描边颜色 | `AppearanceIconBeautification` / `desktop.iconBeautify.outlineColor` |

## 文件分类（4）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 标签显示文件数量 | `DesktopCategories` / `desktop.categoryCounts` |
| 分类规则 | `DesktopCategories` / `desktop.categoryRules` |
| 分类 | `DesktopCategories` / `desktop.categories` |
| 新增分类类型 | `DesktopCategories` / `desktop.category.add` |

## 组件（13）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| Lua 组件行高 | `AppearanceWidgets` / `personalization.luaWidgetRowHeight` |
| 组件与 Dock 弹窗主题 | `AppearanceTheme` / `personalization.collectionPopupTheme` |
| 组件与布局 | `AppearanceWidgets` / `personalization.cornerRadius` |
| 底栏高度 | `AppearanceWidgets` / `personalization.barHeight` |
| 标签与搜索框高度 | `AppearanceWidgets` / `desktop.categoryLayout` |
| 已安装组件 | `Widgets` / `widgets.installed` |
| 搜索 | `Widgets` / `widgets.search` |
| 从文件安装… | `Widgets` / `widgets.install` |
| 打开创意工坊 | `Widgets` / `widgets.workshop` |
| 自己开发 | `Widgets` / `widgets.developer` |
| SnowDesktop 自带 | `Widgets` / `widgets.included` |
| 来源与创意工坊 | `Widgets` / `widgets.sources` |
| 权限 | `Widgets` / `widgets.permissions` |

## Dock（19）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 悬停形式 | `AnimationPerformance` / `animation.hover` |
| 悬停放大比例（%） | `AnimationPerformance` / `animation.hoverScale` |
| 应用启动动画 | `AnimationPerformance` / `animation.launch` |
| 窗口最小化与还原 | `AnimationPerformance` / `animation.window` |
| 浮动 Dock 快捷方式 | `Dock` / `dock.floatingShortcutMode` |
| 快捷键 | `Dock` / `dock.floatingShortcutMode.hotkey` |
| Dock 外观 | `AppearanceTheme` / `personalization.dockAppearance` |
| 启用 Dock | `Dock` / `dock.enable` |
| Dock 位置 | `Dock` / `dock.position` |
| Dock 布局 | `Dock` / `dock.layout` |
| 显示范围 | `Dock` / `dock.monitor` |
| Dock 厚度 | `Dock` / `dock.thickness` |
| 允许 Dock 遮挡桌面内容 | `Dock` / `dock.allowDesktopContentOverlap` |
| 仅在唤起时显示 Dock | `Dock` / `dock.showOnlyWhenSummoned` |
| 常用项目 | `Dock` / `dock.showFrequentItems` |
| 屏幕边缘轻扫唤起悬浮 Dock | `Dock` / `dock.floatingEdgeSwipe` |
| 全屏应用时禁用手势唤起 | `Dock` / `dock.floatingEdgeSwipeBlockFullscreen` |
| 显示 Windows 按钮 | `Dock` / `dock.showWindowsButton` |
| 显示数量 | `Dock` / `dock.frequentItemCount` |

## 快捷导航（3）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 快速导航 | `General` / `general.quickNavigation` |
| 快捷键 | `General` / `general.quickNavigation.hotkey` |
| 快捷导航主题 | `AppearanceTheme` / `personalization.quickNavigationTheme` |

## 主题（16）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 主题与材质 | `AppearanceTheme` / `personalization.theme` |
| 颜色与渐变 | `AppearanceTheme` / `personalization.backgroundColor` |
| 边框颜色 | `AppearanceTheme` / `personalization.borderColor` |
| 背景不透明度 | `AppearanceTheme` / `personalization.widgetAlpha` |
| 边框不透明度 | `AppearanceTheme` / `personalization.borderAlpha` |
| 边框宽度 | `AppearanceTheme` / `personalization.borderWidth` |
| 边缘高光 | `AppearanceTheme` / `personalization.edgeHighlight` |
| 边缘高光宽度 | `AppearanceTheme` / `personalization.edgeHighlightWidth` |
| 边缘高光强度 | `AppearanceTheme` / `personalization.edgeHighlightStrength` |
| 启用底部渐变 | `AppearanceTheme` / `personalization.enableGradient` |
| 渐变末端不透明度 | `AppearanceTheme` / `personalization.gradientEndAlpha` |
| 玻璃效果 | `AppearanceTheme` / `personalization.glass` |
| 亚克力效果 | `AppearanceTheme` / `personalization.acrylic` |
| 模糊半径 | `AppearanceTheme` / `personalization.blurRadius` |
| 文字颜色 | `AppearanceTheme` / `personalization.contentTheme` |
| 右键菜单外观 | `AppearanceTheme` / `personalization.contextMenu` |

## 动画与性能（6）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 动画模式 | `AnimationPerformance` / `animation.mode` |
| 弹窗动画 | `AnimationPerformance` / `animation.popup` |
| 动画速度 | `AnimationPerformance` / `animation.speed` |
| 动画更新上限 | `AnimationPerformance` / `animation.frameLimit` |
| 节能时降低动画频率 | `AnimationPerformance` / `animation.energySaver` |
| 电池供电时降低动画频率 | `AnimationPerformance` / `animation.onBattery` |

## 任务栏（16）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 自动隐藏任务栏 | `Taskbar` / `taskbar.autoHide` |
| 任务栏对齐 | `Taskbar` / `taskbar.alignment` |
| 任务栏外观 | `Taskbar` / `taskbar.theme` |
| 任务栏图标和文字颜色 | `Taskbar` / `taskbar.contentTheme` |
| 背景颜色 | `Taskbar` / `taskbar.backgroundColor` |
| 边框颜色 | `Taskbar` / `taskbar.borderColor` |
| 背景不透明度 | `Taskbar` / `taskbar.backgroundOpacity` |
| 边框不透明度 | `Taskbar` / `taskbar.borderOpacity` |
| 毛玻璃背景 | `Taskbar` / `taskbar.glass` |
| 模糊半径 | `Taskbar` / `taskbar.blurRadius` |
| 亚克力噪点 | `Taskbar` / `taskbar.acrylic` |
| 开始菜单、搜索、任务视图或系统面板打开时 | `Taskbar` / `taskbar.dynamic.shellUi` |
| 本显示器有最大化应用窗口时 | `Taskbar` / `taskbar.dynamic.maximizedWindow` |
| 本显示器有应用窗口时 | `Taskbar` / `taskbar.dynamic.visibleWindow` |
| 系统面板样式 | `Taskbar` / `taskbar.systemTheme` |
| 重启文件资源管理器 | `Taskbar` / `taskbar.restartExplorer` |

## 备份与数据（5）

| 项目 | 目标页面 / 控件 |
| --- | --- |
| 布局备份 | `BackupAndData` / `backup.layout` |
| 完整备份与迁移 | `BackupAndData` / `backup.full` |
| 数据目录 | `BackupAndData` / `backup.directory` |
| 迁入完整数据… | `BackupAndData` / `backup.migration` |
| 清除布局 | `BackupAndData` / `backup.clearData` |

## 补齐与边界

- Dock 独立外观：加入目录并登记现有主题控件定位。
- Lua 组件行高：加入目录并登记现有组件布局控件定位。
- 页面行数：加入目录并复用现有 `pages.rows` 定位。
- 高级功能解锁卡片继续遵守原设置目录显隐规则；索引不绕过桥与解锁判断。
- 自定义外观的颜色、透明度、边框和材质子编辑器归入父外观入口；不新增公共组件接口。
- 屏幕/窗口输入及实际控件聚焦、滚动定位需按仓库规则由用户实机确认。
