# Native top-title-bar export verification / 原生顶部标题栏导出验证

Verified on 2026-10-29. This record covers the private offscreen exporter and
website asset provenance, not a full application test run or publication.
本记录验证私有离屏导出与网站素材来源，不代表宿主全量测试或发布。

| Attempt / 尝试 | Candidate Run | Result / 结果 |
| --- | --- | --- |
| `61b06a97c4b9db7d286281f7b660484bb44106a2` | `a49a1386a23c4ca284c19372503e9ae2` | Standard Release product build passed; `widget_author_preview_cli` passed in 40.34 s / 标准 Release 产品构建通过，原生预览测试通过 |
| `2a98d76a61dd1e1b6cb16126ffb37a113931b54d` | `465d034e45ca424eafe7af040c361957` | Targeted `SnowDesktopWidgetAuthorPreviewCliTests` and host/tool dependencies built; 1/1 test passed in 43.54 s / 定向目标及宿主、工具依赖编译通过，1/1 测试通过 |

The final test exercises private argument counts 14/16/18 and the explicit
19-argument `top`/`bottom` forms. Legacy output omits the three title-bar fields
and every collection-mode image equals the explicit bottom image. Top images
change for supported large storage modes; compact/large-folder modes retain
their pixels. Invalid `middle` fails before creating the output directory.
最终测试实际调用旧 14/16/18 参数与新 19 参数形态；旧输出无新增三字段，所有集合模式
保持底态像素。支持的大型收纳模式产生顶部图像，紧凑与大文件夹模式像素不变；非法值在
创建输出目录前失败。两个构建日志均未检出编译或链接警告。

The website's 1,080 images retain first-run provenance: 54 supported variants,
10 languages and two foregrounds. Generation verified the runtime snapshot and
preserved SHA-256 of all 1,643 existing component resource files.
网站 1080 张图保留首轮真实来源：54 布局 × 10 语言 × 2 前景；运行依赖快照已核对，
原有 1643 个组件资源文件 SHA-256 全部保持不变。

After the second attempt, all 71 English/light-foreground images were rendered
again at DPI 216 on 1024 × 1024 canvases. Using the same frozen first-run
dependencies with the immutable second-run host/tool executables, every decoded
RGBA pixel matched the first-run PNG. This controlled comparison does not claim
to re-export all 20 language/foreground batches with the second binary.
第二次尝试后，在相同首轮冻结依赖上使用第二轮不可变宿主、工具，重新导出全部 71 张
英语浅色前景图；DPI 216、1024 × 1024 画布的解码 RGBA 逐像素一致。该受控比对不冒称
用第二轮二进制重新导出了全部 20 批次。

Evidence / 证据（local ignored receipts / 本地忽略目录中的回执）：

- `.build/collaboration/94c678e4238f4664991a8c24ba8db53a.json`
- `.build/collaboration/704eaa0ee99847328fbf878dbd87659a.json`
- `website/.tmp/native-titlebar-previews/generation.json`
- `website/.tmp/native-titlebar-compatibility/comparison.json`

Host SHA-256 / 宿主哈希：

- First / 首轮：`56395edfdb94c2fe012a9b8a2994fe265b817fe1340ea5363638da6616322fe6`
- Final / 最终：`76226144ab54f44c72052cd7f79b62a4231bd31dcc26734e4d4d7797e24a74f5`

The first joint product batch activated its saved runtime. The final targeted
compatibility batch saved test artifacts without activating them; this record
does not claim the running application was switched to the final binary.
首轮共同产品批次已激活；最终兼容修订的定向批次只保存测试产物，未激活。
