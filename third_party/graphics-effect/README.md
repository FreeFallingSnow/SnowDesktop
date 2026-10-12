# OpenHarmony GraphicsEffect

SnowDesktop 改编了 OpenHarmony GraphicsEffect 的部分边缘光和阴影计算，用于玻璃材质的透明度遮罩。

## 来源

- 上游：[openharmony/graphic_graphics_effect](https://github.com/openharmony/graphic_graphics_effect)。
- 固定提交：`da8e11652a705ea2141c35de1a1fff501148740e`。
- 版权：Copyright (c) 2025-2026 Huawei Device Co., Ltd.
- 许可证：Apache-2.0，完整文本见 [LICENSE](LICENSE)，并保留于发行包中的 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)。

## 改编的计算

| 上游文件 | 使用的内容 |
| --- | --- |
| `src/effect/shader/ge_sdf_edge_light_shader.cpp` | 边缘宽度上下限、smoothstep 宽度响应、内外光晕边界、阈值及距离衰减 |
| `src/effect/shader/ge_frosted_glass_effect.cpp` | 对向扇区遮罩、smoothstep 羽化及指数内阴影衰减 |

本地实现见 [flat_glass_rim.h](../../src/theme/flat_glass_rim.h)。SnowDesktop 于 2026 年将这些计算移植到 CPU 透明度遮罩，供图标底板、原生面板、Dock 和组件预览共用；光照方向、角度、亮度、宽度和光晕参数由本项目调校。

背景色彩增强与内外折射取样仍仅作为参考，未移植。背景使用 Windows 原生合成，发行包不包含 OpenHarmony 运行时或着色器编译器。上述来源不表示与荣耀 MagicAnimation 或 MagicOS 的实现及参数相同。
