# Wallpaper Engine 重复覆盖通知验证（2026-10-20）

关联：[Issue #29](https://github.com/FreeFallingSnow/SnowDesktop/issues/29)。
候选源码：`15476bf4dceb31737dc0df47aef41df217dd60b9`。

本记录区分可观察的壁纸覆盖通知与画面重播。报告者的视频和实际播放体验仍需原环境验收，
不能由通知计数、源码边界检查或构建通过替代。此记录不表示已向 Steam `public` 交付，
也不构成关闭 Issue 的依据。

## 复现对象与环境

- Windows 11 26H2，OS Build `26300.9807`。
- Wallpaper Engine 界面版本 `2.8.42`，文件版本 `2.8.0.42`。
- 凌晨的各组试验保持同一 Wallpaper Engine 进程，只按需重置 Explorer；上午的进一步隔离
  重启了 Wallpaper Engine，以排除进程内残留 Hook，详见后文。
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

该轮对照记录了一个可复现组合，但没有证明托盘采集或应用通知是必要条件。后续纯 Win32
试验进一步缩减了触发步骤，见下文；Wallpaper Engine 或 Explorer 内部的具体反馈实现仍未确定。

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

## 上午的纯 Win32 缩减与进程清洁检查

独立程序 [wallpaper_minimal_repro.cpp](../../tests/shell/wallpaper_minimal_repro.cpp)
不依赖项目头文件或 DLL。`workerw none` 模式只执行一次以下调用，然后通过自身的隐藏窗口
记录通知；不建立托盘采集，不注入 DLL，不向应用发送 `TaskbarCreated`：

```cpp
SendMessageTimeoutW(FindWindowW(L"Progman", nullptr), 0x052C, 0, 0,
    SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &result);
```

`observe none` 模式省去此调用。默认 `--self-test` 只向自身窗口发送消息；
`--check-environment` 只检查进程。手动诊断拒绝仍在运行的 SnowDesktop、多个 Wallpaper Engine
进程、Explorer 的托盘驻留属性，以及 Explorer 或 Wallpaper Engine 内的 `SnowDesktop*` 模块。

09:19 的组合试验虽由用户确认重播，09:21 的官方完整日志却显示 Wallpaper Engine 内仍加载
`SnowDesktopWallpaperHook.dll`。退出主程序不足以排除这个残留因素，不能把该次结果直接
归因于外部软件。随后由用户彻底退出并重新启动 Wallpaper Engine，保持覆盖选项关闭。
09:25、09:26 的模块检查均确认新的 Wallpaper Engine 与 Explorer 没有 SnowDesktop 模块。

### 已完成的干净正对照

- Wallpaper Engine PID `57248`，启动于 09:24:40；Explorer PID `13284`，09:25:27 重建。
- 09:25:31.872 仅发送一次 WorkerW 请求。09:25:32.303 收到的 `TaskbarCreated` 来自真实
  Explorer 启动，并非诊断程序投递。
- 随后由用户开启“覆盖壁纸图像”、确认并关闭设置和浏览器；用户反馈“已开启，重播”。
- 09:26:00.474 至 09:28:44.884 记录到 24 次 `SPI_SETDESKWALLPAPER` 通知，间隔
  6.981～7.398 秒，平均 7.148 秒。请求总数始终为 1。
- 触发程序 09:28:51.887 退出后，独立的无动作观察器仍在 09:28:59、09:29:06、09:29:13、
  09:29:20 收到更新。后续关闭覆盖及重置 Explorer 的操作不计入这个退出后观察区间。
- 用户在该次重播中导出的 `info-workerw-only.txt` 没有 SnowDesktop DLL 条目，SHA256 为
  `0185DFFD3057063036B14638503B699A01B4917E471AC56ABDC8B9A806B20FFD`。

这一组排除了托盘采集、应用通知和 SnowDesktop Hook 为该触发路径的必要条件；约 7 秒的
持续更新不是由本次程序按该周期重复发送请求形成。通知计数仍不等于画面重播次数，画面
重播由用户实测确认。尚不能据此断定 Wallpaper Engine 内部是哪段代码形成循环。

### 空白对照与复测

09:30 的首次空白对照因覆盖选项未在预定时刻真正关闭而作废，不作为阴性证据。
09:31:24 与 09:33:25 直接读取配置，均确认 `overridewallpaper=false`，随后由用户重启
Wallpaper Engine。新的 PID `68884` 启动于 09:33:56，无 SnowDesktop 模块。
09:34:22 重建 Explorer 后启动纯观察模式；用户重新开启覆盖并反馈“已开启，无重播”。
09:34:48.132 仅出现一次开启覆盖相关更新，至 09:37:42.975 的 200 秒完整观察结束没有
新增更新；WorkerW 请求总数为 0。09:35:04 直接读取配置确认覆盖已开启，Wallpaper Engine
仍为 PID `68884`，无 SnowDesktop 模块。对照日志为 `fresh-noop-enable-after.log`。
该组已要求关闭设置和浏览器，但用户没有单独确认关窗状态，因此不把它写成覆盖全部窗口
状态因素的严格对照；不为这个细节追加重复试验。
保留同一 Wallpaper Engine PID `68884` 和 Explorer PID `40588`，用户关闭覆盖后，
09:38:30.334 使用 `7119bdb6` 程序只发送一次 WorkerW 请求；没有重启 Explorer，也没有
发送应用通知。用户重新开启覆盖并反馈“已开启，重播”，09:39:00 起记录到连续约 7 秒
一次的通知。此次请求发生在 Explorer 启动四分钟后，排除了必须赶在 Shell 启动阶段的条件。
用户随后明确说明设置窗口当时仍打开、开关已在运行中生效；磁盘配置尚未写回，不能把
其旧值用于否定运行状态，也不能将该阶段写成已经关闭设置和浏览器。随后用户关闭两个
窗口并明确反馈仍重播；09:41:19 读回配置为开启，两个目标进程仍无 SnowDesktop 模块。

09:39:00.122～09:41:47.303 共记录 25 次更新，平均间隔 6.966 秒，请求总数为 1。
09:41:50.343 触发程序退出后，独立无动作观察器继续在 09:41:53.964、09:42:00.898、
09:42:07.936、09:42:15.001 及以后收到更新。对应日志为 `settled-workerw-enable-after.log`
和 `settled-after-trigger-exit.log`。这次正例使用的就是带模块前置检查的交付二进制。
独立观察于 09:44:08.852 正常结束；触发程序退出后仍有 20 次更新，最后一次为
09:44:06.936，距触发程序退出 136.593 秒。

当前结论是本机已验证的充分复现顺序：一次 WorkerW 请求后启用覆盖，即可出现持续重播，
无需托盘采集、应用通知或 SnowDesktop Hook；不是 SnowDesktop 每约 7 秒重复发起该请求。
不据此断言 WorkerW 请求是所有重播路径的必要条件，也未确定 Windows/Wallpaper Engine
内部维持该状态的具体代码位置。设置打开与关闭均已取得正例，无需再为这点追加试验。

### 独立程序构建来源

| 提交与协作批次 | 实际结果 |
| --- | --- |
| `a8b83115`；`75957ac7bb7342748bd36b5cc92b35cf` | 标准 Release 构建通过；`wallpaper_minimal_repro` 与 `wallpaper_shell_probe` 首次通过，2/2，总耗时 0.47 秒 |
| `7119bdb6`；`2f4362d626ef4329aedae20cacf2c9d1` | 增加模块残留拒绝与只读环境检查；标准 Release 构建通过；`wallpaper_minimal_repro` 首次通过，1/1，总耗时 0.38 秒 |

两批使用共同 CTest 预设的并行度 24，相关原生窗口条目按声明互斥；无编译或链接警告，
未执行应用全量。第一组干净正对照使用 `a8b83115` 的诊断程序，其 SHA256 为
`4243C2F202C46800EA98179E139B97ED5509D20E923A8C67D09F41F4B14EB1ED`；进程模块清洁性
由同场外部检查确认。增加前置模块检查的 `7119bdb6` 程序 SHA256 为
`48B9BE55B5E7A6B61AEB591DE5B7B5055D80545B3A33EADB17368059E7E1F5BC`。

此轮未修改生产行为。原始证据另存于 `.codex-probes/issue29-minimal-20261020/`。
官方完整日志仅用于本地邮件草稿附件，不随 Git 提交或公开上传。
09:44:26 为清理实验状态重置 Explorer，保留同一 Wallpaper Engine 进程及覆盖开启状态；
至 09:45:26 的 60 秒收尾观察仅出现 09:44:35 的一次初始化更新，无持续通知序列。
所有本轮诊断程序均正常退出；SnowDesktop 保持用户为实验准备的关闭状态。

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
