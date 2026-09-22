# 2026-09-22 15:27 本地首图路径复验

验收对象：`c70a0c4218fc5df3b1c909d2a0338158c6ad2621`。
本地首图路径在实际启动中生效，但启动长时间占位仍复现；用户进一步确认
“点击、拖动或右键也明显失去响应”。整体问题验收失败，不能只按后台图标排队结案。

## 运行身份与证据

- 启动时间 `2026-09-22 15:27:04.283`，PID `68672`，run `68672-182900625`，
  主线程 `72628`（十六进制 `11bb4`）。进程路径为
  `D:\Code\Other\SnowDesktop\.build\Release\SnowDesktop.exe`。
- EXE SHA256 `4d7aa8d939df6dc8211a818cbc7e1ba00e487add89009adc33d24c5846f4df55`，
  修改时间 `15:17:18.506`，匹配该尝试全量测试后的最终产物。
- 日志发生轮转，启动记录在 `SnowDesktop.log.1` 第 2311 行；本次同时保留两个文件，
  没有将 `SnowDesktop.log` 文件头误当作新启动。
- 本地证据目录 `.codex-probes/startup-local-runtime-20260922/` 保存原始 UTF-16LE 日志、
  解析收据与恢复后线程栈。两个归档日志 SHA256 分别为：
  - `.1`：`4281c7c5d63cb6dbaf699f2da1d03b8c113a2cc22e149bc2b4b5b146d746487e`
  - 当前文件：`95ce91e803e9c4cf5b94e3e0a2e50eed72174bd585b8781c11bc4e295861fb6d`

## 已观察结果

本轮记录 171 次本地首图请求，其中 97 次得到位图，74 次未得到位图而进入回退。
本地读取时间最大 16 ms，排队时间最大 141 ms；这些是请求计数，不等同于桌面唯一条目数。
第一条本地成功日志在 `15:27:04.939`，最后一条本地请求在 `15:27:05.739`。
日志记录的是后台取得位图，不能据此断言用户已看到全部图标，也不能证明交互正常。

首帧 `1093 ms`，桌面交接 `1406 ms`。仍然缓慢的具体调用如下：

| 通道 / 对象 | 调用 | 等待 | `.1` 日志行 |
| --- | --- | --- | --- |
| 完整桌面，第 19 次请求 | `IEnumIDList::Next` | 47531 ms | 2703 |
| 下载.lnk，首图回退线程 66568 | `IPersistFile::Load` | 46797 ms | 2706 |
| 图片.lnk，首图回退线程 65664 | `IShellItemImageFactory::GetImage` | 46859 ms | 2721 |
| AfterFX.lnk，分类线程 70832 | `SHCreateItemFromParsingName` | 47219 ms | 2704 |
| Adobe Media Encoder.lnk，分类线程 11116 | `SHCreateItemFromParsingName` | 47219 ms | 2705 |
| 两个 Adobe 项，细节线程 72152 / 16396 | `SHGetFileInfoW` | 各 47250 ms | 2708 / 2715 |

下载和图片占满默认两个首图回退线程，后来的文档等项目排队达到约 47.6 秒。
因此本地路径保护了能直接读取的图标，却没有消除回退队列内部的队头阻塞。
两条细节通道和两条分类通道也分别被占满。

这些底层慢调用在 `15:27:52.468–15:27:52.541` 集中返回。完整桌面读取共 `48703 ms`，
于 `15:27:53.525` 结束；完整快照在 `15:27:59.886` 应用，应用本身 `234 ms`。
期间有重新读取和交互保护，不能把读完到应用的全部间隔认定为主线程阻塞时间。

## 交互冻结的证据边界

1. 用户实际确认点击、拖动、右键失去响应；仅解释工作线程排队不足以解释或关闭该问题。
2. 已定位的七个慢调用来自后台线程，日志没有记录当时主线程的调用栈。因此暂不能证明
   主线程在等哪个 Shell API、锁或输入状态，也不能把后台等待时长直接当成主线程冻结时长。
3. 只读源码检查发现右键构建仍有主线程同步 Shell 路径，例如
   `src/app/app_shell_menu.cpp` 的 `ShowDesktopBackgroundContextMenu` 中
   `CreateViewObject` / `QueryContextMenu`。这是候选风险，不是本轮已复现的直接根因。
4. 随后对现有 PID 使用 Windows SDK CDB 非侵入检查，命令为
   `cdb -pv -p 68672 -y .build/Release -c "~* k 30; qd"`，成功退出。
   采样时主线程已处于 `DesktopApp::Run -> NtUserMsgWaitForMultipleObjectsEx`；
   这是恢复后的消息等待状态，不是冻结现场，不能用它否定用户反馈或还原先前阻塞。
   非侵入检查期间调试器短暂暂停进程读取栈，未重启、结束或操作宿主界面。
5. 多通道同时返回支持共同 Shell 依赖的推测，但无法确定具体服务、扩展、锁或网络目标。
   第 19 次 `Next` 只是请求序号，仍不能确定将返回的对象，不能归责于某个应用。

后续必须抓取冻结期间主线程与 Shell 工作线程的同期栈，或补充主线程消息/同步调用分段
诊断，区分同步调用阻塞与输入状态未解除。普通文件夹、文档和特殊快捷方式的首图仍需
缓存或无需等待 Shell 的展示路径；继续增加同一回退队列线程数不能证明能够解除交互冻结。

本轮仅读取日志、源码、进程身份和线程栈并记录失败验收，未修改生产代码，未运行新的
构建或自动测试。该尝试此前的标准 Release 构建和 120/120 全量结果仍是同一代码输入的
已有证据，不能代替本次失败的实际交互验收。

## English

Runtime verification of `c70a0c42` failed overall: long placeholders remain and the user confirmed
that clicking, dragging or right-clicking also becomes unresponsive. The matching development host
started at 15:27:04.283. Of 171 local first-image requests, 97 produced bitmaps and 74 required
fallback; local work took at most 16 ms and local queueing at most 141 ms. These worker results do
not prove that pixels were presented or that interaction remained responsive.

Both Shell first-image fallback workers were occupied by Downloads (`IPersistFile::Load`, 46.797 s)
and Pictures (`GetImage`, 46.859 s). Full desktop enumeration waited 47.531 s on its 19th `Next`
request; Adobe classification and metadata also waited about 47.2 s. Their near-simultaneous return
suggests a shared Shell dependency but identifies neither an internal cause nor a faulty application.
Full enumeration took 48.703 s. First frame was 1.093 s and final snapshot application itself 234 ms.

The observed slow calls belong to background threads. The UI freeze is confirmed by user feedback,
but its direct call or input-state cause is not established. A later successful noninvasive CDB
inspection found the main thread back in its message wait; this recovered-state sample cannot
explain or disprove the earlier freeze. The debugger briefly suspended the process for inspection,
then detached without restarting or terminating it. Synchronous UI Shell menu paths are code-derived
candidates only. A simultaneous in-freeze stack capture or UI call diagnostics is still needed.

This turn changed only the verification record, with no new build, test run or production change.
The prior standard build and 120/120 full test result remain existing evidence, not runtime acceptance.
