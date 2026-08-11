# SnowDesktop 仓库工作规则

本文件适用于整个仓库。所有自动化 Agent 在执行 Git、Pull Request、版本发布和构建操作时必须遵守以下规则。

## 远程仓库

- 官方仓库为 `https://github.com/FreeFallingSnow/SnowDesktop.git`。
- 不得将 GitCode 仓库设为 `origin`，也不得向 GitCode 推送本项目变更。

## 分支与 Pull Request

- `main` 是稳定发布分支，不直接承接功能、修复或外部贡献的 Pull Request。
- `version.json` 是应用版本号的唯一来源，统一使用 `A.B.C.D` 四段整数格式。为兼容 Microsoft Store，`A` 必须为 `1` 至 `65535`，`B`、`C` 为 `0` 至 `65535`，`D` 固定为 `0`。
- 版本开发分支统一命名为 `release/vA.B.C.D`，例如 `release/v1.0.0.0`。
- 处理新的 Pull Request 时：
  - 如果已经存在当前开发版本的 `release/vA.B.C.D` 分支，应将改动合入该版本分支。
  - 如果不存在合适的版本分支，应根据下一个版本号创建 `release/vA.B.C.D`，再将改动合入。
  - 不得未经用户明确授权，直接将 Pull Request 或普通开发提交合入 `main`。
- 外部贡献者若将 Pull Request 直接提交到 `main`：
  - 优先要求或协助其将目标分支改为当前版本分支；
  - 如果提交已由维护者整合进版本分支，可以关闭原 Pull Request，并说明整合位置；
  - 评论中应提醒后续 Pull Request 不要直接以 `main` 为目标。
- 合入版本分支时应保留贡献者提交及作者信息。需要补充修改时，使用独立提交，不改写贡献者原提交。

## Commit 信息规范

### 通用要求

- 由维护者或 Agent 新建的 Commit 必须同时提供中文和英文信息；中英文必须表达同一事实范围，
  不得在其中一种语言中省略风险、限制或未验证状态。
- 首行摘要应简洁、具体，不写“修复问题”“更新代码”“一些调整”“fix bug”等无法识别改动
  对象的笼统描述，不以句号结尾。
- Commit 只能描述该提交实际包含的改动和已经达到的验证状态。视觉、交互、兼容性等未完成实际
  复现验证的改动，只能作为明确标记的 `try` Commit 保存，不得使用 `fix` 类型或“修复 / 解决 /
  resolved / fixed”等结论性措辞。
- 非平凡改动应在正文中分别说明中文内容、英文内容和实际验证；中英文说明必须对应，验证命令
  及结果只记录真实执行过的内容。
- 推荐正文格式：

  ```text
  中文：
  - 说明改动及影响范围

  English:
  - Describe the change and its impact

  验证 / Validation:
  - scripts/test.bat（26/26 通过 / 26/26 passed）
  - scripts/build.bat（Release 构建通过 / Release build passed）
  ```

- 纯文档、注释或无需完整构建的改动，应如实写明实际检查方式，不得虚构测试或省略“未运行”的
  原因。存在已知限制时，使用 `限制 / Limitations` 段落中英双语说明。
- 外部贡献者的既有 Commit 不得仅为符合本规范而改写；维护者新增的补充 Commit、版本分支 PR
  标题和最终版本 Commit 仍须遵守本规范。

### 版本开发分支 Commit

- `release/vA.B.C.D` 上的每个开发 Commit 使用以下首行格式：

  ```text
  <type>(<scope>): <中文摘要> / <English summary>
  ```

- `<scope>` 可省略；使用稳定、简短的英文模块名，例如 `icon`、`dock`、`widget`、`build`、
  `release`、`l10n`。
- `<type>` 使用以下类型：
  - `feat`：经过验证的新功能；
  - `fix`：已经复现并验证解决的缺陷；
  - `perf`：经过测量或明确验证的性能改进；
  - `refactor`：不改变外部行为的代码重构；
  - `test`：测试新增或调整；
  - `docs`：文档或注释；
  - `build`：构建系统或依赖；
  - `ci`：持续集成配置；
  - `chore`：不属于以上类别的维护工作；
  - `try`：编译已经通过、但目标场景尚未完成实际验证的独立尝试；
  - `verify`：对一个或多个 `try` Commit 完成实际场景验证，并记录结论和证据；
  - `revert`：撤销已有 Commit，并在正文注明被撤销的哈希和原因。
- 示例：

  ```text
  test(icon): 增加图标尺寸选择规则测试 / Add icon size selection rule tests
  docs(agent): 规范双语提交信息 / Define bilingual commit message conventions
  ```

- 每个编译成功的代码尝试都必须独立提交，且必须在继续下一轮代码修改前创建 Commit；不得把多个
  已经分别编译通过的尝试压成一个版本分支 Commit。编译失败的中间状态不得提交，应继续修改到
  下一次编译通过后再创建一个 `try` Commit。
- `try` Commit 的首行必须同时包含“编译通过，待验证”和
  `build passed, validation pending`，例如：

  ```text
  try(icon): 尝试调整高分辨率图标加载（编译通过，待验证） / Try adjusting high-resolution icon loading (build passed, validation pending)
  ```

- `try` Commit 正文必须写明：实际通过的编译命令、已运行的测试、尚未运行的目标场景验证、已知
  限制。只通过定向目标编译时必须写出目标名，不得笼统写成 Release 构建通过。
- 一个尝试后续完成实际验证且无需再改代码时，使用独立的 `verify` Commit 记录验证对象、步骤、
  结果以及对应的 `try` Commit 哈希；允许使用空 Commit 作为纯验证记录。验证失败时也应使用
  `verify` Commit 如实记录失败结论，再由后续 `try` Commit 保存下一次编译通过的调整。
- `fix`、`feat` 或 `perf` 仅用于该 Commit 本身包含最终改动，并且提交前已经完成对应缺陷复现、
  功能验收或性能测量的情况。不得事后改写已有 `try` Commit 来伪装其在创建时已经验证。

### `main` 版本 Commit

- 版本分支通过 **Squash and merge** 合入 `main` 时，唯一版本 Commit 的首行格式为：

  ```text
  vA.B.C.D - <中文简要更新> / <Brief English update>
  ```

- 中文和英文摘要应概括该版本最重要、已经验证的用户可见变化，不罗列内部实现细节，不使用
  Conventional Commit 的 `feat:`、`fix:` 等前缀。
- Commit 正文按“中文 / English / 验证”格式列出主要更新和发布验证；发布说明可更详细，但不得
  把未验证内容写成已完成结果。
- 示例：

  ```text
  v1.0.4.0 - 改进图标显示与 Dock 稳定性 / Improve icon rendering and Dock stability
  ```
- Pull Request 标题若将作为 GitHub Squash Commit 的默认标题，应在合并前调整为上述双语版本格式。

## 版本发布

- 一个版本的所有功能、修复和资源更新先在对应的 `release/vA.B.C.D` 分支完成并验证。
- 从版本分支发布到 `main` 时必须使用 **Squash and merge**，确保 `main` 每个版本只新增一条提交。
- `main` 上的版本提交必须遵守“`main` 版本 Commit”规范，使用中英双语摘要。
- 版本分支压缩合入 `main` 并完成发布后：
  - 在 `main` 对应提交上创建 `vA.B.C.D` 标签；
  - 不继续复用旧版本分支；
  - 下一个版本从最新 `main` 新建新的 `release/vA.B.C.D` 分支。
- 不使用普通 merge 将版本分支的全部开发提交带入 `main`。
- 本地压缩合并与版本标签创建应使用 `scripts/squash_release_to_main.bat`。该脚本只允许操作本地分支、提交和本地标签，严禁包含 `fetch`、`pull`、`push`、远程 API 或删除分支操作。
- `scripts/squash_release_to_main.bat` 完成后，必须由用户检查并测试本地 `main`，再由用户明确决定是否推送。
- `scripts/squash_release_to_main.bat` 应在唯一的版本提交上创建与 `version.json` 一致的本地注释标签 `vA.B.C.D`。
- `scripts/release.bat` 无参数时是人工发布的统一 TUI 入口，带命令参数时是 Agent 与自动化的非交互 CLI；两种模式必须复用 `scripts/release_manager.ps1` 中的同一套检查与动作。
- 每个版本的发行包、校验文件、发布说明、状态和日志必须统一保存到 `artifacts\vA.B.C.D\`，不得继续将不同版本的文件平铺到 `artifacts\` 根目录。
- TUI/CLI 可以提供远程发布动作，但必须与本地压缩合并分开；只有在用户测试本地 `main` 后，通过交互式版本确认或 CLI 的 `-Yes -ConfirmVersion A.B.C.D` 才能推送远程 `main` 和标签。
- `scripts/squash_release_to_main.bat` 仍只允许执行本地 Git 操作；统一发布界面不得通过环境变量或参数改变这一限制。

## 构建与验证

- Release 构建的标准验证入口是 `scripts/build.bat`。
- 在报告构建通过前，必须实际运行 `scripts/build.bat` 并确认 `.build\Release\SnowDesktop.exe` 成功生成。
- `scripts/build.bat` 默认不得终止 SnowDesktop 或 Explorer。若应用或 Hook DLL 被占用，Agent 可在
  执行前明确提醒将终止 SnowDesktop 并短暂重启 Explorer，随后直接使用 `--reload-shell`，无需等待
  用户再次确认。
- 执行标准构建前先检查 `SnowDesktop.exe` 是否运行，以及 Explorer 是否仍加载
  `SnowDesktopTaskbarHook.dll`。存在占用时不要先做一次必然失败的编译；应先提醒副作用，再直接重载
  Shell。`scripts/build.bat` 自身也必须以预检退出码阻止这种无效构建。
- CMake Preset、Ninja、直接调用 CMake 或其他构建方式只能用于诊断，不能替代最终的
  `scripts/build.bat` 验证；脚本、CI 与 IDE 的配置必须以 `CMakePresets.json` 为共同来源。
- 构建警告应如实报告，并区分既有警告与本次改动引入的警告。
- 完整测试的统一入口是 `scripts/test.bat`；CMake 中的 `SnowDesktopTests`
  聚合目标是测试可执行文件的唯一清单。新增测试不得在批处理脚本中再维护一份目标列表。
- CTest 使用 `contract`、`integration`、`rules` 等标签支持定向验证；Agent 可在开发中
  按标签执行，但交付前仍需运行完整测试。
- 创建 `fix`、`feat`、`perf` 或 `verify` Commit 前，必须对其所声称的结果完成与问题性质相匹配的
  实际验证。对于视觉、交互、兼容性等无法仅由自动化测试证明的问题，必须使用用户提供的复现
  对象、原始场景或等价的可观察证据验证；仅凭代码推断、构建成功或通用测试通过，不得宣称问题
  已经修复。
- 未完成实际验证、但编译已经通过的改动必须按“版本开发分支 Commit”规范及时创建 `try`
  Commit。其提交信息必须使用“尝试”“调整”“待验证”等中性表述，并明确区分“编译通过”与
  “目标问题验证通过”，不得包含表示问题已经确认解决的措辞。

## 仓库内容边界

- `widgets/` 同时包含内置 Lua 组件与面向用户提供的
  `snowdesktop-lua-widget` Agent Skill；后者是产品的组件开发功能，必须继续随软件分发，
  不得当作临时 Agent 文件删除。
- `snowdesktop-lua-widget` Agent Skill 是提供给 SnowDesktop 用户开发、调试和打包 Lua
  组件的产品能力，不是本仓库常规开发工作的默认操作指南。排查或修改宿主原生代码、Dock、
  拖放、构建、测试、发布流程，以及维护该 Skill 自身的文档或分发文件时，不得仅因仓库中存在
  该 Skill、任务提到组件或路径位于 `widgets/` 就自动加载它。
- 只有用户明确要求使用 `snowdesktop-lua-widget`，或当前任务确实是在创建、修改、调试、验证
  或打包一个 SnowDesktop Lua 组件包时，才允许加载该 Skill；加载前应先确认任务对象是 Lua
  组件包而不是宿主功能。
- `tests/` 仅保存测试源码，测试目标统一在 `CMakeLists.txt` 注册。
- `scripts/` 保存人工与自动化入口；根目录不再新增脚本副本。
- `.build/`、`.build_debug/`、`artifacts/` 和 `docs/html/` 是生成目录，不得提交。
- `.codex-probes/` 是 Agent 临时探测目录，不得提交或依赖其中内容。

## 本地化

- 新增或修改面向用户的界面文案时，必须同步更新 `lang/` 中的全部语言目录，并使用各目录对应语言的真实翻译。
- 不得用英文或其他语言的占位文本凑齐翻译键集合。`en-US` 仅可作为运行时意外缺失翻译时的回退，不能替代已提交目录中的目标语言翻译。
- 完成文案改动后必须运行本地化契约测试；通过键集合检查不代表翻译质量合格，还应人工检查是否存在语言混搭、未翻译文本和占位文本。

## 工作区安全

- 用户已有的未提交修改不得被覆盖、丢弃、暂存或混入 Agent 的提交。
- 提交前必须检查暂存区，只暂存本次任务涉及的文件。
- 不得使用 `git reset --hard`、`git checkout --` 或其他破坏性命令清理用户改动。
- 创建、切换、合并、推送分支以及关闭或合并 Pull Request 前，应核对当前分支、目标分支和远程仓库。
- **清理 `.build` 目录前必须征得用户明确确认。** `.build\Release\data\` 目录包含用户当前的桌面布局、设置、组件存储数据和布局备份，删除后无法恢复。需要清理构建缓存时：
  - 优先只删除 `CMakeCache.txt` 和 `CMakeFiles/`，保留 `Release/data/`；
  - 如确需完整清理 `.build`，必须先向用户说明数据丢失风险并等待确认；
  - 清理前应尝试将 `.build\Release\data\` 备份到安全位置。
