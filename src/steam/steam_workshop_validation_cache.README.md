# 创意工坊后台验包缓存

`SteamWorkshopSource::QuerySubscriptions` 的本地订阅发现会复用已成功验过、内容未变化的组件包。实现位于 [steam_workshop_validation_cache.h](steam_workshop_validation_cache.h)，每个宿主进程共享缓存；轮询重建 source 对象不会清掉结果。

## 复用条件

- 每次读取仍计算文件 SHA-256，不单凭大小或修改时间判断。相同大小、相同时间戳的内容替换会重新验包。
- 缓存键包含归档路径与校验 staging 根；只存成功的校验报告和未本地化 manifest，语言筛选和来源身份按当前查询重新处理。
- 读取租约使用 `FILE_SHARE_READ`，限制验包期间的普通并发写入和替换；首次完整校验后再核对摘要，内容不一致不存缓存。
- 文件不存在、不可读、读取摘要失败、超过归档配额或者校验失败时，继续完整校验并保留原诊断，不返回旧成功结果。
- 默认最多保存 256 项，按最近使用淘汰。缓存不写磁盘，重启后首次检查仍完整验包。
- 主动在线查询、详情/物化以及实际安装保持原来的完整校验路径；缓存不代替权限、版本或来源确认。

## 与前台呈现的关系

本次降耗也合并同一调度快照中的组件呈现，参见 [ui_animation_scheduler.cpp](../ui/render/ui_animation_scheduler.cpp) 和 [ui_presentation_scope.h](../ui/render/ui_presentation_scope.h)。截止时间与动画回调先准备状态，再按 owner 呈现；没有额外定时唤醒。

主桌面、浮动 Dock、浮动弹窗的原生消息处理及指针批次进入同步呈现范围。定时回调中的嵌套消息循环也保留即时反馈；即时呈现会取代同 owner 先前排队的呈现，返回后继续后台合并。不要把原生消息入口改为后台延迟路径。

## 回归与观察

- [workshop_validation_cache_tests.h](../../tests/steam/workshop_validation_cache_tests.h) 使用真实私有归档验证内容替换、相同元数据、错误、删除、写入互斥、进程级复用及容量淘汰，纳入现有 `widget_author_tools`。
- [ui_animation_scheduler_tests.cpp](../../tests/ui_animation_scheduler_tests.cpp) 使用私有 HWND_MESSAGE 验证 SendMessage、PostMessage/DispatchMessage、同步范围恢复、owner 取代、取消与异常；不操作桌面宿主。
- `SNOWDESKTOP_MENU_TRACE` 为可选诊断，`phase=workshop.validation` 下的 `validate.archive` 与 `cache.hit` 可核对首次验包及后续复用。不记录包路径或选择名。

## 2026-10-16 验证范围

隔离原生程序对同一批 9 个真实包副本（17,289,781 字节）切换完整校验/缓存复查，各排除一轮预热、测 6 轮。完整校验平均耗时 855.123 ms、CPU 789.063 ms；缓存复查平均耗时 10.607 ms、CPU 10.417 ms。全部 9 个包保持有效，后续 54 次复查均命中，未再解压。该路径 CPU 时间下降约 98.68%，不是整个宿主的降幅。

最终标准程序的 65 秒被动采样，剔除开始 3 秒后有效约 62 秒：外部平均 CPU 0.603%、内部平均 0.575%、中位数 0.321%、P95 2.238%、约 200 ms 窗口最高 3.860%。期间没有设置或构建进程，采样鼠标位置未变化，前台为 ShellHost。启动记录含首次 9 次完整验包、随后至少 27 次缓存命中。较早的多组件活动窗口记录每批平均 1.94 个组件，证明合并路径生效；最终静置窗口只有频谱和时钟更新，不用于推断多组件合并比例。

标准 Release 宿主和测试编译无警告；本项相关回归通过。共享全量为 142/147，另有五项既存托盘/Hook 集成失败，不写成全量通过。运行程序包含同批其他改动，前后场景也有变化，不能从 CPU 百分比推导本任务的整机因果降幅。桌面交互观感仍由用户实机确认；前次未抓到执行回调的 Windows 线程池峰值未据此标为解决。

候选为 `4a1cda8bd0e8ad81e2cc1ed999564896ce8e8d46` 与 `b234bf10412cf71cd4804dac25db04cb31011035`。本轮一次性证据位于忽略目录 `artifacts/v1.0.11.0/performance/cpu-optimization-20261015-232001/`；该目录不随源码交付。
