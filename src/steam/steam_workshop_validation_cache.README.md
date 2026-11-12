# 创意工坊后台验包缓存

`SteamWorkshopSource::QuerySubscriptions` 的本地订阅发现按归档内容复用校验结果。实现位于 [steam_workshop_validation_cache.h](steam_workshop_validation_cache.h)，轮询重建 source 对象时继续使用本进程缓存。

## 复用条件

- 每次读取仍计算文件 SHA-256，不单凭大小或修改时间判断。相同大小、相同时间戳的内容替换会重新验包。
- 缓存键包含归档路径与校验 staging 根。成功、归档格式错误及确定的清单错误（包括最低宿主版本）均可复用，语言筛选与错误提示按当前查询生成。
- 读取租约使用 `FILE_SHARE_READ`，限制验包期间的普通并发写入和替换；首次完整校验后再核对摘要，内容不一致不存缓存。
- 文件不存在、不可读、读取摘要失败、超过归档配额或暂存访问失败时，保留完整校验路径。内容变化后重新验包。
- 默认最多保存 256 项，按最近使用淘汰。缓存不写磁盘，重启后首次检查仍完整验包。
- 主动在线查询、详情/物化以及实际安装保持原来的完整校验路径；缓存不代替权限、版本或来源确认。
- 宿主按工坊条目和已校验归档的摘要保存确定性加载失败。后台继续查询订阅，跳过该归档的暂存与安装并保留错误状态。宿主升级、归档内容变化或手动重试时重新评估。回滚成功后隔离本次失败的新版本，便于后续重新安装。

## 与前台呈现的关系

同一调度快照中的组件呈现会合并，参见 [ui_animation_scheduler.cpp](../ui/render/ui_animation_scheduler.cpp) 和 [ui_presentation_scope.h](../ui/render/ui_presentation_scope.h)。截止时间与动画回调先准备状态，再按 owner 呈现；没有额外定时唤醒。

主桌面、浮动 Dock、浮动弹窗的原生消息处理及指针批次进入同步呈现范围。定时回调中的嵌套消息循环也保留即时反馈；即时呈现会取代同 owner 先前排队的呈现，返回后继续后台合并。不要把原生消息入口改为后台延迟路径。

## 回归与观察

- [workshop_validation_cache_tests.h](../../tests/steam/workshop_validation_cache_tests.h) 使用真实私有归档验证内容替换、相同元数据、错误、删除、写入互斥、进程级复用及容量淘汰，纳入现有 `widget_author_tools`。
- 动画调度与呈现合并目前没有专门的长期行为测试；修改时需定向核对同步消息、排队呈现、owner 取代、取消和异常收尾，不由验包缓存测试代称通过。
- `SNOWDESKTOP_MENU_TRACE` 为可选诊断，`phase=workshop.validation` 下的 `validate.archive` 与 `cache.hit` 可核对首次验包及后续复用。不记录包路径或选择名。
