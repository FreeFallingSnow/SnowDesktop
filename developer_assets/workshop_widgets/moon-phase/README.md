# 月相 / Moon Phase

离线桌面组件，默认占用 2×2 格。打开组件设置，在组件预设中选「常规」或「纯月相」。常规模式显示月相标题和下一次关键月相的本地日期，纯月相模式只显示月面。两种模式共用相同的月面比例和中心。

An offline 2×2 widget. Choose **Card** or **Moon Only** in the component presets. The card shows the local date of the next major phase. Moon Only displays the lunar disk alone. Both presets use the same disk size and position.

## 数据和素材 / Data and artwork

- 使用 [Astronomy Engine 2.1.19](https://github.com/cosinekitty/astronomy) 离线生成 2000–2100 年间每 12 小时的月面光照角和精确到分钟的关键月相时刻。月面按地心、北向朝上示意绘制，日期使用 Windows 系统时区。超出范围时，常规模式显示日期范围提示。组件包包含生成的数据，运行时使用 Lua 查表。
- Moon textures adapted from [CGI Moon Kit](https://svs.gsfc.nasa.gov/4720/), NASA’s Scientific Visualization Studio. The December 2025 color map (lroc_color_2k.jpg) was projected on a sphere and shaded into 32 transparent lunar disks. The Moon uses a north-up, geocentric view with a subtle fill on its unlit side.
- 组件代码采用 GPL-3.0-only，见 LICENSE。月面源素材适用 [NASA SVS 使用说明](https://svs.gsfc.nasa.gov/help/#frequently-asked-questions)。
