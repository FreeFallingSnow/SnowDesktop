# Wallpaper Engine 重复覆盖通知验证（2026-10-20）

关联：[Issue #29](https://github.com/FreeFallingSnow/SnowDesktop/issues/29)。
候选源码：`15476bf4dceb31737dc0df47aef41df217dd60b9`。

本记录区分可观察的壁纸覆盖通知与画面重播。报告者的视频和实际播放体验仍需原环境验收，
不能由通知计数、源码边界检查或构建通过替代。此记录不表示已向 Steam `public` 交付，
也不构成关闭 Issue 的依据。

## 复现对象与环境

- Windows 11 26H2，OS Build `26300.9807`。
- Wallpaper Engine 界面版本 `2.8.42`，文件版本 `2.8.0.42`。
- Wallpaper Engine 全程保持同一进程；各隔离试验只按需重置 Explorer。
- “覆盖壁纸图像”原为开启，“覆盖锁屏图像”原为关闭。顺序试验临时关闭前一项，
  在候选启动后重新开启；结束后恢复原值。
- Wallpaper Engine 浏览器关闭后才开始判定持续表现。打开浏览器期间曾观察到通知暂停，
  因此浏览器保持打开时的阴性结果不足以说明播放正常。
- 未捕捉或操作 SnowDesktop 桌面宿主界面。完整应用试验采用正常启动、控制 IPC 正常退出、
  自有隐藏观察窗口接收系统通知、壁纸覆盖文件变化及应用日志作为证据。

## 触发组合与调整

旧候选在完整应用中可出现约每 7～9 秒一次的 `SPI_SETDESKWALLPAPER` 通知，
同时重写 Wallpaper Engine 覆盖图像。退出 SnowDesktop 后仍持续；重置 Explorer 后停止。
只保留 Dock、关闭软件桌面、状态栏、任务栏美化、永久隐藏和 Windows 自动隐藏，仍可复现。

独立原生对照进一步复现了组合顺序：先向 Progman 请求创建 WorkerW，随后建立生产托盘
采集并按生产收件人规则请求应用重新注册图标。单独请求 WorkerW、单独投递通知以及
单独运行同一托盘 Service，均未在各自对照窗口中形成相同的持续序列。两次 Service 对照
使用的 Hook `.text` 均为 257536 字节，SHA256 为
`7E8BB3E459C77847011D33B10F7CF94F798B22EA94006683322E7F1800248821`。

候选移除启动和真实 Explorer 恢复期间的额外 WorkerW 创建请求。覆盖层继续挂到
Explorer 现有 DefView 的父窗口；保留托盘采集、重新注册握手与排除 Explorer 的收件人规则。
删除未再使用的内部 `EnsureDesktopWorkerWindow` 声明和实现。

此对照确认了可复现的触发边界，未确定 Wallpaper Engine 或 Explorer 内部形成反馈的具体实现。

## 标准构建与定向回归

通过协作批次 `adfdd4e9b24c4ba0b4afe7a549adfb52` 实际运行标准 `scripts/build.bat`
及选择的三项原生测试，构建输入起止摘要一致：
`336db9cac372bea32e09e1071e424380ac2bd291d9c7cdbae8b4f769b4e1968c`。
构建时 HEAD 为 `79819091`，候选改动尚未提交；随后以相同文件内容保存为 `15476bf4`。

| 项目 | 结果 |
| --- | --- |
| 标准 Release 主程序和运行库 | 通过 |
| `dock_and_window_rules` | 首次通过，36.80 秒 |
| `shell_integration_contract` | 首次通过，0.04 秒 |
| `wallpaper_shell_probe` 默认自检 | 首次通过，0.47 秒 |
| 本组选中测试 | 3/3，失败 0、跳过 0、未运行 0，实际总耗时 37.40 秒 |
| 编译与链接警告 | 完整批次日志中未发现 |

CTest 使用共同预设的 `execution.jobs=24`；本组涉及原生窗口的条目按声明互斥执行。
没有执行或声称通过应用全量测试。

候选主程序 SHA256：
`9822A575BED60DF7429BBFA777BAD2C650CE5F1C9B533A333B9FF32FEF94769E`。
新增探针 EXE SHA256：
`753DBB9AD414FEE00AECA2E92904EB1F67292941D577D555A2341233426960EA`。

## 完整应用场景

时间均为 Asia/Shanghai。

| 场景 | 实际步骤与结果 |
| --- | --- |
| 先启动软件，再开启覆盖 | 01:27:47 启动候选，保留原开发目录的软件桌面、Dock、状态栏及任务栏设置；01:29:39 核对覆盖开启；01:29:57 关闭浏览器。01:29:59 出现一次初始化壁纸通知，至 01:33:23 下一实验前无新增壁纸通知 |
| 桌面启动 | 日志确认 01:27:51 首帧就绪，随后完成桌面交接；未把单纯进程存在当作桌面已经初始化 |
| 保持应用运行，重启 Explorer | 01:33:23 重置 Explorer；SnowDesktop 和 Wallpaper Engine 的进程及启动时间均不变。托盘重新连接到新 Explorer，`resync=0`；180 秒观察中只有 01:33:34 一次壁纸通知 |
| 正常退出后的同会话观察 | 01:36:55 候选正常退出，不重置 Explorer。观察至 01:37:53 无新增壁纸通知；只记录退出相关工作区变化 |
| 覆盖预先开启时再次启动 | 01:43:01 恢复完整候选运行；至 01:45:35 的约 154 秒启动后观察仅记录 01:43:09 一次壁纸通知。观察器另记录启动前 Explorer 初始化的一次更新，不计入启动阶段 |

完整应用启动时的 `incomplete=0` 只表示已收到的托盘图标没有缺失回调或像素，
不表示所有原生冷图标均已补采，不能据此宣称初始快照完整。

## 原生自动化对照

探针源码：[wallpaper_shell_probe.cpp](../../tests/shell/wallpaper_shell_probe.cpp)。
默认 CTest 只向自有隐藏窗口发送消息。下列场景为显式手动诊断，会操作真实 Shell；
必须退出 SnowDesktop，并在每个场景前建立新的 Explorer 会话。保持同一动态壁纸及覆盖选项，
关闭 Wallpaper Engine 浏览器，避免其他测试同时使用桌面资源。

先用无动作观察让 Explorer 和壁纸完成初始化：

```powershell
& $probe observe - $baselineLog 15 --expect-stable
```

旧触发组合的正对照：

```powershell
& $probe workerw-tray-applications $hook $positiveLog 60 --expect-repeated
```

省去额外 WorkerW 请求的对照，在另一个新 Explorer 会话及同样的无动作观察后运行：

```powershell
& $probe tray-applications $hook $negativeLog 60 --expect-stable
```

`$probe` 为构建生成的 `SnowDesktopWallpaperShellProbeTests.exe`，`$hook` 为与本轮匹配的
`SnowDesktopTaskbarHook.dll`。可将两者按字节复制到独立诊断目录并核对 SHA256，避免与共享
输出重建争用文件；复制不等于另一次构建。每个动作结束后还观察 15 秒。
正对照可能留下持续状态，运行后须重置 Explorer 清理。

`--expect-repeated` 在至少三次壁纸通知时通过；`--expect-stable` 在最多两次时通过。
这一阈值用于受控环境的初始化更新与重复通知对照，不是一般桌面会话的播放判定算法。
探针不会把壁纸通知次数当成画面重播次数。

| 本轮原生场景 | 结果 |
| --- | --- |
| 正对照前无动作观察 | 1 次壁纸通知，稳定断言通过 |
| WorkerW 后应用通知 | 10 次壁纸通知；退出采集后仍有通知，重复断言通过 |
| 省去 WorkerW 的对照 | 新会话基线 1 次；动作及退出后观察合计 1 次壁纸通知，稳定断言通过；采集退出握手成功，`resync=0` |

## 证据与限制

本机完整原始证据保留于 `.codex-probes/issue29-20261020/`，包括环境、进程身份、
二进制指纹、设置恢复记录、通知日志和各次正常退出/Explorer 重置回执。
构建结果、覆盖与 CTest 报告位于 `.build/collaboration/` 和 `.build/Testing/`。
原始日志可能包含个人文件路径，不随本记录纳入 Git。

另行执行的 `tray_live_integration` 在不响应 `TaskbarCreated` 的冷图标补采断言失败。
本机准确匹配的 Taskbar PDB 获取失败，`resolverError=2`；`modernTrayError=997` 是
相应阶段尚未执行时保留的默认值，不能将它另当作实际根因。
本候选未更改冷图标补采机制，不声称这项集成诊断通过。

报告者机器的可见播放、托盘菜单和 Dock 交互仍须按原场景复测；正式交付渠道及 Build ID
应按仓库 Issue 发布规则记录，不能以本地构建或这份验证记录关闭修复类 Issue。
