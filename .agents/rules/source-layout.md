# 源码与文件归属

### 文件归属

- `src/` 根目录只保留 `main.cpp`。新增或移动源码、头文件、资源声明前，按下表确定负责模块；目录导航与持续放置规则统一维护在本说明，不另建 `src/README.md`。
- 文件沿用稳定的功能命名，相关 `.cpp`、`.h`、内部 `.inc`、模块专用 helper 与 `*_rules.h` 放在同一职责模块。

| 目录 | 应放置的实现 |
| --- | --- |
| `src/app/` | `DesktopApp` 组合根及应用协调逻辑，按下文功能子目录归档 |
| `src/core/` | 项目、槽位、容器、桌面、Dock 等核心模型 |
| `src/common/` | 与具体功能无关的通用类型、缓存、JSON、后台工作和本地化；不得作为未分类文件的收容目录 |
| `src/platform/` | 启动、单实例、部署形态、应用恢复、操作反馈及 Windows 平台服务 |
| `src/data/` | 数据路径、布局持久化、原子写入、备份及便携数据迁移 |
| `src/demo/` | 隔离演示的固定数据场景、配置校验、确定时间线及导出任务协调；应用接线归 `app/features/`，通用像素捕获与视频编码归 `ui/preview/`，不读写个人桌面布局 |
| `src/desktop/` | 桌面枚举、显示设置、命名空间及多屏规则 |
| `src/dock/` | Dock 设置、布局、动画、窗口预览及窗口转换 |
| `src/drag_drop/` | 拖放载荷、OLE 规则、文件、图片、文本及 URL 处理 |
| `src/layout/` | 页面、网格、集合、文件夹及弹出布局规则 |
| `src/icons/` | 图标加载、渲染、美化、大图标及网站图标 |
| `src/navigation/` | 快捷导航、查询匹配、拼音及 Everything 搜索 |
| `src/settings/` | 设置模型、控制器、路由、搜索、独立进程及 IPC |
| `src/shell/` | Windows Shell 文件操作、启动、上下文菜单及扩展管理 |
| `src/system/` | 系统快照；设备控制、日历、系统面板、状态栏和托盘分别归入 `controls/`、`calendar/`、`panel/`、`status_bar/`、`tray/` |
| `src/theme/` | 个性化、主题库、主题预览及外观材质 |
| `src/ui/` | 共用原生界面设施，分别归入 `menu/`、`input/`、`render/`、`preview/` |
| `src/widget/` | Lua 组件宿主、运行时及组件开发工具共用实现，按下文职责子目录归档 |
| `src/widgets/` | 原生集合、文件组、文件夹映射及 Lua 容器实现 |
| `src/winui/` | WinUI 设置窗口、XAML、页面 presenter 及相应 UI 桥接 |
| `src/steam/` | Steam 启动器、运行环境、解锁及创意工坊同步 |
| `src/taskbar_hook/`、`src/taskbar_dynamic/` | 独立任务栏 Hook 及 ShellView 投影 |
| `src/wallpaper_hook/` | Wallpaper Engine 捕获 Hook 及独立 32 位构建 |
| `src/resources/` | Windows 资源 ID、图标/字体资源声明及版本资源模板；素材本身仍归 `assets/` |
| `src/diagnostics/` | 日志、崩溃及性能追踪；已有诊断说明按原功能模块保留 |

### 应用协调与模块实现

- `src/app/app.h` 保留 `DesktopApp` 的组合根声明。实现分别归入 `lifecycle/`、`shell/`、`input/`、`layout/`、`render/`、`drag_drop/`、`dock/`、`popup/`、`navigation/`、`widgets/`、`menus/`、`tray/`、`features/`，不得继续在 `app/` 根目录堆放实现文件。
- 直接实现 `DesktopApp`、接入窗口消息与用户输入、协调多个模块或依赖整个应用状态的代码放入 `app/` 对应子目录；功能本身的服务、模型、规则、存储与渲染能力放入外层职责模块。例如 `app/dock/` 负责应用中的 Dock 事件接线，`dock/` 负责 Dock 能力本身。
- 仅供应用协调使用的 helper 随其 `app/` 功能子目录放置；可由宿主、独立工具或测试使用的模块能力归入外层模块。模块拆分按实际构建边界审查依赖。
- `src/widget/` 分为 `api/`、`authoring/`、`runtime/`、`packages/`、`permissions/`、`settings/`、`storage/`、`tasks/`、`data/`、`view/`、`preview/`。`api/` 保存内部注册与契约实现；对组件作者分发的 Skill 与 API 文档仍放在仓库根目录的 `widgets/snowdesktop-lua-widget/`。
- `src/platform/window_support/` 保存管理员窗口支持的私有能力路由、授权任务、辅助进程生命周期及通信；功能实现仍归 `dock/` 等原职责模块，应用接线归 `app/`，设置卡片归 `winui/`。功能回退核对具体能力可用性及目标上下文；文件操作与任意程序的管理员启动不复用这项持久授权。
- `src/platform/control/` 保存对外应用控制 IPC 的协议、传输与长期契约说明；宿主操作接线归 `app/features/`，操作本身复用对应职责模块。场景回放与后续 Skill 复用该协议，不另建绕过宿主规则的修改入口；设置进程及窗口辅助进程的私有 IPC 保留各自边界。
- `widget_author_test.cpp` 等实现产品 `snowwidget test`、lint、预览功能的文件属于 `widget/authoring/`；验证这些功能的回归测试属于 `tests/`，按文件实际职责归档。
- 需要新增职责模块时，先更新本节归属规则并明确边界，再创建目录；不为单个未分类文件随意增加 `misc/`、`helpers/`、`temp/` 等收容目录。

### 引用、测试与移动

- 同目录头文件使用文件名引用；宿主内部跨模块引用使用相对 `src/` 的明确模块路径，例如 `#include "dock/dock_settings.h"`。独立 Hook 及仓库外层调用方按构建边界保留必要的相对路径，构建目标显式声明所需 include 根。
- 存在同名头文件时必须核对目标的实际 include 顺序，并使用能确定目标的路径。尤其要区分宿主 `src/steam/steam_app_identity.h` 与 `steam_bridge/src/steam_app_identity.h`，以及 `src/settings/`、`src/system/panel/` 与 `src/winui/` 中的同名头文件。
- 正式入库的长期测试源码、驱动及专用辅助代码放在 `tests/`，新增文件归入 `tests/<module>/`；程序输出位于 `.build/<Configuration>/tests/`。临时验证按 [根规则](../../AGENTS.md#临时验证与长期测试准入)使用忽略的任务目录。
- 长期测试统一在根目录 `CMakeLists.txt` 注册；整理目录不得在脚本中复制目标清单，也不得改变测试选择与资源隔离契约。
- 移动源码时同步更新 CMake、头文件引用、Windows 资源路径、生成工具、源码契约测试及已有模块说明中的链接。保持 Visual Studio 源码分组与实际目录一致，并核对 Git 默认重命名检测结果。
- 公共组件 API 的语义与版本按 [公共契约规则](../../AGENTS.md#公共契约与本地化) 执行；目录整理不得顺带改变接口契约。构建、提交及验证按 [根规则的任务索引](../../AGENTS.md#按任务读取) 执行。

## 第三方材料

- 项目维护的第三方来源、版本或固定提交、用途、改编和分发说明统一写入根 `THIRD_PARTY_NOTICES.md`，中英文事实及条目格式一致。`third_party/<component>/` 放源码、资源、许可证和构建材料，不另建来源 README 或 `SNOWDESKTOP.md`；上游自带构建、API 文档与许可原文保留。
- `steam_bridge/` 是独立项目，保留自身 `LICENSE` 和中英双语 `THIRD_PARTY_NOTICES.md`；主项目和桥各自维护声明，构建与打包复制对应文件。
