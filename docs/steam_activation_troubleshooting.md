# Steam 高级功能注册排查

Steam 客户端已打开，不代表已经连接 Steam 服务。注册要求当前账户在线且拥有
SnowDesktop；客户端底部显示“无连接”时，应先恢复连接再注册。

## 收集证据

1. 记录 SnowDesktop 版本、Steam 测试分支、Windows 版本，以及失败的大致时间。
2. 在设置的“常规 → 高级功能”点击“通过 Steam 注册”，保留完整提示。
3. 点击设置中的“打开数据目录”，收集 `SnowDesktop.log`；若存在
   `SnowDesktop.log.1`，一并收集，避免本次记录已被轮转。
4. 搜索 `[SteamActivation]`，按同一 `id` 查看一次注册的请求、桥接调用与最终结果。

无需启用调试日志。Steam 托管安装的数据目录位于安装根的 `data`，不在
`.snowdesktop/runtime` 的版本目录中。日志保留本地路径，分享前可遮盖私人路径。
日志不记录完整 Steam ID 或受保护解锁缓存内容；SDK 错误中的长数字标识会脱敏。
日志写入仍依赖数据目录可写；若整个数据目录禁止写入，现有日志入口也可能无法落盘。

## 按提示定位

| 提示类别 | 下一步 |
| --- | --- |
| 无法连接 Steam 客户端 | 启动 Steam；已启动时，核对 Windows 用户与管理员权限等级 |
| 离线或未登录 | 恢复网络并在线登录，确认客户端不再显示“无连接” |
| 客户端版本不兼容 | 更新并重启 Steam |
| Steam 接口不可用 | 更新、重启 Steam；仍失败时验证 SnowDesktop 文件完整性 |
| 应用身份不匹配 | 从 Steam 库启动 SnowDesktop，验证文件完整性 |
| Steam Bridge 不支持 Steam 功能 | 检查安装来源，并在 Steam 中验证文件完整性 |
| 其他初始化失败 | 重启 Steam，从 Steam 库启动 SnowDesktop，并提供错误详情 |
| 当前账户未拥有 SnowDesktop | 核对当前登录账户的所有权 |
| 所有权已验证，但保存失败 | 检查数据目录写入权限、磁盘空间及文件占用 |

初始化结果 `2` 只表示当前进程无法连接客户端，不能证明 `steam.exe` 未运行。
分类不足以确定根因时，保留通用提示和 SDK 原始错误，不根据错误文本猜测根因。

## 日志字段

| 事件或字段 | 用途 |
| --- | --- |
| `entry=manual registration` / `startup revalidation` | 区分手动注册和启动复核 |
| `configuration` | 核对宿主/Bridge 版本、协议、App ID、SDK 支持及兼容性 |
| `registration requested` | 注册编号、宿主 PID、会话与提权状态 |
| `bridge begin` / `bridge started` | 实际 Bridge 路径、命令、超时设置及子进程 PID |
| `bridge finished` | 退出码、输出字节数、耗时 |
| `bridge failed stage=... win32_error=...` | 启动、管道、等待、取消、超时等具体失败位置 |
| `ownership fields` / `ownership response` | App ID、在线状态、所有权结果及 SDK 错误详情 |
| `steam_init_result` | `1` 通用失败，`2` 无法连接客户端，`3` 客户端版本不兼容；`absent` 表示未提供 |
| `cache read` / `cache decrypt` / `cache schema` | 本地缓存读取、DPAPI 解密及格式检查 |
| `cache save success=0` | 保存失败的具体文件操作错误 |
| `registration complete` | 最终解锁状态、失败类别、有效期及结果是否已发布 |

`registration complete` 的 `failure`：`0` 无失败、`1` Steam 不可用、`2` 未拥有、
`3` 桥接/响应错误、`4` 保存错误。`registered=1` 且存在 Steam 失败，可能表示本次
复核失败但此前的离线有效期尚未结束，不代表本次联网验证成功。

这些是宿主激活日志，不包含独立创意工坊管理器的操作日志。管理器会在提示下保留
错误码和详情，应同时收集。CLI 的可选诊断字段及兼容策略见
[Steam Bridge 说明](../steam_bridge/README.md#initialization-diagnostics)。
