# Fluent System Icons

Microsoft 的图标资源，用于 SnowDesktop 的菜单、状态栏和控制中心。

## 来源

- 上游：[microsoft/fluentui-system-icons](https://github.com/microsoft/fluentui-system-icons)。
- 固定提交：`21d5d02f724be2aaf586564775fff73a18a76eb6`。
- 许可证：MIT，完整声明见 [LICENSE](LICENSE)。

## 本地资源与使用方式

| 资源 | 本地位置与用途 |
| --- | --- |
| Regular 图标字体 | [FluentSystemIcons-Regular.ttf](../../assets/fonts/FluentSystemIcons-Regular.ttf)，嵌入应用，提供菜单等界面的图标 |
| Battery Charge 20 Filled | [status_bar_battery.h](../../src/system/status_bar/status_bar_battery.h)，保留上游 SVG 路径并转换为等价 Direct2D 路径，用于充电指示 |
| Alert 24 Filled | [status_bar_view.cpp](../../src/system/status_bar/status_bar_view.cpp)，使用上游轮廓显示通知图标，创建几何失败时回退到 Regular 字形 |

Regular 字体 SHA-256：`53204be5637d3872892e64aa7012f79f1e7480f03b534ae1d29a66e62a96d7fc`。

上述图标轮廓保持上游形状，颜色由 SnowDesktop 的界面状态和主题决定。发行包包含 Regular 字体和 `licenses/FluentSystemIcons-LICENSE.txt`，不包含 Filled 字体。
