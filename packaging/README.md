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
scripts\release.bat package -ReloadShell
scripts\release.bat sync-release
scripts\release.bat prepare
scripts\release.bat prepare -ReloadShell
scripts\release.bat open
```

其中 `prepare` 等价于“构建打包 + 本地同步二进制 Release 仓库”，不会创建
提交或推送。如果二进制 Release 仓库在同步前已有未提交修改，Agent 必须先
审查，再为 `sync-release` 或 `prepare` 增加
`-Yes -ConfirmVersion A.B.C.0`。

发布 CLI 默认不会关闭 SnowDesktop 或重启 Explorer。构建产物被正在运行的
SnowDesktop 或 Explorer 任务栏 Hook 占用时，应先正常退出应用；需要自动解除占用时，
为 `package` 或 `prepare` 增加 `-ReloadShell`。该选项会终止 SnowDesktop 并短暂重启
Explorer。发布 TUI 检测到占用时会显示相同副作用并请求确认。

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

携带版和 MSIX 使用同一份 `SnowDesktop.exe` 与运行时载荷。程序在运行时检查包身份：

- 携带版将数据写入 `exe\data`；
- MSIX 将数据写入 `LocalState\data`；
- 两种部署统一通过当前用户的 `\SnowDesktop\Startup` 登录计划任务管理自启，
  同一时间只允许其中一个部署成为任务目标；1.0.4.0 仍保留旧 MSIX
  `StartupTask` 清单声明作为一次性迁移输入，迁移后会禁用它并删除旧携带版
  Run/StartupApproved 值；
- 携带版直接使用程序目录的 `widgets`；
- MSIX 将内置组件复制到可写的 `LocalState\data\widgets`。

主程序使用 Microsoft Windows App SDK 2.4.0 自包含部署。MSBuild 会在
`.build\Release\SnowDesktop.deployment.json` 中记录构建实际选择的运行时 DLL、
PRI、XBF、WinMD、资源文件、哈希及官方 `package.appxfragment`；携带版、MSIX
和 Steam 包都只按这份清单逐项复制，不会通配复制构建目录。打包会拒绝绝对路径、
路径穿越、大小写冲突、缺失文件及哈希不一致。MSIX 还会把清单列出的官方
activatable-class 扩展合并进 `AppxManifest.xml`，因此目标机器无需预装 Windows
App SDK Runtime。清单中来源为 Windows App SDK、Windows ML 和 WebView2 的运行时
DLL、PRI、WinMD、XAML 资源及语言卫星文件统一放在 `SnowDesktop.Runtime` 私有程序集
目录；SnowDesktop 自带的任务栏/壁纸 Hook 与 32 位注入器也放在该目录。主程序的 XBF、
PRI、WinMD 与应用资源仍保留在包根目录。打包脚本会将主程序内嵌
WinRT 激活清单迁移到私有程序集清单，并同步重写 MSIX activatable-class 路径。
Windows App SDK 与 C++/WinRT 的许可和 NOTICE 也由同一清单
复制到载荷的 `licenses` 目录。Windows App SDK ML 以独立的 MSBuild 项提供
Machine Learning、ONNX Runtime 和 DirectML DLL；清单会显式收集这三个文件及其许可和
NOTICE，避免仅复制主 Windows App SDK 项时遗漏。WebView2 WinRT Core DLL 与 WinMD 也
来自独立的 NuGet 项，并以同样方式连同许可和 NOTICE 纳入清单。携带版和 MSIX 包含
`snowwidget.exe`，但不携带只适用于 Steam 的创意工坊管理器；Steam 包才包含管理器，且将
`steam_api64.dll` 放入同一运行时目录并通过私有程序集加载。所有载荷都会校验 Agent Skill
内嵌 CLI 与独立 CLI 完全一致。

任务栏 Hook 通过 XAML Diagnostics TAP 接入 Explorer。该接口没有对应的进程级关闭 API，
因此 Hook 模块可能一直映射到 Explorer 重启为止。SnowDesktop 不再直接注入构建或发行目录
里的 DLL，而是先复制到 `%TEMP%\SnowDesktop\RuntimeHooks\` 下的进程专属目录；Wallpaper
Engine 的 32/64 位 Hook 也复用同一部署机制，避免目标进程无法加载受保护的 MSIX 安装目录。
正常退出后即使临时副本仍在目标进程中，也不会锁住便携目录、安装目录或下一次构建的输出文件。
启动时会清理已经不再占用的旧临时副本。

打包脚本会为任务栏、开始菜单、搜索和系统设置等 Shell 场景生成透明的
target-size、`altform-unplated` 和 `altform-lightunplated` 图标，并通过
Windows SDK `makepri.exe` 创建 `resources.pri`。16–48 像素使用简化图标，
60–256 像素使用完整图标。MSIX 的包级 PRI 会合入 Windows App SDK 自带的
`Microsoft.UI`、`Microsoft.UI.Xaml` 和运行时资源图，使安装态 WinUI 能加载主题
字典。生成索引时只暂时排除已经由应用 PRI 或组件 PRI 覆盖的重复输入，完成后会
恢复 `SnowDesktop.pri` 与 WinUI 根目录资源，并校验上述组件资源图确实存在。

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
