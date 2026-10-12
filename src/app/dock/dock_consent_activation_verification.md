# Dock 授权提示激活验证

验证日期：2026-10-10（Asia/Shanghai）。

对应尝试：`630244b17418dc8eed6f7c08ffe7eb7ea3d045b1`。

## 复现与结果

原场景是后台的 Windows 授权提示已经显示在 Dock 中，但点击图标无法唤出实际授权界面。用户确认 Windows 自带任务栏能够唤出同一提示。

先前对普通桌面代理窗口执行恢复、默认窗口处理与前台请求，日志虽出现代理窗口的前台状态，用户仍未看到授权界面。这一结果为失败，不能用代理窗口的前台状态替代实际验收。

本次候选针对已点击的授权代理窗口请求一次 Windows 任务切换。通过带有私有隐藏 owner 的 `ShellExecuteExW` / `runas` 调用 `C:\Program Files\WinRAR\Rar.exe -?` 生成测试授权；Agent 没有自动批准授权，也没有使用桌面自动化操作 SnowDesktop。

实际验收问题：

> 这次点击新版 Dock 中的授权图标，授权提示是否到前台了？

用户答复：

> 已到前台

这次反馈验证的是新版 Dock 的实际点击入口。此前仅直接调用 Windows API 的成功反馈单独保留，不作为 Dock 点击成功的替代证据。

## 候选与日志

运行候选来自标准 Release 构建批次 `3ef4e7bde1514d4ca89ca6f0c1ac16a7`。该批次执行 `scripts/build.bat ready-and-wait shell-shortcut-elevation-20261009 -Batch 3ef4e7bde1514d4ca89ca6f0c1ac16a7 -Revision 4 -ReloadShell`，Release 构建通过，应用测试 130/130 通过（并行度 4，CTest 190.24 秒）。本次只新增验证记录，未再次编译或执行全量测试。

实际运行的 `.build/Release/SnowDesktop.exe` SHA-256：

`f3556f43441c179a763a09763b5aeda118de29717094a7883d1424cfe227a444`

日志在本地时间 00:03:34 和 00:03:40 分别记录了两个测试授权代理的 `consent-task-switch-queued`、`consent-task-switch-request` 和 `consent-task-switch-returned`。这些阶段只证明请求经过了新入口；实际授权界面可见的结论来自用户反馈。

忽略目录中的详细证据：

- `.build/verification/dock-consent-630244b-20261010/result.json`
- `.build/verification/dock-consent-630244b-20261010/dock-activation.log`
- `.build/collaboration/3ef4e7bde1514d4ca89ca6f0c1ac16a7.log`

## 验证边界

- 验证了当前机器上授权图标的 Dock 点击唤出行为；没有验证其他 Windows 版本的授权代理兼容性。
- 此结果不包含 PowerShell 菜单首次发起授权时自动显示到前台的验收。
- WinRAR 普通文件菜单探测仍存在入口缺失；该独立失败保存于 `.build/verification/shell-menu-dispatch-20261009/result.json`，不因 Dock 验证成功而关闭。

## English record

The user reproduced the original failure: the consent prompt had a Dock icon, but clicking it did not reveal the actual consent UI, while the Windows taskbar could. Ordinary restore and foreground calls only foregrounded the proxy and failed actual acceptance.

For candidate `630244b17418dc8eed6f7c08ffe7eb7ea3d045b1`, the user clicked the new Dock icon for a background consent request and confirmed that the actual prompt reached the foreground. The request used `ShellExecuteExW` / `runas` for the installed `Rar.exe -?` with a private hidden owner. The Agent did not automatically approve consent or automate the SnowDesktop desktop host.

The tested binary came from standard Release batch `3ef4e7bde1514d4ca89ca6f0c1ac16a7`, which passed the Release build and 130/130 application tests with 4 jobs in 190.24 seconds of CTest. This record adds no production change and reruns neither compilation nor the full suite. The binary hash and local evidence paths are listed above. Request/returned log phases support routing evidence; the user's observation establishes actual visibility.

Acceptance is limited to this machine's Dock click behavior. Other Windows versions, PowerShell's initial elevation foreground behavior, and the separately failing ordinary-file WinRAR menu probe are outside this acceptance.
