# 天空素材生成记录

工具：Codex 内置 `imagegen`。模式：新图生成，不引用 Bing 截图的图像内容；非透明背景。
生成文件复制进本组件的 `assets/`，原始生成文件保留。没有对生成位图进行手工修图或拼贴。
图标、昼夜色调、阴天加深和雷暴闪电由 Lua 绘制/叠加，文字和预报卡片来自真实组件渲染。

晴空图的构图目标：方形，蓝色天空与右侧轻薄白云，左侧和下方留出低细节区域，
不用太阳圆盘、地平线、文字或 UI；钴蓝与柔和灰蓝渐变，清晰而柔和的真实云层。

其余五幅图的完整共同提示：

> Create one original high-quality square sky-only atmospheric background bitmap for a compact desktop weather widget. Photorealistic soft cinematic sky and cloud texture, elegant restrained Microsoft-weather-card-like mood, no UI, no text, no letters, no numbers, no weather symbols, no buildings, no landscape, no horizon, no sun disc. Composition: left 65% and lower half calm and low detail with generous negative space for temperature and forecast tiles; weather detail concentrated toward upper right. Avoid high contrast behind text. 1024x1024.

每幅追加的场景提示：

- `cloudy.png`: Pearl-grey and slate-blue cloudy daytime sky, generous fluffy stratocumulus filling upper right, blue glimpses left; distinctly cloudy rather than a sunny clear blue sky.
- `rain.png`: Rainy slate and indigo sky, layered dark storm clouds upper right, fine visible diagonal rain streaks concentrated right, muted hazy wet atmosphere, no lightning.
- `snow.png`: Snowy winter sky, frosty desaturated blue and soft silver cloud veil, gentle small falling snowflakes scattered mostly right and bottom; calm winter atmosphere, no landscape.
- `fog.png`: Dense fog atmosphere, misty silver-blue horizontal layers, very low contrast soft diffuse light, cloudy overcast sky obscured by fog, no distinct clouds or landmarks.
- `night.png`: Clear night sky, deep indigo blue with subtle few small natural stars concentrated upper right, barely visible wispy night clouds, serene realistic sky, no galaxy, no moon disc.

工坊封面使用仓库统一背景 `developer_assets/workshop_widgets/community-preview-background.png`，
按默认 4×3 跨度真实渲染到 512×512 画布，48 像素边距。
