# 构建工具测试的生命周期

工具测试的正式入口、选择范围和执行权见[脚本手册](../../scripts/README.md)。新增长期条目仍按[长期测试准入](../../AGENTS.md#临时验证与长期测试准入)判断。这里维护已有条目的生命周期及继续执行条件，不登记一次性运行日志。

## 标签与构建参与

`temporary` 标识只在当前调查或验证测试检出能力时需要的场景。已有 CTest 临时条目同时带 `manual`，沿用现有选择器从自动 `tools` 集合中排除；按名称或标签显式选择仍可执行。临时条目保留目的和所属任务，新的一次性程序继续放在忽略的任务目录，不因有此标签获得加入正式工程的许可。

`manual` 本身只表示显式按需执行。实际 DSH SDK、浏览器看板和真实编译预算检查有持续的接口、界面或调度契约，保留长期手动诊断身份，不因依赖本机环境或成本较高就标为临时。

标签控制执行选择；是否参与编译由目标依赖和源文件归属决定。下列七项与正常回归共用 `SnowDesktopBuildCandidateExecutionTests`，其实现仍在 `build_candidate_execution_tests.cpp`：自动执行已经退出，默认编译尚未退出，不能称为完成构建归档。测试名称不会造成七次独立编译；对应驱动需要重编时，同一编译单元内的临时代码仍会编译。

真正归档须分别核对默认执行集合、默认构建目标依赖及混合源文件。保留有效二进制、对象和原始证据供按需复用；标注、归档或整理记录本身不构成重新配置、编译、链接或重跑的理由。归档材料保留在默认构建图外时，累积成本主要是存储和维护；仍在混合编译单元或默认目标中的条目继续产生构建成本，必须如实标注。

## 已标记的临时检出能力对照

这些场景主动恢复旧行为或撤掉生产保护，再要求对应的正常回归在指定业务断言失败；用途是验证断言是否能检出目标错误。它们不属于每次工具回归都必须重做的准备。仅在对应断言、夹具或故障注入方式改变，或已有检出证据受到质疑时，按需重新选择；结果保留原始失败与实际对照证据。

| CTest 条目 | 注入的旧行为或故障 | 保留在长期集合的正常回归 |
| --- | --- | --- |
| `build_candidate_checkout_lock` | 恢复占用原检出输出的执行方式，确认编辑被阻塞的断言能检出它 | `build_candidate_execution` |
| `build_candidate_source_lock` | 撤掉候选源码锁，确认并发写入的断言能检出它 | `build_candidate_execution` |
| `build_candidate_batches_control` | 冻结后仍占用编辑窗口，确认捕获后释放窗口的断言能检出它 | `build_candidate_batches` |
| `build_candidate_tool_verification_control` | 使用旧测试预设，确认工具用例未执行的断言能检出它 | `build_candidate_tool_verification` |
| `build_completed_verification_control` | 禁用已完成证据引用，确认重复执行的断言能检出它 | `build_completed_verification` |
| `build_compatible_verification_control` | 只接受完全相同键，确认无关候选变化导致重复执行的断言能检出它 | `build_compatible_verification` |
| `build_automatic_selection_control` | 撤掉已完成证据选择，确认混合计划重复运行已审阅测试的断言能检出它 | `build_automatic_selection` |

例如只查看临时条目而不执行、配置或编译：

```bat
ctest --test-dir .build -C Release -N -L "^temporary$"
```

该命令读取最近一次已配置的 CTest 清单；源码中的新标签会随下一次有实际需要的正常配置进入清单，不为刷新显示单独配置。确需运行时，先核对已有驱动及输入是否匹配；匹配则仅执行选中条目，缺失或失效才按共享执行权安排必要构建。
