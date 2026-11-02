# 组件包与分发

本说明适用于实际创建、修改、调试、验证、同步或发布 Lua 组件包。宿主原生代码、API 和构建工具仍按 [构建与验证](validation.md)；提交措辞按 [Git、Issue 与发布](delivery.md#提交规范)。

## 来源与验证入口

| 来源 | 源码位置 | 运行来源与用途 |
| --- | --- | --- |
| 内置组件 | `widgets/<slug>/` | 构建输出的 `widgets/<slug>/`，随应用分发 |
| 官方社区组件 | `developer_assets/workshop_widgets/<slug>/` | 目标构建的 `data/widgets/dev/<slug>/`，独立发布 Workshop |
| 产品 Skill | `widgets/snowdesktop-lua-widget/`、`widgets/snowdesktop-control/` | 随应用分发；分别提供组件开发与对外控制能力，不作为临时文件删除 |

- 仅改组件包及其素材和说明、未改宿主原生代码/公共 API/CMake/共享构建脚本时，复用 `snowwidget lint`、组件测试、包校验和打包等入口，无需宿主构建或宿主全量。记录实际命令和结果；越出边界重新选检查。
- 修改内置或官方社区组件的 Lua、manifest 或本地化后，必须实际运行 [`localization_contract`](../../tests/localization/README.md)，检查语言集合、键引用、译文及硬编码文案。独立社区组件免登记仍须完成此项；组件 lint、quality 或包校验不能替代。测试程序及依赖匹配当前源码时直接补验，需要重编时按[构建与验证](validation.md)取得执行权并只构建相关目标。
- 用户明确要求编译、任务越出纯包范围或需验证内置/开发来源切换时，运行标准 `scripts/build.bat`。独立候选构建不提前退出宿主或重启 Explorer，交付运行文件后再同步开发来源。
- 组件开发 Skill 的加载条件见 [根规则](../../AGENTS.md#公共契约与本地化)。
- 官方社区组件的特殊流程，包括免登记条件、仓库来源目录、dev 同步和仓库验证要求，只维护于仓库规则，不写入随软件分发的产品 Skill；产品 Skill 只保留面向普通组件作者的通用开发与发布要求。

## 独立社区组件免登记

- 修改范围仅限 `developer_assets/workshop_widgets/<slug>/` 的官方社区组件包、素材和说明，且不修改宿主原生代码、宿主公共 API、CMake 或共享构建脚本时，无需 Task 登记或协作登记，直接开展组件开发、检查、预览和打包；不调用 `task-open/task-stage`、`begin/claim/plan/ready-and-wait`，不加入宿主构建屏障。
- 已登记任务确认符合上述范围时，先按[脚本手册](../../scripts/README.md)撤回自身编辑登记并关闭 Task，保留源码和验证记录，再继续独立开发。保留其他任务登记及未提交修改。
- 需要修改宿主或共享构建设施时，在操作前恢复根规则规定的登记和执行权流程。开发来源同步、权限、用户数据保护及发布授权仍按本说明执行。

## 内置组件同步

内置组件从构建输出的 `widgets/<slug>/` 加载。交付桌面实测前二选一：

1. 标准 `scripts/build.bat` 自动复制到 `.build/<Configuration>/widgets/<slug>/`。
2. 仅本轮验证手动临时同步到上述精确目录。先核对源/目标绝对路径、slug、manifest UUID，只覆盖该组件；不改 `data/`、其他输出或整个 `.build`。同步后核对文件哈希，按需刷新/重启并确认加载目标构建的内置来源。

同步按实际执行方式记录，内置来源使用上述输出目录。

  > 例如，手动复制后记录“组件已临时同步，宿主未重新编译”；`widget-dev.bat` 用于下一节的开发来源。

## 官方社区组件与 Workshop

- 社区源码不放 `widgets/`、不随应用发行；仅用户明确要求提升为内置时迁入并同步内置契约检查。
- 官方社区组件每轮开发完成并通过必要检查后，交付回复前必须同步到目标构建的 `data/widgets/dev/<slug>/`；此步骤同时适用于源码、manifest、本地化和预览图更新。先确认目标 `.build/<Configuration>/SnowDesktop.exe` 由当前源码标准构建，再运行：

  ```bat
  scripts/widget-dev.bat developer_assets/workshop_widgets/<slug> -Configuration <Configuration> -Once
  ```

  镜像到该构建 `data/widgets/dev/<slug>/`。同步后核对源与 dev 包的 manifest UUID、版本及全部文件哈希，交付记录目标构建、dev 目录和同步结果；同步受阻时记录具体阻断和下一步，保持待交付状态。新候选、来源切换或尚未发现目录时，打开组件设置/“添加组件”菜单重扫；旧宿主不支持入口刷新才加 `-RestartHost`。新发现且通过校验的 UUID 默认激活开发来源，保留已保存停用决定及旧注册表现有选择。核对当前来源与权限，再完成交互验收。
- Workshop 发布或更新前先同步 dev 目录；Workshop Manager 项目与 CLI `--source` 均绑定该目录，显式传同构建 `--data-directory`。由 dev 目录生成计划，核对包版本、哈希、预览及现有 PublishedFileId。
- 曾误绑源码时，把原 PublishedFileId、发布版本、哈希、时间、偏好迁移到 dev 项目，移除重复源码登记；保留 UUID 和工坊条目，不重建条目。
- 来源切换交付分别核对错误的 `widgets/<slug>` 不存在、目标 `data/widgets/dev/<slug>` 存在、“开发中”入口可见。

## 旧内置输出的精确处理

- CMake 的 `copy_directory` 不删除已无源码对应项的旧子目录。仓库 `widgets/<slug>/` 已不存在、输出仍存在时，核对绝对路径与 UUID，只移走或删除该精确生成子目录，再标准构建、重启刷新来源。
- 错误源码仍存在时，先在版本分支独立提交删除源码，再整理输出。不清整个 `.build`；不删除或手工编辑 `data/widgets/packages.json`，不删 `installed/`、`dev-disabled/`、组件存储、布局等用户状态。
- 需证据时优先把精确旧目录移到 `.codex-probes/`，确认新来源正常后由用户决定清除。

## 官方社区封面

- manifest 预览名统一 `workshop-preview.png`。背景使用 `developer_assets/workshop_widgets/community-preview-background.png`，它是商店源 `SnowDesktop_SteamAssets/source/key_art_portrait.png` 的标准副本。
- 不用 `page_bg_raw.jpg`、系统壁纸、已叠组件内容的成品或另选单独背景。按真实跨度生成 512×512 并目检后写入 manifest、打包：

  ```text
  snowwidget preview ... --appearance glass-light --background developer_assets/workshop_widgets/community-preview-background.png --canvas-size 512 --padding 48
  ```

- 源美术更新时先从上述商店源同步标准副本，再重生成全部官方社区封面。
