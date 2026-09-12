# 相册轮播 / Photo Album

独立社区组件，UUID `104e773c-f2a7-4b19-a784-02a20311342b`，不随宿主作为内置组件分发。

- 从“管理相册”多选图片或文件夹，可重复添加、移除和调整来源顺序。
- 按来源顺序播放，文件夹内按文件名排序，仅当前层；通过“刷新”重新扫描。
- 支持单张静态照片、手动上一张/下一张、暂停、3–300 秒间隔和随机播放。
- “照片”列表可选择某一张并暂停；关闭“裁切填充相框”可完整显示图片。
- 最多 128 个来源、10000 张照片；每个目录最多 10000 个条目。超限或读取失败会提示。
- 支持 JPG/JPEG、PNG、BMP、GIF、TIFF；WebP 依赖 Windows 解码器。只播放静态首帧。
- 来源存储为实例专属的授权句柄。只请求读取权限，不修改、复制或删除原图。
- 移除来源会回收其授权句柄；暂停、文件夹排序等组件状态互不影响其他相册实例。
- 示例萌宠仅用于添加预览；新实例从空列表开始，不自动读取任何本地目录。

## 宿主兼容与发布顺序

最低版本为 `1.0.6.0`，并且必须支持：
`task.filesystem.image`、`task.filesystem.list.names`、`task.filesystem.picker.multiple`。
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

A standalone community photo slideshow. Add multiple files or folders, reorder or remove sources,
browse the expanded photo list, pause on one photo, choose a 3–300 second interval, shuffle,
or switch between cover and contain. Folders include direct children only; refresh rescans them.
Limits: 128 sources and 10000 photos, with at most 10000 entries per folder. Animated files use
their first frame; WebP depends on Windows codecs. Originals are never modified. Saved sources
are opaque instance-scoped read grants. New instances start empty.

Publish only after a host with all three required filesystem features is released. Version
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
