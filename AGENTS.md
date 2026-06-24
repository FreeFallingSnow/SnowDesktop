# SnowDesktop 仓库工作规则

本文件适用于整个仓库。所有自动化 Agent 在执行 Git、Pull Request、版本发布和构建操作时必须遵守以下规则。

## 远程仓库

- 官方仓库为 `https://github.com/FreeFallingSnow/SnowDesktop.git`。
- 不得将 GitCode 仓库设为 `origin`，也不得向 GitCode 推送本项目变更。

## 分支与 Pull Request

- `main` 是稳定发布分支，不直接承接功能、修复或外部贡献的 Pull Request。
- 版本开发分支统一命名为 `release/vX.Y.Z`，例如 `release/v0.1.15`。
- 处理新的 Pull Request 时：
  - 如果已经存在当前开发版本的 `release/vX.Y.Z` 分支，应将改动合入该版本分支。
  - 如果不存在合适的版本分支，应根据下一个版本号创建 `release/vX.Y.Z`，再将改动合入。
  - 不得未经用户明确授权，直接将 Pull Request 或普通开发提交合入 `main`。
- 外部贡献者若将 Pull Request 直接提交到 `main`：
  - 优先要求或协助其将目标分支改为当前版本分支；
  - 如果提交已由维护者整合进版本分支，可以关闭原 Pull Request，并说明整合位置；
  - 评论中应提醒后续 Pull Request 不要直接以 `main` 为目标。
- 合入版本分支时应保留贡献者提交及作者信息。需要补充修改时，使用独立提交，不改写贡献者原提交。

## 版本发布

- 一个版本的所有功能、修复和资源更新先在对应的 `release/vX.Y.Z` 分支完成并验证。
- 从版本分支发布到 `main` 时必须使用 **Squash and merge**，确保 `main` 每个版本只新增一条提交。
- `main` 上的版本提交建议命名为 `vX.Y.Z - 简要更新说明`。
- 版本分支压缩合入 `main` 并完成发布后：
  - 在 `main` 对应提交上创建 `vX.Y.Z` 标签；
  - 不继续复用旧版本分支；
  - 下一个版本从最新 `main` 新建新的 `release/vX.Y.Z` 分支。
- 不使用普通 merge 将版本分支的全部开发提交带入 `main`。
- 本地压缩合并与版本标签创建应使用根目录的 `squash_release_to_main.bat`。该脚本只允许操作本地分支、提交和本地标签，严禁包含 `fetch`、`pull`、`push`、远程 API 或删除分支操作。
- `squash_release_to_main.bat` 完成后，必须由用户检查并测试本地 `main`，再由用户明确决定是否推送。
- `squash_release_to_main.bat` 应在唯一的版本提交上创建与 `version.json` 一致的本地注释标签 `vX.Y.Z`。
- `release.bat` 负责构建、整理发行物并提示或调用本地压缩合并与标签流程；远程 `main` 和标签必须由用户检查后手动推送。

## 构建与验证

- Release 构建的标准验证入口是仓库根目录的 `build.bat`。
- 在报告构建通过前，必须实际运行 `build.bat` 并确认 `.build\Release\SnowDesktop.exe` 成功生成。
- `build.bat` 会终止正在运行的 `SnowDesktop.exe`；执行前应在进度说明中告知用户这一副作用。
- Ninja、直接调用 CMake 或其他构建方式只能用于诊断，不能替代最终的 `build.bat` 验证。
- 构建警告应如实报告，并区分既有警告与本次改动引入的警告。

## 工作区安全

- 用户已有的未提交修改不得被覆盖、丢弃、暂存或混入 Agent 的提交。
- 提交前必须检查暂存区，只暂存本次任务涉及的文件。
- 不得使用 `git reset --hard`、`git checkout --` 或其他破坏性命令清理用户改动。
- 创建、切换、合并、推送分支以及关闭或合并 Pull Request 前，应核对当前分支、目标分支和远程仓库。
