# 本地首图路径与 Shell 回退

2026-09-22 的实际启动中，四个首图线程同时在 `IPersistFile::Load` 等待约 35 秒，
详见 `startup_icon_validation_20260922_144611.md`。本轮将本地资源读取和 Shell
首图回退分开调度，候选仍需原始桌面启动场景验证。

## 执行与资源边界

- 四个本地首图线程直接读取 `.lnk` 字节、`.url` 显式图标配置，或本地 `.exe`/`.ico`。
  `.lnk` 首图不创建 Shell COM 对象，不解析 PIDL，不调用 `IPersistFile::Load`。
- `.lnk` 最多读取 1 MiB，检查头部、CLSID、各段长度和偏移，再取显式图标、环境变量
  图标块或本地可执行目标；目标 PIDL、网络目标、移动跟踪和未支持的表示留给 Shell。
- 资源仍通过现有 `SHDefExtractIconW` 和位图转换获得；这是局部资源提取，不是完整
  Shell 项解析。回退结果与本地结果沿用相同的身份、版本和取消检查。
- 本地路径限制为固定磁盘上的文件；读取前检查路径及其祖先的重解析点、离线和召回
  属性，拒绝 UNC、网络盘、设备路径和备用数据流。这是保守的性能筛选，不是对并发
  文件替换或慢本地过滤驱动的安全/超时保证。
- 本地未取得位图时，由主线程完成回调把该项转交两个独立的 Shell 首图回退线程。
  细节读取、快捷方式分类继续各用两个线程。回退饱和时，本地图标仍可独立完成。
- 每个队列沿用 `BackgroundWork` 的 2048 个在途键上限；没有每项新建线程，也没有
  超时后无限补线程。Shell 调用本身仍可能长时间不返回，同类回退项仍可能排队。
- 成功结果逐项交付；分类在首图结果被当前宿主接受后才开始。取消、关闭弹窗和退出
  同时清理本地/回退/细节/分类队列的交付状态，迟到结果不能覆盖新请求。

## 格式依据与回退

字节读取参照微软 MS-SHLLINK 的
[ShellLinkHeader](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-shllink/c3376b21-0931-45e4-b2fc-a48ac0e60d15)、
[StringData](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-shllink/17b69472-0f34-4bcf-b290-eccdb8de224b)、
[LinkInfo](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-shllink/6813269d-0cc8-4be2-933f-e96e8e3412dc)
及图标/目标环境变量块。路径字段只作为取图提示，不用于启动、修改或重新解析目标。
损坏、超限、无法提取图标或不属于支持范围的项目仍由原有 Shell 取图逻辑处理。
第二阶段可以更新最终表示，首图不会阻止其返回更准确的系统图标。

`Local icon:` 记录本地排队时间、读取时间、是否获得位图和 `trace`；`bitmap=0`
表示需要回退，不能当作“此项加载完成”。原有 `Shell call slow:` 保留，可分别核对
首图是否已经交付以及 Shell 等待是否仍存在。两者不是同一验收指标。

## 验证范围

复用 `shortcut_application_rules` 中的文件夹具，验证 Unicode/ANSI 图标、负索引、
LinkInfo、相对目标、环境变量图标，以及截断/越界/非本地输入的回退。
复用 `slot_runtime_contract` 的真实队列，模拟全部 Shell 首图回退线程阻塞，确认
后来的本地图标、细节和分类仍完成，并检查取消、相同键复用、迟到结果及销毁。
测试只替换外部提供程序，不替换被测队列调度。

独立探针直接调用本轮 `ReadLocalIconResources`，随后用 `SHDefExtractIconW` 提取图标，
未初始化 COM、未启动宿主。用户桌面原始 CC Switch、Docker Desktop、Excel、KOOK、
AfterFX 和 Adobe Media Encoder 六份快捷方式均取得图标，单次读取加提取约 4–33 ms。
这是当前文件和系统缓存状态下的部件证据，不含位图转换或宿主交付，也不是冷启动耗时。
隔离副本中恢复 Shell 读取后，无 COM 场景的文件用例失败；让回退复用首图队列后，
后续本地图标不能在慢项放行前完成。恢复本轮实现后，两组相同输入均通过。

本轮不改变完整桌面 `IEnumIDList::Next` 的等待行为；首图隔离是否改善用户看到的
长时间占位，须结合本次产物、原始桌面及下一次启动日志验收。构建和自动测试不能
替代该场景，也不能直接给出启动提速比例。
