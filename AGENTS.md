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
- 除下述纯组件更新外，`try` Commit 的首行必须同时包含“编译通过，待验证”和
  `build passed, validation pending`，例如：

  ```text
  try(icon): 尝试调整高分辨率图标加载（编译通过，待验证） / Try adjusting high-resolution icon loading (build passed, validation pending)
  ```

- 纯组件更新不执行宿主编译；尚待 SnowDesktop 实际场景验证时，应使用 `try(widget)` Commit，
  首行同时包含“组件验证通过，待实机验证”和 `widget checks passed, runtime validation pending`。
  正文必须写明实际通过的组件级检查、尚未运行的目标场景验证和已知限制，不得声称宿主编译通过。
- `try` Commit 正文必须写明：实际通过的编译命令、已运行的测试、尚未运行的目标场景验证、已知
  限制。纯组件更新按上一条记录组件级检查；只通过定向目标编译时必须写出目标名，不得笼统写成
  Release 构建通过。
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

- 仅修改 SnowDesktop Lua 组件包，且未修改宿主原生代码、公共组件 API、CMake 或构建脚本时，
  属于纯组件更新：无需运行 `scripts/build.bat` 编译宿主，也无需运行 `scripts/test.bat` 的宿主完整
  测试。应改用 `snowwidget lint`、组件测试、包校验和打包等组件级入口完成与改动相匹配的验证，
  并在 Commit 和交付说明中如实记录实际执行的命令与结果。只要改动越出组件包边界，仍须遵守
  下列标准构建与完整测试要求。
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
- `scripts/test.bat` 支持分层验证：`name <regex>` 按测试名定向运行，`label <regex>` 按 CTest
  标签定向运行，`core` 运行核心测试，`fast` 排除 `integration` 标签，`full` 或无参数运行完整测试。
  筛选模式必须从 CTest 清单动态推导构建目标，不得在脚本中复制测试目标列表。
- 每次 Commit 不要求机械地运行完整测试。开发中和创建中间 Commit 前，应先运行覆盖本次改动的
  最小充分测试组；原生代码改动还应完成与改动范围匹配的编译。一个连续任务在最终交付前、Pull
  Request 合入前和版本发布前仍须运行 `scripts/test.bat full`。修改构建、测试基础设施或跨模块公共
  行为时，最终交付前也必须运行完整测试。纯文档、注释或不改变运行时行为的测试调整，可如实记录
  与改动匹配的静态检查或定向测试，不强制重复完整测试。
- 对视觉、交互、兼容性等尚待用户实机验证的 `try` Commit，交付待验证版本不视为上述“最终交付”，
  无需运行 `scripts/test.bat full`；应完成与改动范围匹配的必要构建和最小充分定向测试，并在 Commit
  和交付说明中明确记录完整测试未运行及待验证场景。连续实机调校期间不得为每轮 `try` 重复运行完整
  测试；待用户确认目标场景后，在最终交付、创建 `verify` Commit 或合入 Pull Request 前运行一次完整
  测试即可。若改动涉及构建/测试基础设施、跨模块公共行为或其他高风险边界，仍按上一条执行完整测试。
- 测试只在能够独立防止以下至少一类风险时保留：用户可见行为回归，公共 API、安全或数据不变量，
  已知缺陷复发，或者无法由更低层测试覆盖的关键架构边界。新增测试必须说明其防止的失败，并优先
  加入现有测试目标和标签，不得仅为增加数量创建新的测试程序。
- 下列测试属于非必要测试，应在同一改动中删除或改写，不得长期留存：只复述实现细节、空白、换行、
  排版或无语义顺序且不保护稳定契约；与同层或更低层测试重复而没有新增风险覆盖；只验证编译器、
  框架或生成代码自身行为；对应功能已经删除；长期跳过、注释掉或无法给出确定失败信号。源码契约
  测试仅用于关键负向约束或架构边界，并须归一化换行，避免依赖格式化后的多行文本。
- 环境相关的真实端到端场景应标记为 `integration`，不进入 `fast` 通道。不得通过无条件重试掩盖
  不稳定测试；应消除不确定性，或在其没有可重复防回归价值时删除。
- 创建 `fix`、`feat`、`perf` 或 `verify` Commit 前，必须对其所声称的结果完成与问题性质相匹配的
  实际验证。对于视觉、交互、兼容性等无法仅由自动化测试证明的问题，必须使用用户提供的复现
  对象、原始场景或等价的可观察证据验证；仅凭代码推断、构建成功或通用测试通过，不得宣称问题
  已经修复。
- SnowDesktop 的桌面宿主窗口不能通过 `computer-use` 或同类桌面自动化稳定发现、捕捉或操作。
  不得使用这些工具验证桌面宿主中的视觉、框选、拖放、Dock 悬停等桌面交互，也不得为此启动
  应用、枚举窗口、切换窗口、重复捕获或尝试其他绕过。此类场景应直接交由用户实机验证，或改用
  稳定、可观察的等价证据；用户提供的原始场景实测反馈可作为实际验证依据并按 `verify` Commit
  记录。`computer-use` 仅可用于设置窗口、Workshop Manager 等能够稳定枚举为唯一可操作窗口的
  独立非桌面宿主界面。
- 未完成实际验证、但编译已经通过的改动必须按“版本开发分支 Commit”规范及时创建 `try`
  Commit。其提交信息必须使用“尝试”“调整”“待验证”等中性表述，并明确区分“编译通过”与
  “目标问题验证通过”，不得包含表示问题已经确认解决的措辞。

## 仓库内容边界

- `developer_assets/workshop_widgets/` 保存由 SnowDesktop 维护、准备独立发布到 Steam Workshop 的
  官方社区组件源码；它们不是内置组件，不得放入 `widgets/`，也不得随应用发行包复制。只有用户
  明确要求将社区组件提升为内置组件时，才允许迁入 `widgets/` 并同步更新内置组件契约测试。
- 官方社区组件源码不会因为位于 `developer_assets/workshop_widgets/` 就自动出现在宿主的“开发中”
  列表。需要桌面实机验证时，先保证 `.build/<Configuration>/SnowDesktop.exe` 已由当前源码完成标准
  构建，再运行
  `scripts/widget-dev.bat developer_assets/workshop_widgets/<slug> -Configuration <Configuration> -Once`，
  将组件镜像到该构建的 `data/widgets/dev/<slug>/`。新候选、来源从错误的内置组件切换为开发组件，
  或宿主尚未重新发现该目录时，应追加 `-RestartHost`。同步成功只表示组件进入“开发中”候选；开发
  包默认不激活，仍须在设置的组件开发入口明确启用“开发版本”，才能将它作为当前来源添加到桌面
  或验证现有实例。未完成这一步时不得声称已进入运行时验证。
- CMake 的组件构建步骤使用覆盖式 `copy_directory`，不会删除输出目录中已经失去源码对应项的旧
  子目录，因此重新编译本身不能清掉误放过的内置组件。若 `widgets/<slug>/` 已不存在，但
  `.build/<Configuration>/widgets/<slug>/` 仍存在，应先核对二者的绝对路径和清单 UUID，只移走或删除
  这个精确的生成子目录，再运行标准构建并重启宿主刷新来源；若错误源码仍位于 `widgets/<slug>/`，
  则先用新的版本分支 Commit 删除源码，不得只清构建产物掩盖问题。不得为此清理整个 `.build`，不得
  删除或手工编辑 `.build/<Configuration>/data/widgets/packages.json`，也不得删除 `installed/`、
  `dev-disabled/`、组件存储或布局数据；这些是用户状态，不是内置组件来源。需要保留排查证据时，
  优先将精确的旧目录移到 `.codex-probes/`，确认新来源正常后再由用户决定是否清除。
- 用户明确要求“编译”、任务越出纯 Lua 包边界，或需要验证上述内置/开发来源切换时，不得以“纯组件
  更新无需宿主编译”为由跳过构建。按构建预检规则运行 `scripts/build.bat`；存在 SnowDesktop 进程或
  Explorer 已加载 Release Hook 时，先说明副作用并直接使用 `scripts/build.bat --reload-shell`。构建
  通过后再运行 `scripts/widget-dev.bat`，最后分别核对 `widgets/<slug>` 不存在、
  `data/widgets/dev/<slug>` 存在以及“开发中”入口可见，三者不能互相替代。
- 官方社区组件的清单预览统一命名为 `workshop-preview.png`。生成封面时必须使用
  `developer_assets/workshop_widgets/community-preview-background.png` 作为背景；该文件是 Steam 商店美术
  源文件 `SnowDesktop_SteamAssets/source/key_art_portrait.png` 的仓库内标准副本。不得改用商店页
  `page_bg_raw.jpg`、Windows 系统壁纸或其他图片，不得把已经叠有组件内容的成品预览当作背景，也不得
  为单个组件另换背景。使用组件真实跨度，并通过
  `snowwidget preview ... --appearance glass-light --background developer_assets/workshop_widgets/community-preview-background.png --canvas-size 512 --padding 48`
  生成 512×512 封面，目检后再写入清单和打包；若源美术更新，应先从上述源文件同步替换这份标准
  副本，再重生成所有官方社区组件封面。
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
