# 测试规范重编：调研与现状审计

日期：2026-09-10。仓库审计基线：`0b1fe404`，`release/v1.0.6.0`。

本报告为[测试规范说明](testing_policy.md)提供历史调研依据，不替代 [AGENTS.md](../AGENTS.md)，不表示已实现新的测试选择或缓存机制。调研期间未安装候选 Skill，未修改当时的规则、测试脚本或生产代码。后续用户已要求采用草案，现已将规则纳入 AGENTS；下文“建议”及现状数据保留为调研时记录。

## 1. 结论与适用范围

目前的测试有防回归价值，但 **117 个 CTest 条目通过，不能证明拖放等核心业务链已经贯通**。本次发现两类需要分别解决的问题：一些断言只能证明源码里出现了文字；一些有效的模块测试没有覆盖模块之间的实际调度。增加组合数量不能自动补上这两种缺口。

开发效率也需要独立治理。当前规则已经允许开发阶段定向测试，却仍在最终交付、验证提交、合入和发布等多个位置要求全量，未明确同一份证据能否复用。建议按变更风险和阶段决定检查范围，把全量集中在稳定检查点，并为证据复用规定输入一致性条件。

本次完成测试清单盘点、重点边界抽查、一个源码契约测试的隔离对照实验及上游规则调研；**尚未逐项审完 117 个条目中的全部断言**，不能给出“多少项无效”的比例。以下上游机制仅提取适用原则，不照搬它们的框架、基础设施或时间预算。

## 2. 大型开源项目的一手规则

### 2.1 控制开发反馈成本

| 来源 | 上游实际做法 | 对 SnowDesktop 的启示与边界 |
| --- | --- | --- |
| [Rust：Running tests](https://rustc-dev-guide.rust-lang.org/tests/running.html) | 本地开发通常只运行预计受影响的测试集合，也支持目录和单例筛选；PR CI 与合入队列承担不同范围；成功结果可缓存并支持强制重跑 | 开发用定向测试，合入承担全面检查。Rust 的 `ui` 测试主要检查编译诊断，不能当作桌面 GUI 测试方案 |
| [Chromium：CQ](https://chromium.googlesource.com/chromium/src/+/main/docs/infra/cq.md) | 常规 CQ 使用筛选过的测试及平台；Mega CQ 扩大范围，成本更高，适用于特别高风险的变更；部分任务按改动目录触发；增加检查需要证明收益值得其成本 | 区分日常反馈和高风险检查；明确选择理由。其通常的 dry-run/full-run 测试集合相同，full-run 的区别是提交，不能误读成“局部/全量” |
| [Bazel：cache_test_results](https://bazel.build/versions/9.1.0/reference/command-line-reference#flag--cache_test_results) | 自动缓存模式会考虑测试及依赖变化、外部测试、前次失败等条件 | 借鉴按输入决定有效性的机制。SnowDesktop 仍使用 CMake/CTest，当前没有等价的自动测试结果缓存 |
| [Qt：Test Best Practices](https://doc.qt.io/qt-6/qttest-best-practices.html) | 避免重复或过量测试数据及无必要等待；异步检查使用有界条件等待；测试隔离并清理资源 | 优先消除等待和重复工作，不能仅靠跳过集成测试压缩时间；不因此引入 Qt 测试框架 |

Chromium 还存在重试及失败归因机制。本项目不照搬“重试通过即可忽略第一次失败”的策略；它不符合当前仓库对不稳定测试的要求。完整扫描或高成本故障注入可以安排在明确的检查阶段，但已知核心缺陷不能只留给非阻断的后台检查。

### 2.2 提高测试的证明能力

| 来源 | 上游实际做法 | 对 SnowDesktop 的启示与边界 |
| --- | --- | --- |
| [Chromium：Browser Tests](https://www.chromium.org/developers/testing/browser-tests/) | 使用真实浏览器的进程内集成测试，并区分需要系统输入、焦点等条件的交互测试 | 真实生产调度与边界测试替身可以组合；真实系统交互另列验收，不能用回调替身冒充整个宿主 |
| [ChromeOS：Unit testing best practices](https://www.chromium.org/chromium-os/developer-library/guides/testing/unit-tests/) | 重视快速、隔离、可控的依赖；避免局部模拟被测类本身；谨慎使用 mock | 模拟网络、时钟、系统失败等边界，保留被测路由、调度及提交逻辑；有契约意义的参数或调用顺序断言仍有价值 |
| [VS Code：Writing Tests](https://github.com/microsoft/vscode/wiki/Writing-Tests)、[测试目录](https://github.com/microsoft/vscode/blob/main/test/README.md) | 区分单元、集成、扩展、smoke 和 sanity 等检查 | 标签应说明测试实际覆盖哪一层，不能把所有 `integration` 条目解释成完整桌面验收 |
| [VS Code：Smoke](https://github.com/microsoft/vscode/blob/main/test/smoke/README.md)、[Sanity](https://github.com/microsoft/vscode/blob/main/test/sanity/README.md) | 面向实际构建和平台验证，强调构建与测试版本匹配、状态隔离及可观察的就绪条件 | 验收记录必须绑定实际程序和组件来源。文档中的工具版本或安装命令不能未经核对直接采用 |
| [Qt：Test Full Stack](https://doc.qt.io/qt-6/qttest-best-practices.html) | 要求测试能到达实际后端，并使用接近真实的数据；mock 是补充 | 外部拖入要验证真实内容读取、提交、落盘及归属；仅测试“允许拖入”不足以接受功能 |
| [LLVM：Testing Guide](https://www.llvm.org/docs/TestingGuide.html) | 区分单元、回归与完整程序测试；从真实问题提炼最小复现 | 回归用例应对应原始故障机制。上游的提交前测试要求依赖自身体系，不直接换算成每轮 SnowDesktop 全量 |
| [SQLite：How SQLite Is Tested](https://www.sqlite.org/testing.html) | 使用故障注入、不同测试体系和变异等方法检查错误路径及测试敏感性 | 对文件误删、事务失败、异步落点等关键风险做定向故障注入。TH3 并非可直接采用的开源测试包，也不把 SQLite 的覆盖投入变成本项目统一指标 |

共同可取之处是：小范围测试负责快速定位，真实边界测试负责验证连接关系，系统验收负责环境相关行为。不同层的结果分别报告，不能互相替代。

## 3. 相关 Skill 的筛选

使用本地 `find-skills` 检索了 `testing`、`test-driven-development`、`mutation-testing`，并阅读了下列候选原文。它们在这里是调研材料，不是本仓库已经启用的指令。链接指向调研时的上游分支，采用前应固定版本并重新审查。

| 候选及原文 | 适用内容 | 需要裁剪或验证的部分 |
| --- | --- | --- |
| [obra/superpowers：test-driven-development](https://github.com/obra/superpowers/blob/main/skills/test-driven-development/SKILL.md) | 缺陷回归先观察正确原因的失败，再验证修正后通过 | 不采用“所有方法都必须新增测试”或因顺序不符删除已有实现的机械规定；不能覆盖用户工作区保护规则 |
| [同技能：writing-good-tests](https://github.com/obra/superpowers/blob/main/skills/test-driven-development/writing-good-tests.md) | 每个测试对应具体生产故障，预期独立于实现，mock 位于边界，检查测试能否发现错误 | 对关键架构负向约束保留有限静态检查；不能把资源、schema、本地化等数据契约一并否定 |
| [obra/superpowers：verification-before-completion](https://github.com/obra/superpowers/blob/main/skills/verification-before-completion/SKILL.md) | 结论必须有对应验证证据，构建通过与原始症状消失分别判断 | “新鲜证据”在本项目应定义为输入仍有效，不能解释成每次回复或提交都重跑全量 |
| [Anthropic：testing-strategy](https://github.com/anthropics/knowledge-work-plugins/blob/main/engineering/skills/testing-strategy/SKILL.md) | 根据关键路径、失败、边界、安全和数据完整性制定测试计划，避免测试琐碎实现 | 适合作为测试计划提纲；缺少 C++/OLE、构建来源和桌面宿主的具体规则 |
| [Trail of Bits：mutation-testing](https://github.com/trailofbits/skills/blob/main/plugins/mutation-testing/skills/mutation-testing/SKILL.md) | 限定目标与预算，区分发现变异、未发现、超时和跳过 | 上游列举的工具未证明适配本仓库 C++/MSVC；采用方法不等于已选定可用工具；超时不算成功发现缺陷 |
| [Trail of Bits：property-based-testing](https://github.com/trailofbits/skills/blob/main/plugins/property-based-testing/skills/property-based-testing/SKILL.md) | 用独立性质验证序列化、路径、排序、状态转换等大量输入 | 防止同义反复、无有效样本和重复实现；不能替代真实桌面拖放，也不预设新增框架依赖 |

网页测试类 Skill（例如基于 Playwright 的 webapp-testing）与本次桌面宿主问题不匹配；不得用于绕过仓库禁止自动化捕捉、操作桌面宿主的规定。

若后续选择安装，可从以下命令进入安装流程；本次未执行安装：

```text
npx skills add obra/superpowers --skill test-driven-development
npx skills add obra/superpowers --skill verification-before-completion
```

更适合本项目的落地方式是先确立仓库规范，再考虑提炼一个短的测试工作流 Skill，引用规范中的选择表和证据要求，避免维护第二套冲突规则。

## 4. 117 个测试目前说明了什么

清单来自配置后的 CTest JSON；`117` 是 CTest 条目数，一个条目内可能有多个用例和大量断言。

| 标签抽样 | 条目数 |
| --- | ---: |
| `contract` | 91 |
| `integration` | 21 |
| `core` | 19 |
| `rules` | 14 |
| `widget` | 55 |
| `interaction` | 15 |

标签相互重叠，不能相加，也不能据此推算有效测试比例。`contract` 不等于“字符串扫描”，`integration` 不等于“用户交互端到端”。

### 4.1 应保留的有效保护

- [组件存储事务测试](../tests/widget_storage_transaction_tests.cpp)使用生产事务对象检查失败不污染原状态、最终配额和实例隔离，保护数据不变量。
- [HTTP 安全测试](../tests/http_security_tests.cpp)检查生产解析与策略对 URL、地址和凭据等输入的处理，保护安全边界。
- [Shell 文件操作测试](../tests/shell_file_operation_worker_tests.cpp)调用实际工作器，核对复制后的源文件保留、移动后的文件位置及多来源操作结果，具备真实文件系统断言。

这些价值不因拖放宿主存在漏测而消失。应按断言保护的风险逐项审计，不能整体删掉 `contract` 或低层测试。

### 4.2 已证实的源码扫描假阳性

[shell_integration_contract_tests.cpp](../tests/shell_integration_contract_tests.cpp) 中，`blocking registry and Shell notification work executes in the worker` 对应的检查只是搜索两个调用文本；没有运行对应控制器。

隔离实验将该测试读取的 22 个文件复制到临时目录，用现有 `SnowDesktopShellIntegrationContractTests.exe` 的源码根目录参数指向副本。只在副本的 `src/dock_settings.cpp` 中改变调用：

| 副本内容 | 实际结果 |
| --- | --- |
| 保留原始 `ApplyWindowsSystemLightThemeEnabled(systemTheme.value);` | 退出码 0，通过 |
| 改为 `(void)0; /* ApplyWindowsSystemLightThemeEnabled(systemTheme.value); */` | 退出码 0，仍通过 |
| 改为 `(void)0;`，同时去掉注释里的调用文本 | 退出码 1，对应断言失败 |

因此，该断言对文本缺失敏感，却不能证明调用执行。实验仅运行这一个源码扫描测试，**没有编译变异后的应用，没有运行全部 117 项的变异测试，也没有修改仓库生产文件**。

同一测试还存在依赖特定多行空白的检查。后续应按“关键负向架构约束保留或改善；正向执行宣称改为行为测试”处理，不能通过增加更多搜索词补救。

### 4.3 拖放测试的连接缺口

[拖放验收矩阵](drag_drop_validation_matrix.md)已明确各层证据边界。4500 个路由组合有独立预期，能保护规则；COM 适配器和 Lua 容器测试也有价值。但目标处理器使用替身、或未经过 `DesktopApp::HandleOleDrop` 时，不能证明 Explorer 释放后触发了真实组件提交。

本轮增加的内容选择回归确实先失败后通过；实际下载探针也成功下载并解码用户的花瓣 WebP。这仍不覆盖原始 Edge 数据对象、整个宿主调度和组件最终落点。DND-03 的连续异步落点风险仍待复现和覆盖，不能因全量通过关闭。

## 5. 开发耗时的现状与改编重点

[test.bat](../scripts/test.bat) 和 [test_manager.ps1](../scripts/test_manager.ps1)已有 `name`、`label`、`core`、`fast`、`full`、`list`。定向模式从 CTest 清单推导要构建的目标；无匹配会报错。应继续复用这个入口，避免再造目标列表。

本轮拖放调整记录的历史全量为 117/117，CTest 测试执行部分耗时 **58.28 秒**。这个数字不包括前面的配置、测试编译及宿主构建，也不是多次测量的稳定基准。当前 `integration` 标签累计耗时是进程时间，不能直接当作并行后的墙钟时间节省量。

目前未发现基于完整依赖的受影响测试选择器或可验证的结果缓存。规则优化应分两步：

1. **先改善执行习惯和检查点。** 开发显式选择定向命令；同一输入不要因写回复、提交或重复交付而重跑；高风险在本轮改动稳定后集中全量；报告编译、测试及实机证据的不同范围。
2. **再实现并验证自动化。** 补充风险映射和结果记录；自动选择器先只输出建议，与全量结果对照验证，再允许省略测试。遇到未映射改动、公共依赖或不明影响范围时保守扩大检查。

是否删除低价值测试、增加真实调度测试以及是否减少每轮全量，分别评估；不能把“更快”当成漏测的理由，也不能把“跑得更多”当成有效性的证明。

## 6. 建议实施顺序

1. 先采用[规范](testing_policy.md)中的风险与阶段选择表、结果复用条件和结论用语，统一修改 AGENTS 中重复的全量触发规则。
2. 优先补上 DND-01/02 的宿主调度到实际提交证据，以及 DND-03 的可控完成顺序测试；核心拖放缺口继续阻断相应的“已修复”结论。
3. 审计 117 个条目的风险、生产入口、断言和耗时，逐项决定保留、改写、合并或删除；优先处理已证实的源码扫描假阳性。
4. 最后实现依赖选择、证据记录和耗时统计；用已知缺陷、受控变异和全量对照验证选择器，确认没有漏选后启用自动省略。

调研阶段只交付报告与草案。后续采用规则不等于已完成测试清单与执行机制重构，尚不能据此宣称已获得性能提升。
