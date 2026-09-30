# OpenHarmony GraphicsEffect references and adapted portions

Upstream: <https://github.com/openharmony/graphic_graphics_effect>

Pinned commit: `da8e11652a705ea2141c35de1a1fff501148740e`.

Copyright (c) 2025-2026 Huawei Device Co., Ltd. Licensed under Apache-2.0;
the complete license is retained in `LICENSE` and reproduced in the root
`THIRD_PARTY_NOTICES.md`, which is included in release packages.

`src/flat_glass_rim.h` adapts calculations from these upstream files:

- `src/effect/shader/ge_sdf_edge_light_shader.cpp`: minimum/maximum edge width,
  smoothstep width response, independently bounded inner/outer bloom,
  bloom threshold and rational distance falloff.
- `src/effect/shader/ge_frosted_glass_effect.cpp`: opposing angular fan masks
  with smoothstep feathering, and the inner-shadow exponential decay.

SnowDesktop evaluates the adapted equations in its existing CPU opacity-mask
paths shared by icon plates, native panels, Dock and author previews. Its
angular direction, fan opening/feather, ambient level, relative lobe strengths,
widths, bloom amount and shadow placement are its own visual tuning. These
values are not claimed to be Honor MagicAnimation or MagicOS parameters.

The upstream frosted shader's background vibrancy and inward/outward refraction
sampling are references only; they have not been ported. SnowDesktop retains
its existing native Windows composition backdrop and avoids a second backdrop
blur for icons already inside a glass panel. No OpenHarmony runtime or shader
compiler is bundled, and no Honor installer binary or asset is redistributed.

本项目把上述边缘光、扇区遮罩和指数衰减计算移植到现有 CPU 透明度遮罩中。
光照方向、角度、最低亮度、宽度和光晕范围为 SnowDesktop 自身调校，不能据此
认定与荣耀实现相同。上游背景色彩处理和内外折射取样尚未移植；背景仍由现有
Windows 原生合成提供。
