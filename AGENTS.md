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

## 版本发布

- 一个版本的所有功能、修复和资源更新先在对应的 `release/vA.B.C.D` 分支完成并验证。
- 从版本分支发布到 `main` 时必须使用 **Squash and merge**，确保 `main` 每个版本只新增一条提交。
- `main` 上的版本提交建议命名为 `vA.B.C.D - 简要更新说明`。
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

## 仓库内容边界

- `widgets/` 同时包含内置 Lua 组件与面向用户提供的
  `snowdesktop-lua-widget` Agent Skill；后者是产品的组件开发功能，必须继续随软件分发，
  不得当作临时 Agent 文件删除。
- `tests/` 仅保存测试源码，测试目标统一在 `CMakeLists.txt` 注册。
- `scripts/` 保存人工与自动化入口；根目录不再新增脚本副本。
- `.build/`、`.build_debug/`、`artifacts/` 和 `docs/html/` 是生成目录，不得提交。
- `.codex-probes/` 是 Agent 临时探测目录，不得提交或依赖其中内容。

## 工作区安全

- 用户已有的未提交修改不得被覆盖、丢弃、暂存或混入 Agent 的提交。
- 提交前必须检查暂存区，只暂存本次任务涉及的文件。
- 不得使用 `git reset --hard`、`git checkout --` 或其他破坏性命令清理用户改动。
- 创建、切换、合并、推送分支以及关闭或合并 Pull Request 前，应核对当前分支、目标分支和远程仓库。
- **清理 `.build` 目录前必须征得用户明确确认。** `.build\Release\data\` 目录包含用户当前的桌面布局、设置、组件存储数据和布局备份，删除后无法恢复。需要清理构建缓存时：
  - 优先只删除 `CMakeCache.txt` 和 `CMakeFiles/`，保留 `Release/data/`；
  - 如确需完整清理 `.build`，必须先向用户说明数据丢失风险并等待确认；
  - 清理前应尝试将 `.build\Release\data\` 备份到安全位置。
