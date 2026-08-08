# SnowDesktop 统一发布流程

`scripts\release.bat` 是人工发布入口。它打开一个无需额外依赖的
PowerShell TUI，集中显示当前版本、源码分支、工作区、二进制 Release 仓库和
发行包状态，并提供以下动作：

1. 使用仓库标准入口 `scripts\build.bat` 构建 Release；
2. 生成携带版、MSIX、调试符号和 Partner Center 上传包；
3. 将携带版内容同步到 `release\` 二进制仓库，但不立即提交；
4. 将 `release/vA.B.C.0` 压缩合并到本地 `main` 并创建本地标签；
5. 在人工测试本地 `main` 后，推送源码及二进制仓库；
6. 可选创建 GitHub Release 并上传公开附件。

本地压缩合并与远程发布是两个独立步骤。选择远程动作时必须再次输入当前
版本号，避免跳过本地检查。

## Agent/CLI 入口

`scripts\release.bat` 无参数时进入 TUI，带命令时使用同一个
`scripts/release_manager.ps1` 引擎进入 CLI。CLI 不会等待按键，适合
Agent、CI 或其他自动化调用：

```bat
scripts\release.bat status
scripts\release.bat status -Json
scripts\release.bat package
scripts\release.bat sync-release
scripts\release.bat prepare
scripts\release.bat open
```

其中 `prepare` 等价于“构建打包 + 本地同步二进制 Release 仓库”，不会创建
提交或推送。如果二进制 Release 仓库在同步前已有未提交修改，Agent 必须先
审查，再为 `sync-release` 或 `prepare` 增加
`-Yes -ConfirmVersion A.B.C.0`。

会改变源码 Git 历史或远程状态的命令必须显式确认版本：

```bat
scripts\release.bat squash -Message "v1.0.0.0 - 更新说明" ^
  -Yes -ConfirmVersion 1.0.0.0

scripts\release.bat publish -Yes -ConfirmVersion 1.0.0.0

scripts\release.bat github-release -Yes -ConfirmVersion 1.0.0.0
```

`squash` 只执行本地压缩合并和本地标签创建。`publish` 仅适用于已经测试过的
本地 `main`，它依次推送源码 `main`/标签，并提交和推送二进制 Release 仓库。

`github-release` 优先使用已安装并登录的 GitHub CLI (`gh`)；没有 `gh` 时也
可使用环境变量 `GITHUB_TOKEN` 调用 GitHub API。不要把令牌写进参数、脚本或
仓库。Agent 还可以在外层通过已授权且支持 Release API 的 GitHub 连接器完成
同一动作。GitHub Release 默认公开上传携带版和 SHA-256 清单；只有 MSIX
已经签名时才会同时公开上传 MSIX。Partner Center 专用的 `.msixupload` 不会
作为公开附件上传。

## 每版本目录

`version.json` 是唯一版本来源。所有发行文件和过程文档统一写入：

```text
artifacts\
└─ v1.0.0.0\
   ├─ SnowDesktop-portable-x64-1.0.0.0.zip
   ├─ SnowDesktop-Store-x64-1.0.0.0.msix
   ├─ SnowDesktop-Store-x64-1.0.0.0.appxsym
   ├─ SnowDesktop-Store-x64-1.0.0.0.msixupload
   ├─ SHA256SUMS.txt
   ├─ package-info.json
   ├─ release-summary.md
   ├─ release-notes.md
   ├─ release-state.json
   ├─ release-repository-status.txt
   └─ logs\
```

`release-notes.md` 首次打包时生成模板，后续重新打包不会覆盖人工填写的内容。
其他摘要、哈希和状态文件由脚本更新。临时打包目录会在成功后自动删除。

只需要生成安装包时，使用 `scripts\release.bat package`。底层实现位于
`scripts\package_release.ps1`，通常无需直接调用。

## 包内容与运行模式

携带版和 MSIX 使用同一份 `SnowDesktop.exe` 与
`SnowDesktopTaskbarHook.dll`。程序在运行时检查包身份：

- 携带版将数据写入 `exe\data`，通过注册表 Run 项管理开机自启；
- MSIX 将数据写入 `LocalState\data`，通过 MSIX `StartupTask` 管理自启；
- 携带版直接使用程序目录的 `widgets`；
- MSIX 将内置组件复制到可写的 `LocalState\data\widgets`。

打包脚本会为任务栏、开始菜单、搜索和系统设置等 Shell 场景生成透明的
target-size、`altform-unplated` 和 `altform-lightunplated` 图标，并通过
Windows SDK `makepri.exe` 创建 `resources.pri`。16–48 像素使用简化图标，
60–256 像素使用完整图标。

包清单中的支持语言由 `lang\*.json` 文件名自动生成。文件名必须是有效的
BCP-47 语言标签，例如 `en-US.json`、`zh-CN.json`。默认语言 `en-US`（如果
存在）始终排在清单首位；Partner Center 会据此识别软件包支持的语言。

## Microsoft Store 身份

正式产品的公开身份保存在 `packaging\store-identity.json`。Partner Center
中的 Identity、Publisher、PFN、Package SID 或 Store ID 变化时，应同步更新
该文件。

未配置正式身份时，可以生成仅供结构验证的开发包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/package_release.ps1 -Development
```

开发包默认不签名，不能直接安装或提交。正式商店上传包无需使用本地测试
证书；如需旁加载测试，证书主题必须与清单 Publisher 完全一致，并在测试机
信任该证书。

推荐先通过 Windows 的安全证书导入界面将 PFX 导入个人证书库，再按指纹签名：

```powershell
.\scripts\release.bat package -CertificateThumbprint 0123456789ABCDEF0123456789ABCDEF01234567
```

机器证书库可额外传入 `-CertificateStoreLocation LocalMachine`。这种方式不会把
PFX 密码放进 PowerShell 或 SignTool 的子进程命令行。无密码 PFX 仍可使用
`-CertificatePath`，但脚本不再接受明文 `-CertificatePassword`。PFX 和 Partner
Center 登录材料不得提交到仓库。
