# WinUI 亚克力噪点纹理

此目录保存微软 WinUI 的亚克力噪点原图，供 SnowDesktop 的宿主和组件预览共用。

## 来源

- 上游：[microsoft/microsoft-ui-xaml](https://github.com/microsoft/microsoft-ui-xaml)。
- 固定提交：`7b68d3e0b771a57d80098799406234efee479517`。
- 上游路径：`controls/dev/Materials/Acrylic/Assets/NoiseAsset_256X256_PNG.png`。
- 版权：Copyright (c) Microsoft Corporation. All rights reserved.
- 许可证：MIT，完整声明见 [LICENSE](LICENSE) 和 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)。

## 本地资源与使用方式

[NoiseAsset_256X256_PNG.png](NoiseAsset_256X256_PNG.png) 保持上游原文件，SHA-256 为 `4f2aa94a2e345a32dae689176b86c644bef87081ff9daef8c77417731862031b`。

[generate_acrylic_noise_asset.py](../../scripts/generate_acrylic_noise_asset.py) 将原图嵌入 [acrylic_noise_asset.h](../../src/ui/render/acrylic_noise_asset.h)。在仓库根目录执行 `python scripts/generate_acrylic_noise_asset.py --check` 可核对生成内容。

渲染时使用 256×256 纹理，以 2% 画刷透明度、物理像素尺寸平铺，采用上游 `AcrylicBrush::sc_noiseOpacity` 的取值。复用范围是这张纹理及该参数，不包含 WinUI 画刷实现代码。
