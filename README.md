# SnowDesktop

原生 Win32 桌面图标引擎 —— 替代 Windows 资源管理器桌面图标层，使用 Direct2D / DirectComposition 硬件加速渲染。

## 概述

SnowDesktop 隐藏了资源管理器原生的 `SysListView32` 桌面图标层，在一个独立的全屏窗口中使用 DirectComposition + Direct2D 重新绘制桌面图标。它实现了完整的 Shell 集成：图标枚举、Shell 上下文菜单、OLE 拖放、内联重命名、图标可见性控制，以及固定自由排列的网格布局。

## 功能

- **硬件加速渲染** — 基于 Direct3D 11 + Direct2D 1.1 + DirectComposition，支持多显示器
- **Shell 深度集成** — 通过 `IShellFolder` / `IEnumIDList` 枚举桌面项，`SHChangeNotifyRegister` 实时监听文件变化
- **固定自由排列网格** — 图标拖入空槽位，不挤压其他图标；布局持久化到 `SnowDesktop.layout.json`
- **OLE 拖放** — 完整的 `IDropTarget` / `IDropSource` 实现，支持桌面图标间拖放和外部资源管理器拖入
- **原生 Shell 上下文菜单** — 在正确的位置调用 `IContextMenu`，支持所有 Shell 扩展
- **内联重命名** — F2 触发，表现与资源管理器一致
- **桌面图标可见性控制** — 读取 Windows 注册表设置，托盘菜单切换"此电脑"、"用户的文件"、"网络"、"控制面板"、"回收站"
- **快捷键** — `Delete`、`Ctrl+C/X/V/A`、方向键导航、`F5` 刷新、`Esc` 退出
- **边框拖拽框选** — 多选支持
- **托盘菜单** — 重新加载、排序、切换原生/自定义桌面、退出
- **紧急恢复** — `--restore-explorer-icons` 标志恢复原生桌面图标

## 构建

### 要求

- Windows SDK（10.0.19041 或更新）
- Visual Studio 2022 / 2026 或更新
- CMake 3.24+

### 步骤

```bat
git clone https://github.com/FreeFallingSnow/SnowDesktop.git
cd SnowDesktop
build.bat
```

或手动：

```bat
mkdir .build && cd .build
cmake -G "Visual Studio 18 2026" -A x64 ..
cmake --build . --config Release
```

输出位于 `.build\Release\SnowDesktop.exe`。

## 使用

直接运行 `SnowDesktop.exe`。程序会自动：

1. 查找 `Progman` → `SHELLDLL_DefView` → `SysListView32` 窗口链
2. 隐藏原生桌面图标层
3. 创建覆盖所有显示器的 DirectComposition 窗口
4. 枚举并渲染桌面图标

托盘图标提供重新加载、排序、切换原生/自定义桌面和退出功能。

### 命令行选项

| 参数 | 说明 |
|------|------|
| `--restore-explorer-icons` | 恢复资源管理器原生图标后退出 |
| `--smoke-test-ms=N` | 启动 N 毫秒后自动退出（用于冒烟测试） |

### 紧急恢复

如果程序异常退出导致桌面图标丢失：

```powershell
SnowDesktop.exe --restore-explorer-icons
```

## 架构

```
main.cpp
  └─ SnowDesktopApp (app.h, ~9900 行)
       ├─ 窗口管理 (Progman 挂钩、多显示器、DirectComposition 视觉树)
       ├─ Shell 集成 (IShellFolder、PIDL、SHChangeNotify)
       ├─ 图标渲染 (Direct2D / IShellItemImageFactory)
       ├─ 网格布局 (GridPage、固定槽位、JSON 持久化)
       ├─ 输入处理 (键盘、鼠标、框选)
       ├─ 拖放 (IDropTarget / IDropSource)
       ├─ 上下文菜单 (IContextMenu)
       └─ 托盘菜单

types.h       — 数据结构 (GridCell、DesktopItem、DesktopWidget 等)
constants.h   — 窗口类、命令 ID、网格布局常量
utils.h/.cpp  — Shell、注册表、位图、JSON 工具函数
```

### 关键依赖

| 库 | 用途 |
|------|---------|
| `d3d11` / `dxgi` | Direct3D 11 GPU 渲染 |
| `d2d1` | Direct2D 1.1 矢量与文本渲染 |
| `dcomp` | DirectComposition 视觉树合成 |
| `dwrite` | DirectWrite 文本格式化 |
| `shell32` / `shlwapi` | Shell 项枚举、PIDL 操作 |
| `ole32` | OLE 拖放 (`IDropTarget`/`IDropSource`) |
| `comctl32` | 通用控件版本 6 (视觉样式) |
| `windowscodecs` | Windows 映像编解码器 |

无第三方框架 —— 纯 Win32 C++20 API。

## 路线图

该概念验证专注于桌面图标引擎本身，不移动实际的桌面文件或快捷方式。后续方向可能包括：

- 桌面图标与用户文件完全解耦
- 自定义图标分组/分页
- 更丰富的布局选项

## 许可

本项目代码仅供学习参考。
