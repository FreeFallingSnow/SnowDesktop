# Microsoft WinUI acrylic noise

Upstream: <https://github.com/microsoft/microsoft-ui-xaml>

Pinned commit: `7b68d3e0b771a57d80098799406234efee479517`.

`NoiseAsset_256X256_PNG.png` is the unmodified upstream asset from
`controls/dev/Materials/Acrylic/Assets/NoiseAsset_256X256_PNG.png`.
SHA256: `4f2aa94a2e345a32dae689176b86c644bef87081ff9daef8c77417731862031b`.

Copyright (c) Microsoft Corporation. All rights reserved. The MIT license is
retained in `LICENSE` and reproduced in the root `THIRD_PARTY_NOTICES.md`.

`scripts/generate_acrylic_noise_asset.py` embeds the original PNG into
`src/acrylic_noise_asset.h`; run it with `--check` to verify the generated copy.
The host and author preview decode this built-in texture once and apply it at
2% brush opacity, following upstream `AcrylicBrush::sc_noiseOpacity`. They tile
it at physical-pixel size without selecting black or white noise from the text
theme. No WinUI brush implementation is copied.

此目录保留微软官方亚克力噪点原图及 MIT 许可证。宿主和组件预览共用嵌入的
256×256 灰度纹理，以 2% 画刷透明度叠加，并保持物理像素尺寸和稳定平铺锚点。
