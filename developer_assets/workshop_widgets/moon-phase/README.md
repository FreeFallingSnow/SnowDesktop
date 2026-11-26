# 月相 / Moon Phase

离线桌面组件，默认占用 2×2 格并跟随全局主题。可在组件设置或组件右键菜单切换「纯月相」。开启后背景和边框完全透明，只显示月面。关闭后恢复全局主题、标题和下次关键月相的本地日期。切换时月面尺寸和位置保持一致。

This offline 2×2 widget follows the global theme by default. Toggle **Moon Only** in component settings or the context menu to display just the lunar disk over a transparent surface. Turning it off restores the global appearance, title and local date of the next major phase. The lunar disk keeps the same size and position.

1.1.0 保留原组件 UUID，并将旧实例的纯月相预设迁移为开关。组件使用现有 API v2 外观与存储能力。

Version 1.1.0 keeps the component UUID and migrates the previous Moon Only preset to the toggle. The widget uses the existing API v2 appearance and storage capabilities.

## 数据和素材 / Data and artwork

- 使用 [Astronomy Engine 2.1.19](https://github.com/cosinekitty/astronomy) 离线生成 2000–2100 年间每 12 小时的月面光照角和精确到分钟的关键月相时刻。月面按地心、北向朝上示意绘制，日期使用 Windows 系统时区。超出范围时，常规模式显示日期范围提示。组件包包含生成的数据，运行时使用 Lua 查表。
- Moon textures adapted from [CGI Moon Kit](https://svs.gsfc.nasa.gov/4720/), NASA’s Scientific Visualization Studio. The December 2025 color map (lroc_color_2k.jpg) was projected on a sphere and shaded into 32 transparent lunar disks. The Moon uses a north-up, geocentric view with a subtle fill on its unlit side.
- 组件代码采用 GPL-3.0-only，见 LICENSE。月面源素材适用 [NASA SVS 使用说明](https://svs.gsfc.nasa.gov/help/#frequently-asked-questions)。
