> 项目目前处于高频开发期，暂不接受外部 Pull Request 的代码合并。
> The project is under rapid development and is temporarily not accepting external pull requests for code integration.
>
> 欢迎通过 [Issues](https://github.com/FreeFallingSnow/SnowDesktop/issues)、[QQ 群 976422547](https://qm.qq.com/q/HyazkCIRig) 和 [Steam](https://store.steampowered.com/app/5080330/SnowDesktop/) 测试版交流与反馈。
> Feedback is welcome through Issues, the QQ group, and Steam test builds linked above.
>
> 本模板保留供维护者开发使用。 / This template remains available for maintainer development.

## 变更说明 / Description

<!-- 用中文或英文说明问题、用户可见变化和影响范围；维护者版本 PR 使用中英双语。
Describe the problem, user-visible behavior, and scope in Chinese or English; maintainer release PRs use both. -->

关联 Issue / Related issues:

## 验证 / Validation

<!-- 记录真实执行的命令与结果；未运行或不适用时说明原因。不要把编译通过写成实机验证通过。
Record actual commands and results. Explain omitted or inapplicable checks. A successful build is not real-world validation.
按 CONTRIBUTING 选择验证范围：原生代码使用标准构建和对应测试，纯组件使用组件检查，纯文档使用静态检查。
Follow CONTRIBUTING: standard build and relevant tests for native code, component checks for widget-only changes, static checks for documentation. -->

| 检查 / Check | 命令、结果或未运行原因 / Command, result, or reason not run |
| --- | --- |
| 构建或组件检查 / Build or component checks | |
| 定向测试 / Targeted tests | |
| 完整测试 / Full suite | |
| 实机场景及证据 / Real-world scenarios and evidence | |

待验证场景与已知限制 / Pending validation and known limitations:

## 兼容性与来源 / Compatibility and provenance

<!-- 不适用的项目填写 N/A 并简述原因。
Mark inapplicable items N/A with a brief reason. -->

- 公共 API、配置或数据变更及兼容方案 / Public API, configuration, or data changes and compatibility plan:
- 第三方内容的来源、版本、许可证及修改 / Third-party sources, versions, licenses, and modifications:

## 提交检查 / Submission checklist

<!-- 勾选前完成对应检查；不适用的项目在上文解释。 / Check only after review; explain N/A items above. -->

- [ ] 已阅读贡献指南，PR 指向当前 `release/vA.B.C.D` 分支 / Read the contribution guide and target the active `release/vA.B.C.D` branch
- [ ] 有权按目标许可提交内容，且已披露第三方材料 / Authorized to submit under the destination license and disclosed third-party material
- [ ] 已按改动范围完成验证或明确标注待验证项 / Completed appropriate validation or explicitly identified pending checks
- [ ] 文案涉及的全部语言已更新并通过本地化检查，或已说明不适用 / Updated all affected languages and passed localization checks, or explained N/A
- [ ] 测试保护实际行为或数据/API 约束，不依赖个人数据或残留产物 / Tests protect behavior or data/API invariants without depending on personal data or stale artifacts
- [ ] 未清理或覆盖用户数据，未混入无关修改、凭据或生成物 / Preserved user data and excluded unrelated changes, credentials, and generated output
