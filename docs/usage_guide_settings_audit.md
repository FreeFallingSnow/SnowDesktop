# 使用指南内容与入口核对

2026-09-17：指南从全量设置索引改为四个功能标签、26 条精选内容。每页把操作教程和关键偏好放进同一列表，使用按钮区分操作。不再逐项复制设置搜索目录。

## 当前目录

| 标签 | 条目 | 动作 / 目标 |
| --- | --- | --- |
| 基础 | 开机自启 | `General` / `general.autoStart` |
| 基础 | 添加和调整组件 | 桌面指引 |
| 基础 | 调整桌面网格 | `DesktopPages` / `pages.grid` |
| 基础 | 调整桌面图标 | `AppearanceDesktopIcons` / `desktop.iconSize` |
| 基础 | 美化应用图标 | `AppearanceIconBeautification` / `desktop.iconBeautify` |
| 基础 | 更换主题与材质 | `AppearanceTheme` / `personalization.theme` |
| 基础 | 保存布局与备份 | `BackupAndData` / `backup.layout` |
| 内置收纳组件 | 用集合收纳应用 | 桌面指引 |
| 内置收纳组件 | 把集合合成一组 | 桌面指引 |
| 内置收纳组件 | 收纳桌面文件 | 桌面指引 |
| 内置收纳组件 | 显示其他文件夹 | 桌面指引 |
| 内置收纳组件 | 把文件组件合成一组 | 桌面指引 |
| 内置收纳组件 | 调整文件分类 | `DesktopCategories` / `desktop.categoryRules` |
| Dock 与快捷导航 | 开启 Dock | `Dock` / `dock.enable` |
| Dock 与快捷导航 | 把应用放到 Dock | 桌面指引；Dock 关闭时先定位 `dock.enable`，不自动开启 |
| Dock 与快捷导航 | 把集合放到 Dock | 桌面指引；Dock 关闭时先定位 `dock.enable`，不自动开启 |
| Dock 与快捷导航 | 把文件夹放到 Dock | 桌面指引；Dock 关闭时先定位 `dock.enable`，不自动开启 |
| Dock 与快捷导航 | 调整 Dock 位置和大小 | `Dock` / `dock.position` |
| Dock 与快捷导航 | 一直显示，还是需要时显示 | `Dock` / `dock.showOnlyWhenSummoned` |
| Dock 与快捷导航 | 在其他应用里唤起 Dock | `Dock` / `dock.floatingShortcutMode` |
| Dock 与快捷导航 | 给 Dock 留出空间 | `Dock` / `dock.allowDesktopContentOverlap` |
| Dock 与快捷导航 | 调整 Dock 外观和动画 | `AppearanceTheme` / `personalization.dockAppearance`；动画按钮 → `AnimationPerformance` / `animation.hover` |
| Dock 与快捷导航 | 使用快捷导航 | `General` / `general.quickNavigation` |
| Lua 组件 | 添加内置 Lua 组件 | 桌面指引 |
| Lua 组件 | 从创意工坊获取组件 | 已有 Steam 工坊打开回调；按实际工坊可用性显示并再次检查 |
| Lua 组件 | 让 Agent 帮你做组件 | 显式打开现有开发工具入口 → `DeveloperTools` / `developer.agentSkill`；不安装 Skill |

## 内容取舍

- 基础前两条是开机自启、添加和调整组件；通用组件操作以集合为例。网格、桌面图标、图标美化、主题、备份保留父入口，颜色参数、边框宽度等不单列。
- 集合和文件整理统一为内置收纳组件。集合组、文件组介绍按住 Ctrl 拖放成组；需要添加时可先预览。文件分类独立为设置条目，自动收集仍在桌面文件教程中说明。
- Dock 固定与 Ctrl 映射合并，解释移动入口和保留桌面图标的区别。文件区包括文件夹、文件夹快捷方式、桌面文件、文件夹映射，保留来源说明。没有改变原拖放逻辑。
- Dock 的位置、形式、大小、显示器合并；常驻/按需、全局唤起、是否预留空间各一条。外观与动画一条、两个设置按钮。快捷导航直接进入设置，不启动桌面说明。
- Lua 内容分为添加内置组件、浏览 Steam 创意工坊、让 Agent 制作组件。开发说明指向现有 Skill 安装入口，由用户选择目标并应用；指南不会替用户安装。
- 普通设置条目沿用搜索目录的可见性约束，工坊另查既有可用性回调。开发帮助保留显式开启入口，不要求开发页已经显示。动作执行检查当前设置会话，Dock 和工坊执行时再次检查条件。

## 呈现与兼容

- 折叠区只有标题；默认展开且仅持久化折叠偏好。没有学习完成、跨教程推进或自动桌面弹出。
- 四个主题使用一层原生标签，横向滚动条隐藏而滚动保留。每页一张有界列表，无操作/设置分组。详细说明按需展开；窄窗口按钮换行。
- 桌面显示相同的完整正文，删除摘要开关、更多菜单和分页。小屏幕或大字体下仅正文滚动，滚轮及可拖动滚动条可到达末尾；返回指南和收起始终在正文之外。
- 标题可拖动，菜单仍位于上层且不驱动避让。返回指南定位原主题及条目。`start.application`、`start.resize`、`start.layout`、`start.dockMapping` 兼容指向合并后的教程。

## 验证边界

调用链：设置宿主 → 指南动作/既有设置路由；指南声明 → 桌面只读说明 → 宿主指针与绘制。持久化仍仅折叠偏好，私有 IPC 载荷和公共组件 API 均未改变。

受影响回归包括设置窗口规则、设置控制器/IPC、WinUI 通用页与组件边界、本地化、资源打包。正文滚动和前置条件的六个隔离负向对照已通过；标准构建与最终全量结果记录于 [实现记录](start_page_onboarding.md)。

实际视觉、键盘、跨屏拖动、长正文滚动和 Steam/开发入口需要用户实机验收。源码核对、构建、自动测试不代表这些场景已经通过。
