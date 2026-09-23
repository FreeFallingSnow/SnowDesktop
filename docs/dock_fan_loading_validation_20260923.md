# Dock 文件夹加载阶段的扇形动画

## 失败反馈与身份

- 用户反馈：设置为扇形的 Dock 映射文件夹在启动时未加载完成后，后续打开会先播放普通弹窗再闪到扇形。此场景未通过 `2387509b` 的实机验收；不推定其他动画场景的验收结论。
- 2026-09-23 09:51:21 只读核对，运行 PID 38792，路径为本仓库 `.build/Release/SnowDesktop.exe`，SHA-256 为 `ec3eee44a54b2d7e427eedb2d632c5342424b2d32c126722e166f7a8481e19e7`，与 `5f81b8e3` 交付身份一致。
- 日志反复记录 `folder=1 fan=0 items=0 driver=compositor`，随后约数毫秒返回目录结果。代码每次打开清空临时条目，再以条目数量为零拒绝扇形；原生快照存活期间也拒绝扇形，直到普通动画完成。
- 这说明存在每次重新选择错误打开视图的路径，不是已证实的磁盘动画缓存损坏。当前日志不能证明实际显示帧连续性。
- 本次只读记录用户反馈和当前运行身份，未为记录反馈重跑构建/测试，也未使用桌面宿主自动化。原始日志和收据在本机忽略目录 `.codex-probes/dock-fan-loading-20260923/`。
## 本轮尝试 / Current attempt

用户补充：Dock“快捷访问”先播放小弹窗，随后瞬间拉大。当前实现首次目录条目为空，原生动画固定空列表尺寸到结束；第一批真实列表在约 4–9 ms 到达，最终接管时尺寸跳变。扇形还会因空列表退成网格，再在原生动画结束后切回扇形。

本轮保留文件夹的扇形偏好，用已有加载/空/不可访问文案作为非操作状态；首批条目到达后按完整时长展开。矩形首批列表改变尺寸时重新准备对应尺寸的展开动画；普通 Dock 文件夹在内存保留已读条目顺序，使再次打开可以预留已知尺寸。已有条目图标更新、重复列表、关闭或隐藏中的迟到结果不重启展开。

The user additionally reported a small Quick Access popup jumping to a larger frame. The native opening snapshot retained empty-list bounds until completion, although the first directory result arrived about 4–9 ms later. Empty fan folders also selected a grid before switching back to a fan. This attempt retains the fan presentation for folder status, reveals the first entries over a full timeline, rebuilds an enlarged first grid snapshot, and retains successful plain Dock-folder order keys in memory for subsequent geometry. Populated refreshes and late closing/hidden results do not restart opening.

验证 / Validation:
- `scripts/build.bat --reload-shell`: exit 0, 63.547 s; `.build/Release/SnowDesktop.exe` SHA256 `e4f8bb8d66024976ec3b02567153e028a4011ad7715b3e48e8c4565e7458bf3d`; no compiler/linker warnings.
- `scripts/test.bat name "^(slot_runtime_contract|dock_and_window_rules|popup_animation_rules|ui_animation_scheduler)$"`: exit 0, 4/4, 30.500 s including test build; no compiler/linker warnings.
- Isolated first-listing behavior probe: candidate passed; restoring empty-folder downgrade, skipping first reveal, and allowing late reopening each produced the expected assertion failures. These probes exercise production state transitions with controlled read timing, not GPU frame presentation.
- Evidence: `.codex-probes/dock-fan-loading-20260923/`; input manifest SHA256 `d1bfc753e9e45d2f87099be606f926c254c0b15324c19913fcdcc01e0391b1d9`.

限制 / Limitations: 桌面宿主视觉尚待用户实机验证；首次未知尺寸且读取很慢时仍可先显示加载状态，再展开真实列表。全量将在本轮图标排查完成、最终输入稳定后执行。Desktop-host visuals await user validation; an initially unknown slow directory can still show loading status before the real listing reveal. Full regression is deferred to the stable combined candidate after icon investigation.
