# skill 目录说明

本目录存放面向 AI Agent / 开发者的 **SparkDesktop 开发 Skill**，用于辅助开发 SparkDesktop 的 Lua 桌面组件。

## 包含的 Skill

### [sparkdesktop-lua-widget](./sparkdesktop-lua-widget/)

**作用**：指导 AI Agent 创建、修改、调试、验证和打包 SparkDesktop 的 Lua 桌面组件（widget）。

适用场景：

- 编写 `widget.json` 清单与 `main.lua` 脚本
- 添加绘图（`render()`）或鼠标交互行为
- 使用组件存储（`storage`）与设置界面（`settings`）
- 查询桌面项目（`desktop` API）
- 声明组件权限（`permissions`）
- 诊断组件无法加载或渲染的问题

该 Skill 基于 SparkDesktop 内置的沙箱 Lua API，每个可运行组件是一个包目录：

```text
my-widget/
├── widget.json   # 组件清单（id、slug、入口、权限、本地化）
└── main.lua      # 组件脚本（render 等回调）
```

## 目录结构

```
skill/
└── sparkdesktop-lua-widget/
    ├── SKILL.md                     # Skill 主文档（工作流与规则）
    ├── agents/openai.yaml           # Agent 接口定义（display_name / prompt）
    ├── assets/widget-template/      # 组件模板（复制为新的组件包起点）
    └── references/
        ├── api.md                   # SparkDesktop Lua API 参考
        └── package-v1.md            # 组件包格式 v1 规范
```

## 使用方式

1. **AI Agent**：将本 Skill 加载为可用的开发指导（引用 `SKILL.md` 与 `references/` 文档），按 `SKILL.md` 中的工作流创建/修改组件。
2. **开发者**：直接阅读 `SKILL.md` 与 `references/api.md` 学习组件开发；复制 `assets/widget-template` 作为新组件起点。

## 说明

- 该 Skill 不随软件运行时加载，仅作为仓库级开发文档随发布包分发（`release/skill/`）。
- 组件开发完成后使用 `snowwidget validate <directory>` 验证、`snowwidget pack` 打包为 `.snowwidget` 组件包。
