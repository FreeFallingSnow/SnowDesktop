# 贡献指南

简体中文 | [English](./CONTRIBUTING.en.md)

官方仓库为
[FreeFallingSnow/SnowDesktop](https://github.com/FreeFallingSnow/SnowDesktop)。
本指南说明贡献流程；自动化 Agent 还必须遵守 [AGENTS.md](./AGENTS.md) 中的完整规则。

## 当前参与方式

**项目目前处于高频开发期，暂不接受外部 Pull Request 的代码合并。**
欢迎通过以下渠道交流，帮助改进功能、稳定性、翻译和文档：

- [GitHub Issues](https://github.com/FreeFallingSnow/SnowDesktop/issues)：报告问题、提出功能建议，或反馈翻译与文档中的错误。
- [QQ 用户群：976422547](https://qm.qq.com/q/HyazkCIRig)：讨论使用体验、交流问题和测试反馈。
- [Steam](https://store.steampowered.com/app/5080330/SnowDesktop/) 测试版：参与新功能体验与问题验证，加入方式见下方说明。

### 加入 Steam 测试版

| 项目 | 内容 |
| --- | --- |
| 分支 | `internal-dev` |
| 分支说明 | SnowDesktop internal development builds |
| 访问密码 | `SnowDestopDev0927` |

1. 在 Steam 库中右键 SnowDesktop，打开“属性”，进入测试版设置。
2. 输入上表中的访问密码并验证，然后选择 `internal-dev` 分支。
3. 等待 Steam 下载更新后启动。

操作说明参见 [Steam 测试分支文档](https://partner.steamgames.com/doc/store/application/branches)。

## 开始之前

以下规范适用于维护者开发，并供今后开放代码贡献时参考。

- 先搜索已有 Issue 和 Pull Request，避免重复工作。较大的功能、架构调整或公共 API 变更，请先通过 Issue 与维护者讨论范围和兼容性。
- 问题报告请提供 SnowDesktop 版本、发行渠道、Windows 版本、复现步骤、预期与实际结果，以及相关日志或截图。分享前移除个人路径、凭据和其他隐私信息。
- 一个 PR 聚焦一个问题。说明用户可见变化、已知风险和未完成事项，避免混入无关重构或格式化。
- 可以使用中文或英文讨论；请尊重参与者，并围绕可复现的行为和具体代码提出意见。

## 分支与 Pull Request

1. 从官方仓库当前开发版本的 `release/vA.B.C.D` 分支创建工作分支。
2. PR 的目标分支也应为该版本分支。`main` 是稳定发布分支，不直接接收普通开发 PR。若没有合适的版本分支，请先由维护者确定目标。
3. `version.json` 是版本号的唯一来源，格式为 `A.B.C.0`：`A` 为 1–65535，`B`、`C` 为 0–65535。除非贡献本身涉及已商定的版本更新，否则不要自行修改版本号。
4. 使用 [PR 模板](./.github/pull_request_template.md)，关联相关 Issue，并记录验证命令、结果、未运行原因和待验证场景。尚未完成的工作请使用 Draft PR。
5. 合入版本分支时保留贡献者的提交及作者信息；维护者的补充修改使用独立提交，不仅为格式统一而改写外部贡献者的既有提交。

版本分支经验证后，由维护者通过 Squash and merge 发布到 `main`。本地版本合并与标签使用
`scripts/squash_release_to_main.bat`；远程发布必须在维护者测试本地 `main` 并明确确认后单独执行。
普通贡献 PR 不执行发布操作，也不向 GitCode 推送。

## 提交信息

版本分支上的新开发提交采用以下中英双语格式，中英文应描述相同范围：

```text
<type>(<scope>): <中文摘要> / <English summary>
```

`scope` 可省略。常用类型为 `feat`、`fix`、`perf`、`refactor`、`test`、`docs`、`build`、
`ci`、`chore`、`try`、`verify` 和 `revert`。摘要应具体，不写“修复问题”或“fix bug”。
非平凡改动在正文中分别写明中文说明、英文说明和实际验证，例如：

```text
docs(contributing): 补充贡献流程 / Expand the contribution workflow

中文：
- 说明分支选择与验证要求

English:
- Explain branch selection and validation requirements

验证 / Validation:
- git diff --check（通过 / passed）
- 纯文档改动，未运行构建或测试 / Documentation only; build and tests not run
```

`feat`、`fix`、`perf` 只用于已经完成对应验收、缺陷复现验证或性能测量的结果。
编译通过但视觉、交互或兼容性仍待实机验证时，使用 `try`，摘要同时包含
“编译通过，待验证”和 `build passed, validation pending`，并在正文记录验证范围与限制。
每轮单独编译成功的代码尝试分别提交，再继续下一轮修改；不提交编译失败的中间状态。
后续完成实测时以独立 `verify` 提交记录原 `try` 哈希、步骤和实际结果，不改写历史验证状态。
纯组件更新使用组件级验证及对应的 `try(widget)` 格式，详见 [AGENTS.md](./AGENTS.md)。

## 构建与验证

环境要求见 [README 的构建说明](./README.md)，脚本说明见 [scripts/README.md](./scripts/README.md)。
在仓库根目录使用以下入口，按改动范围选择测试：

```bat
scripts\build.bat
scripts\test.bat list
scripts\test.bat name "<regex>"
scripts\test.bat label "<regex>"
scripts\test.bat core
scripts\test.bat fast
scripts\test.bat full
```

- 原生代码改动需完成相应编译；最终 Release 构建验证使用 `scripts/build.bat`，并确认 `.build/Release/SnowDesktop.exe` 已生成。直接调用 CMake 或 Ninja 仅用于诊断，不替代标准入口。
- 构建前检查 SnowDesktop 和任务栏 Hook 的占用。默认脚本不会结束进程；需要解除占用时，`scripts/build.bat --reload-shell` 会终止 SnowDesktop 并短暂重启 Explorer，运行前告知受影响的使用者。
- 开发期间先运行覆盖改动的最小充分测试组。宿主改动最终交付、合入 PR 或发布前运行 `scripts/test.bat full`；构建、测试基础设施和跨模块公共行为改动也需要完整测试。
- 交付待实机验证的 `try` 版本可先完成必要构建和定向测试，注明完整测试未运行；用户确认场景后、最终交付或创建 `verify` 前运行一次完整测试。高风险边界仍需完整测试。
- 仅修改 Lua 组件包且未涉及宿主、公共 API 或构建脚本时，使用 `snowwidget lint`、组件测试、包校验和打包验证，无需为此编译宿主或运行宿主完整测试。纯文档、注释改动可采用链接、内容和差异检查，说明未运行构建和测试的原因。
- 测试应防止实际行为回归、数据或 API 约束被破坏，以及已知缺陷复发；优先扩展现有目标。测试源码放在 `tests/`，由 `CMakeLists.txt` 的 `SnowDesktopTests` 聚合目标统一登记，避免仅验证源码排版或复述实现。
- 视觉、交互和兼容性结果需要原始场景或等价的可观察证据。编译和自动化测试不能代替实测。桌面宿主不支持稳定的桌面自动化验证，请由使用者实机验证；独立设置窗口等可稳定操作的界面可采用相应工具。

## 代码、组件与本地化

- 保持现有代码风格，优先复用已有模块和接口。公共组件 API、清单、协议或 capability 变更应在实现前说明受影响的调用方、兼容性风险，以及 `apiVersion`、`minHostVersion`、能力检测和降级方案；不能用同一版本号代替兼容性验证。
- 修改用户可见文案时，同步更新 `lang/*.json` 的全部语言，并使用真实翻译。内置及官方社区组件的 `locales` 应与这些语言文件完全一致；运行本地化契约测试并人工检查翻译质量。
- `widgets/` 保存内置组件及随软件分发的组件开发 Skill。官方社区组件位于 `developer_assets/workshop_widgets/`，不随应用发行，不得擅自迁入内置目录。
- 内置组件运行时读取构建输出，修改源码后需通过标准构建复制，或按 [AGENTS.md](./AGENTS.md) 精确临时同步到 `.build/<Configuration>/widgets/<slug>/`，才能交付实机验证。临时同步不等于宿主编译。
- 官方社区组件实测前先标准构建宿主，再运行 `scripts/widget-dev.bat developer_assets/workshop_widgets/<slug> -Configuration <Configuration> -Once`；首次发现或切换来源时按需追加 `-RestartHost`。随后在组件开发入口启用“开发版本”，确认实际来源后再验证。
- 高级功能入口、Fluent 设置图标、组件封面及发布顺序的具体约束见 [AGENTS.md](./AGENTS.md)。关闭效果、恢复普通状态等反向操作不得因 Steam 桥或解锁状态而失效。

## 数据与提交范围

保留已有用户改动，不覆盖或混入其他人的未提交工作。提交前检查暂存区，仅包含本 PR 的文件。
不要提交 `.build/`、`.build_debug/`、`artifacts/`、`docs/html/`、`.codex-probes/` 中的生成物，
也不要提交日志、凭据、证书私钥或用户数据。

**`.build/Release/data/` 可能包含正在使用的桌面布局、设置、组件存储和备份。**
不要将它当作构建缓存删除。完整清理 `.build` 前必须说明数据丢失风险、取得使用者明确确认，
并先尝试备份数据；普通贡献和测试不应依赖清空用户数据。

## 贡献许可与商业发行

SnowDesktop 是多许可证仓库。提交贡献即表示你有权按目标位置适用的许可证提供该内容：

| 贡献位置 | 适用许可证 |
| --- | --- |
| `steam_bridge/` | [MIT](./steam_bridge/LICENSE) |
| 其他位置的项目内容 | 根目录 [GNU GPL v3.0](./LICENSE)；文件另有第三方许可声明时按其声明处理 |

你保留原创贡献的版权。SnowDesktop 可按相应许可证使用这些贡献，包括商业使用和收费发行。

## 第三方内容与来源说明

- 仅提交自己创作或有权提供的内容。若雇主、客户或其他权利人拥有相关权利，先取得足以覆盖所提交许可的授权。
- 引用第三方代码、图片、字体、数据或其他材料时，遵守许可要求，注明准确的上游地址、版本或提交、许可证及修改内容。保留版权与许可声明，并按需更新 [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)。不要把第三方内容标成自己的原创贡献。
- Steamworks SDK 及其头文件、库、工具和可再分发文件不得提交到仓库。构建启用 Steam 的桥接程序时配置外部 SDK 路径。
- AI 辅助生成的贡献同样需要检查来源、许可兼容性、正确性和敏感信息。
