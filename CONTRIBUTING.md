# 贡献指南

简体中文 | [English](./CONTRIBUTING.en.md)

官方仓库为
[FreeFallingSnow/SnowDesktop](https://github.com/FreeFallingSnow/SnowDesktop)。
本指南说明参与方式与贡献要求。

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
| 访问密码 | `SnowDesktopDev0927` |

1. 在 Steam 库中右键 SnowDesktop，打开“属性”，进入测试版设置。
2. 输入上表中的访问密码并验证，然后选择 `internal-dev` 分支。
3. 等待 Steam 下载更新后启动。

操作说明参见 [Steam 测试分支文档](https://partner.steamgames.com/doc/store/application/branches)。

## 问题反馈与建议

提交前先搜索已有 Issue。问题报告请提供 SnowDesktop 版本、发行渠道、Windows 版本、
复现步骤、预期与实际结果，以及相关日志或截图。分享前移除个人路径、凭据和其他隐私信息。

较大的功能、架构调整或公共 API 变更，请先通过 Issue 与维护者讨论。
可以使用中文或英文交流，围绕可复现的行为和具体建议展开。

## 代码贡献

恢复接收代码贡献后，PR 应提交到当前开发版本的 `release/vA.B.C.D` 分支；
`main` 为稳定发布分支。使用 [PR 模板](./.github/pull_request_template.md) 说明改动、
关联问题、验证结果和未完成事项。

本地构建参见 [README](./README.md) 和 [脚本说明](./scripts/README.md)。
维护者及自动化 Agent 的工作流程统一见 [AGENTS.md](./AGENTS.md)。

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
