# SnowDesktop Lua 组件引擎升级计划

- 状态：设计基线
- 制定日期：2026-08-14
- 适用范围：SnowDesktop Lua 组件宿主、组件包、组件管理、预览、作者工具、全部内置组件迁移及 WebView 类组件可行性评估
- 当前实现基线：SnowDesktop 1.0.4.0、11 个内置组件的 schema/API v2 代码迁移已完成；
  真实桌面验收、声明式视图完整面和 v1 执行入口移除仍未完成

## 1. 结论与执行摘要

Lua 组件引擎采用分阶段实现、单版本切换，不把预发行阶段的 API v1 固化为长期兼容层：

- 正式切换后的宿主只执行 schema v2/API v2；API v1 只作为迁移工具能够识别的输入格式，不进入发布运行时。
- 11 个现有内置组件必须全部迁移到 schema v2/API v2，迁移完成和 v1 执行入口移除是 v2 稳定版的发布硬门槛。
- API v2 使用独立的组件描述、生命周期和能力协商，不继续扩大全局函数与零散回调集合。
- 组件安装与组件执行分离。安装只把包放入组件库；首次激活前必须完成高风险权限授权。
- Lua API v2 保留 Direct2D 原生渲染和 Lua 沙箱，不把 DOM、JavaScript 或 WebView 混入 Lua VM。
- WebView 类组件进入独立 WebView2 运行时评估轨道；只评估离线打包内容，不承诺纳入 v2，也不允许任意远程网页包装器。
- 保留 `render()` / `draw.*` 即时绘制，同时增加可选的声明式 `view()`；二者不能在同一表面混用。
- 将系统快照、HTTP、媒体、日历和桌面访问收敛到统一的数据订阅与任务模型。
- 将计时器、刷新间隔和可预测时间点收敛到统一调度器，由宿主处理可见性、休眠恢复和任务合并。
- 普通 `.snowwidget` 不允许加载 DLL、执行任意命令、访问任意文件路径或调用通用 WMI。
- API 元数据成为代码、文档、Lua 类型声明和契约测试的共同来源。

最终目标不是复制 Apple、Android、前端框架或 Rainmeter，而是组合其有效部分：

- WidgetKit 的快照、时间线、配置和动作边界；
- Android App Widgets / Glance 的被动状态、响应式尺寸和宿主更新策略；
- 前端框架的状态驱动视图、稳定节点和副作用清理；
- Rainmeter 的数据采集与显示解耦、独立更新频率和可观察诊断。

## 2. 当前基线

### 2.1 已有能力

当前引擎已经具备继续演进所需的关键基础：

- 每个实例独立 `lua_State`。
- 默认 16 MiB Lua 内存上限、50 ms 回调时间上限、500000 条指令预算。
- 只读宿主 API 表和受限 Lua 标准库。
- 包级不可变 UUID、SemVer、`apiVersion`、`dataVersion` 和原子安装/回滚。
- Direct2D 绘制、图片和 Windows Shell 图标。
- 鼠标、滚轮、文本输入、面板和右键菜单。
- 实例存储与 `migrateStorage()` 数据迁移。
- 异步 HTTP、请求取消、大小限制、缓存和最多三次重定向。
- 系统、媒体、桌面、Everything 和日历数据。
- 命名计时器、可见/隐藏、尺寸、语言及数据变化回调。
- 真实隔离的组件预览实例和稳定预览数据。
- 包校验、导出、来源抽象、last-known-good 回滚和 Steam Workshop 接入。
- 构建、完整测试、本地化契约和若干安全契约测试。

### 2.2 主要结构性问题

1. `WidgetEngine` 同时承担 VM 生命周期、API 注册、绘制、输入、数据桥接、计时器、HTTP 回调、存储、设置和诊断，`src/widget_engine.cpp` 已成为扩展瓶颈。
2. `apiVersion` 当前固定为 1；由于软件尚未正式发行，若现在建立长期 v1/v2 双栈，只会增加测试和安全负担并固化尚未发布的旧语义。
3. API 注册由 C++ 手工完成，文档、权限说明、预览行为和测试矩阵没有共同契约源。
4. `storage` 只有字符串并且每次修改立即落盘，临时 UI 状态和持久用户数据容易混用。
5. 异步能力使用独立回调，如 `onHttpResponse`、`onTimer`、`onCalendarChanged`，继续增加数据源会放大回调面。
6. 绘制以绝对坐标和逐帧命令为主，复用布局、键盘焦点、屏幕阅读器和差量更新困难。
7. `manifest.refreshIntervalMs`、命名计时器、数据变化通知和手工 `invalidate()` 缺少统一调度策略。
8. 初次安装时，清单权限直接写入 `grantedPermissions`；权限显示不等于用户授权。
9. `network.http` 当前允许任意 HTTP/HTTPS、localhost 和私有网络，`networkDomains` 不作为运行时白名单。
10. 注册表中空授权集合会回退为清单全部权限，无法可靠表达“用户明确拒绝全部权限”。
11. 包格式列出了 `modules/`，但运行时没有正式、安全、可诊断的包内模块加载协议。
12. 缺少公开的 Lua 类型声明、API 能力查询、组件级性能视图和可自动运行的组件测试规范。
13. 当前系统/媒体快照服务在第一次读取后启动单一后台线程，之后约每秒同时轮询系统和媒体直到引擎销毁；没有按数据源、可见订阅或最后订阅释放生命周期。

## 3. 升级目标与非目标

### 3.1 升级目标

- 正式宿主只执行 API v2；迁移工具可以读取 API v1 清单并生成迁移报告，但不能运行 v1 代码。
- 任何高风险权限都不能在用户授权前进入运行时权限集合。
- 预览、首次激活、正常运行、后台隐藏、暂停、故障和卸载具有明确状态机。
- 数据获取、状态变化、视图更新和外部动作彼此解耦。
- 新 API 默认异步、可取消、有限额并且不会阻塞 UI/渲染线程。
- 新组件可以使用响应式布局、宿主控件、键盘导航和无障碍语义。
- v2 组件仍可选择即时绘制；迁移不强制把适合自绘的组件改成声明式视图。
- 全部内置组件迁移到 API v2，并保留现有实例数据、视觉、交互、本地化和权限拒绝降级行为。
- 新权限、新 API 和新清单字段都能被作者工具、组件管理页和测试识别。
- 引擎模块能够被独立测试，减少对完整桌面应用集成测试的依赖。

### 3.2 明确非目标

- 不将 Lua API v2 变成 HTML/CSS/JavaScript 页面，也不在一个组件实例中混合 Lua 和 WebView 生命周期。
- 不在完成 WebView2 独立运行时的合成、安全、资源和分发验证前承诺支持 WebView 类组件。
- 即使 WebView2 轨道通过评估，也不支持把任意远程网站直接包装成桌面组件。
- 不在 API v2 首版支持跨包代码依赖。
- 不允许组件下载后执行 Lua、DLL、EXE、PowerShell 或批处理文件。
- 不向组件开放通用 COM、注册表、WMI、Win32 句柄或任意 Shell 命令。
- 不承诺后台任务在精确时间执行；调度时间表达最早唤醒时间。
- 不把用户私密数据自动同步到不同组件实例或不同组件包。
- 不使用“所有能力一个 network/filesystem 权限”的粗粒度安全模型。
- 不在一个发布分支内完成整个 v2；各阶段按当时有效的版本发布分支交付。

## 4. 设计原则

1. **单一发布契约**：预发行阶段直接收敛到 API v2，不为尚不存在的生态长期维护 v1 运行时。
2. **执行前授权**：权限检查发生在 Lua VM 创建和入口脚本执行之前。
3. **最小权限**：读取、修改、互联网、本机网络和用户选择的文件范围分别授权。
4. **宿主拥有生命周期**：任务、订阅、控件、焦点和资源由实例作用域自动回收。
5. **数据与视图分离**：数据提供者不直接绘制，视图不执行同步 I/O。
6. **显式副作用**：网络、通知、打开文件、媒体控制等操作只能从任务或动作入口发起。
7. **无环境凭据**：HTTP 不继承浏览器 Cookie、系统代理凭据或 Windows 集成认证。
8. **预览无副作用**：预览可读稳定样例数据，但禁止网络、本机访问、通知和写操作。
9. **可观察性内建**：每个刷新、订阅、权限拒绝、任务失败和限额触发都有诊断来源。
10. **单一契约源**：API 定义驱动注册、文档、类型、权限标签和测试清单。
11. **运行时隔离**：Lua 与潜在 WebView2 运行时共享包管理、权限代理和宿主生命周期，但不共享脚本桥、网络旁路或实例状态机实现。

## 5. 目标架构

目标依赖方向如下：

```text
WidgetPackageManager
    -> WidgetPermissionBroker
    -> WidgetRuntimeManager
         -> LuaVm / ApiV2Registry
         -> WidgetStateStore
         -> WidgetTaskService
         -> WidgetScheduler
         -> WidgetDataBroker -> Built-in Data Providers
         -> WidgetScene -> D2D Renderer / Input / Accessibility
         -> WidgetDiagnostics

Experimental Web Runtime (separate track)
    -> WebView2 Composition Controller
    -> Restricted JSON Message Bridge
    -> same Permission Broker / Task Service / Diagnostics

Component Preview
    -> Restricted Permission Context
    -> Mock Data Providers
    -> WidgetRuntimeManager

API Contract Registry
    -> C++ API registration
    -> LuaLS definitions
    -> documentation tables
    -> contract tests
    -> permission UI labels
```

WebView2 轨道是独立运行时适配器，不是 `LuaVm` 的附加 API。Lua v2 稳定版不依赖 WebView2 评估结论；若评估不通过，删除实验适配器不会改变 Lua 包格式、API 契约或内置组件迁移结果。

### 5.1 代码拆分目标

逐步从 `src/widget_engine.cpp` 提取以下模块；每次提取必须保持外部行为不变并单独验证：

```text
src/widget_runtime/
  api_contract.h/.cpp
  api_v2_registry.h/.cpp
  runtime_instance.h/.cpp
  runtime_manager.h/.cpp
  permission_broker.h/.cpp
  state_store.h/.cpp
  task_service.h/.cpp
  scheduler.h/.cpp
  data_broker.h/.cpp
  diagnostics.h/.cpp
  preview_context.h/.cpp
  scene/
    node.h
    builder.cpp
    diff.cpp
    layout.cpp
    input.cpp
    accessibility.cpp
    renderer.cpp
  api/
    widget_api.cpp
    draw_api.cpp
    view_api.cpp
    data_api.cpp
    schedule_api.cpp
    storage_api.cpp
    network_api.cpp
    desktop_api.cpp
    media_api.cpp
    calendar_api.cpp
```

现有 `WidgetEngine` 在迁移期间作为门面，避免一次性修改 `DesktopApp` 和设置窗口的全部调用点。

## 6. 版本模型与切换策略

### 6.1 三种版本的职责

- `schemaVersion`：描述 `widget.json` 的语法和字段语义。
- `apiVersion`：描述 Lua 运行时主契约。
- `dataVersion`：由组件作者控制实例数据迁移。

三者不得互相替代，也不得因应用发布版本变化而自动增加。

### 6.2 宿主支持范围

宿主公开：

```lua
local info = widget.apiInfo()
-- info.current = 2
-- info.supported = { 2 }
-- info.features = { "view.tree.core", "data.subscribe", ... }

if widget.hasFeature("draw.path") then
    -- optional enhancement
end
```

规则：

- 正式宿主只激活 schema v2 / API v2 包，入口必须返回组件描述对象。
- schema v1 / API v1 包不得进入 Lua VM；组件管理页显示“需要迁移”，并可调用作者工具生成迁移报告。
- 迁移工具只解析清单和静态扫描已知 API，不执行入口脚本，也不自动授予任何权限。
- 研发阶段允许在未发布构建中暂时保留当前 v1 引擎以维持迁移施工，但不把它重构成公共 `ApiV1Adapter`；M7 完成时删除 v1 API 注册和执行分支。
- 只有破坏性语义变化才增加 `apiVersion`；纯新增能力通过 feature 标识和 `minHostVersion` 表达。
- 未满足 `requiredFeatures` 时拒绝激活；未满足 `optionalFeatures` 时允许降级。
- 组件管理页必须显示包 schema、API、最低宿主和能力缺失原因。

### 6.3 API v2 入口

建议形式：

```lua
return widget.define({
    setup = function(ctx)
        return {
            cpu = data.subscribe("system.cpu", {
                maxAgeMs = 1000,
                whenHidden = "throttle"
            })
        }
    end,

    view = function(ctx, model)
        local cpu = model.cpu:value()
        return view.column({
            key = "root",
            gap = 8,
            children = {
                view.text({ key = "title", text = "CPU", role = "heading" }),
                view.progress({
                    key = "usage",
                    value = cpu and cpu.usagePercent / 100 or 0,
                    accessibilityLabel = "CPU usage"
                })
            }
        })
    end,

    event = function(ctx, model, event)
        if event.kind == "action" and event.id == "refresh" then
            model.cpu:refresh()
        end
    end,

    dispose = function(ctx, model, reason)
        -- subscriptions and tasks are automatically cancelled;
        -- this hook is only for component-owned state cleanup.
    end
})
```

约束：

- `setup`、`view`、`event`、`dispose` 在实例环境中运行。
- `setup` 最多执行一次；热重载会创建新实例并在成功后原子替换旧实例。
- `view` 必须无同步 I/O 和持久化写入。
- `view` 返回视图树，或 v2 组件改用兼容 `render` 即时绘制；一个表面只能选择一种模式。
- 所有宿主资源绑定到实例作用域，卸载时自动取消。

当前过渡实现已经接通组件的 `setup(context) -> model -> render/view(context,
model) -> dispose(context, model, reason)` 路径，并将其作为 `lifecycle.model`
feature 发布。setup 失败不会替换热重载前的可用 VM，dispose 在卸载、热重载和
宿主关闭时至多执行一次。surface 级 `event` 已接通可见性、尺寸、计时器、
动作、选择、环境与面板事件，并作为 `lifecycle.event` feature 发布；原始指针生命周期
只向即时绘制 surface 投递，声明式元素由节点 action 通道提供精确指针事件。`menu` 已接通即时 region 和
`view.tree.core` 元素；声明式核心树已经开放，组件声明语义后由宿主生成 UIA 树，
不向 Lua 暴露原生 Provider 或任意键盘旁路。

## 7. 生命周期状态机

### 7.1 状态

```text
Installed
  -> PendingConsent
  -> Ready
  -> Loading
  -> ActiveVisible <-> ActiveHidden
  -> Suspended
  -> Faulted
  -> Unloaded

PreviewLoading -> PreviewActive -> Unloaded
```

### 7.2 关键规则

- `PendingConsent` 状态不能创建 Lua VM，也不能执行入口脚本。
- 首次添加到桌面应先授权，授权成功后再创建布局实例。
- 启动恢复布局时不得主动弹出模态授权窗口；使用宿主绘制的“需要授权”占位卡。
- 从隐藏变为可见时恢复被节流的数据订阅和调度，不补跑全部错过的周期任务。
- 系统休眠恢复后，重复任务只触发一次合并事件，并重新计算下一截止时间。
- 连续错误熔断继续保留，但故障状态不得破坏 last-known-good 包或实例存储。
- 热重载只有在新 VM 完整加载、迁移和首帧构建成功后才替换旧 VM。
- 权限撤销立即取消相关请求、任务和订阅，然后重新加载实例。

## 8. 权限、授权与信任模型

### 8.1 包状态与授权状态分离

组件包记录以下集合：

- `requestedRequiredPermissions`
- `requestedOptionalPermissions`
- `grantedPermissions`
- `deniedPermissions`
- `pendingPermissions`
- `grantMetadata`：用户、策略或迁移来源，授权时间和能力范围摘要。

空集合必须表示真实的空集合，禁止继续用“空集合回退到清单权限”的方式兼容。

### 8.2 首次授权流程

触发点是用户点击“添加到桌面”或主动启用组件之后、`EnsureWidgetLoaded()` 之前：

```text
读取并验证包
  -> 计算请求权限和已授权权限差异
  -> 无待处理高风险权限：直接激活
  -> 有待处理权限：显示完整授权页
       -> 必要权限拒绝：不添加
       -> 可选权限拒绝：降级添加
       -> 授权：原子保存授权并激活
```

授权以 `packageId + publisher identity + source identity + permission scope hash` 为绑定单位。同一包的多个实例共享授权，但各实例存储仍隔离。

只有经过可验证签名或受信任发布渠道证明的作者才具有 authenticated publisher identity。普通本地导入包显示为“未验证作者”，其授权绑定到包内容和来源，不能仅信任清单中可自行填写的 `author` 字段。

### 8.3 风险分级

| 等级 | 示例 | 默认行为 |
|---|---|---|
| 基础环境 | 包内资源、实例存储、本地化、时间/时区、当前 surface 的主题/DPI/无障碍环境、普通宿主控件 | 不弹窗，组件页展示；不得包含用户/设备唯一标识 |
| 系统状态读取 | `system.performance.read`、`system.power.read`、`system.storage.read`、`system.network.read`、`system.display.read`、`audio.output.read` | 首次激活可在一个“系统状态”页面分组说明，但按子权限分别保存和撤销，不再使用含混的 `system.read` |
| 内容/活动读取 | `media.read`、`desktop.read`、`app.discovery`、`calendar.read` | 首次激活分组授权，并展示读取的数据类别 |
| 外部通信/提示 | `network.internet`、`notification.post` | 首次激活明确授权；通知另有频率和免打扰约束 |
| 高风险读取/分析 | `network.local`、`everything.search`、`clipboard.read`、`process.summary.read`、`audio.output.analyze` | 单独列出，默认不勾选；剪贴板读取要求当前用户手势，音频分析运行时持续可见 |
| 修改/控制 | `desktop.action`、`app.launch`、`shell.launch`、`calendar.write`、`media.action`、`audio.output.control`、`clipboard.write` | 单独授权，动作受用户手势、目标类型和速率约束 |
| 用户授予范围 | `filesystem.userSelected.read`、`filesystem.userSelected.write`、`filesystem.userSelected.watch` | 清单先声明用途，实际通过系统选择器授予具体不透明句柄；不授予任意路径 |
| 实时传感器/输入 | 未来的 `audio.microphone.capture`、摄像头、位置 | 独立系统隐私授权和宿主授权，不由其他权限推导 |

`ui.input` 只在组件获得焦点时接收输入，不授予全局键盘监听能力，因此可以作为基础能力，但必须在组件说明中展示。

### 8.4 清单 v2 示例

```json
{
  "schemaVersion": 2,
  "id": "bea2cf61-ce15-4dd7-aec0-af3c29a16440",
  "version": "2.0.0",
  "apiVersion": 2,
  "dataVersion": 1,
  "entry": "main.lua",
  "requiredPermissions": [
    "network.internet"
  ],
  "optionalPermissions": [
    "ui.notify",
    "network.local"
  ],
  "permissionReasons": {
    "network.internet": "widget.weather.permission.internet",
    "network.local": "widget.weather.permission.local"
  },
  "network": {
    "origins": [
      "https://api.example.com:443"
    ],
    "localOrigins": [
      "http://127.0.0.1:8123"
    ]
  }
}
```

`permissionReasons` 引用组件自己的本地化键。授权窗口应明确标注该文本是“开发者说明”，宿主自己的风险描述不可被覆盖。

### 8.5 更新、撤销和恢复

- 更新新增权限或扩大 origin 范围时，旧版本继续运行，新版本保持 staged 状态。
- 授权窗口一次展示完整差异，不使用当前“逐个权限失败后再确认”的探测方式。
- 用户撤销权限后，所有实例立即丢失该能力并重新加载。
- 已有布局中的待授权实例显示静态占位卡，不在启动时连续弹窗。
- 组件管理页提供查看、撤销、重新授权和来源验证入口。
- 回滚到旧版本时，权限集合取旧版请求集合与当前有效授权的交集，不自动恢复已撤销权限。

### 8.6 预发行权限切换

- SnowDesktop 内置签名只证明包来源和完整性，不代替用户同意；内置组件首次请求隐私读取、外部通信、高风险读取或修改/控制能力时，同样进入授权流程。
- 仅含基础能力的内置组件可以无弹窗激活，但组件页仍展示能力摘要。
- 升级后已有布局中的内置高风险组件进入 `PendingConsent` 并显示宿主占位卡；用户第一次主动恢复它时完成授权，启动阶段不批量弹窗。
- 预发行 API v1 包的隐式授权不迁入 v2；重新打包为 v2 后按新清单首次授权，不能把旧注册表状态伪装成用户同意。
- 已有布局若引用尚未迁移的 v1 包，显示“组件需要升级”占位卡，不执行其 Lua；完成迁移且包身份匹配后再恢复实例。
- 切换前原子备份包注册表和实例存储；失败时恢复数据，但不通过重新启用不安全的 v1 运行时来掩盖失败。

## 9. 网络安全模型

### 9.1 API v2 权限拆分

- `network.internet`：只访问清单声明的公网 HTTPS origin。
- `network.local`：访问清单声明的 localhost、私有地址或本地 DNS origin。
- API v2 不再提供语义含混的广域 `network.http`。

origin 包含 scheme、规范化主机和端口。禁止通配符、用户信息、非规范 IDN 和隐式子域继承。

### 9.2 请求检查

- 初始 URL 和每次重定向都重新校验 origin 和权限。
- 公网权限下，在 DNS 解析前后检查目标，并在连接建立后检查实际远端地址。
- 公网请求禁止回环、RFC1918、链路本地、ULA、`.local` 和其他本地目标。
- 公网到内网的重定向必须同时拥有 `network.local` 且目标位于 `localOrigins`。
- 禁用环境 Cookie、自动认证和系统凭据。
- 保留请求体、响应体、并发、超时、重定向和缓存上限。
- HTTP 缓存键必须包含方法、规范化 URL、相关请求头和授权范围，不能跨组件泄露。
- 诊断页显示目标 origin、状态、缓存命中、耗时和拒绝原因，但不显示敏感头和秘密。

### 9.3 API v1 网络迁移

- 正式宿主不执行声明 `network.http` 的 v1 包，也不保留任意 HTTP/HTTPS、localhost 和私网访问的兼容旁路。
- v2 包禁止声明旧 `network.http`；作者必须选择 `network.internet`、`network.local` 和精确 origin。
- 作者工具静态扫描 v1 `http.request` 调用并生成待确认目标列表；不能静态确定的动态 URL 必须由作者明确填写，工具不得自动生成通配授权。

## 10. 状态与存储

### 10.1 三类状态

1. `state`：实例内存状态，VM 卸载后丢失，修改自动请求视图更新。
2. `storage`：实例持久化状态，类型化、事务化、受容量限制。
3. `secrets`：使用 Windows 数据保护能力保存的敏感值，禁止进入预览、日志、导出和普通备份明文。

建议 API：

```lua
state.set("expanded", true)
local expanded = state.get("expanded", false)

storage.transaction(function(tx)
    tx:set("city", "Shanghai")
    tx:set("units", "metric")
end)

local settings = storage.getJson("settings", { units = "metric" })
storage.setJson("settings", settings)
```

### 10.2 规则

当前过渡实现（2026-08-15）已开放 `state.get/set/remove/has/keys/clear` 和
`state.transient` feature。状态按实例 Lua VM 隔离，set/get 都进行受配额的
JSON-like 深拷贝；循环、metatable、混合数组/对象、非有限数和超限值会被拒绝，
同值写入不会重复触发失效，多个真实变化折叠为 dirty 信号。持久存储已开放
`storage.transaction`：回调使用隔离快照读写，回调错误、最终配额失败或写盘失败时
整批回滚，最终快照一次原子替换；`storage.typed` 让直接写入和事务保存受限 JSON-like
值，同时以宿主保留元数据区分既有原始字符串；v2 的直接写入和事务均禁止从 `render`
调用。`storage.writeBudget` 对每个真实实例提供 32 次突发提交，之后每秒恢复一次；事务
只按最终真实变化计一次，未改变、预览和迁移覆盖层不计。`settings.secretReference` 已将
声明式 `password` 接入每用户 DPAPI 私有存储：Lua 的 storage API 只观察实例作用域 opaque
reference；`task.network.secretReference` 在发送前解析 header/body descriptor，任务队列、
预览、日志、完成事件和数据备份均不保存秘密正文。

- `state.set` 在同一事件周期内批量合并，只触发一次视图更新。
- `storage` 支持 string、number、boolean、null、array 和 object，序列化语义固定。
- 持久化事务使用原子替换；失败时不暴露部分写入。
- `view()` / `render()` 中禁止持久化写入，违规记录诊断并在开发模式报错。
- 每实例设置总容量、单值容量和写入频率上限。
- `migrateStorage(oldVersion, newVersion, snapshot)` 继续运行在隔离事务中。
- 预览存储是只读覆盖层，任何写操作只修改临时内存。
- `password` 等配置字段只把 opaque secret reference 交给 Lua，不直接返回秘密正文。
- 网络任务允许在请求头或请求体字段中引用 secret reference，由宿主在发送前注入，日志和诊断只显示引用名称。
- API v2 首版不提供通用 `secrets.reveal()`；确需读取秘密正文的未来能力必须单独设计高风险权限、用途说明和泄漏防护。

## 11. 调度与刷新

### 11.1 统一调度 API

```lua
schedule.every("clock", 60000, {
    whenHidden = "throttle",
    coalesce = true
})

schedule.at("market-open", time.add(time.now(), { hours = 1 }))

schedule.timeline("agenda", {
    { at = time.add(time.now(), { hours = 1 }), value = "meeting" },
    { at = time.add(time.now(), { hours = 2 }), value = "available" }
}, { reload = "atEnd" })
```

调度事件通过统一 `event` 回调传递：

```lua
{
    kind = "schedule",
    id = "clock",
    missed = 3,
    coalesced = true,
    now = 1786669380000 -- UTC epoch milliseconds
}
```

timeline 事件还会携带 `value/timelineIndex/timelineCount/timelineEnded/reload`；跨过多个
条目时只提交最新到期值，`missed` 记录被省略的较早条目数。`reload="atEnd"` 只在
最终事件标记 `reload=true`，由组件在事件回调中发布下一组 timeline，不隐式重跑 VM。

### 11.2 调度规则

- 默认最小周期仍为 100 ms，但作者工具对低于 1000 ms 的后台刷新发出警告。
- 周期只是最早执行时间，不提供硬实时保证。
- 相同截止时间在宿主层合并，减少窗口消息和重复重绘。
- 隐藏组件支持 `continue`、`throttle`、`pause`，默认 `throttle`。
- 恢复可见时只发送一条合并事件，组件可检查 `missed`。
- 预览禁止注册真实计时器，使用固定虚拟时钟。
- 内置组件迁移时必须把 `refreshIntervalMs` 和 `widget.setTimer` 显式改为 v2 schedule，不在发布运行时保留隐式适配。

当前过渡实现已发布 `schedule.basic`：`schedule.every(id, ms)`、
`schedule.after(id, ms)`、`schedule.at(id, epochMilliseconds)` 和
`schedule.cancel(id)` 复用宿主截止时间队列，限制每实例
32 个、ID 128 字节、最小实际周期 100 ms，并通过 `event.kind="schedule"` 返回
`id/missed/coalesced`。`schedule.visibility` 已增加第三个
`{ whenHidden="pause"|"throttle"|"continue" }` 参数，默认 throttle；pause 在恢复
可见时合并错过的截止时间，throttle 使用 5000 ms 隐藏下限。卸载、热重载和关闭
自动取消。`schedule.absolute` 将最远 366 天的 UTC 绝对截止时间重新投影到单调时钟，
过去截止时间在下一宿主唤醒合并触发。`schedule.timeline` 已支持每计划 1–64 个严格
递增绝对条目、受限 JSON-like value、跨条目合并和 `reload="atEnd"` 最终事件标志；
预览已通过 `time.previewClock` 使用固定 wall/monotonic 时间；schedule 在预览实例内
完成同等参数校验和登记，但不创建系统计时器或自动推进虚拟时间。至此统一调度的
基础、绝对时间、timeline、可见性和确定性预览契约均已落地。

## 12. 数据订阅与任务

### 12.1 数据订阅

```lua
local cpu = data.subscribe("system.cpu", {
    maxAgeMs = 1000,
    whenHidden = "throttle"
})

local snapshot = cpu:value()
-- snapshot.available
-- snapshot.value
-- snapshot.timestamp
-- snapshot.stale
-- snapshot.error
```

原则：

- 多实例订阅相同数据源时由 `WidgetDataBroker` 合并底层采样。
- 数据源返回统一包络，不用 `available=false` 和空字符串组合猜测错误状态。
- 订阅自动随实例销毁，允许组件主动 `unsubscribe()`。
- 数据变化只使真正依赖该订阅的视图失效。
- 提供者必须声明最小刷新间隔、缓存策略、预览数据和所需权限。

供 11 个内置组件迁移使用的首批数据源如下；完整 v2.0 系统能力面见第 12.6 节：

- `system.cpu`
- `system.memory`
- `process.summary`
- `system.gpu`
- `system.power`
- `system.network.status`
- `system.network.traffic`
- `system.storage.volumes`
- `system.storage.io`
- `system.display.current`
- `media.sessions` / `media.current` / `media.timeline`
- `media.current`
- `media.timeline`
- `media.artwork`
- `audio.output.default`
- `audio.output.volume`
- `audio.output.analysis`（独立高风险权限，按可见订阅启动）
- `desktop.items`
- `desktop.selection`
- `desktop.changes`
- `app.indexStatus`
- `calendar.events`
- `calendar.selectedDate`
- `filesystem.watch`（只接受用户选择的 folder handle）

这些主题不表示底层必须轮询：状态变化优先使用系统事件，差分指标才进行周期采样。后续评估：

- 逐应用音频会话、Wi-Fi、蓝牙、VPN 和硬件温度/风扇。
- 天气和位置不作为无权限系统数据源；必须使用明确的网络/位置授权。
- 不提供通用 WMI 查询数据源。

### 12.2 数据提供者启停策略

所有提供者采用引用计数生命周期，而不是应用启动即常驻：

当前已发布 `data.subscribe`、`data.system.cpu`、`data.system.memory`、
`data.process.summary` 和 `data.system.gpu`、`data.system.power`，以及相互独立的 `data.system.network.status` /
`data.system.network.traffic`、`data.system.storage.volumes`、
`data.system.storage.io`、`data.system.display.topology` 和
`data.system.display.current`，以及 `data.audio.output.default`、
`data.audio.output.volume`、`data.audio.output.analysis`，和
`data.media.sessions/current/timeline/artwork`、
`data.desktop.items/selection/changes`、
`data.calendar.events/selectedDate`、`data.app.indexStatus` 和
`data.filesystem.watch` feature。
Lua 订阅句柄已接通 `WidgetDataBroker` 与独立工作线程 provider：首个合格订阅按需
启动对应 topic，多实例共享有效采样率，数据变化只使依赖实例失效，隐藏状态进入
pause/throttle，最后订阅进入 idle grace，卸载、热重载和关闭自动释放。CPU 冷启动
差分通过 `warmingUp` 表达；GPU topic 枚举全部非软件 DXGI adapter，并在 PDH
差分就绪后按 adapter LUID 聚合利用率，最后 GPU 订阅释放后即使共享 worker 仍为
其他 topic 运行也会关闭 PDH query；网络状态使用 Windows 连接 profile 与 cost 提示，
网络流量只聚合活动接口计数且不返回 IP、MAC、SSID、BSSID 或主机名；电源快照
区分电池不存在、暂不可用和采样失败；存储卷枚举在后台线程进行，返回不透明卷
ID、显示用挂载点、类型、容量可用性、可用空间和只读/可移除状态，不授予文件读取；
远程卷不会被同步探测容量，避免断开映射阻塞共享 provider；预览只
返回固定模拟快照，不启动真实 provider。当前 `continue` 对这些数据源会按
provider 能力收敛为隐藏 throttle。

`system.storage.io` 使用独立 PDH query 读取物理磁盘聚合读写速率与忙碌度，
冷启动通过 `warmingUp` 表达，不公开磁盘序列号或文件路径；最后一个 I/O 订阅
释放时即使共享 worker 仍在运行也会关闭该 query。

`process.summary` 使用共享系统 provider 按需枚举宿主可查询的进程，固定返回按 CPU 与
内存排序的前 12 项。公开字段只含生命周期 opaque ID、显示名、总机器 CPU 占比、working
set/private bytes 和截断状态；不公开 PID、路径、命令行、窗口标题、用户名、token、内存
内容或控制句柄。首轮建立 CPU 时间基线并标记 warming，隐藏时强制 pause，最后可见订阅
释放后立即清除公开快照。

`system.display.topology` 枚举全部活动显示器，返回不透明 ID、用户可见名称、
逻辑/像素边界及工作区、有效 DPI/scale、刷新率、方向和可用时的 HDR 状态；
原始 GDI 设备名只在宿主内部用于 DisplayConfig 映射，不作为稳定标识公开。
`system.display.current` 复用同一采样模型，但由引擎使用订阅所属实例的 surface
边界匹配显示器；匹配失败时返回稳定不可用状态，不擅自回退主显示器。

`audio.output.default` 与 `audio.output.volume` 已接入默认 multimedia render
endpoint，分别返回不透明 endpoint ID、友好名称/状态和有界主音量/静音值；只有
存在合格订阅时才访问 Core Audio，不会启动 loopback 或暴露原生设备 ID。当前先
使用低频兜底采样，IMMNotificationClient 与 IAudioEndpointVolumeCallback 事件接入
仍属于本数据域的后续工作。

`audio.output.analysis` 已使用独立 WASAPI loopback 捕获线程；默认向 Lua 发布 128 点
mono waveform、64 个归一化频谱 bin、RMS/peak/静音和设备变化状态，也允许订阅在
`features` 中选择派生结果、用 `waveformPoints`/`spectrumBins` 请求有界尺寸，并用
`updateHz` 请求 1–60 Hz 的发布上限。首个已授权可见订阅启动，最后一个可见订阅消失、
权限撤销或卸载时立即停止并清空快照，不使用 idle grace。多订阅由 broker 合并为一条
捕获管线：provider 使用所有合格订阅所需特征的并集、最大点数和最高刷新率计算一次，
再为每个订阅裁剪字段与降采样；预览使用确定性模拟数据。运行状态指示仍待后续收口。

`media.sessions`、`media.current`、`media.timeline` 和 `media.artwork` 已接入同一按需 WinRT GSMTC
采样路径：最多返回 32 个会话、不透明 session ID、受限元数据、播放状态、时间线和
逐动作 `can*`，同一轮到期的四个 topic 只查询一次 Windows 会话管理器。列表为空是
正常可用状态，current/timeline 使用稳定 `notPresent`；预览不读取开发机媒体状态。
artwork 在 provider 工作线程中读取最多 4 MiB 编码内容，将边长不超过 16384 的源图
限尺寸解码为最长边 512 的 PBGRA，并以临时 image resource handle 交给即时绘制或声明式
视图；同一媒体身份复用解码结果，最后订阅释放时清除像素和 GPU bitmap。媒体控制已通过
task/action broker 提供，并在执行时复核权限、会话能力和可信手势。

`desktop.items`、`desktop.selection` 和 `desktop.changes` 已作为无轮询的宿主事件
数据源接入 broker；列表分别限制为 2048/512 项，只返回稳定宿主引用和展示元数据，
不把 v1 快照中的绝对路径带入 v2。宿主 reload/application 变化推进单调 revision
并只使订阅实例失效，预览返回固定数据；更细的 added/removed/modified 差分和完整
选择变化通知仍待桌面模型的统一 revision 源收口。

`calendar.events` 和 `calendar.selectedDate` 已接入现有本地 CalendarService 的事件
回调，不增加轮询线程。events 支持可选、成对的 ISO 日期闭区间并限制在 366 天，
未指定时围绕当前选中日期取前后各 62 天；每次最多返回 512 项并显式报告
`truncated`。事件修改与选择变化分别推进 revision，预览使用固定日期和事件。
calendar 写入已通过 task/action broker 提供 `calendar.create/update/remove`，并保留 revision
冲突检查；remove 在执行时要求可信用户动作，不能借只读订阅直接修改事件。

`app.indexStatus` 已作为宿主应用索引事件的轻量状态订阅接入，返回
`indexing/ready/unavailable + revision`，不在每次变化时复制完整应用目录；预览固定
为 ready。应用结果检索进入有界 `app.search` 任务，只有任务启动时才在 UI 线程复制
一次目录快照，随后转交 worker 匹配。

```text
Stopped
  -> Starting       first eligible subscription
  -> Active         one or more active subscribers
  -> IdleGrace      last ordinary subscriber released
  -> Stopped        grace expired / permission revoked / shutdown
```

- 包安装、组件预览和仅加载 manifest 不启动真实数据提供者。
- 组件在 `setup()` 中成功创建订阅、权限已授予且可见性策略允许后，broker 才增加底层引用；读取已有缓存不等于自动启动高成本提供者。
- 每个数据源独立启停。订阅 `system.cpu` 不得顺带启动 GPU、网络、媒体或音频采集。
- 多组件、多实例对同一数据源的请求合并。broker 在提供者允许的范围内选择订阅者要求的最高有效刷新率，只进行一次采样，再向各订阅按其节奏发布。
- `whenHidden="pause"` 不保留活跃引用；`throttle` 只保留提供者允许的低频引用；`continue` 仅适用于明确支持后台运行的低风险数据源。高隐私/高功耗源可以强制覆盖为 `pause`。
- 普通系统采样在最后引用消失后可以保留短暂 idle grace，避免桌面切换或热重载反复初始化；高风险捕获、权限撤销和应用退出不使用 grace，立即停止并清空缓冲。
- CPU、网络和 GPU 利用率依赖差分采样，冷启动阶段返回 `warmingUp=true`，取得足够样本后再发布有效速率；不得用旧基线计算跨长暂停区间的虚假尖峰。
- 内存等廉价快照可以按发布节奏读取；电源、媒体会话、设备变化等优先使用 Windows 事件，事件可用时不进行固定周期轮询。
- provider 初始化、采样和聚合在工作线程进行；UI 线程只接收不可变快照和失效通知。
- 诊断面板显示 provider 状态、活跃/隐藏订阅数、实际采样率、共享命中、最后启动/停止原因和资源占用。

建议默认策略：

| 数据源 | 默认行为 | 可见刷新 | 隐藏策略 | 最后订阅释放 |
|---|---|---:|---|---|
| `system.cpu` / `system.network.traffic` / `system.storage.io` | 周期差分采样 | 1000 ms，可受限提高 | throttle 5000 ms | 短暂 grace 后停止 |
| `process.summary` | 受限进程枚举与差分采样 | 1000 ms | 强制 pause | 立即停止并清除快照 |
| `system.gpu` | 高成本周期采样 | 1000 ms，设硬下限 | 默认 pause | 短暂 grace 后释放 PDH |
| `system.memory` | 廉价按需快照 | 1000 ms | throttle | 短暂 grace 后停止 |
| `system.power` / `system.network.status` / `system.display.current` | 事件优先、低频兜底 | 变化时 | 事件保留 | 无轮询时无持续成本 |
| `system.storage.volumes` | 设备/卷变化事件优先 | 变化时 | 事件保留 | 解除通知或共享轻量监听 |
| `media.current` / `media.timeline` | GSMTC 事件优先 | 变化时 | 允许事件保留 | 解除会话事件订阅 |
| `audio.output.default` / `audio.output.volume` | 设备和 endpoint volume 通知 | 变化时 | 允许事件保留 | 解除设备/音量通知 |
| `filesystem.watch` | 用户句柄范围内的变更通知 | 合并变化事件 | 默认 pause | 关闭目录句柄并清空待发事件 |

具体最小间隔和 grace 时长由基准测试冻结到 API 契约；组件提出的频率是上限内的请求，不是硬实时保证。

### 12.3 音频波形与频谱

媒体元数据和音频采样是不同能力。`media.read` 只允许读取标题、播放状态和时间线，不能获得音频样本。音频可视化使用独立高风险权限 `audio.output.analyze` 和独立数据源：

```lua
local audio = data.subscribe("audio.output.analysis", {
    features = { "waveform", "rms", "peak", "spectrum" },
    updateHz = 30,
    waveformPoints = 128,
    spectrumBins = 64,
    whenHidden = "pause"
})

local frame = audio:value()
-- frame.waveform: normalized -1.0 .. 1.0
-- frame.rms / frame.peak: normalized 0.0 .. 1.0
-- frame.spectrum: normalized bins
-- frame.timestamp / frame.silent / frame.deviceChanged
```

运行规则：

- 只有用户已明确授予 `audio.output.analyze`，并且至少有一个可见实例持有该订阅时，宿主才创建 WASAPI loopback capture。
- 最后一个可见订阅隐藏、取消、卸载或失去权限时立即停止捕获、释放 endpoint/线程并清空 PCM 环形缓冲；不使用 idle grace，不允许组件把 `whenHidden` 改为 continue。
- 多组件共享一个按默认输出 endpoint 建立的捕获与分析管线；broker 取受限后的最高 updateHz/FFT 配置，再为各订阅降采样，不能为每个实例创建一个捕获客户端。
- 普通 Lua 组件不获得原始 PCM、音频文件、进程级音频内容或无限长度历史，只获得宿主计算的定长 waveform/RMS/peak/spectrum 数组。
- `features` 是 1–4 个不重复的 `waveform/rms/peak/spectrum`；`waveformPoints` 限制为 16–256，`spectrumBins` 限制为 16–128，且尺寸选项只能与对应特征同时使用。未配置时默认启用全部特征并使用 128/64 点。
- `updateHz` 必须是 1–60 的整数，并与通用 `maxAgeMs` 互斥；它是最大投递频率请求，不是硬实时保证。捕获、混音、FFT 和降采样全部离开 UI/Lua 线程。
- 无播放或持续静音时发布低频静音状态并允许 provider 进入受控 idle；检测到新音频会话后恢复，恢复机制必须由宿主事件驱动，不允许组件轮询拉起。
- 默认输出设备变化时原子重建捕获，发布 `deviceChanged`/warming 状态；旧 endpoint 数据不得混入新设备时间线。
- 授权页明确说明“分析当前电脑正在播放的声音”，组件运行时提供可见的使用状态和一键撤销入口。
- 预览只提供确定性的模拟波形，绝不启动真实音频捕获。
- 麦克风不包含在该权限中。未来若支持，使用独立 `audio.microphone.capture`、Windows 系统隐私授权和更严格的可见指示；不得由输出分析权限推导获得。

### 12.4 一次性任务

网络请求、搜索和可能较慢的查询统一使用任务 ID。当前公网请求已经以精确域名、
HTTPS 请求、受限 method/header/body、secret descriptor、响应上限和每跳重定向复检接入任务代理：

```lua
local request = task.start("network.request", {
    url = "https://api.example.com/data",
    cacheSeconds = 300,
    maxBytes = 512 * 1024
})

-- event.kind == "task.complete"
-- event.taskId, event.ok, event.value, event.error
```

当前已公开 `media.play/pause/toggle/stop/next/previous/seek/setRate/setShuffle/setRepeat`，
以及有界分页的 `app.search` 与引用化 `app.launch`。媒体动作必须从可信用户手势同步
调用栈启动，并由同一个通用完成事件返回。可传入 `media.sessions/current` 提供的
不透明 `sessionId` 精确控制组件正在展示的会话；省略时控制 Windows 当前会话：

```lua
local taskId, err = task.start("media.toggle", { sessionId = session.id })
-- 后续 event.kind == "task.complete" 且 event.taskId == taskId
```

要求：

- 任务可取消、有限额、可诊断。
- 任务完成事件按实例串行投递，不能并发重入同一 Lua VM。
- `dispose`、权限撤销或包更新自动取消未完成任务。
- v2 首版不引入隐式 Promise；协程封装可在稳定任务契约之上后续增加。

当前已公开 `task.start`、`task.cancel`、`task.media.control`、
`task.audio.output.control`、`task.app.search`、
`task.app.launch`、`task.notification.show`、`task.notification.lifecycle`、
`task.notification.schedule`、`task.notification.structured`、
`task.notification.actions`、`task.calendar.write`、
`task.network.request`、`task.network.headers`、`task.network.requestBody`、
`task.network.secretReference`、`task.shell.openUri`、`task.desktop.search`、
`task.everything.search`、`task.shell.item`、`task.system.openSettings`、
`task.clipboard.text`、`task.clipboard.image`、`task.clipboard.fileReference`、
`task.filesystem.picker`、`task.filesystem.access`、`task.filesystem.binary`、
`data.filesystem.watch` 和
`task.desktop.refresh` feature，完整媒体
控制动作、两个应用任务、实例作用域通知 ID 与 show/update/dismiss/schedule/cancel、
本地日历 create/update/remove、公网 HTTPS 有界请求、
可信手势外链、用户选择文件/目录、桌面/Everything 项目搜索及受控打开、定位和刷新任务。
`WidgetTaskBroker` 生命周期内核负责任务描述符注册、全局/实例/
任务类型并发上限、权限和可信手势门禁、preview 标记、显式取消、撤权取消、实例
dispose 与 shutdown 原因，以及执行器完成确认均有独立契约测试。实时与预览引擎均
持有独立 broker，点击、指针按下/抬起、滚轮、菜单命令、宿主按钮以及由调用方明确
标记来源的打开回调使用仅限同步调用栈的可信手势作用域；任务还携带 Lua VM owner
token，避免热重载时同名实例的旧任务完成事件误投给新 VM。媒体执行器在独立 MTA
工作线程调用 GSMTC，按目标会话 `can*` 能力执行，并返回 accepted 或稳定错误码；
seek 以时间线起点为基准且受最小/最大可跳转范围约束，预览只产生确定性 mock。
默认音频输出控制只接受 `setVolume/setMute`，始终在独立 COM MTA 线程重新解析当前
multimedia render endpoint；音量钳制到 0–1，Core Audio 调用携带 SnowDesktop 来源
GUID，同一实例以 100 ms 最小间隔限速，不开放逐进程或非默认设备控制。
通知生命周期内核为 show/schedule 返回宿主不透明 ID，允许更新已投递或预约文本、包资源图、
0–1 进度和最多两个操作按钮，
区分 dismiss 与 cancel，并在统一 runtime tick 到期投递 `notification.delivered`；每实例
最多 64 个记录/32 个预约、每分钟最多实际投递 5 次；卸载、热重载和撤权删除预约，
shutdown 只清理当前提供者状态。预约已用原子文件绑定实例 ID、包 ID 和 opaque ID，
重启后重新绑定 VM token，
并对 24 小时内错过的期限补投；纯文本继续使用托盘气泡，结构化内容使用非模态、不抢焦点的
宿主通知窗。按钮通过通知 ID 回传 `notification.action` 到原实例并建立可信手势作用域；实例
已销毁时安全丢弃。预约只持久化包资源名，恢复时重新按当前包根解析。
`system.openSettings` 只接受宿主枚举的 notifications/audio/display/network/
bluetooth/power/storage/apps/personalization 页面，并在可信手势和 `shell.launch` 权限下
映射为固定 `ms-settings:` URI；Lua 不能传 scheme、查询参数或原始 URI。
剪贴板公开 `clipboard.read/write/clear`：读取和修改分别要求独立权限，三者均要求
可信手势、100 ms 每实例限速、异步取消和确定性预览。read 显式支持 text、image 和
file-reference；文本限制为 256 KiB UTF-8，图片限制 64 MiB 输入、16384 源尺寸并缩放
到 512 像素以内的实例临时图片句柄，文件引用一次最多 32 项且只返回可用于图标、打开
和定位的不透明引用，不授予路径或内容读取。write/clear 仍只支持文本，历史不开放。
`filesystem.pickOpen/pickSave/pickFolder` 已通过系统选择器公开首批用户授予范围；
返回值只包含随机 opaque handle、kind、access 和显示名称，注册记录在 Lua 普通存储之外
持久化并同时绑定 package ID 与实例 ID，删除实例或卸载包时撤销。
`task.filesystem.access` 在同一边界上增加异步 `stat/list/read/write/release`：只枚举
一层目录并分页，将子项继续转换为 opaque handle；UTF-8 与 `task.filesystem.binary`
探测后的原始字节整文件读写都限制为 1 MiB，写入使用原子替换、可选 expected revision
冲突检测和每实例限速。
`filesystem.watch` 现已作为实例参数化数据订阅公开：只接受 folder handle，以 IOCP
承载非递归 `ReadDirectoryChangesW`，合并 added/removed/modified/renamed 并在丢失变化时
报告 overflow；隐藏默认且强制 pause，退订、实例 dispose、撤权和 shutdown 会关闭目录
句柄并清空事件。组件仍不能解释句柄或回退为路径。API v1
同步 `media.playPause/next/previous` 已通过函数版本上限从 v2 VM 隐藏，不能绕过
手势门禁。应用搜索从 UI 线程复制宿主索引为不可变、有上限的目录快照，在独立任务
线程完成名称/拼音排序与分页，只向 Lua 返回展示字段和实例作用域的不透明引用；
`app.launch` 只接受该引用并再次检查目录 revision、`app.launch` 权限与可信用户手势，
不接受路径、参数或工作目录。公开 feature 为 `task.app.search/task.app.launch`，
预览使用确定性引用与结果。日历任务复用本地 `CalendarService` 的 revision 冲突和稳定
错误，Lua 只收到异步完成事件；create/update 不要求手势，remove 要求直接指针或菜单
来源。`desktop.search` 从 UI 线程取得最多 2048 项的不可变桌面快照，再由独立执行器
完成有界名称/拼音检索；`everything.search` 在 worker 中执行并与进程级 Everything
SDK 的其他调用串行。两类搜索都只返回实例作用域的不透明项目引用；v2 `draw.icon`、
`shell.openItem/revealItem` 只解析该引用，Lua 不取得路径。项目动作和 `desktop.refresh`
再次检查 `desktop.action` 与可信手势；桌面 revision 改变会使旧桌面引用失效。

### 12.5 系统 API 缺口审计与分层

当前运行时注册的系统相关公共面只有：`sys.getTime/notify/cpu/memory/battery/network/gpu`、`media.current/playPause/next/previous`、HTTP、桌面项目/应用搜索/打开/定位、Everything 搜索和宿主本地日历。现状存在以下结构性缺口：

- `system.read` 把性能、电源、存储、网络和显示混成一个权限，无法做到最小授权。
- 只有单一聚合 CPU/GPU/网络快照，缺少多 GPU/多卷/多显示器结构、网络连接状态与流量拆分，以及统一的单位、时间戳和 warming/stale 语义。
- clipboard 文本/图片/文件引用、文件选择句柄、stat/list、UTF-8/二进制整文件 read/write
  和非递归文件 watch 已有首批能力；分块流式文件访问、递归目录和剪贴板历史仍未完成。
- 媒体会话列表、当前会话、时间线、限尺寸封面句柄、seek/stop 和逐源动作能力已形成首批
  公共面；后续缺口是 GSMTC 事件驱动更新以及更多播放器的兼容性矩阵和实机验证。
- 默认音频 endpoint、主音量/静音订阅、受控修改和音频分析均已进入公共面；非默认 endpoint
  枚举、逐应用会话和音量控制仍属于 v2.x 独立权限评估项。
- v2 通知已补齐 ID、结构化更新、撤销、持久调度、频控、失败状态、包资源图、进度、
  最多两个按钮、动作回传和跨应用重启的预约恢复；后续只保留系统免打扰策略及不同 Windows
  通知提供者的实机兼容矩阵。
- OS/架构 feature probe、Windows 时区键和基础区域格式化已进入公共面；更专业的
  日历制、数字骨架与相对时间样式作为 v2.x 扩展评估，不扩大 v2.0 必选面。

API v2 不把 Win32、COM 或 WinRT 原样暴露给 Lua，而是固定为四个平面：

1. **纯查询/格式化**：`system.info/capabilities`、`time.*`、`l10n.format*`；同步、无 I/O、无外部副作用。
2. **实时状态**：统一由 `data.subscribe(topic, options)` 提供；支持共享、节流、可见性和取消。
3. **慢读取与用户选择**：统一由 `task.start(name, args)` 提供；可取消、有限额，不能阻塞 UI/Lua 线程。
4. **外部动作**：仍通过 task/action broker 执行，并自动绑定当前受信任用户手势上下文；Lua 不取得可保存或伪造的 gesture token。

### 12.6 API v2.0 必选系统能力目录

#### 12.6.1 基础环境与纯工具

| API | v2.0 契约 | 权限 |
|---|---|---|
| `widget.context()` | 当前 surface 的 logical size、DPI/scale、size class、monitor/work-area 摘要、主题、accent token、高对比度、reduced motion、text scale、locale、region、time zone、input language、可见/预览/焦点状态 | 基础；只描述当前组件环境 |
| `system.capabilities()` | API/数据主题/动作的版本和 available/reason；用于硬件、Windows 版本和宿主 feature probe | 基础；不能返回设备唯一标识 |
| `system.info()` | OS family/build、process/native architecture、host/app/API version、portable/packaged 等运行模式 | 基础；不返回 user name、computer name、SID、serial 或安装 ID |
| `time.now()` / `time.monotonic()` | UTC epoch milliseconds 与不可回拨的 monotonic milliseconds；所有时长计算使用 monotonic | 基础 |
| `system.uptime()` | 系统启动以来的有界 duration 及 `includesSleep` 契约；不能用进程启动时间冒充系统 uptime | 基础 |
| `time.parts/format/add/compare()` | 明确 time zone 和 locale 的日期拆分、格式化、安全加减与比较；处理 DST、闰日和系统时区变化 | 基础 |
| `l10n.formatNumber/bytes/duration/relativeTime/list()` | 使用当前或显式 locale 的宿主格式化，不要求组件自己拼接单位和复数 | 基础 |

当前实现（2026-08-15）已提供 `widget.context()` 和 `widget.context` feature。返回值同时区分 logical/pixel size，包含 DPI/scale、size class、栅格跨度、当前显示器及工作区的 logical/pixel 摘要、主题与系统 accent、高对比度、reduced motion、text scale、locale、region、time zone、UTC offset、输入语言，以及 visible/preview/focused/selected 状态。预览使用显式预览 DPI 且将显示器摘要标记为 unavailable，避免把开发机真实显示器误当预览契约；`focused` 已覆盖宿主管理的文本输入和声明式元素键盘/UIA 焦点，表示当前 surface 是否拥有任一宿主焦点。

#### 12.6.2 可订阅系统状态

| 数据主题 | v2.0 最小字段/行为 | 权限 |
|---|---|---|
| `system.cpu` | aggregate usage、logical processor count；可选受限 per-logical usage；明确 warming 和采样区间 | `system.performance.read` |
| `system.memory` | physical total/available/used、commit limit/used/available、usage percent；全部使用 bytes | `system.performance.read` |
| `process.summary` | 最多 12 个可查询进程的 opaque ID、显示名、总机器 CPU 占比、working set/private bytes 和截断状态；不返回 PID、路径、命令行、窗口标题、用户或控制能力 | `process.summary.read` |
| `system.gpu` | adapter 数组、opaque adapter ID、display name、usage、dedicated/shared memory；不得只返回第一块 GPU | `system.performance.read` |
| `system.power` | AC/battery/charging/saver、percent、可用时的 estimated remaining、状态变化；无电池设备正常返回 unavailable | `system.power.read` |
| `system.storage.volumes` | opaque volume ID、display name、mount point display value、kind、capacity/free、removable/readOnly；不因此授予文件读取 | `system.storage.read` |
| `system.storage.io` | aggregate 或按 opaque volume ID 的 read/write bytes per second、busy percent（平台可用时）和 warming | `system.storage.read` |
| `system.network.status` | none/local/internet、transport kind、metered/roaming/over-limit 状态；变化事件优先，不把状态提示当作请求一定成功的证明 | `system.network.read` |
| `system.network.traffic` | aggregate 或按 opaque adapter ID 的收发速率/累计字节；不返回 IP、MAC、SSID、BSSID 或 host name | `system.network.read` |
| `system.display.current` | 当前 surface 所在显示器的 logical/pixel bounds、work area、DPI/scale、refresh rate、orientation、HDR 状态（可用时） | 基础摘要；扩展字段需要 `system.display.read` |
| `system.display.topology` | 多显示器数组、opaque display ID、primary、bounds/work area、scale、refresh/orientation/HDR；拓扑变化事件 | `system.display.read` |
| `media.sessions/current/timeline` | 会话列表、opaque session ID、来源显示名、标题/作者/专辑、播放状态、position/duration、每个动作的 `can*` 能力 | `media.read` |
| `media.artwork` | 宿主限尺寸解码并返回临时 resource handle；不返回缓存文件路径或无限原图 | `media.read` |
| `audio.output.default` | 默认 render endpoint 的 opaque ID、display name、状态和设备变化 | `audio.output.read` |
| `audio.output.volume` | master volume、mute、可用范围和 endpoint 变化；使用 endpoint callback，不做高频轮询 | `audio.output.read` |
| `audio.output.analysis` | 第 12.3 节的 waveform/RMS/peak/spectrum 有界分析 | `audio.output.analyze` |
| `desktop.items/selection/changes` | 宿主管理桌面项目、选择和变更；项目使用稳定引用，路径字段按权限和来源裁剪 | `desktop.read` |
| `app.indexStatus` | 宿主应用索引的 ready/indexing/error/revision 状态；不把完整应用目录作为每次订阅负载 | `app.discovery` |
| `calendar.events/selectedDate` | 宿主本地日历事件和共享选择日期；读取与写入继续分权 | `calendar.read` |
| `filesystem.watch` | 仅监听用户选择的 folder handle；事件合并为 added/removed/modified/renamed/overflow，隐藏默认暂停 | `filesystem.userSelected.watch` + 有效句柄 |

#### 12.6.3 任务与受控系统动作

| 任务/动作 | v2.0 契约 | 权限/手势 |
|---|---|---|
| `network.request` | 任意公网 HTTPS；可选精确域名收窄；逐跳重定向、DNS/连接地址复查、请求/响应额度和取消 | `network.internet` |
| `notification.show/update/dismiss/schedule/cancel` | 宿主生成 notification ID；文本、包资源图、有限按钮和进度；动作回传到实例，实例不存在时安全丢弃或进入宿主处理 | `notification.post`；频控，紧急/闹钟场景另行批准 |
| `clipboard.read` | 只读明确请求的 text/image/file-reference 类型，限制大小，不枚举历史 | `clipboard.read` + 当前用户手势 |
| `clipboard.write/clear` | 写入白名单格式和有限数据；来源可诊断 | `clipboard.write` + 当前用户手势；禁止后台循环覆盖 |
| `filesystem.pickOpen/pickSave/pickFolder` | 由宿主显示系统选择器，返回包/实例绑定的不透明 file/folder handle；Lua 不取得绝对路径 | 清单声明用途 + 当前用户手势；用户在选择器中授予范围 |
| `filesystem.stat/list/read/write` | 只接受有效 handle 或其受限子项；分页、字节/文件数/深度/编码额度，原子写和冲突检测 | 对应 `filesystem.userSelected.read/write`；write 不能由 read 推导 |
| `desktop.search` / `app.search` / `everything.search` | 分页、有上限、可取消和可 debounce 的搜索任务；结果为宿主稳定 item/app reference，不在每次按键同步创建无界 Lua table | 分别需要 `desktop.read`、`app.discovery`、`everything.search` |
| `shell.openUri/openItem/revealItem` | URI scheme allowlist 或宿主 item/file handle；先解析最终目标再执行，不接受任意 shell verb | `shell.launch` 或 `desktop.action` + 用户手势 |
| `desktop.refresh` | 只作用于 SnowDesktop 管理的数据 | `desktop.action` + 用户手势 |
| `app.launch` | 只接受 `app.search` 返回的宿主引用；不接受任意 executable path、命令行或 working directory | `app.launch` + 用户手势 |
| `media.play/pause/toggle/stop/next/previous/seek/setRate/setShuffle/setRepeat` | 只在目标 session 的 `can*` 为真时执行；异步返回 accepted/result，不伪造成功 | `media.action` + 用户手势 |
| `audio.output.setVolume/setMute` | v2.0 仅作用于当前默认 render endpoint；范围钳制、来源标记、速率限制 | `audio.output.control` + 用户手势；不提供非默认 endpoint 或每进程静音 v2.0 |
| `calendar.create/update/remove` | revision 冲突、日期/时区规则、稳定错误码；沿用宿主本地日历。共享日期选择使用无副作用的 `calendar.selectDate()`，不创建或修改事件 | `calendar.write`；删除需要用户动作来源 |
| `system.openSettings` | 只接受宿主维护的设置页枚举，如 notifications/audio/display/network；不接受任意 `ms-settings:` 字符串 | `shell.launch` + 用户手势 |

文件选择器返回的原生路径只在宿主内部使用。句柄绑定 `packageId + instanceId + grant revision + target identity + access mode`，可被用户撤销；包更新扩权、文件被移动/删除、卷卸载和权限撤销都必须产生稳定状态，不能退回为任意路径访问。

### 12.7 v2.x 评估项与明确禁止的系统直通

v2.0 稳定后再评估以下独立能力；它们不能借 `system.*` 通配权限提前进入：

- `process.summary` 已以独立 `process.summary.read` 权限提供受限 top-N 资源摘要；更细的进程历史、筛选、I/O/GPU、逐进程动作和诊断仍不进入 v2.0。
- `audio.sessions.read/control`：逐应用音频会话和音量控制，独立于 master endpoint 权限。
- Wi-Fi SSID/BSSID、蓝牙、VPN、位置、摄像头、麦克风、环境光和其他传感器；需要独立隐私设计和 Windows 系统授权。
- 系统/账户日历、联系人、邮件和云账户；当前 `calendar.*` 只是 SnowDesktop 本地日历，不能把它描述成 Windows 或 Outlook 数据访问。
- CPU/GPU/主板温度、风扇和电压；Windows 没有跨硬件稳定一致的基础契约，不以私有驱动或常驻 WMI 作为 v2.0 保证。
- 亮度、色温、HDR 切换、非默认音频 endpoint 枚举/音量、默认音频设备切换、锁屏、睡眠、休眠、关机和重启；属于额外隐私或高影响动作，控制项至少要求逐次宿主确认。
- 壁纸读取、回收站统计、活动窗口/前台进程、用户空闲时间、通知历史和浏览器历史；先完成用途、可见性和隐私评审。
- 天气、股票和在线账户不是“无权限系统 API”；应由明确的网络服务/账户连接器提供。

API v2 明确禁止：任意 WMI 查询、注册表读写、PowerShell/cmd/进程创建、DLL/FFI/COM/WinRT 激活、任意 Shell verb、任意绝对路径、原生 HWND/HANDLE/指针、进程内存、服务/驱动控制、全局键鼠 hook、屏幕抓取，以及绕过权限代理的系统调用。确有新场景时应增加窄能力，而不是增加 `system.raw`、`exec` 或 `native.call`。

### 12.8 系统能力公共契约

每个系统数据主题和任务必须进入第 17 节共同契约源，并额外声明：

- `schemaVersion`、所需权限、支持的 Windows/硬件条件、provider 类型、启动/停止策略、默认/最小刷新率和预览 mock。
- 字段类型、单位、范围、是否可空、数据来源、采样区间、monotonic `timestamp`、`available/warming/stale/error` 语义。
- 多设备数据使用数组和 opaque stable ID；不能假定单 GPU、单电池、单网卡、单卷、单显示器或单媒体会话。
- `unsupported`、`permissionDenied`、`notPresent`、`temporarilyUnavailable`、`warmingUp` 和 `stale` 必须可区分，不能全部折叠成 `nil`。
- 所有 potentially blocking 操作走 task；同步 API 不访问磁盘、网络、WMI、设备枚举或 Shell。
- 订阅按 topic/设备/选项共享，事件可用时不用轮询；权限撤销、设备移除、休眠和最后订阅释放都有确定清理行为。
- 输出默认去标识化并最小化；路径、IP/MAC、SSID/BSSID、用户名、机器名、序列号等不能因“读取系统状态”顺带泄露。
- 每个能力有独立额度、诊断、审计事件、拒绝降级示例和确定性预览数据。

当前实现已将第 12.6 节的 15 个基础函数、25 个数据主题和 41 个任务集中到
`widget_api_registry` 的系统能力契约。data/task broker 直接由该契约注册，
`system.capabilities()` 可按 feature 或公开 API 名返回权限、手势、预览、刷新率与并发上限；
数据主题还返回 LuaLS options/value 类型，任务返回 arguments/result 类型；离线
`snowwidget system-contract` 复用这些引用，契约测试同时校验每个类型在随产品分发的 LuaLS 中
存在，以及 feature、权限目录和开发者文档覆盖。硬件存在性和 provider
运行状态仍由订阅快照表达，不能把 host feature 存在误写成设备一定存在。

## 13. 视图与绘制

### 13.1 双渲染模式

- **即时绘制模式**：继续使用 `render()` / `draw.*`，适合时钟、仪表盘和自由图形。
- **声明式视图模式**：使用 `view()` 返回宿主管理的树，适合列表、表单、信息卡和可访问控件。

组件必须在描述对象中选择一种主模式。面板可以独立选择，但同一表面不能混合两种命中测试体系。

### 13.2 即时绘制增强

优先增加：

- `draw.arc`
- `draw.path`
- `draw.gradientRect`
- `draw.imageFit`
- `draw.shadow`
- `draw.sparkline`
- 统一颜色结构、描边、抗锯齿和透明度参数表。

所有新资源句柄由宿主缓存并按实例/包生命周期释放。

当前实现（2026-08-15）已经发布可探测 feature `draw.advanced`，并以 API v2
专用入口提供上述六项增强：圆弧最多一圈并拆为至多 3 段；路径为拒绝未知字段的
1–256 条严格命令；sparkline 为 1–512 个有限样本；`imageFit` 只接受实例资源句柄并由
宿主完成 contain/cover/none 的缩放与裁切；渐变矩形限制方向和最终几何；阴影以最多
16 层的宿主受控衰减近似柔化，blur 上限为 64，不开放任意 shader。坐标、尺寸、圆角、
线宽、透明度和扩展后的最终边界统一限制，纯几何规则通过独立契约测试覆盖。

即时绘制本身没有可命中的“元素”，因此 v2 同时提供独立交互区域：

```lua
interaction.region({
    key = "chart-point:" .. point.id,
    shape = { type = "circle", x = point.x, y = point.y, radius = 8 },
    cursor = "hand",
    events = {
        click = { id = "point.select", value = { pointId = point.id } },
        contextMenu = { id = "point.menu", value = { pointId = point.id } }
    },
    accessibility = {
        role = "button",
        label = point.label
    }
})

if interaction.isHovered("chart-point:" .. point.id) then
    draw.circle(point.x, point.y, 6, 0xFFFFFF)
end
```

- region 首版支持 rect、roundedRect 和 circle，复杂 path 命中作为可选 feature。
- 每次成功 render 原子替换该表面的 region set；稳定 key 保留 hover、focus 和 capture 身份，消失的 region 自动触发 leave/cancel。
- 指针状态变化自动请求重绘；自绘 hover/pressed 的视觉由组件产生，因此需要 Lua 下一帧，但命中、click 合成、菜单、焦点和用户手势证明仍由宿主处理。
- region 和即时绘制语义使用同一 key，作者工具对可点击但无无障碍信息、重叠歧义和超出组件边界的区域发出警告。

当前过渡实现（2026-08-15）已开放 `interaction.region/isHovered/isPressed`，支持
rect、roundedRect、circle、稳定 key、反向绘制顺序命中、宿主 click 配对、
pointer enter/leave/down/up/move、click、doubleClick、wheel 动作以及 region 独立
原生右键菜单。成功 render 原子提交，失败 render 保留上一成功集合；菜单选择按
region generation 校验，旧菜单不会落到新一代 region。普通区域交互和菜单本身不
要求权限，菜单内触发的启动、媒体控制等动作仍由各自 broker 校验权限与可信手势。
即时绘制溢出已增加 `interaction.scroll/setScrollOffset`：默认纵向，探测
`interaction.scroll.orientation` 后可使用横向 contentWidth；滚动位置按实例和稳定 key 隔离，
宿主负责对应主轴的 wheel、钳制、重绘和滚动条，组件使用 `draw.pushClip/popClip`
裁剪并按返回 offset 绘制。声明式轨道现已另行开放 `view.scroll`：支持纵向/横向单子树、
宿主 offset、测量、滚轮、裁剪绘制和裁剪命中，滚出视口的元素不会继续响应交互；
`view.scroll.programmatic` 允许非渲染回调按绝对/相对偏移定位，并按 1-based 索引定位固定行高
virtualList/virtualGrid 及已测量的可变高度 virtualList，所有结果仍由宿主范围钳制。
`view.scroll.initialTarget` 另提供普通 scroll
按可见后代稳定 key、固定行高虚拟集合按 1-based 索引的一次性 nearest 初始定位；宿主只在该
surface 尚无容器偏移时采用声明值，成功提交后不再用重渲染覆盖用户位置。
desktop 与宿主辅助 surface 的即时绘制均有各自原子提交的 region 集合；
`view.keyboardNavigation.basic` 已让可点击、受控和文本输入 region 进入所属 surface 的宿主
焦点序列。触控长按和完整 UIA scene tree 仍按 M6 后续交付物推进。受控 submenu
已通过 `interaction.contextMenu.submenu` 提供三层、全树 64 项的宿主原生子菜单；
`interaction.pointerCapture` 已允许声明式节点和即时 region 显式捕获主指针，在目标删除、surface
关闭、窗口失焦或系统取消捕获时由宿主释放；包内菜单图片通过
`interaction.contextMenu.resourceImage` 复用已声明、已解码的 `resource.image`，由宿主按菜单 DPI
缩放并在菜单关闭时释放显示副本。

文本编辑的过渡宿主控件已增加 `control.textInput/textArea/focus`：组件在 render 中
提交严格的稳定 key、storageKey、rect 与白名单视觉属性，宿主复用 Direct2D 光标、
选择、滚动、剪贴板规范化和 IME 候选位置。单行默认 4 KiB、多行默认 64 KiB，所有
插入按最终 UTF-8 大小原子校验；程序化聚焦只接受直接可信用户手势。该能力用于在
完整声明式 view tree 到来前迁移现有编辑型组件，不把 `ui.input` 当成普通文本编辑的
高风险权限，也不向 Lua 开放通用剪贴板读取。桌面 surface 的 Tab 顺序已经由
`view.keyboardNavigation.basic` 接管，UIA 输出仍未完成，不能把此过渡控件计作第 13.4 节
声明式控件全集完成。

`widget.define.panel(context, model)` 已接入 `widget.openPanel` 创建的宿主辅助 surface。
探测 `view.surface.panel` 后可返回与桌面 `view` 相同的声明式节点，返回 `nil` 仍走即时绘制；
宿主分别保存 panel 的上一成功树、region、滚动偏移、host control 与焦点状态，并把指针、
滚轮、元素菜单、键盘和 action/change 路由到 `surface="panel"`。面板 render 与桌面 render
一样禁止持久存储写入，失败提交保留上一成功场景。当前仍缺 panel 语义树的 UIA Fragment
导出，逻辑槽位的原生重排/移除也只面向 desktop，因此还不能把 M6 面板表面计为完整验收。

`widget.define.dialog(context, model)` 已通过 `view.surface.dialog` 复用同一辅助 surface
场景管线：`widget.openDialog` 创建居中的非阻塞模态 surface，宿主绘制遮罩并阻断背景桌面的
左/右/中键、滚轮、移动和双击输入，声明式焦点、输入、菜单及 action 均保持
`surface="dialog"`。默认 Escape 关闭、点击遮罩不关闭，可分别用 `dismissOnEscape` 和
`dismissOnOutside` 调整；宿主关闭按钮始终保留。panel 与 dialog 互斥，打开新 surface 会关闭
旧 surface。当前仍缺 dialog UIA Fragment 导出与真实桌面交互验收，故按已编译待验证能力记录。

`widget.define.popover(context, model)` 已通过 `view.surface.popover` 复用辅助 surface 场景管线。
`widget.openPopover` 只能在可信 desktop 手势中使用上一棵成功 scene 的稳定 `anchorKey`，宿主从
已裁剪命中范围计算锚点，支持 auto/top/bottom/left/right 与 start/end 变体，并在工作区内翻转、
钳制；Lua 不得提供屏幕坐标。无标题时使用紧凑 chrome，默认外部点击与 Escape 关闭。未知、禁用、
滚出裁剪区或陈旧 key 会返回 false，辅助 surface 内嵌套打开也会拒绝。popover 与 panel/dialog
共享唯一辅助 surface 槽位；当前仍缺 UIA Fragment 导出与真实桌面定位/关闭验收。

### 13.3 参考优先级与完整性边界

SnowDesktop 不照搬某一个框架，参考优先级如下：

1. **Windows UI Automation + WinUI/Fluent**：作为控件行为、键盘、焦点、视觉状态、ControlType、Control Pattern、属性和事件的主规范。SnowDesktop 是 Windows 桌面宿主，最终必须让 Narrator 和自动化工具看到符合 UIA 契约的控件。
2. **WidgetKit + Jetpack Glance**：决定组件场景应保持 glanceable、响应式和受限交互，不把 45+ WinUI 应用控件全部塞进桌面组件。参考 Button/Toggle、Row/Column/Box、Lazy collection、尺寸环境和宿主拥有生命周期。
3. **CSS Flexbox/Grid + Compose modifier 思路**：参考主轴/交叉轴、grow/shrink、gap、alignment、grid track 和约束组合，但不实现 CSS 选择器、级联、继承或字符串样式解析。
4. **React 的纯 view、稳定 key 和单向状态更新**：只借鉴树 diff 与副作用边界，不引入 JSX、DOM、Promise 或 JavaScript 事件对象。
5. **WAI-ARIA**：补充作者可理解的 role、state 和关系词汇；Windows 上的实际输出仍以 UIA ControlType/Pattern 要求为准，不能只设置一个 ARIA 风格 role 就宣称无障碍完成。
6. **Rainmeter**：只参考图表、meter 和数据/显示分离，不复制其逐项配置语法。

“完整”分三层定义：

- **v2.0 场景完整**：能无私有 API 地实现全部 11 个内置组件，以及时钟、天气、系统监控、媒体控制、便笺、任务、日历、RSS、启动器和音频可视化等常见组件。
- **控件契约完整**：每个公开节点都有类型、默认值、限制、适用属性、事件、视觉状态、键盘规则、UIA 映射、预览行为、性能额度和测试；没有“实现了外观但没有交互/无障碍”的半控件。
- **不是应用框架全集**：v2.0 不以覆盖整个 WinUI、HTML 或 Flutter 控件库为目标。复杂应用导航、富文档编辑、视频、Web、Ink 和任意原生控件属于延期或明确非目标。

### 13.4 API v2.0 必选节点

| 类别 | v2.0 必选节点 | 主要覆盖场景 |
|---|---|---|
| 布局 | `box`、`row`、`column`、`grid`、`stack`、`flow`、`scroll`、`spacer`、`divider` | 卡片、水平/垂直布局、覆盖层、换行、滚动和响应式尺寸 |
| 内容 | `text`、`styledText`、`image`、`icon`、`shape`、`badge` | 文本、有限富文本 span、包内图片/字体、图标和状态标记 |
| 动作 | `button`、`iconButton`、`link`、`toggle`、`checkbox`、`radioGroup`、`slider` | 点击、开关、单选、范围调节和用户手势动作 |
| 文本与选择 | `textInput`、`textArea`、`searchBox`、`select`、`numberInput` | 便笺、提醒、搜索、有限选项和数值设置 |
| 集合 | `list`、`virtualList`、`gridList`、`virtualGrid`、`listItem` | RSS、日程、提醒、搜索结果和图片/快捷方式集合 |
| 状态 | `progressBar`、`progressRing`、`meter` | 确定/不确定进度、CPU/内存/电量等读数 |
| 数据图形 | `sparkline`、`lineChart`、`barChart`、`waveform`、`spectrum` | 系统历史、趋势、音频波形和频谱；只接收有界数列 |
| 日期 | `monthCalendar` | 月历、日期选择、今日/选中/有事件状态和 Calendar UIA 语义 |
| 宿主逻辑槽位 | `slotSurface`、`slotItem` | 自定义收藏夹、启动器和队列接收宿主项目引用，并获得原生拖放预览、重排和无障碍语义 |
| 宿主表面 | `tooltip`、`popover`、`contextMenu`、`panel`、`dialog` | 提示、元素菜单、日历/日程编辑和设置；内部内容仍使用同一 view 节点 |

当前实现进度（2026-08-15）：`view.scroll` 已覆盖最多 32 个宿主管理的纵向/横向
有界视口，统一保存偏移、钳制、滚轮、滚动条、绘制 clip 与交互 clip；
`view.collection.basic` 已覆盖非虚拟 `list/gridList/listItem`，限制 256 个稳定项，
每项有独立 action、hover、菜单目标与 listitem 语义；`view.collection.orientation` 已让普通
list 支持默认纵向与显式横向，并让固有尺寸、flex 分配和条目位置使用同一主轴；grid 与虚拟集合
原先保持行优先纵向模型。后续 `view.collection.virtual.orientation` 已把 fixed/variable virtualList
扩展到横向主轴，窗口布局、裁剪、滚轮、滚动条和按索引定位共用内容宽度与 columnGap；横向
可变宽度由 `view.collection.virtual.variableExtent` 共用测量缓存与首个可见项锚定，分组 sticky
header 和 virtualGrid orientation 继续拒绝。`view.collection.virtual` 进一步
提供固定行高 `virtualList/virtualGrid` 和 `view.virtualRange`，按实例滚动位置只实体化
最多 128 个连续项，宿主按全局 1-based 索引布局并校验窗口覆盖可见行；
`view.collection.virtual.variableExtent` 又允许 virtualList 以 estimatedItemSize 启动，成功 scene
后缓存最多 4096 个实测条目主轴尺寸，通过 layoutRevision 失效旧代缓存，并锚定首个可见项修正偏移；
`view.collection.virtual.stickyHeaders` 以最多 4096 个全局 section 索引补足虚拟分组标题，
`view.virtualRange` 返回当前活动标题，Lua 仅在标题早于连续窗口时额外实体化一个 listItem，宿主
按逻辑索引布局、测量、推挤并校验可见窗口；
`view.scroll.programmatic` 已补充 scrollTo/scrollBy 和 nearest/start/center/end 的
scrollToIndex；`view.scroll.initialTarget` 已补充 eager 后代 key 和 virtual 逻辑索引的首次定位，
`view.virtualRange` 接收同名 initialScrollIndex 以在首帧实体化正确窗口，loading 替代态不会提前
消费该初始目标。可操作的已实体化项
已进入通用键盘焦点序列；`view.collection.selection` 又加入父集合统一拥有的 none/single/
multiple 与受控 selectedKeys，条目 selectedStyle、指针/键盘建议事件及 UIA Selection 的
单选、多选、添加和移除使用同一状态来源。`view.collection.contentStates` 现以单个
emptyContent/loadingContent 节点承接 eager/virtual 空态与 busy 加载态，替代态复用同一布局、
命中和语义树，virtual 滚动范围归零；通用 `view.state.busy` 不隐式禁用输入或启动动画。
`view.collection.stickyHeaders` 允许纵向 eager list 的直接 listItem 作为分组标题，在最近纵向
scroll 内由下一个标题推挤并受 list 底部约束；固定后的绘制、命中、菜单和 UIA 几何仍来自同一 scene。
可变高度 virtualGrid、未实体化项的 VirtualizedItem 与 ScrollItem 仍未完成，因此本进度
不代表第 13.4 节集合全集完成。
`view.inputControls` 已一次覆盖 `textInput/textArea/searchBox/numberInput/select`：四类输入
复用宿主键盘、选择、剪贴板代理和 IME 编辑器，使用组件受控 value 与 change 建议值，支持
focus/blur/submit、提交模式、字节上限、数字有效性和方向键 step；select 的展开状态和选择值
同样受控，宿主在组件表面顶层绘制有界选项并返回 expansion/selection 建议。通用焦点遍历
已由 `view.keyboardNavigation.basic` 覆盖；锚定 popover 已实现，但仍缺 UIA Pattern、嵌套
popover 与跨辅助 surface 的焦点恢复，因此这里只发布细粒度 feature，不能把
五类控件计作第 13.4 节“控件契约完整”。
四类文本/数值输入现已支持 `readOnly`：仍保留焦点、选择、复制和 submit，但统一阻止键入、
IME、粘贴/剪切删除、退格/Delete、数值步进和 UIA SetValue；只读节点不再强制声明不会触发的
change 动作。`select` 的只读语义未混入本批次，继续使用受控 expanded/selection 或 enabled。
文本输入后续已通过 `view.input.selection` 开放受控选区：textInput/textArea/searchBox 使用
`{start, finish}` 表示从 0 开始、左闭右开的 UTF-8 字节范围，解析时拒绝越界和落在码点内部的
偏移，并与宿主内部 UTF-16 光标安全换算。声明选区必须绑定 selectionChange 且不能同时使用
selectAll；键盘/指针只移动选区时返回 previousSelection/selection，文本 change 同时返回结果
选区，组件仍负责写回 model。
五类输入/选择节点现已同时接入 `validationState/validationMessage/validationStyle`：状态枚举为
none/info/success/warning/error，宿主在未定制时提供可见状态边框，校验消息纳入文本配额并映射到
UI Automation HelpText。校验状态只负责受控呈现与语义，不隐式拦截 change 或替组件修改数据；
需要常驻可见说明时仍由组件添加相邻文本，避免仅以颜色传达错误。

布局主线新增 `view.grid.placement`：`grid/gridList` 的直接子节点可声明 1-based
`gridColumn/gridRow` 和 1 到 64 的 `columnSpan/rowSpan`，也可只约束一轴并由宿主按声明顺序
寻找首个空位。显式重叠、越过列边界或超出 64 行会在原子提交前拒绝；布局、绘制、命中和
网格位置语义共用同一 resolved frame。后续 `view.grid.tracks` 已允许 `grid/gridList` 使用
1–64 项 fixed/auto/fr/minmax 受限列轨与行轨，先满足固有内容和 min，再向固定 cap 与 fr 权重
分配剩余空间；额外隐式行保持 auto，`virtualGrid` 仍使用整数等宽列以保持固定范围计算。

同日后续实现已增加 `view.styledText.basic` 与 `view.monthCalendar`。前者提供 1–64 个
有界样式 span，并由单个 DirectWrite layout 完成颜色、字号、粗体、斜体、下划线、删除线、
换行和裁剪；后续 `view.styledText.actions` 又为带稳定 key 的 span 增加精确多片段命中、
hover/pressed、click、pointer/key 事件、tooltip、键盘焦点和元素级右键菜单；
`view.styledText.inlineIcons` 再允许 span 以宿主 Font Awesome/Fluent glyph 替代文本，并复用同一
DirectWrite 排版、状态颜色和命中范围。后者提供固定六周的
Gregorian 网格、可配置周起始、受控 ISO 日期选择、今日/相邻月份/事件状态样式，以及每个
日期独立的 hover、pressed、change 建议值和元素右键目标。两者已有解析、限额、布局、命中、
feature probe、LuaLS 和契约测试；日期单元已经进入通用键盘导航，UIA Text/Calendar Pattern
仍归 M6 完整性门禁，不能仅因节点可以绘制就宣称声明式视图已稳定。

同日逻辑槽位基础契约也已落地：v2 manifest 可声明最多 16 个 binding/collection，
宿主按实例持久保存 `reference` 模型并为 Lua 恢复可启动/打开的 opaque 引用；
`slots.binding/collection` 提供读取以及受可信用户动作约束的 bind/add/clear/remove/move，
`slotSurface/slotItem` 会校验 kind、revision、完整项目集合和顺序，伪造或陈旧 scene 不会覆盖
上一棵可用树。后续批次已加入固定的 `LuaLogicalSlot` 原生 surface、完整 slot contract
矩阵、桌面/Explorer 单对象 reference ingress、容量/替换策略命中、插入预览和
`slot.changed` 主动事件；宿主不会把路径、PIDL、`IDataObject` 或 DropRoute 交给 Lua。
后续又加入每实例 32 步、受可信动作约束且清空 redo 分支的 `slots.undo/redo` 事务历史，
以及槽位项独立右键菜单、宿主移除/前后重排和选中单组件时的 Ctrl+Z/Ctrl+Shift+Z/Ctrl+Y。
最新批次又接入受可信 action 约束的 `handle:pick()` 宿主选择器，复用快速导航的桌面、应用
和 Everything 索引并按 manifest `accepts` 过滤，选择后仍走原子持久化事务。原生同槽指针
重排和槽位项键盘焦点/移动已经接入；尚缺槽位项拖出、跨槽重排和 UIA 输出，因此 M6 槽位
退出条件仍未达成。
`view.logicalSlots.dropStyle` 也已进入机器契约：Lua 可用有界 `ViewStyle` 为已经通过宿主策略
校验的目标 surface 设置主题背景、边框和插入线颜色，桌面/Explorer 原生拖入与同槽指针重排
共用该样式；命中范围、插入位置、原生对象和拒绝态仍完全归宿主管理。
`view.logicalSlots.emptyContent` 随后补齐计划中的槽位空态属性：binding 与 collection 都可在
宿主快照确实没有项目时激活一个填满 surface 的可见节点，非空快照仍必须完整提交所有
`slotItem`，且逻辑槽位继续拒绝组件自行伪造 loading 状态。

节点规则：

- 兄弟节点 key 必须稳定且唯一；集合 item key 同时作为 diff、焦点恢复、选择和 AutomationId 的输入。
- 视图树有节点数、深度、文本长度、菜单深度、弹出表面和资源数量上限。
- 超过阈值的集合必须使用 `virtualList` / `virtualGrid`；虚拟化节点必须报告 item count、position 和稳定 key。
- 布局使用 DPI 无关设计单位，宿主负责像素转换和文本基线对齐。
- 组件通过 size class、宽高和环境变量选择内容，而不是依赖固定屏幕像素。
- 差量更新按节点类型、属性和稳定 key 计算，不为每次数据变化重建全部原生资源。
- `styledText` 只支持有界 span、图标和 action link，不解析 HTML/Markdown，也不加载远程内容。
- `panel` / `dialog` 是宿主管理的额外 surface，不是任意 HWND；遵守同一权限、焦点、IME、资源、主题和无障碍契约。
- `slotSurface` / `slotItem` 只声明宿主管理逻辑槽位的 scene geometry 和稳定引用，不向 Lua 暴露原生 `Container`、`Slot`、`IDataObject`、屏幕指针或 DropRoute。
- 控件默认 UIA role/pattern 由节点类型确定；作者只能补充或收窄语义，不能把 Button 伪装成 Text 来绕过键盘和动作要求。

### 13.5 公共属性族

所有属性进入机器可读 API contract，并声明类型、默认值、范围、适用节点、是否可动画、是否影响 layout/paint/hit-test/UIA。Lua 不接受任意 CSS 字符串或未知属性静默忽略。

| 属性族 | v2.0 属性 |
|---|---|
| 身份/诊断 | `key`、`debugName`、`testId`；key 参与 diff，`testId` 仅开发/测试可见 |
| 尺寸 | `width`、`height`、`minWidth`、`maxWidth`、`minHeight`、`maxHeight`、`aspectRatio`；值为有界设计单位或明确 `auto`/`fill` |
| 盒模型 | `margin`、`padding`、`gap`；支持 scalar、水平/垂直和四边结构，不解析 CSS shorthand 字符串 |
| Flex | `flexDirection`、`flexWrap`、`flexGrow`、`flexShrink`、`flexBasis`、`justifyContent`、`alignItems`、`alignContent`、`alignSelf` |
| Grid | `columns`、`rows`、`columnGap`、`rowGap`、`gridColumn`、`gridRow`、`columnSpan`、`rowSpan`；track 支持 fixed/auto/fr/minmax 的受限结构 |
| 位置/裁剪 | `offset`、`zIndex`、`clip`、`overflow`；绝对偏移只在 stack/overlay 语义下有效，不提供脱离可访问顺序的任意 visual order |
| 视觉 | `background`、`foreground`、`opacity`、`borderColor`、`borderWidth`、`cornerRadius`、`shadow`、`transform`、`cursor`；优先支持宿主题 token |
| 状态样式 | `style`、`hoverStyle`、`pressedStyle`、`focusStyle`、`disabledStyle`、`selectedStyle`、`checkedStyle`、`validationStyle` |
| 文本 | `text`、`font`、`fontSize`、`fontWeight`、`fontStyle`、`lineHeight`、`letterSpacing`、`textAlign`、`verticalAlign`、`textWrap`、`maxLines`、`overflowText`、`locale`、`textDirection` |
| 图片/图标 | `source`、`fit`、`alignment`、`interpolation`、`tint`、`alt`；source 必须是包资源或宿主受控句柄 |
| 通用状态 | `visible`（兼容）、`visibility`、`enabled`、`readOnly`、`required`、`focusable`、`tabIndex`、`selected`、`expanded`、`busy`、`validationState`、`validationMessage` |
| 值控件 | `value`、`checked`、`indeterminate`、`min`、`max`、`step`、`placeholder`、`selection`；按节点类型进行强类型约束 |
| 集合 | `orientation`、`selectionMode`、`selectedKeys`、`sticky`（eager 纵向 listItem）、`itemExtent` 或 `estimatedItemSize`、`layoutRevision`、`sectionHeaderIndices/stickyHeaderIndex`（virtualList）、`overscan`、`initialScrollKey`（scroll 后代）、`initialScrollIndex`（virtual）、`emptyContent`、`loadingContent` |
| 宿主槽位 | `binding` 或 `collection`、`reference`、`emptyContent`、`dropStyle`；slot ID/kind 来自 manifest，reference 只能来自对应宿主模型，不能用字符串伪造 |
| 事件 | `events` 中的 pointer/focus/key/click/doubleClick/contextMenu/change/submit/scrollEnd；值只能是序列化 action ID 和有界参数 |
| 提示/菜单 | `tooltip`、`contextMenu`、`accessKey`、`acceleratorText`；简单 tooltip 可为字符串，富 tooltip 和菜单使用有界描述结构，实际命令仍经过 action 与权限代理 |
| 动画 | `transition`、`enterTransition`、`exitTransition`；只允许白名单属性、时长和 easing，并自动遵守 `reducedMotion` |
| 无障碍 | `role`、`accessibilityLabel`、`accessibilityValue`、`accessibilityHint`、`labelledBy`、`describedBy`、`headingLevel`、`live`、`positionInSet`、`setSize`、`rowIndex`、`columnIndex`、`accessibilityHidden` |

属性规则：

- 无样式级联和选择器。父节点只传递文档明确列出的环境值，如 locale、direction、enabled 和主题 token；其余属性均显式。
- 逻辑 child 顺序同时决定默认绘制、Tab 和 UIA 顺序；v2.0 不提供类似 CSS `order` 的视觉重排，避免屏幕阅读顺序与画面不一致。
- `view.state.visibility` 已通过 `visible|hidden|collapsed` 明确定义显示状态：hidden 保留布局但整棵子树退出绘制、命中、宿主输入、逻辑槽位和 UIA，collapsed 不参与布局；旧 `visible=false` 固定等价于 collapsed，冲突的双重声明会拒绝新树。
- 受控输入的 `value/checked/selection` 来自 model，`change` 只报告建议值；宿主不会绕过组件状态直接持久化。
- 未知属性、错误枚举、NaN/Infinity、越界尺寸和不适用于该节点的属性在开发包中报错，在正式包中产生稳定诊断并拒绝该次 tree commit，不静默降级。

### 13.6 v2.x 延期与明确非目标

完成 v2.0 后再评估：`splitButton`、`segmentedControl`、`rating`、`datePicker`、`timePicker`、`colorPicker`、`expander`、`tabs`、`pager`、`tree`、`dataGrid`、拖放排序、富文本/Markdown 只读视图和 pan/zoom。

v2.0 明确不提供：浏览器/HTML 控件、视频播放器、富文本编辑器、Ink、任意 XAML/Win32 控件、任意 shader、应用窗口/标题栏/导航框架。文件、目录、日期等系统选择器应由权限化 task 或宿主 panel 提供，不作为可嵌套 view 节点伪造。

### 13.7 完整性门禁

控件清单只有同时满足以下条件才算 v2.0 完整：

1. 11 个内置组件全部只使用上述稳定节点或即时绘制 region，没有内置专用私有控件。
2. 每个节点和属性由 API contract 生成 LuaLS、文档、参数校验、默认值表和适用性测试。
3. 每个交互控件都有鼠标、触控、键盘、IME（如适用）、焦点、disabled/readOnly、RTL 和高对比度场景。
4. 每个语义控件映射正确的 UIA ControlType、必需/可选 Control Pattern、属性、事件和 tree structure，并通过 Narrator 与自动化客户端实际测试。
5. 每个控件覆盖 loading/empty/error/permission denied、不同 DPI、全部语言、长文本、极端值和节点热替换。
6. 节点数、集合虚拟化、动画、输入到呈现和内存门禁有基准；不能用“功能完整”掩盖不可接受的桌面常驻成本。
7. 新增节点必须说明为何不能由现有节点组合、UIA 语义和兼容策略；不得在 v2.0 冻结前不断追求 WinUI 全集。

M6 冻结前必须生成并评审一份机器可读的 **node-property applicability matrix**。矩阵逐节点列出：允许/必需/禁止属性、默认值、枚举和数值范围、子节点约束、事件负载、视觉状态、键盘操作、焦点行为、UIA ControlType/Pattern/属性/事件、RTL/本地化规则、可动画字段、资源与节点额度、预览降级和稳定错误码。上述表格是候选公开面，只有矩阵、生成物和测试三者一致后才成为冻结 API；不能把“表里出现了名称”等同于实现完整。

当前已建立第一层宿主契约表 `widget_view_contract`：44 个已公开节点的规范名称、类别、细粒度
feature、默认 accessibility role、允许属性和直接必需属性由同一份可枚举矩阵提供，Lua view
解析器已使用该矩阵拒绝未知或不适用属性，节点名称解析和默认 role 也不再各自维护副本。
五类子节点策略现也由节点契约直接登记：组合容器、单子节点、集合窗口、宿主逻辑槽位和叶节点；
叶节点的 `children` 在属性解析阶段即被拒绝，布局校验中的叶/集合分类复用同一契约。
原本会被解析后忽略的通用 typography、gap、容器对齐和后代裁剪字段也已按真实消费节点收窄；
作者写错节点时会拒绝 scene，不再出现“属性合法但没有效果”的假 API。
后续消费审计又拆开基础字号/对齐与高级排版，移除 divider 的无效 track/fill opacity，
停止向 select 暴露未被其宿主选项层读取的 checkedStyle，并按真实绘制路径区分图表的
thickness/trackOpacity。146 个公开属性现已进入同一份可枚举元数据，逐项登记 string、number、
length、resource、node、style、events 等语义值类型；跨节点保持一致的字号、透明度、尺寸约束、
网格位置、虚拟范围等标量也登记宿主数值上下限，属性名称枚举不再维护第二份清单。
Lua view 解析入口已直接消费这些通用标量范围，在构造节点前拒绝越界值；slider/numberInput
的 min/value/max 联动、fontWeight 离散步进等节点相关规则仍由场景校验层负责。string、enum、
boolean、number、integer 和 string-or-number 基础类型也由同一目录执行入口校验，不再沿用 Lua
把数字隐式转换为字符串、把数字字符串隐式转换为 number 的宽松路径；每种语义类型另有稳定工具名。
23 个 enum 属性的完整允许值集合现也由该目录登记并在入口执行，节点类型值直接复用 44 节点目录，
不再由各专用解析函数单独承担拼写白名单。
其余 length、edge-insets、resource、color、array、node、style、transition、tooltip、accessibility、
events 和 action 等结构类型也已接入同一入口外形校验，再由专用解析器检查内部字段、额度和关系。
全部属性现进一步登记 layout、paint、hit-test、input、accessibility、resource 和 tree 影响域位集；
该列描述属性变化需要失效的宿主子系统，为后续差量 scene 更新提供契约，而不是把动画能力或
是否允许该属性混入同一个布尔值。
逐节点属性默认值现也可从契约查询：必填、不可用、固定 Lua 表达式和条件默认分开表示；badge/input
padding、radioGroup gap、styledText 文本流、scroll/list 方向等解析器特例已经进入查询，divider
方向尺寸、数据图自动范围、逻辑槽位二选一和 virtualList 尺寸二选一不会被误报为固定默认。
17 个公开节点事件也已进入同一契约源，登记统一 payload 类别和逐节点适用性；Lua 解析器、
scene 校验与契约测试不再分别维护 change、selectionChange、输入生命周期和 scrollEnd 的类型白名单。
当前已进一步为每个语义节点登记 UIA ControlType、基础 Pattern 和是否参与宿主键盘焦点，并可从
布局后的 view tree 生成保持父子关系、裁剪/offscreen、enabled/focused 和受控值状态的只读语义
快照。桌面窗口已响应 `WM_GETOBJECT`，通过 Windows UIA Fragment Provider 暴露当前组件与元素的
名称、ControlType、AutomationId、RuntimeId、边界、可见性、启用/焦点状态、父子/兄弟导航、
点命中和焦点定位；窗口销毁或 Explorer 重建时旧 Provider 会失效，避免继续访问宿主旧状态。
Invoke、Toggle、RangeValue、Value、ExpandCollapse 和 SelectionItem Pattern 已连接到现有
interaction region 与宿主输入动作通道，Scroll Pattern 则复用宿主滚动状态；辅助技术触发的
action 使用 `source="accessibility"`，
且不会获得 trusted gesture 身份，因此不会绕过已有权限和用户手势门槛。成功桌面帧现在会按
稳定语义 ID 差分并发送结构、焦点、边界、名称、启用、离屏、开关、选择、RangeValue、Value、
展开和滚动状态 UIA 变化，未变化的帧不广播。已进一步把 `radioGroup` 选项、展开 `select` 选项和
`monthCalendar` 日期生成为稳定
SelectionItem 子元素，并为父控件实现 Selection Pattern；任意虚拟化集合的按需子项协议仍未
接通。滚动容器现提供单轴 Scroll Pattern，网格和已实体化单元提供零基 Grid/GridItem；
`view.accessibility.metadata` 与 `interaction.accessibility.metadata` 已进一步开放 value/hint、
heading/live、集合位置和语义隐藏；声明式节点还支持稳定 key 的 labelledBy/describedBy 关系与
一基 grid 行列覆盖。宿主将这些字段映射为 AriaRole、ItemStatus、HelpText、LabeledBy、
DescribedBy、HeadingLevel、LiveSetting、PositionInSet、SizeOfSet 和 GridItem，并为 live 内容变化
发送 LiveRegionChanged；加载/提交阶段拒绝无名或自身关系、越界索引以及隐藏交互子树。
ScrollItem、未实体化项的 VirtualizedItem、真实 Narrator 场景验收以及逐规则细粒度诊断仍未完成；
RTL、逐属性动画映射和稳定的视图管线阶段码已经迁入机器契约，
也尚未由它生成 LuaLS 与本文档，因此这仍不表示契约已经冻结或完整无障碍已经可用。

`snowwidget view-contract` 现已把该运行时目录导出为带 `schemaVersion` 的 JSON：除 44 个节点、
146 个属性和 17 个事件外，每个节点还以 closed-world 策略明确导出允许与禁止属性，两者严格
划分完整公共目录；还直接读取 `ViewTreeLimits` 导出全树、文本、资源、图表、集合和
虚拟化额度；动画部分公开更新 transition 的六种允许属性、入退场的 opacity/transform 字段、
1–2000 ms、四种 easing 和 1–4 项唯一属性约束，并明确宿主驱动、不逐帧执行 Lua、预览与
`reducedMotion` 落到最终状态。预览部分同时登记宿主渲染/校验和隔离存储覆盖层。schema 2
又为每个属性登记 visual/transform/layout 过渡影响，并导出 auto/ltr/rtl 解析、方向感知对齐、
声明顺序规则以及候选树原子拒绝/保留上一成功树的行为。schema 3 进一步导出十个稳定的
视图管线阶段码，并让 desktop 与辅助 surface 的运行时诊断统一使用 `[code] message`；工具可按
阶段码分类，后续仍可在不破坏阶段分类的前提下增加节点/属性规则子码。
当前导出不能视为 M6 已冻结。

即时绘制的 `interaction.region` 现在也会按 role、label、受控状态、形状/clip 和最后提交的宿主
焦点转换为同一种语义节点；`WidgetEngine` 只汇总当前可见、有效、非预览的 v2 实例，并保留
组件身份、边界、选中状态和采集错误。这样后续 UIA Provider 只消费一个实例快照面，不需要
为 `view()` 与 `render()` 维护两套 Windows 无障碍实现。

### 13.8 实时更新与交互状态

声明式视图不是静态快照。宿主维护每个稳定 key 节点的 `hovered`、`pressed`、`focused`、`disabled`、`selected` 和 pointer capture 状态，并在输入、组件 state、数据订阅或调度变化后进行差量更新。

```lua
view.row({
    key = "feed:" .. item.id,
    hoverStyle = { background = 0x2E3640 },
    pressedStyle = { background = 0x3A4654 },
    focusStyle = { borderColor = 0xFFFFFF, borderWidth = 1 },
    transition = {
        durationMs = 120,
        properties = { "background", "opacity" }
    },
    children = { ... }
})
```

规则：

- hover、pressed 和 focus 的纯视觉样式由宿主命中测试与动画器直接更新，不需要先进入 Lua，因此指针反馈可以在下一次可用呈现中出现。当前 desktop 与 panel/dialog/popover 的声明式 surface 在没有绑定对应 pointer action 时会标记已提交 scene 快速帧，并跳过通用 `event.kind="pointer"`；即时绘制 surface 继续接收原始指针生命周期以便自行绘制。
- `focusStyle` 与 `disabledStyle` 已进入公共属性矩阵、Lua 解析和 Direct2D 渲染；未声明 focus 样式时宿主提供默认可见轮廓，disabled 样式最后覆盖其他状态样式。
- `validationState/validationMessage/validationStyle` 已进入输入和 select 的公共属性矩阵；校验样式在 pressed 后、focus/disabled 前叠加，消息同时进入语义 HelpText，但不会改变受控值提交规则。
- `view.layout.constraints` 已把 `minWidth/maxWidth/minHeight/maxHeight/aspectRatio` 纳入公共属性矩阵、Lua 解析、固有尺寸和各容器布局；尺寸使用 0–4096 的有限逻辑单位，宽高比使用 0.01–100，并拒绝同轴上下限、宽高比约束或双固定尺寸互相冲突的树。
- `view.layout.constraints` 的首批盒模型已加入 0–4096 的统一 `margin`：父布局在节点 frame 外保留空间，线性布局、grid、flow、stack、scroll content extent 与虚拟 item 都使用同一外尺寸模型。
- `view.layout.edgeInsets` 已将 `margin/padding` 扩展为 scalar、`horizontal/vertical` 与 `top/right/bottom/left` 结构；轴值先展开、显式边值覆盖，四边值进入固有尺寸、全部容器布局、滚动范围、文本/图片内容区和宿主输入命中。仍不解析 CSS shorthand 字符串，也不支持负边距。
- `view.positioning.basic` 已为 stack 直接子节点加入有界 `offset{x,y}` 与稳定 `zIndex`，绘制和命中使用同一排序而语义顺序保持声明顺序；容器 `clip=true` 同时约束后代绘制、命中、宿主输入和语义可见范围。任意 absolute 布局、裁剪路径与跨容器视觉重排仍不开放。
- `view.layout.overflow` 现以 `overflow=visible|clip` 正式承接容器后代溢出策略，旧 `clip` 仅作一致性兼容入口；`view.shadow` 使用与即时绘制相同的最多 16 层有界衰减模型，`view.image.tint` 通过宿主 ColorMatrix 保留源 alpha 并替换 RGB。阴影不改变布局/命中。
- `view.theme.tokens` 已公开 `widgetBackground/surface/surfaceVariant`、三档文本、两档边框、系统强调色及其前景色和四种状态色。style/全部状态 style、styledText span、shadow color 与 image tint 共用同一解析器；宿主在状态叠加后、transition 前按组件主题解析，高对比度改用 Windows 系统色，未知 token 原子拒绝。即时绘制仍只接受显式 RGB；实际深浅主题切换、高对比度和辅助 surface 场景待验证。
- `view.transform.basic` 已加入布局后的 `translateX/translateY`、正数统一 `scale` 和归一化 `originX/originY`；`view.transform.affine` 又加入正数 `scaleX/scaleY` 乘数、-360–360 度 `rotate` 与各 -80–80 度 `skewX/skewY`，固定顺序为 scale→skew→rotate→translate。单节点最终轴限制为 0.05–8，嵌套仿射矩阵以奇异值限制累计伸缩为 1/64–64；Direct2D 直接消费 scene 的同一局部矩阵，元素命中通过逆矩阵保持 roundedRect/circle/文本片段精度，slider 通过变换后的轴向量解析值，UIA 使用四角包围框。宿主管理输入、scroll 和逻辑槽位以及执行裁剪的节点仍要求正向轴对齐矩阵，透视不开放；当前真实桌面绘制、命中和辅助技术场景待验证。
- `view.transition.visual` 已公开节点级 `background/foreground/borderColor/opacity` 过渡，`view.transition.transform` 又加入平移、缩放、原点、最短路径旋转和斜切插值，`view.transition.layout` 再为稳定 key 节点加入父布局内相对位置与尺寸的呈现过渡；每个更新描述符仍限制为 1–4 个唯一属性、1–2000 ms 与 linear/easeIn/easeOut/easeInOut。`view.transition.enter` 另以 `enterTransition` 公开有界起始 opacity/完整 transform，只对 surface 已有成功 scene 后首次出现的新 key 生效，初始整树不会集体入场；`view.transition.exit` 以同型 `exitTransition` 端点保留被移除节点的不可交互旧子树快照，继承旧父变换/裁剪并在每 surface 最多保留 512 个节点，重新出现的 key 会取消旧快照。宿主在桌面及辅助 surface 上复用统一 16 ms 计时器，并直接重绘上一棵成功树，不在每个插值帧重复执行 Lua `view()`；预览、无计时器和 `reducedMotion` 直接落到最终样式。颜色端点任一未显式声明时切换而不插值，缺失 transform 按单位变换处理；布局坐标在施加滚动偏移前以父相对形式捕获，因此滚动和虚拟窗口平移不触发布局动画。命中、裁剪、宿主控件和 UIA 几何在新 scene 提交时使用目标矩阵，不暴露插值中间几何，退场快照也不保留动作或语义。实际 hover、面板、重排、入退场与减少动态效果场景待验证。
- `view.flex.sizing` 现已为 row/column/list 子项补齐 `flexBasis/flexGrow/flexShrink`：basis 先参与外尺寸求解，正空间按 grow 分配，溢出按 shrink×basis 迭代收缩并在命中 min 约束后重新分配；`fill` 保留隐式 grow=1。`view.flex.layout` 已为 row/column 补齐 row/rowReverse/column/columnReverse 主轴、noWrap/wrap/wrapReverse，以及 start/center/end/stretch/spaceBetween/spaceAround/spaceEvenly 多行对齐；每行独立执行 sizing/justify，逻辑绘制、命中、键盘和 UIA 顺序不随视觉反转。
- `view.text.flow` 现已让普通 text、label 节点与 styledText 共用 `textWrap/maxLines/overflowText/verticalAlign` 的 DirectWrite layout 规则；普通文本默认 noWrap+ellipsis，styledText 默认 wrap+clip，行数限制为 0（无限）到 64。
- `view.text.typography` 现已补入 100–900 的 `fontWeight`、normal/italic `fontStyle`、1–1024 `lineHeight` 与 -64–256 `letterSpacing`；行高参与固有高度和 DirectWrite 行距，字距参与近似固有宽度和 TextLayout1 字符间距；宿主编辑器排版不由本 feature 暗示。
- `view.text.locale` 已为文本、标签、输入和 select 加入有界 BCP 47 `locale` 与 auto/ltr/rtl `textDirection`：auto 使用首个强方向字符并以 locale 兜底，DirectWrite shaping、start/end 对齐及 select/radio/checkbox/toggle 的控件相对位置共用同一方向；声明、Tab 和 UIA 顺序不反转。
- `view.tooltip` 现已提供所有节点通用的有界字符串提示，`view.tooltip.rich` 进一步提供 256/4096 字节上限的 `{title?,text}`：tooltip-only 节点也生成裁剪命中区，宿主以强调标题和普通正文统一绘制并限制在组件 surface 内，同时在无 validationMessage 时把两段文本映射为 UIA HelpText；markup、任意窗口和把必要信息仅藏在 hover 中仍不允许。
- `view.pointer.events` 已补通普通声明式节点此前在 LuaLS 与交互运行时之间断裂的 `pointerMove/wheel`：只有显式绑定才进入 Lua，宿主滚动先更新偏移且仍向 scroll/virtual collection 自身投递可信 wheel action，动作不能取消滚动。即时 `interaction.region` 同时通过 `interaction.tooltip/interaction.tooltip.rich/keyboard` 开放同型字符串或标题+正文提示、focusable/tabIndex、`isFocused` 和配对 keyDown/keyUp；真实触控板、高频合帧与辅助 surface 场景待验证。
- `view.scroll.events` 已把 `events.scrollEnd` 限定到 scroll/virtual collection；滚轮或 UIA 操作从末端前到达最大宿主偏移时只投递一次，离开末端后才能再次触发，UIA 来源不获得可信手势。
- `view.scroll.initialTarget` 已为新出现的稳定容器 key 加入一次性 nearest 初始定位：普通 scroll 解析可见后代 key，fixed/variable virtual 解析 1-based 逻辑索引，`view.virtualRange` 用同一索引生成首帧窗口；成功 scene 才提交宿主偏移，失败事务和 loading 替代态不会提前消费目标，已有用户/脚本位置优先。
- `view.collection.virtual.variableExtent` 已让纵向/横向 virtualList 以 estimatedItemSize 生成首帧范围，并在成功 scene 后缓存最多 4096 个 1-based 实测主轴尺寸；itemCount/estimate/主轴 gap/orientation/layoutRevision 共同界定缓存代，测量变化以原首个可见项为锚点修正宿主偏移后触发合并重绘。virtualGrid 继续要求固定行高，未完成的项目级 UIA 虚拟化不由本 feature 暗示。
- `view.collection.stickyHeaders` 已让纵向 eager list 的直接 listItem 以 sticky=true 固定在最近纵向 scroll 顶部，由下一标题推挤并在所属 list 底部退出；宿主在滚动状态应用阶段移动整棵标题子树，并让呈现中的 sticky 项排在普通兄弟之后绘制/命中。虚拟 section 索引和横向 sticky 不由本 feature 暗示。
- `view.collection.virtual.stickyHeaders` 已为 fixed/variable virtualList 增加最多 4096 个有序唯一的 1-based section 索引；`view.virtualRange` 根据零 overscan 可见窗口返回活动 `stickyHeaderIndex`，Lua 只在其早于连续窗口时前置一个额外标题，宿主以显式逻辑索引完成布局、变高测量、窗口覆盖校验、下一标题推挤和统一绘制/命中。virtualGrid 与横向 sticky 仍不在本 feature 内。
- `view.styledText.inlineIcons` 已为 styledText span 增加与 text 互斥的 `glyph/iconFont`，把宿主内嵌 Font Awesome/Fluent collection 按 range 应用于同一 DirectWrite layout；图标沿用字号、颜色、hover/pressed、精确命中和 action 路由，带 key 的图标必须提供可读 label，任意 inline 图片/HTML/Markdown 仍不开放。
- `view.identity.diagnostics` 已为全部声明式节点加入有界 `debugName/testId`；成功场景可在设置页复制诊断中按 desktop/辅助 surface 导出类型、key、深度、最终 frame 和这两项开发标识。它们不进入 diff、布局、绘制、命中、焦点或 UIA AutomationId，组件行为身份继续只由全局唯一 key 承担。
- `view.keyboardNavigation.order` 已加入 `focusable/tabIndex`：-1 只退出顺序遍历，正数先按升序、再接默认 0 的声明顺序；焦点样式、鼠标焦点、键盘遍历和 UIA IsKeyboardFocusable 使用同一有效状态。
- `view.keyboard.accessKey` 已为单一直接交互目标加入树内唯一的 ASCII 字母/数字访问键；活动辅助 surface 或唯一选中的桌面组件用 Alt+键聚焦输入/slider，并按既有受控 click/change 语义激活其他目标，重复按下不重复触发。Windows 的 `WM_SYSCHAR` 仅在命中时消费，未命中的 Alt 组合键、Alt+Space 和 Alt+F4 继续交给默认窗口过程。UIA 同时公开规范化 AccessKey 和仅作语义描述、不注册全局热键的 acceleratorText；radioGroup/monthCalendar 等多虚拟目标节点不接受父级访问键。真实键盘布局、Alt 系统键与 Narrator 场景仍待现场验证。
- `view.keyboard.events` 已为可聚焦节点加入 keyDown/keyUp 观察：事件包含稳定符号键名、Windows virtual key、重复与修饰键状态，输入代理与桌面窗口走同一入口；按下目标用于配对释放，窗口失焦清理。事件不提供取消返回值，宿主激活与管理快捷键继续执行，字符/IME 仍只走输入控件。
- `view.focus.request` 已将可信手势内的 `control.focus(key)` 从文本输入扩展到最后一棵成功树中的任意启用、可聚焦元素，并同步逻辑槽位焦点；动作中新加入的目标仍只延迟到同 surface 下一次成功提交，失败后立即清理，不允许 render/schedule/异步完成抢焦点。
- `view.state.visibility` 已加入显式 visible/hidden/collapsed：hidden 仍保留父布局空间，但绘制、后代命中、宿主输入、逻辑槽位和 UIA 语义都从同一棵提交树中省略；collapsed 继续复用旧 `visible=false` 的不占位语义，透明度为 0 不等同隐藏。
- `view.input.selection` 已把 textInput/textArea/searchBox 的受控 selection 纳入公共属性与动作矩阵：公共偏移使用 UTF-8 字节码点边界，宿主编辑器内部转换为 UTF-16；selectionChange 与文本 change 返回同一结果范围，`selectAll` 只保留为互斥的一次性兼容入口。
- 只有组件绑定了业务事件时才调用 Lua；状态更新、多个订阅通知和同一帧内的重复 `invalidate` 合并为至多一次 `view()` 求值和一次 scene diff。
- 布局、绘制、命中区域和 UI Automation 边界来自同一棵提交成功的 scene tree；不允许视觉已经变化而点击仍指向旧树。
- 声明式 transition 由宿主运行；`view.transition.visual` 只允许显式列出的颜色和透明度，`view.transition.transform` 允许显式 transform 呈现过渡，`view.transition.layout` 允许稳定 key 节点的父相对位置与尺寸过渡，`view.transition.enter/exit` 允许新增/移除 key 使用有界 opacity/完整 transform 端点。组件不能靠无条件 Lua 逐帧循环模拟动画。
- 即时绘制组件已可通过 `animation.requestFrame(id)` 请求下一帧，收到包含单调时钟 `now` 和 `deltaMs` 的 frame 事件；同帧同 ID 合并，每实例最多 16 个待处理 ID，不再次请求就自动停止，禁止永久隐式 60 FPS 循环。`animation.cancelFrame(id)` 可显式取消待处理请求。
- 当前实现会在组件隐藏、卸载、热重载和宿主关闭时停止并清空 frame 请求；恢复后首帧 delta 归零，不补绘所有错过帧。系统休眠/恢复的真实桌面场景仍列入第 18.5 节验证门禁。
- `reducedMotion` 下宿主关闭非必要 transition，组件不得自行用逐帧回调绕过用户设置。
- “实时”表示输入或状态变化驱动的下一帧更新，不承诺硬实时或始终满刷新率；繁忙系统下允许合帧，但不能阻塞桌面 UI 线程等待 Lua、文件或网络。

### 13.9 环境上下文

```lua
local ctx = widget.context()
-- size.width, size.height, columns, rows, sizeClass
-- dpi, scale, cellWidth, cellHeight, cellGap
-- colorScheme, contrast, reducedMotion
-- locale, timeZone
-- visible, preview, surface
-- selected, focused
```

环境改变产生统一事件；内置组件迁移时将原尺寸、语言和可见性回调改为该事件，不在发布运行时保留旧回调分发。

## 14. 动作、输入与无障碍

### 14.1 动作

声明式控件只绑定序列化动作标识和参数，不直接保存 Lua 闭包：

```lua
view.button({
    key = "refresh",
    label = "Refresh",
    action = { id = "refresh", value = { source = "manual" } }
})
```

动作通过 `event.kind == "action"` 投递。宿主可以据此统一实施：

- 用户手势证明；
- 权限检查；
- 防重复提交；
- 键盘和屏幕阅读器等价操作；
- 动作完成后的最小范围刷新。

### 14.2 输入

- 任意可命中的声明式节点都可以绑定 `pointerEnter`、`pointerLeave`、`pointerDown`、`pointerUp`、`click`、`doubleClick`、`wheel`、`contextMenu`、`focus` 和 `blur`；`pointerMove`、`keyDown`、`keyUp` 等高频事件必须显式订阅。当前 `view.keyboard.events` 已实现聚焦节点的 keyDown/keyUp，且不允许取消宿主默认行为。
- 绑定值是序列化的动作 ID 和小型参数，不保存 Lua 闭包。事件统一携带 `targetKey`、祖先 key path、局部/组件坐标、设备类型、pointer ID、按钮、修饰键、点击次数和可信用户手势标识。
- `click` 由宿主在同一有效节点上的 down/up、移动阈值和捕获状态合成，组件不得用两个裸事件自行猜测点击。
- 事件以最深命中节点为 target，再沿 scene tree 向上查找已绑定处理；enter/leave 不冒泡。处理结果可以标记 handled，但不能阻止 SnowDesktop 的安全和管理快捷入口。
- 同一帧内的 `pointerMove` 和 wheel 可以合并；down/up/click、焦点、菜单请求和按键不能被合并或乱序。
- 只有获得焦点的组件接收键盘输入。
- 焦点移动继续由宿主 Tab/方向键完成；`view.focus.request` 已提供可信动作中的显式焦点请求并支持下一次成功提交解析，焦点可见性由宿主轮廓保证。
- 文本输入继续使用宿主 IME 管线，不由组件创建原生窗口。
- 受控文本选区使用 `{start, finish}` 的 UTF-8 字节半开区间；宿主在键盘或指针只移动选区时投递 `selectionChange`，在文本 change 中附带结果选区，组件写回后才成为下一棵树的权威状态。
- 指针捕获绑定到 `instanceId + nodeKey + pointerId`，必须有超时，并在节点移除、隐藏、失焦、权限撤销和卸载时自动释放。

当前主指针实现通过 `interaction.pointerCapture` 为声明式节点和即时 region 提供显式
`capturePointer=true`，要求绑定 move/up 动作；宿主保留 hover 命中但把 move/up 路由给按下 key，
移出后不合成 click。目标删除、surface 关闭、窗口失焦、系统捕获取消、实例停用以及 30 秒上限
都会结束捕获；多触点 pointerId 和触控长按仍属于后续输入批次。

示例：

```lua
view.row({
    key = "item:" .. item.id,
    hoverStyle = { background = 0x2E3640 },
    events = {
        click = { id = "item.open", value = { itemId = item.id } },
        doubleClick = { id = "item.openDetails", value = { itemId = item.id } },
        contextMenu = { id = "item.menu", value = { itemId = item.id } }
    },
    children = { ... }
})
```

### 14.3 元素级右键菜单

每个元素可以拥有独立菜单，不需要组件自己绘制弹窗。鼠标右键、触控长按、键盘 Menu 键和 `Shift+F10` 统一产生 `contextMenuRequested`：

```lua
return widget.define({
    menu = function(ctx, model, request)
        if request.id ~= "item.menu" then return nil end
        local item = model.items:get(request.value.itemId)
        return ui.menu({
            { id = "item.open", label = l10n.t("open"), enabled = item ~= nil },
            { id = "item.pin", label = l10n.t("pin"), checked = item and item.pinned },
            { type = "separator" },
            { id = "item.delete", label = l10n.t("delete"), role = "destructive" }
        })
    end
})
```

规则：

- 宿主从最深命中元素开始查找 `contextMenu`，未绑定时依次回退到祖先、组件默认菜单和 SnowDesktop 管理菜单。
- 请求包含元素稳定 key 和声明时绑定的小型值；组件不能收到或伪造宿主指针、窗口句柄和绝对屏幕对象。
- menu 允许 enabled、checked、radio、separator、受控二级 submenu、快捷键提示和包内图标；条目 ID 在该次菜单内必须唯一。
- 菜单模型必须同步、快速且无 I/O；动态数据应事先在 model 中。超过 Lua 回调预算或返回非法结构时，显示宿主管理菜单并记录诊断。
- 选择条目后发送普通 `action` 事件，包含 `source="contextMenu"`、`targetKey` 和用户手势证明；删除、启动、媒体控制等动作仍经过权限代理。
- 菜单打开期间 scene tree 可以更新；动作按实例 ID、菜单 session、target key 和绑定值校验。目标节点或实例已失效时丢弃动作，不能误作用于复用 key 的新数据。
- 组件菜单不能隐藏设置、授权、诊断和移除组件的宿主管理入口；至少通过固定菜单区或宿主保留手势始终可达。
- 菜单由宿主原生 UI 呈现，自动获得 DPI、主题、键盘、Narrator 和屏幕边缘定位能力。

### 14.4 无障碍

声明式节点至少支持：

- `accessibilityLabel`
- `accessibilityValue`
- `accessibilityHint`
- `role`
- `headingLevel`
- `disabled`
- `selected`
- 可执行动作列表

即时绘制组件可以提交独立语义树；如果存在可交互区域但没有语义，作者工具必须警告。宿主负责把语义树映射为 Windows UI Automation。

发布门禁包括键盘导航、高对比度、200% DPI、减少动态效果和 Windows Narrator 实际验证。

### 14.5 Lua 逻辑槽位与拖放容器

Lua 组件需要开放槽位能力，否则第三方只能制作展示型仪表盘，无法实现收藏夹、自定义启动器、稍后处理队列和拖放工作流。但开放的是 **宿主管理的逻辑引用槽位**，不是当前 C++ `Container` / `Slot` 实现本身。

三类“槽位”必须分开处理：

| 槽位类型 | 是否开放 | 边界 |
|---|---|---|
| 桌面网格放置槽位 | 是 | 包只声明 default/min/max grid span 和可调整性；具体 page/row/column、碰撞处理和恢复由宿主决定 |
| Lua 组件内部的项目槽位 | 是，v2.0 受限开放 | 开放宿主管理的 0..1 binding 和 0..N collection、drop preview、replace/reorder/remove 及 opaque item reference |
| Dock 专属槽位、CollectionGroup/FileGroup 结构槽位和宿主保留槽位 | 否 | 不能由包声明、占用、嵌套或模拟；将来若开放 Lua 组件进 Dock，使用独立宿主布局契约 |

v2.0 包在 manifest 中静态声明槽位，使宿主在创建 Lua VM 前就能验证基数、接受类型、容量、替换策略和操作语义。`operation` 只描述源对象副作用；`kind` 决定这是单项绑定还是多项集合，两者不能混为一个字段：

```json
"slots": {
  "primaryApp": {
    "kind": "binding",
    "accepts": ["app.reference"],
    "operation": "reference",
    "replacePolicy": "allow",
    "allowClear": true
  },
  "favorites": {
    "kind": "collection",
    "accepts": [
      "desktop.item",
      "app.reference",
      "filesystem.reference"
    ],
    "operation": "reference",
    "capacity": 64
  }
}
```

`binding` 表示 0..1 个宿主引用，适用于应用大图标、固定文件/文件夹、播放器目标和单一设备卡片。已有引用时再次拖入，不是“集合已满”，而是根据 `replacePolicy` 显示“替换为 …”预览并原子替换；旧引用进入宿主 undo 记录，原应用或文件本身不受影响。

```lua
local primary = slots.binding("primaryApp")
local app = primary:item()

view.slotSurface({
    key = "primary-app",
    binding = "primaryApp",
    child = app and view.slotItem({
        key = app.id,
        reference = app.reference,
        child = view.referenceIcon({
            key = app.id .. ".icon",
            reference = app.reference,
            alt = app.title
        })
    }) or view.text({ text = "拖入一个应用" })
})
```

`collection` 表示 0..N 个宿主引用，适用于收藏夹、队列和自定义启动器。Lua 读取宿主模型并用声明式节点报告可放置区域和每个项目的 scene geometry：

```lua
local favorites = slots.collection("favorites")
local children = {}

for _, item in ipairs(favorites:items()) do
    children[#children + 1] = view.slotItem({
        key = item.id,
        reference = item.reference,
        child = renderFavorite(item)
    })
end

view.slotSurface({
    key = "favorites",
    collection = "favorites",
    children = children
})
```

v2.0 契约：

- 固定增加一个宿主 `LuaLogicalSlot` surface，同时承载 binding/collection 两种宿主模型，并进入现有 slot surface × payload × relation 全有向矩阵；包不能注册自己的 `SlotSurfaceKind`、`DragPayloadKind` 或 `DropRoute`。
- `slots.binding(id)` 和 `slots.collection(id)` 是不同的强类型句柄；manifest kind、Lua accessor 和 scene node 的 `binding/collection` 字段不一致时拒绝 tree commit，不能自动互转。
- binding 默认 `replacePolicy="allow"`，只因当前明确 drop/host picker 用户手势而替换；`reject` 可把已占用 binding 设为不可替换。v2.0 不允许 Lua 在拖拽热路径动态决定，避免预览与提交不一致。
- binding 必须支持 empty/bound/unavailable 三态。应用卸载、文件移动或目标暂时不可用时保留 opaque reference 并报告状态，不能静默绑定到同名新目标；用户可重新选择或清除。
- 空 binding 可以通过宿主 `slots.choose(id)` 选择器绑定，已绑定时也可用它替换；选择器只返回用户选中的引用，不把完整应用/桌面/文件目录暴露给 Lua。组件若自行实现应用搜索界面，仍需要 `app.discovery`。
- 首版只提供 `operation="reference"`：拖入表示“添加引用”，原桌面项或文件不被移动、复制、删除或改名；预览必须明确显示复制/引用语义，不能让用户误以为发生文件移动。
- 首版允许接收单个用户明确拖入的 desktop item、application reference 和 external file reference，并支持同一集合内 reorder、remove reference、键盘移动和撤销；不接受 widget、collection/file-group label、文件夹映射子项或任意混合载荷。
- `slots.binding(id)` / `slots.collection(id)` 返回宿主持久化、类型化、事务化且绑定当前实例的模型。Lua 只能取得 opaque reference、kind、display title、受限 metadata、icon resource handle 和 availability，不能取得 Shell path、PIDL、`IDataObject` 或原生句柄。
- 用户拖入一个对象只授权该引用进入集合，不等于授予 `desktop.read` 全量枚举、文件内容读取、Shell 启动或任意路径权限。打开/定位引用仍走 `app.launch`、`desktop.action`、`shell.launch` 或用户文件句柄能力。
- 宿主根据 manifest 静态过滤和已提交 scene geometry 完成命中、插入指示、容量检查与原子提交；拖拽热路径不调用 Lua，不允许 Lua 用超时或异常让 Shell drag session 卡死。
- 提交完成后发送 `slot.changed` 事件，包含 slot ID、kind、revision 和 bound/replaced/cleared/added/removed/moved 的 opaque item IDs；Lua 重新求值 view，但不能反向伪造宿主提交。
- scene 更新时 `slotItem` 的 stable key/reference 必须与宿主 collection revision 对应；漏项、重复项、伪造引用或越权集合 ID 使该次 tree commit 失败，而不是丢失宿主数据。
- 每集合和每实例有容量、图标解码、事件合并、标题长度和存储总量上限；包卸载、实例删除和数据清除遵守统一恢复/备份策略。
- 即时绘制模式若要使用槽位，必须提交与 `interaction.region()` 类似的稳定 slot region set；不能自行解析原生拖放对象或绕过声明式契约。

清除 binding、移除 collection item 和调用 `slots.choose()` 必须来自可信用户 action；Lua 不能在 timer、data event 或 `view()` 中偷偷改掉用户绑定。绑定后的点击启动仍需 `app.launch` 和当前点击手势，拥有一个 app reference 不自动授予启动权限。

v2.0 不开放：真实文件 move/copy/delete、组件嵌套、把 Lua 组件变成 CollectionGroup/FileGroup 子组件、跨 Lua 实例转移、Dock 专属/保留槽位，以及 package-defined drop route。真实文件转移属于 `filesystem.userSelected.write` 和逐动作确认后的 v2.x 能力；Lua 组件整体能否进入 Dock 是独立的宿主布局决策，不能通过内部槽位 API 间接实现。

## 15. 配置、资源与模块

### 15.1 声明式配置

在现有 text/bool/int/float/select/color 基础上增加：

- `password` / secret reference
- `url`、`date`、`time`（已实现严格格式校验和设置页无效草稿隔离）
- `range`（已实现 min/max/step 吸附及 number 类型默认、preset、持久化）
- `multiSelect`（已实现 1-64 稳定选项、本地化标签及 string[] 类型默认、preset、持久化）
- `fileHandle`、`folderHandle`（已实现设置页系统选择器、独立 opaque 授权、替换/清除撤销及只读 storage 投影）
- `appReference`、`desktopItemReference`、`fileReference`、`folderReference` 动态 entity selector；可以直接写入第 14.5 节 binding，而不是把 path/AUMID 当普通字符串
- 分组、说明（已实现有界字段说明、最多 32 个顺序分组及可选折叠）
- 校验、依赖和 `showWhen`（已实现本地化错误提示、Unicode 长度约束、条件显隐/禁用、值保留及静态循环检查）

普通配置值进入类型化实例存储；secret、file/folder handle 和宿主 item/app reference 只保存绑定/授权记录，不进入普通字符串存储和预览。entity selector 由宿主展示，组件只取得用户最终选择的 opaque reference；组件若自行枚举候选，仍需对应 read/discovery 权限。

`settings.appReference` 已提供最终的单应用引用选择器：字段绑定 manifest 的 `app.reference` binding，宿主应用索引在 worker 匹配，选择、替换和清除直接进入逻辑槽位持久事务及撤销历史，不再把显示名、路径或 AUMID 写进普通 storage。应用目录变化会重新核对 `available/unavailable`，不可用引用不能启动。选择器不要求组件取得全量 `app.discovery`；实际启动仍走 `app.launch` 权限和可信手势。`settings.desktopItemReference`、`settings.fileReference`、`settings.folderReference` 也已接入同一 binding、清除和撤销链路；后两者在结果列表及提交候选两层区分文件/文件夹，路径失效时降级为 unavailable，且不会授予枚举或内容读取权限。原 `settings.appSearch` 保留为“选择一个显示文本”的普通配置控件，媒体控制组件用它匹配会话标题，不再承担持久实体引用语义。`settings.secretReference` 已提供 `password` 字段：无 default/preset、遮罩编辑、显式清除、DPAPI 私有持久化及只读 opaque reference；secret 文件位于普通 data 目录之外，不随 `.snowbackup` 导出。

`settings.url/date/time/range/multiSelect` 已补齐：前三者在清单、preset 和设置页提交边界执行严格格式校验；range 按有限 min/max/正 step 吸附并以 Lua number 持久化；multiSelect 限制 1-64 个唯一稳定选项，以 Lua string[] 保存并支持数组 default/preset。宿主在无持久值时也返回同型默认值，恢复默认和切换 preset 不会退化为字符串编码。

`settings.fileHandle/folderHandle` 已接入现有文件句柄仓库和系统选择器。字段按 read/write/readWrite 取得当前包与实例独占的 opaque capability；Lua 只能经只读 storage 投影读取，不能设置、删除或用 `filesystem.release` 绕过用户控制。替换或清除会取消旧句柄关联的任务/监听并撤销授权；路径不进入普通 storage、preset、预览或 Lua。

`settings.description/groups` 已提供宿主管理的设置页信息结构：字段说明限制 2048 字节；最多 32 个稳定 ASCII group ID 可按声明顺序显示 section 或 collapsible header，并携带有界本地化说明。字段和分组重复、未知分组引用以及 API v1 使用会在加载阶段拒绝。

`settings.validation/dependencies/showWhen` 已提供宿主管理的配置约束。字段可声明 `required`、
`minLength/maxLength` 和本地化 `validationMessage`；文本长度按 Unicode code point 计算，无效草稿
不会写入持久存储。`dependsOn` 是 truthy 禁用条件的简写，`enabledWhen/showWhen` 支持稳定字段键与
`equals/notEquals/oneOf/notOneOf/contains/notContains/set/unset/truthy/falsy`；隐藏或禁用不清除
已有值。加载阶段拒绝未知/自身引用、类型不兼容、越界选项以及任意条件循环，宿主引用仅允许测试
是否存在或 truthy，不向条件表达式泄露 secret、文件句柄或实体引用。

### 15.2 包内模块

```lua
local format = module.require("modules/format.lua")
```

规则：

- 只允许当前已校验包根目录内的 `.lua` 文件。
- canonicalize 后必须仍位于包根目录。
- 不允许网络模块、绝对路径、父目录穿越、符号链接或跨包引用。
- 每实例缓存模块执行结果；循环依赖产生可诊断错误。
- 模块共享当前实例的沙箱、权限和配额，不创建新的权限主体。

### 15.3 资源

当前实现（2026-08-15）已为 schema/API v2 增加清单资源表、`resource.exists/image/font/status`、图片/字体不透明句柄和包私有 DirectWrite 字体集合。`draw.image` 只接受图片句柄，已移除正式 VM 中按原始相对路径绘制图片的 v1 分支；`draw.text`/`draw.measureText` 可接受字体句柄。模块与资源都从实例注册的包根解析，因此入口脚本加载期间不依赖组件已经进入运行列表，也不会向 Lua 暴露宿主绝对路径。当前实现已执行扩展名、签名、像素数、文件大小、资源数、字体数、字体许可证、canonical root 和重解析点检查；图片使用内容 SHA-256 作为跨实例只读解码缓存身份，同一路径热重载不会复用旧像素；失败加载、热重载、卸载和关机均按 VM 句柄计数释放，最后引用释放时同步回收 CPU 像素和 D2D 位图。入口求值期间同步创建句柄，失败以稳定错误码拒绝新 VM；成功句柄的状态为 `ready`，失效后为带稳定原因的 `error`，不引入需要组件轮询的 `pending` 生命周期。

API v2 将图片和字体升级为一等包资源。静态资源优先在清单中用稳定名称声明：

```json
"resources": {
  "logo": {
    "type": "image",
    "path": "assets/logo.png"
  },
  "body": {
    "type": "font",
    "path": "assets/fonts/Inter-Regular.ttf",
    "license": "OFL-1.1"
  }
}
```

Lua 只取得不透明句柄，不取得宿主绝对路径：

```lua
local logo = resource.image("logo")
local body = resource.font("body")

draw.image(logo, 0, 0, 64, 64)
draw.text({ x = 72, y = 8, text = "SnowDesktop", font = body })

-- 声明式视图使用同一句柄：
view.image({ key = "logo", source = logo })
view.text({ key = "title", text = "SnowDesktop", font = body })
```

v2.0 资源契约：

- 保证支持包内 PNG、JPEG、BMP、ICO 和静态 GIF；动画图片只解码约定帧，SVG 作为独立可选 feature，在路径、节点和复杂度限额完成前不进入 v2.0 必选能力。
- 保证支持包内 `.ttf` 和 `.otf`；字体进入包私有 DirectWrite font collection，不安装到系统、不修改全局字体表，也不向其他包暴露字体 family。
- 字体声明包含许可证标识；导出/发布工具检查许可证文件或明确的例外说明，但不代替作者承担字体授权责任。
- 安装校验同时检查安全相对路径、canonical package root、重解析点、扩展名与文件签名、单文件大小、资源总量、图片像素数和字体数量。
- 图片解码、字体解析和 GPU 资源创建不得发生在 `view()` 或 `render()` 热路径；入口期同步创建句柄并返回 ready/error，失败提供稳定错误码。v2.0 不公开需要组件轮询的 pending 状态；可选视觉由组件在入口期用 `resource.exists()` 决定 fallback。
- 资源按 `package id + package version/content digest + resource name` 缓存，多实例共享只读解码结果；实例只持句柄，包更新、设备丢失和卸载由宿主统一失效或重建。
- 预览、正式实例和热重载使用同一资源解析规则；预览不得因为开发路径不同而获得包外文件访问。
- `resource.exists()` 只查询清单内名称。v2 不提供任意包外路径读取，也不把图片/字体加载伪装成通用文件系统权限。

## 16. 预览、调试与作者工具

### 16.1 预览

- 预览使用与实际实例相同的 API v2 registry、资源解析和视图管线。
- 预览权限上下文始终拒绝外部副作用，不受用户正常授权影响。
- 为数据订阅提供稳定 mock provider。
- 支持尺寸、主题、语言、DPI、权限拒绝、加载、空数据、错误和过期数据变体。
- 预览使用虚拟时钟，以便稳定展示时间相关组件。

### 16.2 诊断

组件诊断页增加：

- schema/API/feature 信息；
- 请求、授予、拒绝和待授权权限；
- VM 内存、回调耗时、指令超限和熔断状态；
- 当前订阅及其刷新率、缓存和最后错误；
- 当前任务、调度项和取消原因；
- 最近视图更新原因、节点数、布局耗时和绘制耗时；
- 网络 origin、状态和拒绝原因；
- 热重载和 last-known-good 恢复记录。

### 16.3 作者工具

在现有 `snowwidget validate`、`pack`、`publish-local` 基础上增加：

- `snowwidget view-contract`：输出带独立 schema 版本的 API v2 声明式视图 JSON 契约；
  节点允许/禁止属性、必填项、逐节点默认值、事件、类型、枚举、范围、影响域和无障碍映射均来自
  宿主运行时使用的同一份目录；全树/资源额度、动画词汇与约束、宿主驱动策略以及预览隔离/
  静态降级也随同导出，供 LuaLS、文档生成器和编辑器校验复用。
- `snowwidget system-contract`：离线输出 `system.capabilities()` 使用的 15 项系统函数、25 个
  数据主题和 41 类任务；权限、预览策略、订阅刷新/隐藏/空闲边界、风险标记、可信手势和
  每实例并发限制直接读取运行时目录；数据 options/value 与任务 arguments/result 还直接引用
  随工具分发的 LuaLS 类型，同步函数则导出有序参数名/类型/可选性和结果类型，不另建作者
  工具白名单或结构猜测表。
- `snowwidget lint <directory>`：静态 API、权限、硬编码文案和视图 key 检查。
- `snowwidget test <directory>`：在无副作用沙箱中执行组件测试。
- `snowwidget preview <directory> --size ... --locale ... --theme ...`。
- `snowwidget permissions <directory>`：生成权限与 origin 报告。
- `snowwidget migrate-v2 <directory>`：生成迁移草案，不覆盖原文件。
- LuaLS 类型文件和 API 自动补全。

## 17. API 契约源

建立机器可读的 API 契约，例如：

```json
{
  "name": "network.request",
  "sinceApi": 2,
  "feature": "task.network.request",
  "permission": "network.internet",
  "preview": "deterministic-mock",
  "thread": "host-dispatch",
  "arguments": ["url", "timeoutMs", "cacheSeconds", "maxBytes"],
  "result": "taskHandle",
  "limits": {
    "concurrentPerInstance": 2,
    "requestBytes": 0,
    "responseBytes": 1048576
  }
}
```

契约必须驱动或校验：

- C++ 注册名称和最小 API 版本；
- 权限检查；
- 预览策略；
- LuaLS 类型；
- API 文档表格；
- 设置窗口权限标签；
- 参数边界和错误码测试；
- 诊断页显示。

构建或测试应在以下情况失败：

- C++ 注册了契约中不存在的公开 API；
- 契约声明了未注册 API；
- 权限文案缺少任一语言；
- 文档和契约的函数签名不一致；
- API 没有允许、拒绝、预览和边界测试条目。

## 18. 测试与质量门禁

### 18.1 契约测试

每个 API 至少覆盖：

- 正常调用；
- 参数类型错误；
- 数值、长度和数量边界；
- 权限允许和拒绝；
- 预览行为；
- 实例卸载后的取消；
- 超时、内存和指令额度；
- 嵌套宿主回调和 Lua 栈恢复；
- schema/API v1 包拒绝执行和迁移诊断；API v2 正常激活。

### 18.2 权限集成测试

- 初次安装不自动授予高风险权限。
- 授权前不创建 Lua VM、不执行入口脚本。
- 必要权限拒绝时不放置组件。
- 可选权限拒绝时组件可降级运行。
- 第二个实例复用同一包授权。
- 更新扩权保持旧版本运行。
- 撤销权限取消相关任务并重载实例。
- 启动恢复只显示占位卡，不弹模态窗口。
- 空授权集合不会回退为全部权限。
- 内置包和第三方包执行正确的信任策略。
- “系统状态”页面可以分组展示，但 performance/power/storage/network/display/audio output 的授予、拒绝和撤销分别生效；没有 `system.read` 通配回退。
- 文件/目录权限必须同时满足清单声明、系统选择器产生的有效句柄和访问模式；句柄不能跨包、跨实例或从 read 提升为 write。
- clipboard read、Shell/媒体/音量/删除等动作不能使用计时器或伪造字段制造用户手势。

### 18.3 网络安全测试

- 精确 HTTPS origin、端口和 IDN 规范化。
- HTTP、公网、localhost、RFC1918、链路本地、ULA 和 `.local` 分类。
- 公网域名解析到私有地址。
- DNS 重绑定和实际连接地址复核。
- 相对、绝对和跨 origin 重定向。
- 公网到内网重定向。
- 禁用 Cookie、自动认证和环境凭据。
- 请求取消、响应上限、缓存隔离和并发上限。

### 18.4 调度和数据测试

- 使用虚拟时钟，不依赖真实等待。
- 可见、隐藏、暂停、恢复和系统休眠。
- 截止时间合并、错过周期和取消竞态。
- 每数据源独立启停；读取 CPU 不会启动 GPU、网络、媒体或音频 provider。
- 多实例订阅去重、不同刷新请求聚合、缓存失效、idle grace 和最后一个订阅释放。
- pause/throttle/continue、组件桌面可见性切换、热重载、权限撤销、包更新和实例崩溃后的引用计数一致性。
- CPU/网络/GPU 差分采样的 warming、长暂停重置、计数器回绕和设备变化。
- 存储卷、显示器、网卡、GPU、音频 endpoint 和媒体会话的添加、移除、默认项切换及多设备顺序稳定性。
- 事件型 provider 无订阅泄漏，事件可用时不会同时运行多余轮询。
- `audio.output.analysis` 首订阅启动、跨实例共享、波形/FFT 限额、静音 idle、设备切换和最后可见订阅立即停止。
- 音频隐藏不能 continue、权限撤销清空缓冲、普通 Lua 不能取得原始 PCM、预览不启动捕获。
- 数据提供者错误、过期数据和恢复。

### 18.5 视图测试

- 机器可读 node-property applicability matrix 覆盖全部 v2.0 节点；逐项验证允许/必需/禁止属性、默认值、范围、子节点约束、事件负载和稳定错误码。
- 每个语义节点的 UIA ControlType、必需/可选 Pattern、属性、事件、tree structure、AutomationId 和控件状态映射矩阵。
- 稳定 key、插入、删除、移动和属性更新 diff。
- 布局溢出、最小/最大尺寸和虚拟列表。
- 节点级 hover/pressed/focus 样式、transition、下一帧呈现和 `reducedMotion`。
- enter/leave 边界、重叠/裁剪节点命中、click 移动阈值、double click、事件冒泡、高频事件合并和顺序保证。
- 节点更新/删除期间的命中、焦点、IME、剪贴板和指针捕获释放。
- 即时绘制 region 的 rect/roundedRect/circle 命中、稳定 key 状态、逐帧原子替换、消失 cancel 和语义关联。
- 元素独立右键菜单、祖先/组件/宿主回退、鼠标/触控/键盘触发、菜单打开期间节点失效和用户手势权限。
- Lua 逻辑槽位覆盖 binding empty/bound/unavailable、drop/picker bind、原子 replace/clear/undo，以及 collection 的 desktop/app/external-file reference ingress、reorder/remove/undo、满容量、混合/禁止载荷、复制语义提示、节点 revision 失配和实例销毁。
- `LuaLogicalSlot` 与全部现有 slot surfaces/payloads/relations 的全有向矩阵；Lua 槽位不得接受 widget/group label、执行真实文件转移或进入 Dock 保留路由。
- 拖拽预览和 commit 热路径不调用 Lua；恶意节点不能伪造 collection/reference、取得 path/PIDL/IDataObject/HANDLE，或把单项拖入升级为桌面枚举/文件读取权限。
- 即时绘制 `animation.requestFrame` 的请求/停止、隐藏暂停、休眠恢复、帧合并和超时熔断。
- UI Automation 语义树快照。
- 鼠标、触控、键盘、IME、RTL、长文本、多尺寸、多 DPI、四种宿主主题、高对比度和所有语言目录的截图与交互基线。

### 18.6 包资源测试

- 图片和字体清单名称、重复名称、缺失文件、错误类型、扩展名/文件签名不符和稳定错误码。
- 绝对路径、父目录穿越、大小写碰撞、符号链接、junction、重解析点和 canonical root 逃逸。
- 图片文件大小、解码像素、异常尺寸、截断文件、格式炸弹、缓存总量和并发解码限额。
- `.ttf` / `.otf` 字体解析、family/weight/style 选择、坏字形、字体数量/总大小和许可证元数据。
- 同包多实例共享、不同包同名资源隔离、包更新失效、热重载、D2D 设备丢失和卸载后句柄失效。
- 即时绘制、声明式视图和预览使用同一资源；资源加载不发生在渲染热路径。

### 18.7 系统能力测试

- 第 12.6 节每个必选纯 API、数据主题、任务和动作均有契约测试；能力目录、LuaLS、文档、权限表和注册表集合完全一致。
- 时间 API 覆盖 UTC/local、DST 跳变/重复时段、闰日、系统时钟回拨、monotonic 时长和运行中时区/locale 变化。
- 所有 system snapshot 验证字段类型、单位、范围、采样区间、monotonic timestamp，以及 unsupported/permissionDenied/notPresent/temporarilyUnavailable/warmingUp/stale 的区分。
- 多 GPU、多网卡、多卷、多显示器、多媒体会话和设备热插拔使用可重复的 fake provider 测试；不能把“第一项”写死成系统默认项。
- 网络 status 与 traffic 独立启停；metered/roaming 只作为策略提示，不能绕过实际请求权限和连接失败处理。
- 通知覆盖 show/update/dismiss/schedule/cancel、频控、按钮回传、实例已销毁、包更新、系统禁用通知和预览无副作用。
- 文件选择器、句柄持久化/撤销、移动/删除/卷卸载、分页 list、原子 write、冲突、watch overflow、路径穿越和跨实例/跨包伪造。
- 桌面、应用和 Everything 搜索覆盖取消、分页、结果上限、快速输入淘汰旧结果、索引未就绪/更新和稳定引用；搜索结果不能注入可执行命令行。
- clipboard 覆盖格式/大小限制、手势过期、拒绝和后台读取；Shell 目标覆盖 URI scheme、最终目标、危险扩展名、任意 verb 和命令行注入。
- 媒体和音量动作按 `can*`、endpoint/session 有效性、用户手势、速率和异步结果验证；宿主拒绝不能返回伪成功。
- 预览为全部必选系统能力提供 deterministic mock，且不会显示真实选择器、通知、Shell、剪贴板、媒体或音量副作用。
- 恶意包不能取得用户名、机器名、SID、序列号、原生路径、IP/MAC、SSID/BSSID、HWND/HANDLE，不能调用 WMI/注册表/进程创建/FFI。

### 18.8 性能门禁

- 保持当前 16 MiB Lua 内存、500000 指令和 50 ms 单回调硬上限，除非有独立基准和设计评审。
- UI/绘制线程不得执行文件、网络、WMI 或其他可能阻塞的 I/O。
- M0 阶段用当前预切换实现建立固定参考工作负载；后续阶段的 CPU、帧耗时和宿主内存回归不得超过基线 5%，超出必须有测量和说明。
- 声明式视图默认限制节点数和深度；超大集合必须虚拟化。
- 无数据变化、无动画、无到期调度的隐藏组件不得持续请求重绘。
- 纯 hover/pressed/focus 视觉反馈不得等待 Lua 回调；基准记录输入到下一呈现的 p50/p95，并以参考设备下一可用帧为目标。
- 高频指针事件每实例每帧最多投递一次合并 move；click、菜单和键盘事件不得因节流丢失。
- 数据代理必须证明多实例订阅不会按实例重复采样昂贵系统数据。
- 没有活跃订阅时，系统轮询 worker、PDH 查询和 WASAPI 捕获线程必须为零；事件型 provider 只能保留已登记且可诊断的轻量通知。
- 音频分析分别记录 1/5/10 个订阅下的捕获线程数、CPU、内存、FFT 耗时和发布丢帧；实例数增长不得线性增加捕获客户端。
- 图片解码、字体解析和 GPU 上传必须计入独立资源预算；缓存不得绕过 Lua 实例内存限制形成无上限宿主内存占用。

当前过渡实现（2026-08-16）发布 `view.tree.core`，支持 `box/row/column/stack/text/image/button/icon/iconButton/shape/progressBar/progressRing/spacer`，以 `view.grid.uniform` 提供 1–64 个等宽列、行优先顺序和独立行列间距的基础 `grid`，以 `view.grid.placement` 提供 1-based 显式行列、跨行跨列和受限自动放置，并以 `view.grid.tracks` 提供 fixed/auto/fr/minmax 的有界显式列轨与行轨（虚拟网格仍使用整数等宽列），再以 `view.flow.wrap` 提供跳过隐藏项、独立行列间距、逐行 justify 和行内 align 的横向换行 `flow`（不包含纵向 flow/masonry/滚动/虚拟化）；`view.statusVisuals` 公开 `badge/divider/meter`，`view.dataSeries` 公开 `sparkline/lineChart/barChart/waveform/spectrum`，`view.selectionControls` 公开受控 `toggle/checkbox`，`view.actionControls` 一次公开 `link/radioGroup/slider`：link 使用宿主链接语义与实时 hover/pressed 绘制，radioGroup 为每个选项生成独立稳定命中区和 `previousSelection/selection` 建议值，slider 以捕获的左键拖动持续返回 step 对齐的 `previousControlValue/controlValue`；`view.inputControls` 一次公开 `textInput/textArea/searchBox/numberInput/select`，输入复用宿主键盘、IME、选择和剪贴板代理并投递受控文本/数值建议，select 以组件受控 expanded 状态绘制顶层有界选项；所有受控值均由组件写回，宿主不替组件持久化，元素各自支持 contextMenu；meter、slider 与数据图形要求无障碍标签，数据图形每节点最多 512 个有限样本、全树最多 4096 个并支持自动或显式 `min/max` 值域、
稳定全树 key、基础线性布局、基础文本/边框样式、宿主 hover/pressed 视觉、元素 click/
doubleClick/pointer/contextMenu action，以及“先完整校验布局、后原子提交；失败保留上一成功树”。
受控状态继续按细粒度 feature 推进：`view.state.selected` 已贯通通用
`selected/selectedStyle` 与 SelectionItem 语义，`view.checkbox.indeterminate` 已贯通
混合态绘制、交互建议和 UIA Toggle Indeterminate；`view.progress.indeterminate` 已为
progressBar/progressRing 加入仅在可见 surface 运行的宿主动画，隐藏或面板关闭时不再请求帧，
预览与 reducedMotion 使用静态片段且不会向 Lua 投递逐帧事件；`view.input.required` 已贯通
input/select 的 UIA IsRequiredForForm 语义，`view.input.selection` 已贯通文本输入受控选区、
UTF-8/UTF-16 边界换算和 selectionChange 建议，`view.keyboard.events` 已贯通桌面与输入代理的
聚焦按键观察、按下/释放配对和失焦清理，`view.focus.request` 已把可信动作焦点请求扩展到
任意可聚焦声明式节点并支持下一次成功提交解析；这些声明都不把状态持久化责任转移给宿主。
其额度为 512 节点、32 层、单节点 4 KiB 文本、全树 64 KiB 文本和 256 个交互元素；未知字段、
重复 key、非连续 children、错误枚举和越界数值拒绝整次提交。数据图形由宿主直接有界绘制，不展开为逐样本节点或命中区域。`view.theme.tokens` 已让所有声明式 RGB 槽按宿主题、高对比度和系统强调色解析；`view.transform.basic` 已将有界平移、统一缩放与变换原点贯通绘制、命中、宿主输入、裁剪和 UIA 边界，`view.transform.affine` 又开放非统一缩放、旋转、斜切、逆矩阵精确命中和变换后 slider 轴；非轴对齐裁剪、宿主管理控件仿射变换与透视仍未开放。`view.transition.visual` 已为四种视觉样式加入宿主逐帧插值，`view.transition.transform` 已加入分解后的 transform 呈现插值，`view.transition.layout` 已加入与滚动位移分离的父相对布局呈现插值，`view.transition.enter/exit` 已让初始 scene 后新增的稳定 key 从有界 opacity/transform 起点入场，并让移除 key 以最多 512 个不可交互旧 scene 节点完成退场；命中和 UIA 在 scene 提交时切换到目标矩阵。`view.surface.panel`、`view.surface.dialog` 与 `view.surface.popover` 已把同一树、命中、滚动、控件和键盘管线扩展到互斥的宿主辅助 surface 状态；dialog 另有居中遮罩和非阻塞背景输入隔离，popover 由稳定桌面元素 key 锚定。它们尚不包含完整必选节点矩阵、
完整 UIA、RTL、文本换行、可变高度虚拟化和差量资源复用；通用 surface 键盘焦点已作为
`view.keyboardNavigation.basic` 单独发布，因此仍只发布
细粒度 feature，不发布 `view.tree`，也不计作 M6 完成。

### 18.9 最终验证入口

- 开发中可以使用定向 CTest 标签和诊断构建。
- 每个阶段交付前必须运行 `scripts/test.bat`。
- 报告 Release 构建通过前必须运行 `scripts/build.bat` 并确认 `.build/Release/SnowDesktop.exe` 生成。
- 视觉、交互、权限授权和无障碍结论必须完成实际场景验证，不能只凭自动化测试宣称完成。

## 19. 现有内置组件 API v2 强制迁移

### 19.1 发布硬门槛

当前仓库 11 个内置组件均已完成 schema/API v2 代码迁移。它们仍需逐个完成真实桌面的
多 DPI、主题、隐藏唤醒、系统数据、滚动、媒体控制、元素菜单、应用/项目启动和权限
拒绝降级验收，因此当前只能计为代码迁移完成，不能计为最终验证完成。只有全部内置
组件完成第 19.3 节验收后才能宣布稳定。

统一迁移规则：

- 每个包改为 `schemaVersion: 2`、`apiVersion: 2` 和 `widget.define(...)` 入口；包版本按 SemVer 增加。
- 保持包 `id`、slug 和实例身份不变，使现有布局仍能解析到同一个组件。
- 只有持久数据结构实际变化时才增加 `dataVersion`；迁移必须幂等、可失败回滚并保留未知字段。
- 将 `onTimer`、`onHttpResponse` 和数据变化回调迁到作用域化的 schedule、task 和 subscription；隐藏、撤权和卸载必须自动取消。
- 将高风险权限拆成 required/optional，并为全部支持语言提供真实的权限原因；内置签名不得绕过首次同意。
- 权限拒绝必须有可用的降级或明确占位，不得以空数据、无限加载或脚本错误代替。
- 即时绘制组件可以继续使用 v2 `draw`；只有确实受益于布局、集合、文本输入或无障碍的组件才迁到声明式 `view`。
- 内置迁移不改用 WebView2；这 11 个包同时作为 Lua v2 的真实覆盖矩阵。

### 19.2 五波迁移矩阵

| 波次 | 组件 | v2 目标权限 | v2 迁移重点 | 主要验收 |
|---|---|---|---|---|
| A：基础绘制 | `analog-clock` | 无 | `widget.define`、环境上下文、按秒/分钟调度、v2 即时绘制 | 多 DPI/尺寸/主题截图一致；隐藏时无持续帧 |
| A：基础绘制 | `digital-clock` | 无 | `view.tree.core`、可见性作用域、时间线调度、本地化和尺寸响应 | 时间、日期、语言、休眠恢复正确；树失败保留上一帧 |
| B：状态与输入 | `sticky-note` | 无 | 有界宿主文本编辑、可信手势焦点/IME、菜单动作 | 旧便笺内容保留；中文 IME、撤销和重启恢复通过 |
| B：状态与输入 | `reminders` | 无 | 稳定 key 集合、事务存储、编辑动作和可访问语义 | 旧任务顺序/完成状态保留；键盘与 Narrator 可用 |
| B：状态与输入 | `pomodoro` | 可选 `notification.post` | `view.tree.core` 进度环/图标按钮、宿主调度、后台合并、通知可选权限、动作状态机 | hover/pressed/点击/元素菜单可用；休眠恢复不补发多次；拒绝通知仍可计时 |
| C：数据订阅 | `system-monitor` | 必需 `system.performance.read`；可选 `system.power.read`、`system.network.read` | 拆分 topic 的共享采样、可见性节流、v2 draw 与实例滚动 | 每类授权可分别拒绝/撤销；多实例不重复昂贵采样；无订阅即停 |
| C：数据订阅 | `media-controls` | 必需 `media.read`；可选 `media.action`、`app.discovery`、`app.launch` | 媒体订阅、用户手势动作、应用搜索/不透明引用启动降级 | 播放器切换/退出恢复；分别拒绝媒体读取/控制和应用发现/启动权限 |
| D：日历集合 | `month-calendar` | 可选 `calendar.read` | 日历订阅、月视图稳定 key、无权限日期计算和本地共享选择 | 跨月/时区/区域格式正确；拒绝读取仍可使用月视图 |
| D：日历集合 | `agenda` | `calendar.read`、`calendar.write` | 复杂集合、编辑面板、异步日历任务、作用域清理 | 现有功能逐项回归；修改权限拒绝时保留只读日程 |
| E：网络 | `rss-reader` | 必需 `network.internet`；可选 `shell.launch` | 精确 HTTPS origin、网络任务、缓存/错误状态、受控打开链接动作 | 首次联网/打开链接授权；重定向/离线/撤权/恶意 feed 测试通过 |
| E：桌面高权限 | `quick-launcher` | 必需 `desktop.read`；可选 `desktop.action`、`app.discovery`、`app.launch`、`everything.search` | 桌面/应用/Everything 搜索任务、虚拟列表、引用化启动/定位和最小权限降级 | 三类搜索与启动分别授权；大结果集、IME、撤权和索引变化通过 |

执行顺序是 A → B → C → D → E。每一波先迁一个代表组件，补齐缺失的 v2 契约和测试，再完成同波其余组件；不得为迁移某个组件临时增加只对该组件生效的隐式 API。

### 19.3 每个组件的完成清单

每个内置组件必须单独满足：

1. manifest、权限、本地化和入口通过 schema/API 契约测试；源码不再调用 API v1 专属入口或回调。
2. 从旧版本原地升级后，现有实例、存储、设置和布局不丢失；失败时仍可运行 last-known-good。
3. 新装、升级、授权、拒绝、撤销、隐藏、休眠恢复、热重载和卸载路径已覆盖。
4. 默认尺寸、所有声明尺寸、DPI、主题、高对比度和全部语言目录完成截图检查。
5. 鼠标、键盘、IME、菜单、拖动/调整尺寸和 UI Automation 按组件能力实际验收。
6. 记录 CPU、宿主内存、刷新次数和任务/订阅数量，不超过对应预切换基线门禁。
7. 定向测试、`scripts/test.bat` 和 `scripts/build.bat` 按仓库规则真实执行并记录结果。

M7 切换完成后，发布运行时必须删除 API v1 注册和执行分支。预发行自制组件通过 `migrate-v2` 更新，不能以假设中的第三方兼容需求阻止移除。

## 20. 分阶段实施路线

### M0：冻结基线和契约清单

交付物：

- 当前 API v1 函数、回调、权限、限制和预览行为完整清单。
- 第 12.6 节系统能力目录及 system topic/task/action applicability matrix，逐项标出已有、缺失、v2.0 必选、v2.x 延期和禁止直通。
- 11 个内置组件的视觉、交互、存储、权限和性能基线。
- API 契约文件格式和一致性检查器。
- 关键设计决策记录：Lua 单版本切换、执行前授权、Lua 与 WebView2 运行时隔离、无普通原生插件。

退出条件：

- 所有现有公开 API 和内置组件使用面都进入契约清单。
- v2.0 必选系统能力的名称、字段、单位、权限、生命周期、Windows 最低支持条件和 fake provider 方案全部通过设计评审。
- 完整测试和标准构建通过。
- 当前内置组件截图、关键交互和升级样本已保存。

### M1：无行为变化的引擎拆分

交付物：

- 提取 runtime instance、API registry、scheduler façade、diagnostics 和 preview context。
- `WidgetEngine` 保持兼容门面。
- 当前行为由迁移基线和契约测试锁定，避免重构阶段先发生非预期变化。

退出条件：

- API 文档和现有组件无需修改。
- 预切换参考工作负载性能回归不超过门禁。
- 每个编译通过的重构按仓库提交规范独立保存。

### M2：权限代理和首次授权

当前实现进度（2026-08-15）：已落地集中权限代理、来源绑定范围指纹、非阻塞首次授权、受阻占位卡及恢复入口；schema/API v2 以 `permissions` 表示必需权限、`optionalPermissions` 表示可选权限。CPU/内存/GPU、进程摘要、电源、存储、网络、显示、媒体、音频、通知、剪贴板、应用动作和用户选择文件范围已映射到分离权限，组件管理页可查询、调整和撤销授权，撤销必需权限会让实例进入可恢复的暂停占位。公开权限词表已集中到单一 descriptor 契约，包校验、风险分类、首次授权、管理页标签和 15/25/41 系统 API 契约共同使用该来源；剩余工作是更新扩权差异页和各权限撤销/设备变化的真实场景验收，不再以“权限已声明但 API 尚未启用”描述当前状态。

交付物：

- `WidgetPermissionBroker`。
- 请求/授予/拒绝/待定持久模型及原子迁移。
- 移除 `system.read` 通配语义，建立 performance/power/storage/network/display/audio output 等可分别撤销的系统子权限，以及用户选择文件句柄授权模型。
- 添加前授权页、启动占位卡、管理页撤销和更新权限差异页。
- required/optional 权限和授权范围哈希。
- 所有语言真实翻译。

退出条件：

- 声明待授权高风险权限的内置或第三方组件，在用户完成同意/拒绝选择前不能创建 Lua VM、执行入口脚本或获得该能力。
- 更新扩权不会替换当前可用版本。
- 权限集成测试、本地化契约、完整测试和标准构建通过。
- 实际验证同意、拒绝、撤销、重启恢复和更新扩权。

### M3：API v2 运行时和包能力

交付物：

- schema v2/API v2 运行时、`widget.define`、`apiInfo` 和 feature negotiation。
- `system.info/capabilities`、`time.*`、`l10n.format*` 和系统能力共同错误/可用性模型。
- 只读的 schema v1 迁移解析器；它不能创建 Lua VM 或进入激活路径。
- 安全包内模块、图片和包私有字体资源句柄。
- v2 LuaLS 类型和迁移指南初版。
- 当前正式加载路径已在读取入口正文和创建 Lua VM 前强制要求 schema/API 同为 2；
  schema/API v1 仅保留清单解析和迁移诊断，不再进入 API 注册或执行分支。正式 VM 的
  sandbox 与注册目录也已删除 `sys/media/http/desktop/everything/imgui` 等 v1 库，
  不再先创建旧全局再依赖 `_ENV` 隐藏。
- 包资源路径在 VM 建立时一次解析；图片由 `resource.image` 在入口加载期计算内容 SHA-256，
  并解码到 64 MiB 单资源、128 MiB 宿主总量的共享 CPU 缓存；每个成功 VM 保留资源名到
  内容键的绑定，绘制、设备重建和同路径热重载只从对应内容键的内存像素创建 D2D 位图。
  缓存按 VM 中创建的资源句柄计数，最后一个句柄随失败加载、热重载、卸载或关机释放后回收。
  私有字体也必须在入口加载期创建集合，组件卸载不会清空其他实例仍共享的资源缓存。构造失败
  使用稳定错误码并拒绝新 VM；成功句柄不公开伪异步 `pending` 状态。

退出条件：

- v2 最小示例可加载、热重载、故障恢复和卸载。
- 同一示例可以在即时绘制、声明式视图和预览中加载包内图片及 `.ttf`/`.otf` 包私有字体，且不会暴露绝对路径或安装系统字体。
- v1 包得到明确迁移诊断且其入口脚本未执行。
- 不支持的 API/feature 给出稳定、可本地化的错误。

### M4：状态、任务、调度和数据代理

交付物：

- transient state、类型化 storage transaction 和 secret reference 基础。
  当前三项基础均已实现；secret 仅开放声明式录入和受控网络任务注入，不提供正文读取 API。
- 统一 task、schedule 和 data subscription。
- 按 M4A（CPU/内存/GPU/电源）、M4B（存储/网络/显示）、M4C（媒体/音频/通知）、M4D（文件句柄/剪贴板/Shell/桌面/日历）四个可独立验收子波次完成第 12.6 节全部必选 provider、task 和 action。
- 所有 provider 按数据主题引用计数启停、共享采样、可见性节流和诊断；事件可用时不保留多余轮询。
- 现有系统、媒体、日历、桌面和 HTTP 能力接入代理层，并按 v2 细粒度权限和异步结果迁移。
- `audio.output.analysis` 权限、共享捕获/FFT provider、模拟预览和运行状态指示。
- 当前 M4C 通知子波次已接通 `show/update/dismiss/schedule/cancel`、实例作用域 ID、频控、
  统一 tick 调度、到期事件、跨重启预约恢复及清理，以及包资源图、进度、有限按钮和
  `notification.action` 可信动作回传；实机视觉与通知策略兼容仍需验收。
- 内置组件现有计时器、数据回调和可见性回调的明确迁移映射及测试。

退出条件：

- 所有慢操作离开 UI/绘制线程。
- 可见性、休眠、取消和权限撤销没有遗留任务。
- 虚拟时钟和订阅去重测试通过。
- 第 12.6 节目录不存在“已列名但无注册/类型/权限/实现/mock/测试”的半 API；全部必选系统能力通过第 18.7 节门禁。
- 无活跃订阅时没有系统采样、PDH 或 WASAPI worker；音频最后可见订阅释放后立即停止并清空缓冲。

### M5：网络权限 v2

交付物：

- `network.internet` / `network.local`、精确 origin 和授权 UI。
- v2 网络请求任务。
- v1 `network.http` 静态迁移报告和动态 URL 人工确认清单；发布运行时不提供兼容旁路。

退出条件：

- 全部网络安全矩阵通过。
- 实际验证公网请求、localhost 服务、局域网服务、拒绝和撤销。
- 不携带系统凭据、Cookie 或自动认证。

### M6：声明式视图与无障碍

交付物：

- scene node、diff、layout、renderer、节点级 hit testing/input、transition/animation 和 UI Automation。
- 第 13.4 节全部 v2.0 必选布局、内容、控件、集合、图表和宿主表面节点，不以未定义的“首批节点”替代稳定版清单。
- 冻结的机器可读 node-property applicability matrix，以及由它生成的 LuaLS 类型、文档、校验器、默认值表和测试参数集。
  当前节点/属性矩阵已进入宿主校验器并有独立契约测试，UIA ControlType/基础 Pattern/
  键盘焦点列、语义快照及 Windows Fragment Provider 已建立，Invoke/Toggle/RangeValue/Value/
  ExpandCollapse/SelectionItem 动作已接入现有受控 action 通道，基础结构/焦点/属性变化也已
  差分通知，radio/select/calendar 常用内部项也已形成 Selection/SelectionItem 树；生成物、
  运行时全树/资源额度、动画允许词汇和预览降级已进入 JSON 导出。任意虚拟化集合子项及其余
  Pattern/事件仍需继续并入；逐属性过渡影响与 RTL 解析/顺序规则已进入 schema 2，十个稳定的
  视图管线阶段码已进入 schema 3 并用于 desktop/辅助 surface 运行时诊断，但逐节点规则子码与
  生成物仍未达到冻结条件。
- 环境上下文、响应式尺寸和减少动态效果。
- 即时绘制的按需 `animation.requestFrame/cancelFrame` 已接入宿主单次帧计时器、可见性清理和 `reducedMotion` 拒绝；真实桌面帧节奏、休眠恢复和多实例合帧仍需完成第 18.5 节场景验证。
- 元素级 hover/pressed/focus、click/double click、指针捕获和独立原生右键菜单。
- `LuaLogicalSlot`、`slots.binding/collection`、`slotSurface/slotItem`、宿主引用存储和现有 slot contract 全矩阵接入；v2.0 实现 binding 的 reference/replace/clear 和 collection 的 reference/reorder/remove。
- 当前过渡实现已提供 collection slotItem 的宿主指针阈值、插入提示、同槽原子重排，以及同一组件内兼容 collection 间保持 opaque 身份的原子转移；两槽修改共用一个撤销历史项并以 `operation="transferred"`、目标/来源 revision 和 `host.pointer` 通知 Lua。`slots.keyboardNavigation` 提供可见 slotItem 的 Tab/空间方向焦点、焦点轮廓、Enter/Space 自身 click、Alt+方向键同槽重排、Delete 策略化移除和 `host.keyboard` 变化来源。原生拖出、跨组件/跨实例转移、通用节点键盘矩阵和 UIA 仍待后续批次。
- 即时绘制交互 region 与语义树接口。

退出条件：

- 至少三个不同类型的内置组件先以 v2 通过：基础信息、可编辑集合和数据控制各一个。
- 第 13.7 节完整性门禁全部通过；每个公开节点不存在外观、交互、键盘或 UIA 契约缺口。
- 代表组件实际证明列表项分别 hover、click、double click 和显示不同右键菜单；重新排序或删除节点后不误触发旧目标。
- 一个参考 Lua 收藏夹实际证明可接收桌面项、应用和 Explorer 文件引用，可重排/移除/撤销，且不会移动原对象、泄露路径、同步调用 Lua 或接受 widget/Dock 保留载荷。
- 一个参考“应用大图标”组件实际证明 app binding 可通过 drop 和宿主选择器建立、原子替换、清除及撤销；应用卸载后显示 unavailable，不误绑同名目标，点击启动仍分别检查 `app.launch` 与用户手势。
- 多尺寸/DPI/主题/语言截图基线通过。
- 键盘、IME、高对比度和 Narrator 实际验证通过。

### M7：全部内置组件迁移

交付物：

- 按 A 至 E 五波完成 11 个内置组件的 schema v2/API v2 迁移。
- 每个组件的升级脚本、权限原因、拒绝降级、回归场景和性能记录。
- 自动检查内置目录中不存在 API v1 manifest 和 v1 专属调用。
- 删除 API v1 注册、旧全局回调分发和 v1 包执行分支，只保留不执行代码的迁移解析器。

退出条件：

- 第 19 节逐组件完成清单全部通过。
- 旧布局和实例存储可以原地升级，回滚不会损坏数据。
- 所有高风险内置组件实际验证首次同意、拒绝和撤销。
- 宿主对 v1 包只显示迁移诊断，不能创建其 Lua VM。

### M8：作者工具和生态试用

交付物：

- lint、test、preview、permissions、migrate-v2。
- 完整 API v2 文档、权限指南、迁移指南和示例。
- Workshop 元数据支持 schema/API/权限摘要。
- 组件诊断与性能面板。

退出条件：

- 至少一轮真实第三方组件迁移反馈完成。
- 文档示例可由 CI 验证。
- 作者无需阅读 C++ 源码即可定位权限、生命周期和性能问题。

### M9：Lua API v2 稳定

交付物：

- 冻结 Lua API v2.0 契约。
- 发布兼容承诺、弃用策略和安全响应流程。
- 11 个内置组件和正式宿主全部使用 API v2；API v1 只能被迁移工具识别，不能执行。

退出条件：

- API v2 完整矩阵、v1 拒绝/迁移诊断、标准构建和实际场景验收完成。
- 没有未记录的公开 API、权限、后台线程或持久格式。
- WebView2 不属于 Lua v2 发布面；未完成 W3 时保持延期，实验代码不得启用或随稳定版分发。
- 版本分支按仓库发布流程验证并由用户决定是否进入 `main`。

## 21. WebView 类组件评估

### 21.1 结论

建议结论是“Lua v2 主线不引入，允许独立 WebView2 技术验证”。WebView2 对复杂排版、表单、富文本、成熟前端工具链、IME 和 Web 无障碍有明显价值；但它同时引入浏览器进程、独立存储、直接网络面、运行时分发和第二套调试/崩溃恢复体系。没有完成安全与资源门禁前，不应把它作为公开组件能力。

仓库当前没有 WebView2 SDK 或 Runtime 依赖，但桌面渲染已使用 D3D/D2D/DirectComposition，并链接 `dcomp`。WebView2 Composition Controller 可以把浏览器视觉树连接到 `IDCompositionVisual`，因此图形集成具有原型基础；宿主仍需自行转发鼠标、触控和笔输入，并处理焦点、光标和可访问性。这是“值得验证”，不是“已经兼容”。

### 21.2 方案比较

| 方案 | 结论 | 原因 |
|---|---|---|
| 给 Lua 增加 `widget.openWebView()` | 拒绝 | 一个实例出现两套脚本、生命周期、权限和状态源，撤权与卸载边界不可审计 |
| 任意远程 URL 组件 | 拒绝 | 页面内容可随时变化，登录态、跳转、广告、下载和供应链无法由包审核固定 |
| 离线打包 HTML/CSS/JS 的独立 `webview2` 运行时 | 有条件原型 | 能固定入口和资源哈希，也能复用包管理、权限代理和宿主生命周期 |
| 宿主内置、不可扩展的特定 WebView 页面 | 单独产品决策 | 信任面较小，但不属于公共组件 API，不能用来证明第三方 WebView 包安全 |

即使采用，Lua 与 WebView2 也只共享以下宿主服务：包身份、权限状态、任务代理、布局、可见性、诊断和更新/回滚。不得共享 Lua 全局 API，不得从 Web 内容直接取得 COM/Win32 对象。

### 21.3 候选包描述

以下只用于 W0/W1 原型，不在评估通过前冻结：

```json
{
  "schemaVersion": 2,
  "id": "00000000-0000-0000-0000-000000000000",
  "version": "0.1.0",
  "runtime": {
    "type": "webview2",
    "apiVersion": 1,
    "entry": "web/index.html"
  },
  "web": {
    "contentMode": "packaged",
    "allowTopLevelNavigation": false,
    "allowDevTools": false
  },
  "requiredPermissions": [],
  "optionalPermissions": [
    "network.internet"
  ],
  "network": {
    "origins": [
      "https://api.example.com:443"
    ]
  }
}
```

`runtime.apiVersion` 是 Web 运行时桥契约，不等于 Lua `apiVersion`。schema v2 解析器可以把旧的顶层 Lua `apiVersion`/`entry` 规范化为 `{ type: "lua" }` 描述，但在兼容期不得要求现有包改写。

### 21.4 必须证明的安全边界

默认策略是离线、无网、无宿主对象、无浏览器权限：

- 主文档和包内资源通过每包唯一的虚拟 HTTPS host 映射加载，使用 `DenyCors`；不用 `file://`，也不映射包目录外路径。
- 只允许固定虚拟 origin 的顶层导航；拦截并拒绝新窗口、下载、外部协议、弹窗和未声明 frame 导航。
- 麦克风、摄像头、位置、MIDI、通知、剪贴板、屏幕捕获和自动播放默认拒绝；未来只能映射到 SnowDesktop 权限代理，浏览器自身的记忆授权不得成为产品授权源。
- 禁用生产包 DevTools、默认上下文菜单、浏览器扩展、密码保存和通用 host object；开发模式与正式信任状态分离。
- Web/宿主桥仅使用带版本、实例 ID、请求 ID 和 schema 的 JSON 消息。收发前同时校验文档 origin、消息类型、字段、长度和权限；拒绝 `AddHostObjectToScript` 式通用对象投影。
- 网络数据优先通过 SnowDesktop task bridge 获取，使 Lua 与 Web 包共用 origin、重定向、DNS、额度、取消和审计规则。
- 必须证明 WebView2 的直接网络路径可以被默认拒绝，测试 fetch、XHR、图片/字体、iframe、导航、WebSocket、EventSource、WebRTC 和 Service Worker。若宿主 API 不能形成不可绕过的拒绝边界，则不向第三方开放 WebView 包，即使 CSP 测试看似通过。
- CSP 作为纵深防御，由宿主生成并收紧到 `default-src 'self'; object-src 'none'; frame-ancestors 'none'` 等基线；包内 meta CSP 不能放宽宿主策略，也不把 CORS/CSP 当作权限边界。
- 预览使用临时 profile、模拟数据和禁止副作用的 bridge；退出预览后清除数据。

### 21.5 合成、输入和生命周期门禁

- 使用 `ICoreWebView2CompositionController` 挂接到独立的 `IDCompositionVisual`，不用普通子 HWND 覆盖 D2D 桌面层。
- 原型必须验证透明背景、首帧白闪、裁剪、圆角、缩放、DPI、DComp 设备丢失、Explorer 重建和多显示器迁移。
- SnowDesktop 命中测试负责决定组件、拖动、调整尺寸或 Web 内容输入，并向 WebView2 转发 mouse/touch/pen；必须覆盖 leave、wheel、capture、cursor 和双击。
- 验证键盘焦点、Tab 顺序、快捷键冲突、中文 IME 候选窗、上下文菜单、拖放和 UI Automation provider；任何无法访问的视觉宿主方案都不得发布。
- 隐藏时先设 `IsVisible=false`，再调用 `TrySuspend`；恢复时使用 `Resume`。关闭实例必须注销事件、取消 bridge 请求、断开 visual 并释放 controller。
- 处理 `ProcessFailed`、`BrowserProcessExited`、renderer 无响应、GPU 进程退出和 Runtime 更新；恢复只能重建受影响实例，不能破坏布局或永久循环重启。

### 21.6 资源、数据与分发策略

- 一个应用级 Evergreen environment，按包/信任域使用 profiles 隔离 cookie、缓存、权限和 DOM storage；避免每实例独立 UDF 引发额外浏览器进程。
- profile 不能代替 SnowDesktop 实例存储。关键用户数据走宿主 storage bridge；DOM storage 只存可丢弃的 Web UI 缓存。
- 卸载包、清除组件数据和删除 profile 必须等待浏览器进程释放文件；失败时保留待清理记录，不强制删除锁定目录。
- 设置最大同时可见 WebView 数、后台存活数、单实例 bridge 消息/秒、消息大小和请求并发；超限显示可恢复占位。
- W1 必须记录 0/1/5/10 个实例的宿主内存、WebView2 子进程私有工作集、CPU、GPU、首帧时间、交互帧耗时和休眠后回收量，并与等价 Lua 组件比较。
- 分发优先评估 Evergreen Runtime。安装器负责检测和部署边缘缺失情况；添加组件时不得静默在线下载安装 Runtime。
- 长期运行的 SnowDesktop 必须响应 `NewBrowserVersionAvailable`，在安全时机释放旧 environment 或提示重启，不能无限期停留在旧 Runtime。
- Fixed Version 只作为严格兼容或离线环境备选，因为它显著增加安装包并把安全更新责任转移给 SnowDesktop 发布流程。

### 21.7 独立评估阶段与决策门

WebView2 轨道不阻塞 M0–M9：

| 阶段 | 交付物 | 通过条件 |
|---|---|---|
| W0：ADR 与威胁模型 | 运行时边界、包 schema 草案、攻击面、Runtime 分发选择、测量基线 | 明确禁止任意远程网页和 Lua/Web 混合实例；安全负责人认可测试矩阵 |
| W1：离线合成原型 | 单个离线 Web 包、Composition Controller、透明表面、输入/IME/UIA、创建/销毁 | 在 SnowDesktop 真实桌面宿主上完成 DPI、主题、拖动、调整尺寸、Explorer 重建和 Narrator 验证 |
| W2：沙箱与压力验证 | 限制 bridge、网络封锁、临时预览、profile 生命周期、故障恢复、1/5/10 实例基准 | 恶意测试包不能绕过网络/导航/浏览器权限；资源指标达到事先批准的预算 |
| W3：产品决策 | 采用、延期或拒绝 ADR；若采用，制定独立 Web Runtime API 路线 | 证据齐全且不会改变 Lua v2 契约；未通过项不得以“实验功能”绕过安全门禁 |

当前建议：先执行 W0 和 W1。只有 W1 证明合成、输入、IME 和 UIA 可用后才投入 W2；W2 不能证明直接网络不可绕过时，结论应为“不开放第三方 WebView 类组件”。

## 22. 依赖关系与优先级

Lua v2 严格依赖：

```text
M0 contract
  -> M1 modularization
  -> M2 permission broker
  -> M3 API v2 runtime
  -> M4 task/schedule/data
  -> M5 network v2
  -> M6 declarative view
  -> M7 all built-ins on v2
  -> M8 ecosystem
  -> M9 stable
```

WebView2 独立依赖：

```text
M1 runtime boundary + M2 permission model
  -> W0 threat model
  -> W1 composition prototype
  -> W2 sandbox/performance
  -> W3 decision
```

可以有限并行：

- M2 授权 UI 与权限存储可以在接口冻结后并行。
- M4 数据代理与调度器可以在 runtime instance 边界稳定后并行。
- M6 UI Automation 和 scene layout 可以在 scene node 契约冻结后并行。
- 内置组件迁移波次的基线准备可以提前，但代码迁移必须等待所需 v2 契约稳定。
- W0 可以在 M1/M2 期间准备，W1 不得侵入 Lua runtime；W2 必须等待网络和权限模型可复用。
- 文档、LuaLS 类型和契约测试应与每个 API 同一阶段完成，不能推迟到 M8 补写。

不得提前：

- 未完成 M2 前不增加新的高风险 API。
- 未完成 M3 前不把 `kHostApiVersion` 直接改为 2。
- 未完成 M4 前不增加更多独立异步回调。
- secret 只允许在已完成 origin 约束的宿主任务中受控注入；不增加通用 reveal、账户或位置能力。
- 未完成 UI Automation 基础前不把声明式 UI 标记为稳定。
- 未完成 M7 前不宣布 Lua API v2 稳定。
- 未完成 W2 前不接受可分发的第三方 WebView 包。

## 23. 风险登记

| 风险 | 影响 | 缓解措施 |
|---|---|---|
| 大规模拆分引入行为回归 | 内置组件视觉或交互改变 | M0 基线、逐步门面提取、迁移前后截图与交互对比 |
| 权限弹窗疲劳 | 用户机械允许 | 风险分级、基础能力免弹、一次完整授权、清晰 origin |
| 内置组件迁移拖慢稳定版 | v2 长期处于预览 | 五波迁移、每波代表组件、M7 硬门槛，不降低验收范围 |
| 内置数据迁移损坏用户状态 | 便笺、任务、设置或布局丢失 | 保持身份、幂等迁移、样本备份、last-known-good 回滚 |
| 单版本切换窗口内部分组件不可用 | 研发构建短期不完整 | 在未发布迁移分支施工；M7 全量验收后才形成可发布构建 |
| 预发行自制 v1 包停止运行 | 少量早期作者需要改包 | `migrate-v2`、静态报告和清晰错误；不为此保留不安全执行路径 |
| 声明式 UI 范围失控 | 延迟稳定版本 | 首批节点冻结，不追求通用桌面 UI 框架 |
| Lua 槽位破坏宿主拖放/文件语义 | 丢项、误替换绑定、误移动文件、路径泄露或拖拽卡死 | v2.0 仅宿主 binding/collection reference model；原子替换与 undo、静态接受规则、全矩阵测试、热路径不进 Lua，真实文件转移和 widget/Dock 路由不开放 |
| 系统 API 范围失控 | v2 被拖成 Win32/WMI 包装层 | 第 12.6 节只冻结常见组件必选窄能力；高隐私、硬件专用和高影响动作进入 v2.x 独立评审 |
| Windows/硬件差异导致伪兼容 | 某些机器字段为空、指标含义不同或 provider 常驻 | feature probe、统一 unavailable 原因、fake provider、多设备/热插拔测试和最低支持条件契约 |
| 数据订阅泄漏 | 隐藏组件持续耗电/占用 | 实例作用域资源、最后订阅释放、诊断和压力测试 |
| 授权状态切换失败 | 组件不可用或被误授权 | 原子迁移、备份、v2 重新同意、失败回滚 |
| 文档与实现漂移 | 作者使用错误权限或签名 | 单一 API 契约源和 CI 一致性检查 |
| 无障碍后补成本高 | 自绘控件无法访问 | scene node 从第一版包含语义，不接受后补占位 |
| WebView 直接网络绕过代理 | 第三方包绕开用户授权 | W2 恶意路径测试；无法形成硬边界则拒绝公共 Web runtime |
| WebView 多进程资源膨胀 | 桌面常驻内存/CPU 不可接受 | 共享 environment、profile 隔离、实例上限、TrySuspend 和量化门禁 |
| Evergreen 更新改变行为 | 长期运行或更新后兼容问题 | feature detection、预览通道测试、进程重建和恢复策略 |
| WebView2 扩大供应链与调试面 | 安全响应和维护成本上升 | 独立发布决策、离线内容、严格 bridge、Runtime 更新责任明确 |
| 原生 Provider 破坏信任模型 | 普通包获得代码执行 | v2 不开放；未来独立签名、安装和信任层级 |

## 24. 发布与提交纪律

- 实施工作进入当时有效的 `release/vA.B.C.D`，不直接提交或合入 `main`。
- 一个阶段可以跨多个应用版本；应用版本号不等同于 Lua `apiVersion` 或候选 Web Runtime API 版本。
- 内置组件按波次提交，但每个组件的存储迁移和目标场景验证必须可独立追踪。
- WebView2 原型放在明确的实验开关和独立模块中；W3 决定拒绝时可以完整移除而不影响 Lua v2。
- 每个编译通过但未完成目标场景验证的代码尝试使用独立 `try` Commit。
- 实际验证完成后使用 `verify` Commit；只有提交本身已完成对应验证时才使用 `feat`、`fix` 或 `perf`。
- 提交信息遵守仓库中英文双语规范，并记录真实执行的测试和构建。
- 不清理 `.build`；如需处理缓存，优先只处理 CMake 缓存并保留 `.build/Release/data/`。
- 每阶段合入前检查暂存区，只包含该阶段文件，不混入用户已有改动。

## 25. Lua API v2 稳定版完成定义

只有同时满足以下条件，才能宣布 Lua API v2 稳定：

- schema v1/API v1 包只能进入不执行代码的迁移/拒绝路径，正式宿主不存在 v1 API 注册。
- schema v2/API v2 的生命周期、权限、状态、任务、调度、数据和视图契约冻结。
- 第 12.6 节 v2.0 必选系统能力全部实现并冻结；system topic/task/action 目录与注册表、权限、LuaLS、文档、mock 和测试集合一致，不存在 `system.read`、`system.raw` 或原生调用通配旁路。
- 第 13.4 节 v2.0 必选节点及 node-property applicability matrix 全部冻结；每个节点的属性、事件、视觉状态、键盘和 UIA 契约均由共同契约源生成并通过测试。
- 11 个现有内置组件全部使用 schema v2/API v2，并逐个通过第 19 节完成清单。
- 现有内置组件实例、布局、存储和设置可以原地迁移及安全回滚。
- 用户授权发生在任何内置或第三方组件获得高风险能力之前；包来源信任不能代替用户同意。
- 权限授予、拒绝、撤销、更新扩权、回滚和启动恢复均经过实际验证。
- 公网与本机/局域网权限分离，并通过完整重定向、DNS 和实际连接地址测试。
- 所有公开 API 由共同契约源覆盖，文档、LuaLS 类型和测试一致。
- 包内图片和包私有字体通过资源契约、格式/额度校验、预览、设备丢失和卸载测试。
- 预览环境无外部副作用且能够覆盖加载、空数据、错误和权限拒绝状态。
- 系统/媒体 provider 仅在有效订阅存在时运行并共享采样；音频分析具有独立授权、可见使用状态、隐藏强制暂停和最后订阅立即释放。
- 声明式组件支持键盘、IME、高对比度、减少动态效果和 UI Automation。
- 节点级 hover/pressed/focus、click/double click、指针捕获、元素独立右键菜单和按需动画通过真实交互验收。
- Lua 逻辑槽位的单项 binding/replace/clear、集合 reference ingress/reorder/remove、宿主持久化/撤销、权限隔离和全有向 slot contract 矩阵通过；正式 API 不暴露原生 Slot/Container/拖放对象，也不支持真实文件转移、widget 嵌套或 Dock 保留路由。
- 性能不超过基线回归门禁，不存在渲染线程同步 I/O。
- `scripts/test.bat` 完整通过。
- `scripts/build.bat` 完成 Release 构建并生成 `.build/Release/SnowDesktop.exe`。
- 视觉、交互、安全和无障碍场景完成真实验收。
- API v2 文档、迁移指南、权限指南、作者工具和 11 个 v2 内置组件一同交付。
- WebView2 轨道不是 Lua v2 完成条件；未完成 W3 时默认保持延期，且不得把实验 API 计入 Lua v2 兼容承诺。

## 26. 外部设计参考

- Microsoft UI Automation Control Types Overview：<https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-controltypesoverview>
- Microsoft WinUI 控件索引：<https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/>
- Windows App SDK App Notifications：<https://learn.microsoft.com/en-us/windows/apps/develop/notifications/app-notifications/>
- Windows App SDK 文件与目录选择器：<https://learn.microsoft.com/en-us/windows/apps/develop/files/using-file-folder-pickers>
- Windows NetworkInformation：<https://learn.microsoft.com/en-us/uwp/api/windows.networking.connectivity.networkinformation>
- Windows Core Audio EndpointVolume API：<https://learn.microsoft.com/en-us/windows/win32/coreaudio/endpointvolume-api>
- Windows System Power Status：<https://learn.microsoft.com/en-us/windows/win32/power/system-power-status>
- Apple WidgetKit Timeline Provider：<https://developer.apple.com/documentation/widgetkit/timelineprovider>
- Apple 可配置组件：<https://developer.apple.com/documentation/widgetkit/making-a-configurable-widget>
- Apple 组件交互：<https://developer.apple.com/documentation/widgetkit/adding-interactivity-to-widgets-and-live-activities>
- Apple WidgetKit 可用 SwiftUI Views：<https://developer.apple.com/documentation/widgetkit/swiftui-views>
- Android Jetpack Glance：<https://developer.android.com/develop/ui/compose/glance>
- Android Glance 组件创建与响应式尺寸：<https://developer.android.com/develop/ui/compose/glance/create-app-widget>
- Android Glance UI：<https://developer.android.com/develop/ui/compose/glance/build-ui>
- W3C CSS Flexible Box Layout：<https://www.w3.org/TR/css-flexbox-1/>
- W3C WAI-ARIA 1.2：<https://www.w3.org/TR/wai-aria-1.2/>
- React Reactive Effects 生命周期：<https://react.dev/learn/lifecycle-of-reactive-effects>
- Rainmeter Skins、Measures、Meters 与更新周期：<https://github.com/rainmeter/rainmeter-docs/blob/master/source/manual/skins/index.html>
- WebView2 概览与支持平台：<https://learn.microsoft.com/en-us/microsoft-edge/webview2/>
- WebView2 Composition Controller：<https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2compositioncontroller>
- WebView2 安全开发指南：<https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security>
- WebView2 本地内容与虚拟 host 映射：<https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content>
- WebView2 用户数据目录与 profiles：<https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/user-data-folder>
- WebView2 进程故障与恢复：<https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/process-related-events>
- WebView2 Evergreen 与 Fixed Version：<https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/evergreen-vs-fixed-version>
- WebView2 `TrySuspend`：<https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_3>

这些参考只用于提炼调度、状态、布局、动作、数据分层和 WebView2 集成约束。SnowDesktop 的最终安全边界、Windows 集成、包格式和兼容承诺以本计划及仓库实现为准。
