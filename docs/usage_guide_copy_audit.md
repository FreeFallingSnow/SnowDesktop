# 使用指南业务文案核对

2026-09-17。以 `src/usage_guide.h` 的 31 条指南及当前生产代码为依据，核对操作入口、前提条件、自动处理和数据影响。设置页与桌面指引共用同一组本地化说明。本轮调整 7 条说明，同步 10 种语言；其余条目保留。下表是代码审查结论，不代表逐项桌面实测通过。

| 条目 | 核对依据（相对于 src/） | 结论 |
| --- | --- | --- |
| 开机自启 | auto_start_manager.cpp；winui/general_page_presenter.cpp | 保留，入口和设置一致 |
| 添加和调整组件 | widgets/widget_base.cpp；app/app_pointer_move.cpp | 保留，底部移动、中键移动和右下角缩放均有对应处理 |
| 桌面网格 | winui/page_layout_page_presenter.cpp | 保留，按页设置行列 |
| 桌面图标 | winui/desktop_page_presenter.cpp | 保留，尺寸与间距设置对应 |
| 图标美化 | winui/desktop_page_presenter.cpp | 保留，说明与美化入口对应 |
| 主题与材质 | winui/personalization_page_presenter.cpp | 保留，主题和材质控制对应 |
| 保存布局与备份 | full_data_backup.cpp；winui/backup_data_page_backend.cpp | 调整，明确完整备份是 SnowDesktop 数据，桌面原文件及映射目录文件须另行备份 |
| 集合 | app/app_widget_grouping.cpp | 保留，添加、拖入和整理入口对应 |
| 集合组 | widgets/widget_pair_drop.h；app/app_widget_grouping.cpp | 保留，Ctrl 拖叠成组及空组入口对应 |
| 桌面文件 | app/app_widget_context_menu.cpp；app/app_widget_refresh.cpp | 调整，补充同时只有一个组件可自动收集，开启另一处会关闭原处 |
| 文件夹映射 | app/app_widget_grouping.cpp；app/app_widget_refresh.cpp | 保留，对原文件的影响已有说明 |
| 文件组 | widgets/widget_pair_drop.h；widgets/file_group.cpp | 调整，标签切换组内组件，搜索须先启用搜索框 |
| 文件分类 | winui/desktop_page_presenter.cpp | 保留，分类规则入口对应 |
| 开启 Dock | winui/dock_page_presenter.cpp | 保留，开关及显示位置对应 |
| 应用放到 Dock | app/app_dock_menu.cpp；app/app_dock_drop.cpp | 调整，运行图标须匹配桌面入口才显示固定和映射菜单；保留 Ctrl 拖入建立映射说明 |
| 集合放到 Dock | app/app_dock_drop.cpp | 保留，支持对应集合载荷 |
| 文件夹放到 Dock | app/app_dock_drop.cpp | 保留，文件夹及支持的文件组件有对应入口 |
| Dock 位置和大小 | winui/dock_page_presenter.cpp | 保留，位置和厚度控制对应 |
| Dock 显示方式 | dock_settings_rules.h；app/app_floating_dock_lifecycle.cpp | 保留，仅唤起显示与边缘唤起、内容重叠的联动对应 |
| Dock 快捷键 | winui/dock_page_presenter.cpp；app/app_floating_dock_lifecycle.cpp | 保留，快捷唤起与边缘唤起入口分别处理 |
| Dock 留出空间 | dock_settings_rules.h | 保留，预留空间与允许重叠互斥 |
| Dock 外观与动画 | winui/personalization_page_presenter.cpp；winui/animation_performance_page_presenter.cpp | 保留，主题、动画及节能约束对应 |
| 快捷导航 | app/app_quick_navigation_model.cpp | 保留，应用和文件搜索来源对应 |
| Lua 组件 | app/app_widget_grouping.cpp；app/app_widget_context_menu.cpp | 保留，预览、权限和组件设置入口对应 |
| 创意工坊 | winui/widgets_page_backend.cpp；app/app_background_menu.cpp | 调整，补充下载、安装及启用条件，以及到组件管理检查状态的路径 |
| Agent 开发组件 | winui/widgets_page_presenter.cpp；winui/widgets_page_backend.cpp | 保留，Agent 选择及 Skill 应用入口对应 |
| 多页概览 | page_navigation_rules.h；app/app_page_grid.cpp | 保留，固定前序屏幕、末屏翻页，无文件复制 |
| 新增与管理页面 | app/app_page_grid.cpp；app/app_desktop_layout.cpp；widgets/guide_widget_rules.h | 调整，加入内容后自动移除占位组件；清空额外页后自动清理，不要求手动移除占位组件 |
| 边缘点击翻页 | app/app_pointer_down.cpp；app/app_pointer_context.cpp | 保留，须点击，且方向有可用页；多屏在末屏操作 |
| 拖动边缘翻页 | app/app_drag_target_update.cpp；app/app_pointer_move.cpp | 保留，停留触发换页；取消后的源布局另见下方待核对项 |
| 键盘翻页 | app/app_keyboard_input.cpp；app/app_page_grid.cpp | 调整文案及实现，允许图标拖动时翻页，输入框和弹窗仍优先处理键盘 |

键盘翻页本轮移除拖动拦截，换页后重新计算拖放目标、清除旧预览，保留拖动中的窗口层级。组件移动同步更新拖动坐标基准。键盘换页不提前迁移源图标，提交仍交由松手流程处理。验证需覆盖普通及大图标、多选、连续换页、换页后立即松手和 Esc 取消；单元规则检查不能代替这些桌面操作。

边缘拖动的既有路径会在换页时调用 `MigrateSelectedItemsToLastMonitorPage`，普通图标的源坐标可能已改变；仅从取消会话代码不能确认 Esc 会恢复到拖动开始时的页面。这是代码推导的待核对项，尚未实机复现，本轮未改变该路径。指南只承诺取消拖动，不额外承诺恢复源布局。

验证结果在本轮提交正文中记录；桌面交互最终效果待用户实机验证。
