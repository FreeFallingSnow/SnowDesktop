# 共享系统控制与 Lua 接入边界

本文保存共享设备服务的调用链及 Lua 控制边界。2026-09-26 在 `3e8373b3` 发现的 19 项缺口于 59 集中批次接线；当前工作区已包含 registry、七项独立权限、类型文档、执行桥和共享宿主提示，标准构建通过，自动验收／实机／发布尚未完成。最终验证见 [复核记录](STATUS_BAR_AUDIT_20260927.md)。

当前实施顺序见 [STATUS_BAR_IMPLEMENTATION_PLAN.md](STATUS_BAR_IMPLEMENTATION_PLAN.md) 第 6 项。本文是内部实施说明，不是公共 API 公告；实际编辑公共契约前，仍须按 [AGENTS.md](../AGENTS.md) 公告具体接口、权限、feature、兼容风险和旧宿主降级方式。

## 已有公开能力

公共真源是 [widget_api_registry.cpp](widget_api_registry.cpp) 中的 `HostFeatures`、`kSystemDataTopicContracts` 和 `kSystemTaskContracts`，权限真源是 [widget_permission_state.cpp](widget_permission_state.cpp)。不能用原生 `SupportsTask` 的返回值推断 Lua 已支持同名任务。

| 已公开读取主题 | 权限 | feature | Lua 值类型 |
| --- | --- | --- | --- |
| `audio.devices` | `audio.devices.read` | `data.audio.devices` | `SnowAudioDevicesDataValue` |
| `audio.input.volume` | `audio.input.read` | `data.audio.input.volume` | `SnowAudioInputVolumeDataValue` |
| `system.display.brightness` | `system.display.read` | `data.system.display.brightness` | `SnowDisplayBrightnessDataValue` |
| `network.wifi` | `network.wifi.read` | `data.network.wifi` | `SnowWifiDataValue` |
| `bluetooth.devices` | `bluetooth.read` | `data.bluetooth.devices` | `SnowBluetoothDevicesDataValue` |
| `system.power.plans` | `system.power.read` | `data.system.power.plans` | `SnowPowerPlansDataValue` |

上述六个主题已有结构化序列化、类型说明和确定性预览样本，见 [widget_system_control_data.h](widget_system_control_data.h)。`audio.output.default`、`audio.output.volume` 是更早的公开主题，继续保留原有类型和缺省值语义。

此前公开的非媒体设备写任务有两个，继续保留原参数、权限和结果：

| 任务 | 参数与返回 | 权限／feature |
| --- | --- | --- |
| `audio.output.setVolume` | `{volume: number}`；有限值由宿主截断到 0–1；成功结果 `SnowAudioOutputTaskValue` | `audio.output.control`／`task.audio.output.control` |
| `audio.output.setMute` | `{muted: boolean}`；成功结果 `SnowAudioOutputTaskValue` | 同上 |

两者要求当前可信用户手势，结果保留 `accepted` 字段。现有权限文案明确为“控制默认音频输出音量”，不能因增加设备选择任务而静默扩大既有授权。

十个媒体任务已公开：`media.play/pause/toggle/stop/next/previous/seek/setRate/setShuffle/setRepeat`，不属于下面 19 项缺口。已有 Lua `media.setShuffle` 参数叫 `shuffle`，原生共享服务参数叫 `enabled`；不能直接将两套参数解释互换。

`system.openSettings({page})` 已公开，要求 `shell.launch` 和可信手势；`audio`、`display`、`network`、`bluetooth`、`power` 页面可作为能力不足时的降级入口。页面白名单见 [widget_system_settings.cpp](widget_system_settings.cpp)。

## 59 批次新增的 19 项任务

以下名称与原生参数来自 [system_controls.cpp](system_controls.cpp) 的 `rules`；59 已增加严格 Lua 参数解析、task registry、写权限、feature 和类型重载。内部字符串化只用于宿主桥接，不允许组件传字符串代替数字或布尔值。具体公共契约已在编辑前向用户公告。

| 序号 | 任务 | 参数 | 权限组 |
| --- | --- | --- | --- |
| 1 | `audio.output.selectDevice` | `{endpointId: string}` | `audio.devices.control` |
| 2 | `audio.input.selectDevice` | `{endpointId: string}` | `audio.devices.control` |
| 3 | `audio.input.setVolume` | `{volume: number}`，有限值截断到 0–1 | `audio.input.control` |
| 4 | `audio.input.setMute` | `{muted: boolean}` | `audio.input.control` |
| 5 | `system.display.setBrightness` | `{monitorId: string, brightness: number}`，0–100 | `system.display.control` |
| 6 | `network.wifi.setRadio` | `{interfaceId: string, enabled: boolean}` | `network.wifi.control` |
| 7 | `network.wifi.scan` | `{interfaceId: string}` | `network.wifi.control` |
| 8 | `network.wifi.connect` | `{interfaceId, networkId?, profileName?, ssid?, hidden?, security?}`，约束见下文 | `network.wifi.control` |
| 9 | `network.wifi.disconnect` | `{interfaceId: string}` | `network.wifi.control` |
| 10 | `network.wifi.forget` | `{interfaceId: string, profileName: string}` | `network.wifi.control` |
| 11 | `bluetooth.setRadio` | `{radioId: string, enabled: boolean}` | `bluetooth.control` |
| 12 | `bluetooth.connect` | `{deviceId: string}` | `bluetooth.control` |
| 13 | `bluetooth.disconnect` | `{deviceId: string}` | `bluetooth.control` |
| 14 | `system.power.setPlan` | `{planId: string}` | `system.power.control` |
| 15 | `system.power.setMode` | `{mode: "balanced" / "efficiency" / "performance"}` | `system.power.control` |
| 16 | `system.power.lock` | 无参数 | `system.power.action` |
| 17 | `system.power.sleep` | 无参数 | `system.power.action` |
| 18 | `system.power.restart` | 无参数 | `system.power.action` |
| 19 | `system.power.shutdown` | 无参数 | `system.power.action` |

`network.wifi.connect` 的 `interfaceId` 为必填字符串；`networkId`、`profileName`、`ssid` 必须且只能提供一个非空字符串。`ssid` 不超过 32 字节，`hidden` 为可选布尔值，`security` 若提供只能为 `open`、`wpa2` 或 `wpa3`。`networkId` 由后端重新解析真实安全类型；直接指定 SSID 时必须获得明确安全类型才能连接。不得接受 `password`、`hostConfirmed`、任意 XML、脚本或系统命令参数。

下列七个权限组和 feature 为本批新增，尚未随宿主发布：

| 新增权限 | 新增 feature | 授权范围 |
| --- | --- | --- |
| `audio.devices.control` | `task.audio.devices.control` | 切换默认输入／输出设备 |
| `audio.input.control` | `task.audio.input.control` | 默认麦克风音量／静音 |
| `system.display.control` | `task.system.display.control` | 显示器亮度 |
| `network.wifi.control` | `task.network.wifi.control` | 无线开关、扫描、连接、断开、忘记 |
| `bluetooth.control` | `task.bluetooth.control` | 蓝牙开关及支持的设备连接／断开 |
| `system.power.control` | `task.system.power.control` | 电源计划及模式 |
| `system.power.action` | `task.system.power.action` | 锁屏、睡眠、重启、关机 |

使用独立写权限，保留原有读取权限语义；系统电源动作与计划／模式调整分开授权。新增权限应进入既有权限描述与授权流程，并同步所有 `lang/*.json`，不能只向 registry 填入字符串。

## 现有调用链与接线位置

1. Lua `task.start` 进入 [widget_engine.cpp](widget_engine.cpp) 的 `lua_TaskStart`，解析普通参数表，再由 `RuntimeStartTask` 取得实例 `runtimeToken`、权限和 `trustedGestureState_`。不要接受组件自行声明的可信手势。
2. [widget_task_broker.cpp](widget_task_broker.cpp) 的 `Start` 检查注册、权限、手势、实例／全局并发限制，生成 broker task ID。`InitializeWidgetTaskBroker` 根据 `SystemTaskContracts()` 注册任务，因此只在后端补 `SupportsTask` 不会使任务可用。
3. 当前输出音量任务仍由 `WidgetAudioOutputTaskExecutor` 执行；媒体也保留原执行器。新增任务经 [widget_system_control_tasks.cpp](widget_system_control_tasks.cpp) 连接 `WidgetSystemDataProvider::Controls()`，复用应用持有的 [Service](system_controls.h)，不新建设备服务或线程。
4. 原生面板与 Lua 复用 [system_control_prompt.cpp](system_control_prompt.cpp)。Lua 经应用回调提供宿主窗口、真实组件身份及仍授权检查，不需要先打开面板。网络 ID 必须匹配近期 `network.wifi` 缓存，密码保存在 `Secret`，开放网络不弹密码框。
5. 桥保存 broker ID、service ID、实例 runtime token 的对应关系，再进入 broker 完成和 `task.done` 派发。Cancel／Forget 精确取消对应 service ID，最后一个任务清理 consumer；服务直接给已脱离的 Work 标记 `detached`，不永久缓存退休实例代次。

若同时迁移旧输出音量任务到共享服务，必须保留参数、截断、结果和取消语义，单独评估由“系统接受”变成“回读匹配”可能增加的失败结果。不要以补齐这 19 项为由顺带改写媒体接口或控件语言。

## 手势、确认和结果

- 19 项均设置 `requiresTrustedGesture=true`，包括主动无线扫描；后台采样继续读取系统缓存，不自动发起扫描。
- `network.wifi.forget`、`system.power.sleep/restart/shutdown` 必须在有效手势和授权之外取得宿主确认。已有 `RequiresConfirmation` 与后端 `hostConfirmed` 校验继续保留。
- `network.wifi.connect` 未使用已保存 `profileName` 时，通过宿主窗口取得必要的凭据；开放网络不需要收集密码。宿主应重新解析目标，展示组件身份和真实网络／操作，不能接受组件定制的误导性确认文案。
- [system_controls.h](system_controls.h) 的 `Secret` 与 `Request.password` 是宿主内存专用，不能序列化进 Lua、任务参数、日志、配置或预览样本；取消和结束时清理输入及秘密缓冲。
- 宿主确认退出时先令 broker Shutdown 并释放控制桥，取消排队及在途任务；保留 engine／broker 至模态栈退出，阻止同一派发批次后续设备动作。
- 模态提示可能处理重入消息。提示结束后再次核验实例代次、任务取消和权限；已关闭组件、撤权或停机时不能继续提交。提示所有者与关闭回调必须由仍存活的宿主持有。
- 新任务可继续使用 `task.start` 的任务 ID 和 `task.done`，成功值复用既有 `SnowAcceptedTaskValue`。一般设备写操作的成功依据后端回读；最新数值仍从现有订阅主题获得，不把请求发出当成成功。
- 锁屏／睡眠／重启／关机的返回只表示 Windows 接受请求，不能表示用户已进入目标系统状态；进程退出后无法再观测完成。原生 `system_control_power.cpp` 已明确这一限制，公开文档必须保留。

## 生命周期与无副作用预览

- 每个 Lua 实例任务 consumer 应包含可区分重载的身份／代次；实例销毁调用 `RemoveConsumer`，单项取消调用 `Cancel(serviceId)`，并清理映射。完成回调只交付给相同 runtime token 的仍存活实例。
- 撤销写权限需取消对应 pending／active 任务和未完成确认；不能只阻止之后的 `task.start`。继续利用 broker 的 `PermissionRevoked`、`InstanceDisposed`、`Shutdown` 完成原因。
- `Service` 已有采样、执行／回读、Release 的物理来源互斥，ready 控制任务优先于过期采样；没有消费者的直接任务完成后也会安排释放。新适配器不得绕过这些所有权边界，也不得在执行期间直接释放后端。
- 应用统一给共享服务设置 `kWidgetTaskWakeMessage` 唤醒；桥和组件不重设 `SetWake`，应用退出时清除。桥没有常驻轮询；未打开的原生页面也不因 Lua 桥创建刷新计时器。
- 共享任务当前有总时限和取消标志；取消只能阻止后续处理，不能撤销已经提交的系统动作。同步 COM／驱动调用仍不能强制抢占，不得宣称超时能中断任意系统调用。
- `WidgetEngine` 将提示和服务桥的全部副作用放入 `DispatchSystemControlTask` 的 live 闭包。preview 在严格校验后返回，不进入闭包；CLI 仍不放松可信手势。回归分别覆盖真实 Lua 参数／拒绝路径以及成功预览不调用 live 边界，不能把两者混为硬件成功。
- 六个读取主题已有确定性预览；新增写任务应有可重复的成功／拒绝与取消检查，预览不要求真实设备，也不触发无线扫描、权限弹框、密码框或电源动作。

## 后端能力边界

| 后端 | 已有行为与公开时必须保留的限制 |
| --- | --- |
| [system_control_audio.cpp](system_control_audio.cpp) | 设备 ID 来自真实枚举；只选择匹配方向的活动端点。切换同时设置 console、multimedia、communications 三个默认角色；使用私有 Windows 策略 ABI，不支持时返回 `actionUnsupported`。音量／静音作用于执行时的默认 multimedia 端点。 |
| [system_control_brightness.cpp](system_control_brightness.cpp) | 内置屏使用 WMI，外屏使用 DDC；依据真实 monitor ID 重新查找、确认支持并回读，不用未知值伪装 0。同步驱动调用仍有平台阻塞边界。 |
| [system_control_wifi.cpp](system_control_wifi.cpp) | 读取使用缓存；扫描是显式任务。新配置支持开放网络和 WPA2/WPA3 Personal，其他认证返回 `systemSettingsRequired`。新建配置失败后仅在确认未使用时清理，未知状态保留；扫描等待最多 4 秒后读取缓存，连接按实际目标核对；不向 Lua 返回密码。 |
| [system_control_bluetooth.cpp](system_control_bluetooth.cpp) | 枚举已配对设备；连接／断开当前仅支持可解析的经典蓝牙音频设备，其他设备返回 `systemSettingsRequired`。没有配对、取消配对或通用 BLE 连接任务；Lua 必须尊重 `canConnect/canDisconnect`。 |
| [system_control_power.cpp](system_control_power.cpp) | 模式仅在系统支持且计划兼容时可用，`setMode` 同时设置 AC／DC。敏感动作保留确认及系统权限检查，重启／关机不强制关闭其他应用。没有本批次可用的休眠任务。 |

设备消失、策略拒绝、硬件不支持和读回不一致均是实际可能的结果；不要通过复制请求值或假成功消除它们。本批在公共文档中说明错误语义，不直接暴露内部平台码成为新的未审查契约。

## 兼容与发布顺序

- 增量添加任务／权限／feature 可保持 Lua API v2，不修改既有任务参数与数据字段；这只是兼容方案，实施后的兼容结果仍须测试。
- 组件应检测实际新增 feature／capability。旧宿主和同版本早期构建不能按版本号推断支持；缺失时隐藏写入口，或在用户手势下调用已支持且获授权的 `system.openSettings`。其本身也不可用时，仅保留读取状态。
- 新组件声明 `minHostVersion: "1.0.8.0"` 并检测具体 feature；最低版本不能替代 feature，同版本早期构建仍需降级。
- 官方社区组件使用新任务前，先发布包含完整契约的宿主，再发布组件，并再次公告旧宿主／缺失 capability 时的行为。不能因为原生控制中心已能执行而提前发布依赖这些 Lua 任务的组件。

## 实施文件和验收落点

| 范围 | 现有文件／目标 |
| --- | --- |
| registry、feature、参数／结果类型名、能力导出 | `widget_api_registry.cpp`、`widget_api_contract_json.cpp`；后者消费 registry，避免新增第二份任务名单 |
| 独立权限和全部语言文案 | `widget_permission_state.cpp`、`../lang/*.json` |
| 参数校验、服务桥、完成／取消／撤权／清理 | `widget_engine.cpp/.h`、`widget_task_broker.cpp`；共享执行继续使用 `system_controls.cpp/.h` |
| 宿主确认、凭据和模态取消 | 复用已提取的 `system_control_prompt.cpp` 提示设施，接应用持有的回调；不依赖面板模型存活 |
| Lua 类型和正式说明 | `../widgets/snowdesktop-lua-widget/library/snowdesktop-v2.lua`、`../widgets/snowdesktop-lua-widget/references/api-v2.md` |
| 契约、权限、手势和实例生命周期 | 现有 CTest `widget_api_registry`、`widget_permission_state`、`widget_task_broker`，按实际接线补充引擎相关回归 |
| 预览零副作用 | 现有 `widget_author_preview_cli` 验证真实 Lua 参数／权限拒绝；`widget_system_data_provider` 验证 Dispatch 成功预览不进入任何 live 副作用 |
| 后端取消、释放与竞争 | `tests/system_controls_tests.cpp` 隶属 CTest **`widget_system_data_provider`**，可执行目标 `SnowDesktopWidgetSystemDataProviderTests`；不存在单独的 `system_controls` CTest 条目 |

最低回归应覆盖 19 项注册及 feature／权限对应、严格参数类型与未知键拒绝、缺少手势／权限、取消确认、撤权和实例重载后的迟到结果、真实 ID 失效、并发采样／执行／释放、预览零系统调用及旧输出任务兼容。竞争场景用可控同步门复现，不以固定 sleep 猜测调度。

这是公共 API 与异步跨模块改动，本批稳定后按仓库规则执行标准构建及有效全量；实机设备和危险系统动作仍分别记录授权与实际验收，不能用预览成功代替。当前仍为源码静态核对，统一构建与测试结果随后登记；未执行真实设备或危险电源动作。
