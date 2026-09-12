# 相册轮播 / Photo Album

独立社区组件，UUID `104e773c-f2a7-4b19-a784-02a20311342b`，不随宿主作为内置组件分发。

- 从资源管理器直接拖入图片或文件夹，支持同步和异步文件传输；也可从“管理相册”多选。
- 空相册可选择“导入图片”或“绑定文件夹”；添加来源或开始导入后锁定模式，清空全部照片后可重新选择。
- 默认“导入图片”模式：文件夹一次性展开为独立图片条目，不保留目录来源。
- “绑定文件夹”模式只接收文件夹，隐藏单图选择入口并拒绝拖入的单图；已有混合来源保留，清空后按所选模式添加。
- 追加图片和扫描新文件夹时保留当前画面、照片位置和播放进度；扫描完成后一次性更新列表，避免先清空画面再重新加载。
- 右键和管理面板提供“清空全部照片”，确认后取消尚未完成的选图/导入，清空来源及排除记录并回收读取授权；原文件保留。
- 按来源顺序播放，文件夹内按文件名排序，仅当前层；通过“刷新”重新扫描。
- 支持单张静态照片、手动上一张/下一张、暂停、3–300 秒间隔和随机播放。手动前后翻页保持当前播放状态，双击累计两张，手动方向不受随机播放影响。
- 右键“从相册移除此照片”移除打开菜单时的照片，保留原文件；绑定模式用排除记录避免刷新后重现。
- “照片”列表可选择某一张并暂停；右键或轮播设置中的“裁切填充相框”共用同一开关，关闭后完整显示图片。
- 翻页、播放/暂停、计数和管理按钮悬停时叠在图片底部，移开后隐藏；显隐不改变图片尺寸或轮播进度，单张照片保留计数与管理按钮。
- 最多 128 个来源、10000 张照片；每个目录最多 10000 个条目。超限或读取失败会提示。
- 支持 JPG/JPEG、PNG、BMP、GIF、TIFF；WebP 依赖 Windows 解码器。只播放静态首帧。
- 来源存储为实例专属的授权句柄。只请求读取权限，不修改、复制或删除原图。
- 移除来源会回收其授权句柄；暂停、文件夹排序等组件状态互不影响其他相册实例。
- 示例萌宠仅用于添加预览；新实例从空列表开始，不自动读取任何本地目录。

## 宿主兼容与发布顺序

最低版本为 `1.0.6.0`，并且必须支持：
`task.filesystem.image`、`task.filesystem.list.names`、`task.filesystem.picker.multiple`、`interaction.fileDrop`、`interaction.fileDrop.async`。
同版本早期构建不满足条件时应显示不兼容，不尝试用路径或其他权限绕过。
这些能力尚待随宿主发布；先发布并验证新宿主，再发布本组件。当前任务不发布到 Workshop。

开发验证使用当前源码标准构建后运行：

```bat
scripts\widget-dev.bat developer_assets\workshop_widgets\photo-album -Configuration Release -Once -RestartHost
```

实机验收：多文件、多文件夹混合添加；单张静态；顺序及随机轮播；暂停后点击列表固定某张；
重启恢复来源；移除、排序、刷新；取消选择；损坏/删除图片；撤回读取权限；两个实例隔离；
横向、竖向、方形和不同 DPI。桌面交互结论以用户实际验证为准。

## English

A standalone community photo slideshow. Drop local photos or folders from Explorer, including
asynchronous file transfers, or use the picker.
Import mode flattens folders into individual image references without retaining directory sources.
A separate binding mode accepts folders only and retains them for refresh. Choose a mode while the
album is empty; sources or in-progress imports lock it until the album is cleared. Existing mixed
sources are preserved. Appending keeps the current image, position and playback progress while
new folders are scanned. The context menu and management panel offer Clear all photos with a
confirmation; it cancels pending selection/import work and releases grants without deleting originals.
Right-click removes the photo shown when the
menu opened from the album only; bound sources remember exclusions. Manual next/previous preserves
autoplay and double-clicks accumulate two directional steps. Reorder or remove sources,
browse the expanded photo list, pause on one photo, choose a 3–300 second interval, shuffle,
or switch between cover and contain from the context menu or the same setting in slideshow settings.
Navigation, playback, count and management controls overlay the bottom of the photo only while
hovered; showing or hiding them preserves image geometry and playback progress. Single-photo
albums retain the count and management controls. Folders include direct children only; refresh rescans them.
Limits: 128 sources and 10000 photos, with at most 10000 entries per folder. Animated files use
their first frame; WebP depends on Windows codecs. Originals are never modified. Saved sources
are opaque instance-scoped read grants. New instances start empty.

Publish only after a host with the required filesystem and file-drop features is released. Version
`1.0.6.0` alone does not establish compatibility with earlier builds. Desktop interaction,
picker behavior, permission UX and multi-instance runtime checks still require user acceptance.

## 示例素材 / Sample asset

`assets/sample-pet.png` 使用内置 imagegen 生成，仅作相册预览素材。
Generated using the built-in imagegen tool. Prompt:

> Use case: photorealistic-natural. Asset type: sample pet photograph inside a desktop photo album,
> not a UI mockup. A very cute fluffy cream-and-white kitten with round bright eyes and tiny pink
> nose, comfortably lying on a soft oatmeal knit blanket, one little paw resting forward, looking
> gently at the camera. Natural soft window light, warm neutral home interior out of focus,
> detailed soft fur, wholesome candid pet photography. Wide 3:2 composition with the kitten
> centered and enough space around its ears and paws to crop portrait or square. No text, logos,
> borders or watermark.

封面背景固定使用仓库的 `community-preview-background.png`，由真实组件预览命令生成。
The catalog background uses the repository's standard community artwork and the host renderer.

本轮空相册模式选择、拖入画面连续性、清空与右键交互仍待用户实机验证。导入模式最多 128 张独立图片；绑定模式仍可展开最多 10000 张。

Desktop drops, double-clicks and context removal await user acceptance. Import mode supports up to 128 individual photos; binding mode can expand up to 10000. Virtual-file-only and browser URL payloads are not accepted.
