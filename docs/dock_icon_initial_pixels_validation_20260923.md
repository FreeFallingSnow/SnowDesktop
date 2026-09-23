# Dock 初始图标验证记录（2026-09-23）

用户反馈运行中项目和映射文件夹快捷方式图标较慢。最新开发版原始日志中，快捷访问的 `ComfyUI-aki - 快捷方式.lnk` 与工作区两个 ReflectorWorkbench 程序快捷方式均本地提取失败。只读检查确认：前者保存的 `D:\Tools\Stablediffusion\ComfyUI-aki` 目标当前不存在；后两者目标存在，实际调用本地资源读取与 `SHDefExtractIconW` 返回无嵌入图标（S_FALSE）。未修改用户快捷方式或程序文件。

The user reported slow running-item and mapped-folder shortcut icons. Read-only inspection confirmed that the ComfyUI shortcut points to a missing local path, while two existing ReflectorWorkbench executable targets provide no embedded icon to the direct extraction path (S_FALSE). User shortcuts and executables were not modified.

本轮尝试 / Attempt:
- 运行中程序的快速路径在本地资源不可用时查询窗口提供的图标，沿用有界消息超时并核对进程身份；Shell 高质量结果仍独立更新。
- 进程内保留最多 128 个成功的快捷方式图标，按来源路径、修改时间/大小与请求尺寸区分；以独立 HBITMAP 副本提供下次首图，后台仍刷新；失败及迟到低质量结果不覆盖已取得的高质量副本。
- Running items may use window-provided initial pixels with bounded messaging and process-identity checks while independent Shell refinement continues.
- A process-local cache retains at most 128 successful shortcut images, keyed by path, source stamp and requested size. Consumers receive independent HBITMAP copies and still request refinement; failed or late lower-quality results cannot replace refined cache pixels.

当前验证 / Current validation:
- `scripts/build.bat`: exit 0, 49.078 s, no compiler/linker warnings; EXE SHA256 `199d18dc23d94b52e4224942241cbdc7f7db3cf1ea5bbaaec3eeb268b9aa47a8`.
- 新增图像缓存测试首次失败：夹具只在 top-down DIB 第一行着色，却按 CopyImage 产生的 bottom-up DIB 原始第一行检查。诊断实际四像素为 `0 0 ff123456 0`，颜色和 Alpha 保留，失败为测试夹具行序假设；准备独立修正测试。首次完整测试在此候选上启动，结果尚待记录。
- The first cache test failed because its fixture assumed unchanged raw row order after CopyImage converted a top-down DIB to bottom-up storage. Diagnostic pixels `0 0 ff123456 0` retained the color and alpha. The fixture will be corrected separately. The first full run was started on this candidate and is pending at this checkpoint.

限制 / Limitations: 首次尚无缓存且没有窗口图标的对象仍需等待 Shell；不存在的目标不由本次修改修复。视觉及原始桌面场景待用户验证。Uncached objects without window pixels still depend on Shell, and this change does not repair missing targets. Original desktop visuals remain pending user validation.
