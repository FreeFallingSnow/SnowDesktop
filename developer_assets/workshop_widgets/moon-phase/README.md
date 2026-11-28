# 月相 / Moon Phase

离线桌面组件，默认占用 2×2 格并跟随全局主题。可在组件设置或组件右键菜单切换「纯月相」。开启后背景和边框透明，月面放大到组件短边的 84%，中心保持原位。关闭后恢复开启前的主题跟随状态及被覆盖的外观设置，并显示标题、当前月相名称和今日日期。

This offline 2×2 widget follows the global theme by default. Toggle **Moon Only** in component settings or the context menu to display an enlarged lunar disk over a transparent surface. The disk spans 84% of the shorter side and keeps the same center. Turning the mode off restores the previous theme-following setting, any appearance values overridden by the mode, the title, the current phase name, and today's local date.

1.1.0 保留原组件 UUID，并将旧实例的纯月相预设迁移为开关。组件使用现有 API v2 外观与存储能力。

Version 1.1.0 keeps the component UUID and migrates the previous Moon Only preset to the toggle. The widget uses the existing API v2 appearance and storage capabilities.

从旧版升级时，如果纯月相已开启且缺少开启前的记录，首次关闭会回到默认跟随。 On upgrade, an active Moon Only instance with no saved appearance returns to the default Follow Global mode the first time it is turned off.

## 数据和素材 / Data and artwork

- 使用 [Astronomy Engine 2.1.19](https://github.com/cosinekitty/astronomy) 离线生成 2000–2100 年间每 12 小时的月相角。组件按当前月相角选择月面图和八种月相名称之一。月面按地心、北向朝上示意绘制，日期使用 Windows 系统时区。星历超出范围时，月相名称处显示范围提示。
- Moon textures adapted from [CGI Moon Kit](https://svs.gsfc.nasa.gov/4720/), NASA’s Scientific Visualization Studio. The December 2025 color map (lroc_color_2k.jpg) was projected on a sphere and shaded into 32 transparent lunar disks. The Moon uses a north-up, geocentric view with a subtle fill on its unlit side.
- 组件代码采用 GPL-3.0-only，见 LICENSE。月面源素材适用 [NASA SVS 使用说明](https://svs.gsfc.nasa.gov/help/#frequently-asked-questions)。
