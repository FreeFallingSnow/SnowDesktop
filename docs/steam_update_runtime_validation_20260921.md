# Steam 更新实机验收补记（2026-09-21）

验收源码范围：`e171c4212b883c43d44fce073ab93d63aa27753e` 至
`a2a913cd103392e18e05394cb1b7fbea485d801d`，分支 `release/v1.0.7.0`。

用户在本次更新审查后明确反馈：

> 映射目录通知 是验证过的，菜单，穿透，启动占位 可以验证通过， 自启动还没验证需要用户配合，卡顿风险你处理下

据此记录用户实际场景通过，不再把以下项目整体列为未验收：

- 映射目录通知：`83f6f855`。
- 菜单：截至 `a2a913cd` 的菜单候选，包括 `f0381940`、`b19b8846`、`2e40effc`、
  `a7a59006`、`e06fd8b5`；最新反馈补足早期 `33d0d69c` 之后的用户场景验收。
- 穿透切换：`2f4cddc3`。
- 启动分批读取及占位：`0e6fb67a`、`0a2ac339`、`b927e5ef`。

English: The maintainer confirms mapped-folder notifications, the current menu candidate,
desktop passthrough, and progressive startup/placeholders in their actual scenarios for the
source range above. This supplements the earlier menu acceptance and the listed try commits.

## 保留的边界 / Remaining boundaries

- 自启动 `9accfbd3` 尚未获得原用户实际登录环境的验收，需要原用户配合。
  Auto-start remains unverified in the affected user's actual login environment.
- 此反馈不等于逐项测过所有 DPI、多屏、第三方扩展、失联网络盘或持续写入组合。
  The feedback does not establish exhaustive DPI, multi-monitor, third-party extension,
  offline-share, or sustained-write coverage.
- 历史全量中的通知用例失败仍保留，不能用用户验收改写自动化日志。
  The historical automated notification-test failure remains recorded separately.
- 新发现的 UI 线程目录解析卡顿风险 `SHELL-01` 将独立修改、测试并记录；以上反馈
  不预先验收尚未实现的修改。The newly identified UI-thread folder parsing risk SHELL-01
  requires a separate change and validation; this feedback does not pre-approve its outcome.

本补记仅依据用户反馈和提交范围核对；未运行新构建、测试或桌面自动化。
This record uses the user's feedback and commit-scope inspection; no new build, test run,
or desktop automation was performed for this acceptance record.
