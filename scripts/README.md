# 项目脚本

根目录只保留供开发者直接双击或调用的入口：

- `build.bat`：Release 编译；
- `build_debug.bat`：Debug 编译；
- `release.bat`：无参数打开发布 TUI，带参数作为 Agent/自动化 CLI。

本目录保存入口背后的实现：

- `release_manager.ps1`：统一发布状态、打包、仓库同步、合并及发布流程；
- `package_release.ps1`：生成携带版、MSIX、符号包和商店上传包；
- `squash_release_to_main.bat`：只执行本地 squash、提交和标签；
- `check_l10n.bat` / `check_l10n.py`：检查本地化键和语言文件。

常用命令：

```bat
build.bat
build_debug.bat
release.bat
release.bat status -Json
release.bat package
scripts\check_l10n.bat
```

发布流程的完整说明见 `packaging\README.md`。
