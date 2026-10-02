# 晴空天气 / Sky Weather

独立社区组件，UUID `f05e5ddc-1b1c-48ce-a797-4697a1cf0507`。默认 4×3，支持 2×2 紧凑尺寸和 6×2 横向布局。
卡片显示所选地点的当前气温、体感温度与五日高低温。摄氏/华氏、刷新间隔与天空背景可在组件设置中调整。
开启天空背景时始终使用天气图片和浅色文字，图片配色不随浅色主题改变。
关闭后使用宿主主题背景和对应文字色；不以组件自定义蓝色底覆盖主题背景。

点击城市名打开选择面板：支持全球城市搜索、行政区与国家区分和最近六个实际选择。
首次打开提供搜索提示，不预置城市或推断用户偏好；最近选择仅来自该实例的实际选择。
选城面板沿用宿主弹窗背景与语义配色，不另铺整面背景；保留定位操作行和可滚动的城市结果，并声明键盘导航能力。
搜索使用 Open-Meteo Geocoding API（GeoNames 地名数据）；天气使用 Open-Meteo Forecast API。
中文地点搜索同时查询原名和适用的“市”全名，合并去重后优先展示名称匹配、人口较大的地点，
保留原始坐标并显示市级行政区、省份和国家，不用内置城市坐标替换接口结果。
接口使用所选地点的 `timezone=auto` 和 `current.is_day`，不按电脑所在城市推断昼夜。

“使用当前位置”调用 Windows Geolocator，需组件 `location.read` 可选权限和 Windows 系统位置授权，
只在前台面板的直接用户操作中调用。显示卫星、Wi-Fi 或蜂窝网络来源及系统报告的精度。
Windows 返回 IP、默认、模糊或未知来源时不会自动采用该坐标，保留城市选择并提示粗略位置。
组件不调用第三方 IP 定位服务，不在定时刷新中反复定位。成功选择的坐标和最近城市保存在该实例的本地存储；
获取天气时需将所选坐标发送到 Open-Meteo。关闭面板、切换城市、撤销权限或移除实例会取消相应请求。

背景按官方 WMO WW 表覆盖全部 29 个已文档化代码：晴空 0，多云/阴天 1–3，雾 45/48，
毛毛雨/冻毛毛雨 51/53/55/56/57，雨/冻雨 61/63/65/66/67，降雪/雪粒 71/73/75/77，
阵雨 80/81/82，阵雪 85/86，雷暴/冰雹 95/96/97/99。归为晴空、多云、雨、雪、雾与雷暴场景，
按 `is_day` 调整夜间效果。晴夜使用独立星空资源；雷暴使用雨云和闪电形状；未知代码使用中性背景。
背景静态渲染，不增加持续动画定时器。更新失败保留同地点的成功缓存，切换城市不会显示另一城市的缓存。

本组件直接使用 API v2、宿主 1.0.8.0 及清单所列能力，包括 `task.location.current`、
`draw.textInkMetrics` 和 `widget.backgroundLayer`。字形边界用于城市标题、天气图标及温度单位的对齐。
组件不适配缺少这些能力的宿主；同版本早期构建也可能缺少新能力，清单会阻止加载。
组件应在包含定位和字形测量能力的宿主发布后正式发布；未授权定位仍可使用城市搜索。
Windows 授权、实际定位精度和桌面面板交互需要用户实机验收；离屏预览不访问网络或系统定位。

## 开发与检查

```bat
.build\Release\snowwidget.exe lint developer_assets\workshop_widgets\sky-weather
.build\Release\snowwidget.exe test developer_assets\workshop_widgets\sky-weather
.build\Release\snowwidget.exe quality developer_assets\workshop_widgets\sky-weather
.build\Release\snowwidget.exe preview developer_assets\workshop_widgets\sky-weather developer_assets\workshop_widgets\sky-weather\workshop-preview.png --columns 4 --rows 3 --locale zh-CN --appearance glass-light --background developer_assets\workshop_widgets\community-preview-background.png --canvas-size 512 --padding 48
scripts\widget-dev.bat developer_assets\workshop_widgets\sky-weather -Configuration Release -Once
```

确定性预览可通过 `--storage previewCode=97`、`--storage previewNight=true`、`--storage unit=f`、
`--storage previewTemperature=-24` 和 `--storage previewState=empty|loading|error|permission|stale` 检查状态。
预览温度与天气不是真实当前天气。纯模块测试覆盖 JSON 空值、异常响应、城市切换、迟到完成、
缓存身份、拒权、取消、有限重试、中文城市全名查询、合并排序与迟到搜索完成，以及全部天气码的昼夜背景归类。

## 来源与使用条件

- 代码采用 GPL-3.0-only，见 LICENSE。实现为新写的独立组件，未复制已安装天气组件的源码。
- 天气数据：[Open-Meteo](https://open-meteo.com/)，数据采用 CC BY 4.0；地名来源：[GeoNames](https://www.geonames.org/)。界面保留来源署名。
- API 文档：[天气码与变量](https://open-meteo.com/en/docs#weathervariables)、[全球地名搜索](https://open-meteo.com/en/docs/geocoding-api)。
- 当前无密钥端点按 [Open-Meteo 服务条款](https://open-meteo.com/en/terms) 用于个人、非商用验证。官方商业产品整合或正式商业发布前需安排商业 API 授权或许可合适的服务；数据 CC BY 许可不等于免费 API 商用许可。
- 系统定位：[Microsoft Geolocator](https://learn.microsoft.com/en-us/uwp/api/windows.devices.geolocation.geolocator)。
- `assets/*.png` 是本任务用 Codex 内置 imagegen 生成的原创天空素材，未使用 Bing 截图中的位图；天气图标由组件绘制。
- 工坊封面由真实组件渲染和仓库统一 `community-preview-background.png` 合成，不是设计稿拼贴。
