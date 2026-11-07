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

- `src/app/app.h` 保留 `DesktopApp` 的组合根声明，负责启动、应用生命周期、模块装配、窗口事件路由和跨模块协调。功能专属模型、运行状态及其操作按下节确定所有者。应用实现分别归入 `lifecycle/`、`shell/`、`input/`、`layout/`、`render/`、`drag_drop/`、`dock/`、`popup/`、`navigation/`、`widgets/`、`menus/`、`tray/`、`features/`，不得继续在 `app/` 根目录堆放实现文件。
- 应用中的窗口消息、用户输入及跨模块接线放入 `app/` 对应子目录；功能本身的服务、模型、规则、存储与渲染能力放入外层职责模块。例如 `app/dock/` 负责 Dock 事件接线，`dock/` 负责 Dock 能力本身。新增功能操作按实际需要传入数据和服务；对整个应用状态的访问须说明协调需要，既有依赖不作为继续扩大组合类的依据。
- 仅供应用协调使用的 helper 随其 `app/` 功能子目录放置；可由宿主、独立工具或测试使用的模块能力归入外层模块。模块拆分按实际构建边界审查依赖。
- `src/widget/` 分为 `api/`、`authoring/`、`runtime/`、`packages/`、`permissions/`、`settings/`、`storage/`、`tasks/`、`data/`、`view/`、`preview/`。`api/` 保存内部注册与契约实现；对组件作者分发的 Skill 与 API 文档仍放在仓库根目录的 `widgets/snowdesktop-lua-widget/`。
- `src/platform/window_support/` 保存管理员窗口支持的私有能力路由、授权任务、辅助进程生命周期及通信；功能实现仍归 `dock/` 等原职责模块，应用接线归 `app/`，设置卡片归 `winui/`。功能回退核对具体能力可用性及目标上下文；文件操作与任意程序的管理员启动不复用这项持久授权。
- `src/platform/control/` 保存对外应用控制 IPC 的协议、传输与长期契约说明；宿主操作接线归 `app/features/`，操作本身复用对应职责模块。场景回放与后续 Skill 复用该协议，不另建绕过宿主规则的修改入口；设置进程及窗口辅助进程的私有 IPC 保留各自边界。
- `widget_author_test.cpp` 等实现产品 `snowwidget test`、lint、预览功能的文件属于 `widget/authoring/`；验证这些功能的回归测试属于 `tests/`，按文件实际职责归档。
- 需要新增职责模块时，先更新本节归属规则并明确边界，再创建目录；不为单个未分类文件随意增加 `misc/`、`helpers/`、`temp/` 等收容目录。

### 类职责、公共头与依赖边界

- 修改公共头、向组合类增加功能状态或操作、增加跨模块依赖，或发现局部改动导致广泛重编译时，实施前检查本次涉及的类、状态和构建依赖。用简短说明确定功能所有者、需要保留的协调接口及实际消费者；优先处理与当前需求相关的边界问题，其他遗留问题记录位置和影响。
- 一组模型、缓存、异步任务、资源和状态转换具有独立用途、状态约束或生命周期时，由对应功能对象集中持有并维护。只需参数和结果的算法归功能模块；只有需要直接维护对象内部约束或承担应用协调的操作才作为组合类成员。移动 `.cpp` 方法体后，继续检查类声明、嵌套模型和成员状态的职责归属。
- 抽取功能时明确状态的创建、修改、取消和销毁者，通过输入、结果、事件及必要服务与其他模块交互。仅搬出字段的 `State`/`Impl` 可以作为实现隐藏的阶段成果；原组合类仍逐字段操作功能细节时，报告为职责拆分未完成。新增跨模块 `friend` 或整体可变状态暴露前，先检查能否用较窄接口满足实际调用。
- 公共头暴露调用方需要的契约。数据、枚举和配置值存在独立消费者及不同依赖需求时，可拆轻量契约头；相关操作与实现按所属功能保留。取得值类型不应连带引入无关的存储、系统调用、UI 框架或具体控制器。按真实用途组织相关声明，避免为每个函数或结构单独建文件。
- 非模板的解析、持久化、系统调用、资源管理和复杂算法默认放入 `.cpp`。模板、`constexpr`、默认比较及少量简单访问器可保留可见定义；复杂内联的性能需要应有依据，并说明其对包含方的影响。运行时共享状态由明确实现持有，保留其原有实例数量和线程语义。
- 头文件自行提供声明及保留实现所需的依赖；调用文件直接包含实际使用的操作入口。具有明确导出契约的聚合头可作为声明来源，其余情况不依赖偶然的传递包含、包含顺序或 PCH。普通实现文件优先包含自身接口头；WinUI、生成代码及工具链规定的 PCH 顺序按实际约束保留，并单独核对接口头自足性。
- 对项目自有类型，指针、引用和允许不完整类型的声明可用前向声明；按值成员、继承、成员访问及要求完整类型的模板包含完整定义。`unique_ptr` 隐藏实现时，核对析构和其他需要完整类型的定义位置。采用 PImpl 前评估分配、间接访问、调试和生命周期成本；第三方和标准库类型使用其支持的声明入口。
- 目录和命名空间边界同步落实到生产目标及显式链接依赖。每份外移实现明确所属生产目标，沿实际引用检查宿主、独立工具、辅助进程和测试消费者；核对宏、CRT 及不同构建变体。低层模块通过明确接口取得上层服务，保持依赖方向清楚、避免循环；不得通过链接整个宿主或无关大库掩盖缺失依赖。生产实现复用按[构建与验证](validation.md#生产实现夹具与编译边界)执行。
- 迁移前后核对默认值、序列化值、所有权、构造与销毁顺序、线程归属、异步取消及资源释放。纯结构重构与行为调整分别说明；必须同时修改时，标明行为变化和对应验证范围。
- 完成走查时核对四项结果：职责与状态由谁持有；调用接口和访问范围如何变化；直接及传递依赖是否符合目标边界；全部实际消费者的源码和目标接线是否闭合。文件行数、包含数量和静态依赖规模用于定位问题与比较趋势，不设机械拆分指标；仅搬入 `.inc`、改名为另一个大类或缩短文件，不能作为职责拆分完成证据。
- 交付分别报告实现隐藏、职责拆分、构建和行为验证的实际完成程度；分阶段工作明确保留项及下一步。包含图和文本对照不能替代编译、链接或功能验收。检查选择与停止构建时的待验记录统一按[构建与验证](validation.md#开发闭合与检查选择)执行，不因本节自动新增永久测试、全量构建或仓库级清理。

设计参考：[Chromium 的头文件实现建议](https://chromium.googlesource.com/chromium/src/+/main/styleguide/c++/c++-dos-and-donts.md#minimize-code-in-headers)、[Google 的直接依赖要求](https://google.github.io/styleguide/cppguide.html#Include_What_You_Use)、[LLVM 的头文件自足与库分层](https://llvm.org/docs/CodingStandards.html#the-high-level-issues)、[C++ Core Guidelines 的相关数据组织与封装](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#c-classes-and-class-hierarchies)，以及 [Qt QMainWindow 声明](https://github.com/qt/qtbase/blob/dev/src/widgets/widgets/qmainwindow.h)与[私有实现](https://github.com/qt/qtbase/blob/dev/src/widgets/widgets/qmainwindow.cpp)。本节采用职责归属、依赖自足和实现隐藏的原则；保留本仓库 PCH、生命周期和构建边界，不引入上游的全部风格或 ABI 要求。

### 引用、测试与移动

- 同目录头文件使用文件名引用；宿主内部跨模块引用使用相对 `src/` 的明确模块路径，例如 `#include "dock/dock_settings.h"`。独立 Hook 及仓库外层调用方按构建边界保留必要的相对路径，构建目标显式声明所需 include 根。
- 存在同名头文件时必须核对目标的实际 include 顺序，并使用能确定目标的路径。尤其要区分宿主 `src/steam/steam_app_identity.h` 与 `steam_bridge/src/steam_app_identity.h`，以及 `src/settings/`、`src/system/panel/` 与 `src/winui/` 中的同名头文件。
- 现有应用测试源码、驱动及专用辅助代码保留在 `tests/`，程序输出位于 `.build/<Configuration>/tests/`。新增验证只允许按 [根规则](../../AGENTS.md#临时验证与长期测试准入)登记临时条目，源码及夹具放在忽略的任务目录。
- 现有应用测试由根目录 `CMakeLists.txt` 包含的 CMake 模块统一注册；生产共用目标归入 `cmake/targets/`，测试登记与夹具构建归入 `cmake/testing/`。整理目录不得在脚本中复制目标清单，也不得改变测试选择与资源隔离契约。
- 移动源码时同步更新 CMake、头文件引用、Windows 资源路径、生成工具、源码契约测试及已有模块说明中的链接。保持 Visual Studio 源码分组与实际目录一致，并核对 Git 默认重命名检测结果。
- 公共组件 API 的语义与版本按 [公共契约规则](../../AGENTS.md#公共契约与本地化) 执行；目录整理不得顺带改变接口契约。构建、提交及验证按 [根规则的任务索引](../../AGENTS.md#按任务读取) 执行。

## 第三方材料

- 项目维护的第三方来源、版本或固定提交、用途、改编和分发说明统一写入根 `THIRD_PARTY_NOTICES.md`，中英文事实及条目格式一致。`third_party/<component>/` 放源码、资源、许可证和构建材料，不另建来源 README 或 `SNOWDESKTOP.md`；上游自带构建、API 文档与许可原文保留。
- `steam_bridge/` 是独立项目，保留自身 `LICENSE` 和中英双语 `THIRD_PARTY_NOTICES.md`；主项目和桥各自维护声明，构建与打包复制对应文件。
