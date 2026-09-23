# Dock 初始图标验证记录（2026-09-23）

用户反馈运行中项目和映射文件夹快捷方式图标较慢。最新开发版原始日志中，快捷访问的 `ComfyUI-aki - 快捷方式.lnk` 与工作区两个 ReflectorWorkbench 程序快捷方式均本地提取失败。只读检查确认：前者保存的 `D:\Tools\Stablediffusion\ComfyUI-aki` 目标当前不存在；后两者目标存在，实际调用本地资源读取与 `SHDefExtractIconW` 返回无嵌入图标（S_FALSE）。未修改用户快捷方式或程序文件。

The user reported slow running-item and mapped-folder shortcut icons. Read-only inspection confirmed that the ComfyUI shortcut points to a missing local path, while two existing ReflectorWorkbench executable targets provide no embedded icon to the direct extraction path (S_FALSE). User shortcuts and executables were not modified.

本轮尝试 / Attempt:
- 运行中程序的快速路径在本地资源不可用时查询窗口提供的图标，沿用有界消息超时并核对进程身份；Shell 高质量结果仍独立更新。
- 进程内保留最多 128 个成功的快捷方式图标，按来源路径、修改时间/大小与请求尺寸区分；以独立 HBITMAP 副本提供下次首图，后台仍刷新；失败及迟到低质量结果不覆盖已取得的高质量副本。
- Running items may use window-provided initial pixels with bounded messaging and process-identity checks while independent Shell refinement continues.
- A process-local cache retains at most 128 successful shortcut images, keyed by path, source stamp and requested size. Consumers receive independent HBITMAP copies and still request refinement; failed or late lower-quality results cannot replace refined cache pixels.

首次构建检查点 / First build checkpoint:
- `scripts/build.bat`: exit 0, 49.078 s, no compiler/linker warnings; EXE SHA256 `199d18dc23d94b52e4224942241cbdc7f7db3cf1ea5bbaaec3eeb268b9aa47a8`.
- 新增图像缓存测试首次失败：夹具只在 top-down DIB 第一行着色，却按 CopyImage 产生的 bottom-up DIB 原始第一行检查。诊断实际四像素为 `0 0 ff123456 0`，颜色和 Alpha 保留，失败为测试夹具行序假设；准备独立修正测试。首次完整测试在此候选上启动，结果尚待记录。
- The first cache test failed because its fixture assumed unchanged raw row order after CopyImage converted a top-down DIB to bottom-up storage. Diagnostic pixels `0 0 ff123456 0` retained the color and alpha. The fixture will be corrected separately. The first full run was started on this candidate and is pending at this checkpoint.

限制 / Limitations: 首次尚无缓存且没有窗口图标的对象仍需等待 Shell；不存在的目标不由本次修改修复。视觉及原始桌面场景待用户验证。Uncached objects without window pixels still depend on Shell, and this change does not repair missing targets. Original desktop visuals remain pending user validation.


## 最终自动化检查点 / Final automated checkpoint

- Native attempts: `3719483f` (folder presentation) and `92cb3564` (initial icons). Only the test fixture changed afterward: all four pixels have identical color, so assertions no longer depend on DIB scan-line order.
- 首次完整回归 / First full run: exit 1, 119/120, 146.781 s; only the diagnosed fixture row-order assertion failed. This was a test failure, not a reproduced production pixel loss.
- 中间环境阻断 / Intermediate environment block: the corrected-input full run returned exit 1 (119 passed, `test_selection` timed out at 30.04 s, 165.578 s total). Ordinary and noninteractive PowerShell launches did not reach their first script line; non-invasive stacks showed `WNetGetConnectionW → NPGetConnection → NetUseGetInfo → RPC`. No network mappings or system settings were modified. This failed run is not counted as a pass.
- 标准构建受阻与恢复 / Standard build interruption and recovery: build-final took 366.125 s and returned exit 0, but manually terminating the task-owned blocked vcpkg dependency collector caused a dependency-collection warning. The blocked preflight child had also been stopped after independent native process/module checks confirmed no host or Hook occupancy. This run was not accepted as warning-free validation. The subsequent complete standard build succeeded without intervention or warnings in 56.843 s, confirming dependency collection and output arrangement completed. No warning settings or build requirements were suppressed.
- 最终完整回归 / Final full run after environment recovery: `scripts/test.bat full`, exit 0, **120/120**; 139.828 s including build, CTest 113.99 s; 2026-09-23T10:28:24.916155+08:00 to 2026-09-23T10:30:44.752947+08:00. No compiler/linker warnings. Manual diagnostics excluded by the standard selector. Earlier failures remain recorded above.
- 最终标准构建 / Final standard build: `scripts/build.bat`, exit 0, 56.843 s, no warnings; standard-build EXE SHA256 `039a5068fda419667707be5d905982022f2dc59e081b915e0f0128e366ea222a`. Final full-run output EXE SHA256 `24c436c542d03f337c03bbe8c6435f541f0183fd3c75fca20f5b03f8f1ed798a`. Both are bound to the same stable source inputs recorded below; no source edits occurred between these runs.
- 图标负向对照 / Icon negative controls: candidate passed; removing the independent window preview, ignoring source/version keys, and accepting lower-quality cache replacement each failed the intended behavioral assertions. GDI ownership checks use real bitmaps, not desktop automation.
- Evidence: `.codex-probes/dock-fan-loading-20260923/`; final input manifest (1,094 source/build/test/resource files) SHA256 `0e99cfa5d3d6ceb496d13582c92f7067273b6617e5875508a545e135d08ed62e`. The full input is commit `92cb3564` plus the recorded test-fixture diff; this checkpoint binds it to the final native binary and full-run log.
- 原始桌面动画与图标观感仍待用户实机验收；以上结果不视为实际视觉验收。Original desktop animation/icon appearance remains pending user acceptance; these results do not constitute visual validation.
