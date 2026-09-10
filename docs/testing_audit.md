# 现有测试逐项评估

开始日期：2026-09-10。规则基线：`5dc6bc26`。状态：**评估进行中，尚未完成全部条目**。

本轮评估测试价值、生产入口、替身、断言证据与运行成本；测试/生产代码均未修改。发现的生产问题另见[问题清单](testing_audit_product_issues.md)。后续优化决定不等于已经实施。

## 清单与评估方法

重新读取配置后的 `ctest --test-dir .build -C Release --show-only=json-v1`，并与 `CMakeLists.txt` 中的 `snowdesktop_add_test`、脚本和附加参数登记对照：117 个条目，113 个 C++ 测试可执行文件，2 个脚本条目，另有 2 个条目通过不同参数复用图标测试程序。不是 117 个独立业务场景。

每个条目先核对源文件及入口，再阅读用例断言、夹具/替身和相关生产边界；对发现的薄弱断言做定向对照。静态评估不等于用例执行通过，未做的变异、环境或实机验证要保留边界。仅有自动扫描信号的条目不计为已评估。

表中的历史耗时来自本轮拖放调整的全量记录（测试执行共 58.28 秒，117/117）；只用于选择调查顺序，不是本次执行，也不包括配置和编译，不作为当前源码的有效通过证据。

## 已确认的测试问题

### TA-01：源码存在性被当作执行证据

`shell_integration_contract` 中的线程、调用和源码顺序搜索不能证明对应行为。此前隔离实验已验证：禁用主题调用但保留同样注释文本仍通过；删除文本才失败。当前断言仍存在。应保留有明确价值的负向架构检查，将正向执行、生命周期及迁移结论转为生产行为用例；不能只增加搜索词。

### TA-02：图片“可解码”检查只读了头部字段

`drop_image_data_tests.cpp` 的 `CheckOnePixelPng` 仅检查至少 24 字节、PNG 签名及偏移 16/20 的宽高。2026-09-10 构造一个只有这 24 字节的文件：复刻该字面谓词得到 true，Pillow 独立解码报 `UnidentifiedImageError`。这是谓词反例与独立解码实验，**没有运行原 C++ 测试程序或修改生产编码器**。

因此应实际调用独立解码器并检查像素、尺寸与透明度；现有格式优先级、边界限制和禁止覆盖用例仍保留。此项是测试断言不足，不是生产图片必然损坏的证据。

### TA-03：模块测试不能关闭宿主拖放缺陷

`slot_runtime_contract` 的真实 Shell 路径读取和下载选择回归有价值，但未编译/调用 `app_external_slot_drop.cpp` 的宿主提交链；`lua_logical_slot_container` 的宿主提交者是回调替身；`shell_file_operation_worker` 的文件执行不包含组件模型更新。DND-01/02/03/04 需要补充这些连接处的生产测试，不能把多个局部绿灯拼接为完整验收。

### TA-04：部分文件操作只检查存在，不检查内容

`shell_file_operation_worker` 的复制、移动及多源交接在最终阶段主要检查 `exists`，无法发现同名但内容错误、截断或写入了错误来源。应补独立内容断言，并贯通部分成功后的逐项结果；保留真实系统工作器及异步顺序测试。

### TA-05：规则组合与重复次数需要分别审查

`slot_contract_matrix` 的路由预期来自独立表，有防回归价值；坐标往返关系仍需已知坐标样本，避免两个同向错误相互抵消。`lua_logical_slot_container` 的 10000 次相同命中主要反复断言相同身份/版本，没有测量内存等累积风险；拟减少日常重复次数，保留布局失效和状态转换，必要压力检查另设明确目的。尚未修改或测量节省量。

### TA-06：被测分类函数同时决定输入与预期

`slot_runtime_contract_tests.cpp:1002–1027` 使用生产函数 `CanRestoreRecordedWidgetMembers` 的结果决定是否提供重建后的可见项目，又使用该结果决定预期解析次数。如果函数错误地返回 false，测试可能同时补入可见项目并预期零次恢复，从而共同接受错误。此结论来自夹具与断言的数据流审查，尚未实际运行变异。应通过独立场景表固定输入状态和恢复预期，保留实际控制器及身份恢复调用。

### TA-07：失败退出可能绕过夹具清理

`drop_image_data` 和 `shell_file_operation_worker` 的部分夹具依赖正常路径末尾清理，失败断言调用 `std::exit`。失败时可能留下临时文件或系统资源，影响后续诊断。应优先让失败沿正常栈展开，并使用限定作用域的资源管理；不能只增加一次运行前清理。`virtual_file_drop` 使用失败计数和临时目录作用域管理，不属于上述直接退出问题。

### TA-08：异步轮询丢掉了先完成的结果

`widget_audio_output_task_executor_tests.cpp:86–89` 每次把 `DrainCompletions()` 赋给同一个向量，要求单次恰好返回 3 条；`widget_clipboard_task_executor_tests.cpp:130–133` 同样要求单次返回 4 条。Drain 会取走队列内容，若合法地分两批完成，前一批已被测试丢弃，后续无法再满足总数条件。应累积结果并按任务 ID 核对，或用受控屏障明确完成顺序。此为代码审查确认的测试竞态，尚未运行分批完成探针；不应通过重复整个测试或增加睡眠掩盖。

### TA-09：声明的策略与执行该策略的证据需要分开

`widget_task_broker` 的具体任务手势要求由测试自己构造描述符；它能验证 broker 遵守输入策略，不能单独证明宿主注册了正确策略。`widget_api_registry` 的能力目录、版本过滤、原子注册和真实 Lua 调用有独立价值；文档名称搜索只能证明名称出现，不能证明签名完整。应保留稳定契约，分别记录目录配置、注册连接和最终操作的覆盖，避免“参数已校验”等超出断言的描述。

### TA-10：条目粒度与环境标签需要校准

`widget_runtime_diagnostics` 默认入口同时运行快速日志规则、真实文件报告和自动计时停止；`widget_audio_analysis_provider` 同时包含纯信号计算和真实音频设备生命周期。应支持按子场景选择，并给真实环境部分明确标签。`widget_media_task_executor` 已标 integration，但当前媒体动作本身为注入替身；相反，一些真实文件存储/时区用例没有 integration 标签。不能单凭测试名称、是否使用线程或目前标签判定覆盖层级。拆分方案优先复用现有可执行文件和参数，先测量配置/编译/执行分别耗时，再决定收益。

## 逐项记录

“待评估”表示尚未给出代码审查结论；“评估中”表示已发现部分问题但条目内其他用例尚待阅读；“已评估”是静态价值与边界评估完成，不表示已做完整变异或实机验收。建议运行范围只用于本条目及其依赖发生变化时的定向选择，不能替代完整影响分析。

<!-- audit-table-start -->

当前：已评估 38 项，评估中 0 项，待评估 79 项。

| # / CTest | 测试源码入口 | 状态 / 建议 | 证据与边界 | 建议触发范围 | 历史秒 |
| --- | --- | --- | --- | --- | ---: |
| 1. `application_data_lifecycle` | [application_data_lifecycle_tests.cpp](../tests/application_data_lifecycle_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 2.94 |
| 2. `single_instance` | [single_instance_tests.cpp](../tests/single_instance_tests.cpp) | 已评估 / 增强 | 真实部署目录及 Steam 运行时替换判定；独立侧车/路径输入，失败统计及清理有效。名称不代表真实多进程互斥或移交；含文件和系统目录依赖，需明确边界 | 部署/实例规则定向；真实多进程另验 | 0.08 |
| 3. `http_security` | [http_security_tests.cpp](../tests/http_security_tests.cpp) | 已评估 / 保留 | 真实 URL/IP/来源与候选重试策略，固定正反样本区分公共 HTTPS 和允许本地拖入；不执行 DNS、重定向或请求，不能证明传输全过程安全 | HTTP 公共策略，相关调用方需联动 | 0.02 |
| 4. `lua_runtime` | [lua_runtime_tests.cpp](../tests/lua_runtime_tests.cpp) | 已评估 / 增强 | 真实 ProtectedCall 覆盖嵌套限额、跨 VM 重入、错误清理与堆栈恢复；加载事务用例手工固定 loading.state，未调用宿主加载器，需明确保护范围 | Lua 执行与重入定向 | 0.01 |
| 5. `widget_api_registry` | [widget_api_registry_tests.cpp](../tests/widget_api_registry_tests.cpp) | 已评估 / 增强 | 真实版本过滤、原子注册、Lua define/state/capability 与 JSON 字段有独立断言；文档名称搜索须限缩结论。巨型布尔合取宜改具名表便于定位，不删除 API 兼容契约（TA-09） | 公共 API/权限目录联动 | 0.03 |
| 6. `widget_l10n_format` | [widget_l10n_format_tests.cpp](../tests/widget_l10n_format_tests.cpp) | 已评估 / 保留 | 真实格式化，固定多语言完整文本对照；非法值/单位及空输入有失败信号，属于可见文字语义而非排版测试 | 本地化格式化定向 | 0.02 |
| 7. `widget_time` | [widget_time_tests.cpp](../tests/widget_time_tests.cpp) | 已评估 / 增强 | 真实时间与历法函数，固定闰日/月末及 DST 预期有价值；格式化只判含年份且语言不同，宜加强；Windows 时区数据库依赖需声明 | 时间/本地化定向；时区环境明确 | 0.08 |
| 8. `widget_runtime_diagnostics` | [widget_runtime_diagnostics_tests.cpp](../tests/widget_runtime_diagnostics_tests.cpp) | 已评估 / 增强 | 日志淘汰与真实性能捕获/JSON、因果关系、并发聚合均有独立断言；同入口含真实自动计时及落盘，宜分开选择；开销基准为可选参数，未计入默认测试 | 日志定向；性能捕获/计时 integration | 1.58 |
| 9. `widget_preview_context` | [widget_preview_context_tests.cpp](../tests/widget_preview_context_tests.cpp) | 已评估 / 保留 | 真实嵌套作用域、存储覆盖恢复、固定时钟及新线程隔离；不证明具体外部副作用入口遵守预览状态 | 预览上下文定向 | 0.02 |
| 10. `widget_runtime_health` | [widget_runtime_health_tests.cpp](../tests/widget_runtime_health_tests.cpp) | 已评估 / 保留 | 真实熔断与恢复状态机，注入时间覆盖阈值前后、重复失败不推迟恢复及成功复位；无需真实等待 | 运行时健康规则定向 | 0.01 |
| 11. `widget_host_state` | [widget_host_state_tests.cpp](../tests/widget_host_state_tests.cpp) | 已评估 / 保留 | 真实占位动作、故障优先级、授权窗口过期及固定多屏坐标预期；不宣称实窗创建、激活或渲染通过 | 宿主状态规则定向 | 0.01 |
| 12. `widget_data_broker` | [widget_data_broker_tests.cpp](../tests/widget_data_broker_tests.cpp) | 已评估 / 保留 | 真实状态机，固定时间与订阅者验证共享频率、隐藏/撤权即停、预览隔离及各自投递速率；输出为动作计划，不证明设备执行 | 数据订阅/权限/生命周期定向 | 0.01 |
| 13. `widget_task_broker` | [widget_task_broker_tests.cpp](../tests/widget_task_broker_tests.cpp) | 已评估 / 增强 | 真实 broker 的代际归属、并发、撤权覆盖迟到结果与手势作用域有价值；具体任务策略由夹具注册，不能证明生产目录配置或参数已校验 | 任务/权限生命周期定向；目录联动 | 0.01 |
| 14. `widget_notification_runtime` | [widget_notification_runtime_tests.cpp](../tests/widget_notification_runtime_tests.cpp) | 已评估 / 保留 | 真实通知状态机与固定时钟，核对代际归属、按时一次投递、清理及动作白名单；宿主展示为可记录回调，不证明系统通知出现 | 通知生命周期定向 | 0.01 |
| 15. `widget_notification_schedule_store` | [widget_notification_schedule_store_tests.cpp](../tests/widget_notification_schedule_store_tests.cpp) | 已评估 / 增强 | 真实序列化/加载与归属、时间边界、重复 ID/非法 schema 拒绝；正例多为自身往返，宜补独立合法旧版文本及全部文本字段预期，不代表文件持久化 | 通知存储格式定向 | 0.01 |
| 16. `widget_media_task_executor` | [widget_media_task_executor_tests.cpp](../tests/widget_media_task_executor_tests.cpp) | 已评估 / 保留 | 真实队列、参数拒绝、代际 ID 和屏障控制的取消迟到覆盖；媒体系统动作是注入替身。当前 integration 标签宜复核，不把通过称为媒体控制实测 | 媒体工作器定向 | 0.05 |
| 17. `widget_audio_output_task_executor` | [widget_audio_output_task_executor_tests.cpp](../tests/widget_audio_output_task_executor_tests.cpp) | 已评估 / 改写 | 真实参数/限频/取消逻辑，设备执行为替身；等待 3 个结果的轮询会丢弃较早批次（TA-08）。保留固定时钟与取消屏障，改累计结果 | 音量工作器定向；设备边界另验 | 0.05 |
| 18. `widget_clipboard_task_executor` | [widget_clipboard_task_executor_tests.cpp](../tests/widget_clipboard_task_executor_tests.cpp) | 已评估 / 改写 | 真实工作器限频、格式/输出上限和取消抑制；剪贴板读写为替身，4 个结果分批到达会误报（TA-08）；图像/路径保留断言可加强值核对 | 剪贴板工作器定向；系统边界另验 | 0.06 |
| 19. `widget_filesystem_handle_store` | [widget_filesystem_handle_store_tests.cpp](../tests/widget_filesystem_handle_store_tests.cpp) | 已评估 / 增强 | 真实授权落盘重载、独立撤销和实例隔离；包身份字段未单独改变，缺畸形存储及写失败用例；异常路径未管理临时目录，需加强清理和环境标签 | 文件授权存储 integration/security | 0.16 |
| 20. `widget_filesystem_task_executor` | [widget_filesystem_task_executor_tests.cpp](../tests/widget_filesystem_task_executor_tests.cpp) | 已评估 / 增强 | 真实文件任务核对文本/二进制字节、修订冲突及上限；冲突后未独立读盘确认旧内容，分页仅核首批数量。路径直接注入，不证明句柄授权桥 | 文件执行 integration；句柄桥联动 | 0.18 |
| 21. `widget_filesystem_watch_service` | [widget_filesystem_watch_service_tests.cpp](../tests/widget_filesystem_watch_service_tests.cpp) | 已评估 / 保留 | 真实隔离目录订阅、创建/重命名事件及实例清理，等待具体事件而非固定睡眠；范围是直接目录，未证明宿主订阅/重解析点授权 | 文件监听 integration | 0.09 |
| 22. `widget_app_task_executor` | [widget_app_task_executor_tests.cpp](../tests/widget_app_task_executor_tests.cpp) | 已评估 / 增强 | 真实搜索分页/拼音字段匹配、引用隔离、迟到取消及外部异常转换；目录/外部引擎为夹具。外部取消靠 20 ms 睡眠，宜用屏障，短轮询预算需诊断化 | 应用/外部搜索定向 | 0.12 |
| 23. `widget_system_settings` | [widget_system_settings_tests.cpp](../tests/widget_system_settings_tests.cpp) | 已评估 / 保留 | 固定目标到 Windows URI 的真实白名单映射，拒绝原始 URI/未知目标；保护公开参数契约，不启动设置窗口 | 系统设置路由定向 | 0.01 |
| 24. `widget_system_data_provider` | [widget_system_data_provider_tests.cpp](../tests/widget_system_data_provider_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 1.04 |
| 25. `widget_audio_analysis_provider` | [widget_audio_analysis_provider_tests.cpp](../tests/widget_audio_analysis_provider_tests.cpp) | 已评估 / 增强 | 正弦输入与独立峰值区间验证真实频谱，投影端点/特性过滤明确；另含真实设备启动/停止，只保证数据或错误状态。宜分开规则与设备场景（TA-10） | 音频规则定向；设备 integration | 0.11 |
| 26. `widget_composition_layer_rules` | [widget_composition_layer_rules_tests.cpp](../tests/widget_composition_layer_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 27. `widget_package_image_cache` | [widget_package_image_cache_tests.cpp](../tests/widget_package_image_cache_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.20 |
| 28. `widget_lua_lifecycle` | [widget_lua_lifecycle_tests.cpp](../tests/widget_lua_lifecycle_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 29. `widget_runtime_scheduler` | [widget_runtime_scheduler_tests.cpp](../tests/widget_runtime_scheduler_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 30. `widget_permission_state` | [widget_permission_state_tests.cpp](../tests/widget_permission_state_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 31. `application_restart_policy` | [application_restart_policy_tests.cpp](../tests/application_restart_policy_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.11 |
| 32. `widget_layout_context` | [widget_layout_context_tests.cpp](../tests/widget_layout_context_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 33. `calendar_service` | [calendar_service_tests.cpp](../tests/calendar_service_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.07 |
| 34. `localization_contract` | [localization_contract_tests.cpp](../tests/localization_contract_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.72 |
| 35. `general_settings` | [general_settings_tests.cpp](../tests/general_settings_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 36. `demo_mode_rules` | [demo_mode_rules_tests.cpp](../tests/demo_mode_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 37. `settings_window_open_rules` | [settings_window_open_rules_tests.cpp](../tests/settings_window_open_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 38. `deployment_packaging_contract` | [deployment_packaging_contract_tests.cpp](../tests/deployment_packaging_contract_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.61 |
| 39. `steam_entitlement` | [steam_entitlement_tests.cpp](../tests/steam_entitlement_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 1.42 |
| 40. `launcher_icon_resource` | [launcher_icon_resource_tests.cpp](../tests/launcher_icon_resource_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 41. `steam_runtime_update` | [steam_runtime_update_tests.cpp](../tests/steam_runtime_update_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 4.41 |
| 42. `steam_local_deploy_contract` | [steam_local_deploy_contract_tests.ps1](../tests/steam_local_deploy_contract_tests.ps1) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 1.00 |
| 43. `deployment_manifest_integration` | [deployment_manifest_integration.ps1](../tests/deployment_manifest_integration.ps1) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 1.15 |
| 44. `settings_controller` | [settings_controller_tests.cpp](../tests/settings_controller_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 1.17 |
| 45. `settings_host_actions_semantics` | [settings_host_actions_semantics_tests.cpp](../tests/settings_host_actions_semantics_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 46. `settings_search_index` | [settings_search_index_tests.cpp](../tests/settings_search_index_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 47. `winui_settings_navigation` | [winui_settings_navigation_tests.cpp](../tests/winui_settings_navigation_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 48. `hotkey_recorder_rules` | [hotkey_recorder_rules_tests.cpp](../tests/hotkey_recorder_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 49. `winui_personalization_page_presenter` | [winui_personalization_page_presenter_tests.cpp](../tests/winui_personalization_page_presenter_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 50. `winui_desktop_page_presenter` | [winui_desktop_page_presenter_tests.cpp](../tests/winui_desktop_page_presenter_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 51. `winui_dock_page_presenter` | [winui_dock_page_presenter_tests.cpp](../tests/winui_dock_page_presenter_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 52. `winui_home_about_page_presenter` | [winui_home_about_page_presenter_tests.cpp](../tests/winui_home_about_page_presenter_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 53. `settings_update_rules` | [settings_update_rules_tests.cpp](../tests/settings_update_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 54. `auto_start_rules` | [auto_start_rules_tests.cpp](../tests/auto_start_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 55. `winui_widget_settings_presenter` | [winui_widget_settings_presenter_tests.cpp](../tests/winui_widget_settings_presenter_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 56. `winui_widgets_page_presenter` | [winui_widgets_page_presenter_tests.cpp](../tests/winui_widgets_page_presenter_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 57. `winui_backup_data_page_presenter` | [winui_backup_data_page_presenter_tests.cpp](../tests/winui_backup_data_page_presenter_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 58. `winui_backup_data_page_backend` | [backup_data_page_backend_tests.cpp](../tests/backup_data_page_backend_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.04 |
| 59. `winui_widgets_page_backend` | [widgets_page_backend_tests.cpp](../tests/widgets_page_backend_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 60. `winui_settings_window_host` | [winui_settings_window_host_tests.cpp](../tests/winui_settings_window_host_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 61. `settings_no_imgui_contract` | [settings_no_imgui_contract_tests.cpp](../tests/settings_no_imgui_contract_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 62. `slot_contract_matrix` | [slot_contract_matrix_tests.cpp](../tests/slot_contract_matrix_tests.cpp) | 已评估 / 增强 | 真实路由对照独立预期表；几何往返缺少固定坐标判定；4500 组合不代表真实提交（TA-05） | 规则定向；宿主连接另验 | 0.01 |
| 63. `slot_runtime_contract` | [slot_runtime_contract_tests.cpp](../tests/slot_runtime_contract_tests.cpp) | 已评估 / 增强 | 真实控制器、失效、重命名模型、队列合并有价值；外部目标为替身，恢复夹具耦合预期（TA-03/06）。托盘/注册表/快捷方式与拖放混在同一入口，宜细分选择 | 按风险细分；真实 OLE/文件用例保留 integration | 0.32 |
| 64. `widget_interaction_rules` | [widget_interaction_rules_tests.cpp](../tests/widget_interaction_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 65. `widget_interaction_region` | [widget_interaction_region_tests.cpp](../tests/widget_interaction_region_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 66. `widget_draw_geometry` | [widget_draw_geometry_tests.cpp](../tests/widget_draw_geometry_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 67. `widget_view_accessibility` | [widget_view_accessibility_tests.cpp](../tests/widget_view_accessibility_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 68. `widget_accessibility_provider` | [widget_accessibility_provider_tests.cpp](../tests/widget_accessibility_provider_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.18 |
| 69. `widget_view_contract` | [widget_view_contract_tests.cpp](../tests/widget_view_contract_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.05 |
| 70. `widget_author_lint` | [widget_author_lint_tests.cpp](../tests/widget_author_lint_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 71. `widget_author_test_runner` | [widget_author_test_tests.cpp](../tests/widget_author_test_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 72. `widget_author_tools` | [widget_author_tools_tests.cpp](../tests/widget_author_tools_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 73. `widget_author_preview_cli` | [widget_author_preview_cli_tests.cpp](../tests/widget_author_preview_cli_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 19.28 |
| 74. `widget_view_tree` | [widget_view_tree_tests.cpp](../tests/widget_view_tree_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.04 |
| 75. `widget_text_input_rules` | [widget_text_input_rules_tests.cpp](../tests/widget_text_input_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 76. `widget_storage_transaction` | [widget_storage_transaction_tests.cpp](../tests/widget_storage_transaction_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 77. `widget_storage_write_budget` | [widget_storage_write_budget_tests.cpp](../tests/widget_storage_write_budget_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 78. `widget_setting_rules` | [widget_setting_rules_tests.cpp](../tests/widget_setting_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 79. `widget_settings_model` | [widget_settings_model_tests.cpp](../tests/widget_settings_model_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 80. `widget_settings_service` | [widget_settings_service_tests.cpp](../tests/widget_settings_service_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 81. `widget_engine_settings_backend` | [widget_engine_settings_backend_tests.cpp](../tests/widget_engine_settings_backend_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 82. `widget_secret_store` | [widget_secret_store_tests.cpp](../tests/widget_secret_store_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 83. `builtin_widget_source_contract` | [builtin_widget_source_contract_tests.cpp](../tests/builtin_widget_source_contract_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.04 |
| 84. `widget_logical_slot` | [widget_logical_slot_tests.cpp](../tests/widget_logical_slot_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 85. `lua_logical_slot_container` | [lua_logical_slot_container_tests.cpp](../tests/lua_logical_slot_container_tests.cpp) | 已评估 / 增强 | 真实 Lua 槽位接收/命中/提交回调；预期表明确，宿主回调为替身。保留失效/键盘覆盖，减少无新增断言的 10000 次重复（TA-03/05） | 组件及交互定向 | 0.01 |
| 86. `folder_self_drop_rules` | [folder_self_drop_rules_tests.cpp](../tests/folder_self_drop_rules_tests.cpp) | 已评估 / 保留 | 真实自包含规则，独立路径样本覆盖分隔符、大小写和相邻前缀；不证明 Shell 别名或整个文件操作 | 路径规则定向 | 0.01 |
| 87. `folder_mapping_rules` | [folder_mapping_rules_tests.cpp](../tests/folder_mapping_rules_tests.cpp) | 已评估 / 保留 | 真实 ChildPath 对照固定中文/根目录/UNC 结果，保护重复分隔符回归；不覆盖实际目录读写 | 映射路径定向 | 0.01 |
| 88. `desktop_namespace_registry` | [desktop_namespace_registry_tests.cpp](../tests/desktop_namespace_registry_tests.cpp) | 已评估 / 增强 | 真实别名解析和 This PC PIDL 匹配，独立 CLSID 预期；含真实 Shell 环境依赖，建议复核 integration 标签 | 命名空间定向及系统边界 | 0.19 |
| 89. `right_click_contract` | [right_click_contract_tests.cpp](../tests/right_click_contract_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 90. `quick_navigation_rules` | [quick_navigation_rules_tests.cpp](../tests/quick_navigation_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 91. `quick_navigation_search_async` | [quick_navigation_search_async_tests.cpp](../tests/quick_navigation_search_async_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 92. `shortcut_application_rules` | [shortcut_application_rules_tests.cpp](../tests/shortcut_application_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.29 |
| 93. `desktop_keyboard_rules` | [desktop_keyboard_rules_tests.cpp](../tests/desktop_keyboard_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 94. `icon_render_rules` | [icon_render_rules_tests.cpp](../tests/icon_render_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 1.04 |
| 95. `large_icon_shell_assets` | [large_icon_assets_tests.cpp](../tests/large_icon_assets_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.45 |
| 96. `large_icon_rendering` | [large_icon_rendering_tests.cpp](../tests/large_icon_rendering_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.26 |
| 97. `icon_beautify` | [icon_beautify_tests.cpp](../tests/icon_beautify_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.08 |
| 98. `item_location` | [item_location_tests.cpp](../tests/item_location_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.29 |
| 99. `virtual_file_drop` | [virtual_file_drop_tests.cpp](../tests/virtual_file_drop_tests.cpp) | 已评估 / 保留 | 真实描述符解析和落盘；IDataObject/流为受控输入。有精确内容、重名保留、索引、超限清理及悬停不读取断言；不证明 COM 跨线程和宿主整批提交 | 数据物化定向；批次宿主另验 | 0.03 |
| 100. `url_drop_resource` | [url_drop_resource_tests.cpp](../tests/url_drop_resource_tests.cpp) | 已评估 / 保留 | 真实 MIME/文件名/回退决策与独立样本，含原花瓣 URL 和危险后缀；不执行下载或图片解码 | URL 规则定向 | 0.01 |
| 101. `drop_data_url` | [drop_data_url_tests.cpp](../tests/drop_data_url_tests.cpp) | 已评估 / 保留 | 真实解码器核对固定字节、编码错误和上限；PNG 片段仅作为解码字节样本，未宣称图片完整 | 数据 URL 定向 | 0.03 |
| 102. `drop_text_rules` | [drop_text_rules_tests.cpp](../tests/drop_text_rules_tests.cpp) | 已评估 / 保留 | 真实文本分类和资源候选选择；固定期望覆盖私有 URI、控制字符、本地路径限制及 URL 标签 | 文本和来源分类定向 | 0.03 |
| 103. `drop_image_data` | [drop_image_data_tests.cpp](../tests/drop_image_data_tests.cpp) | 已评估 / 增强 | 实际 SaveAsPng；替身覆盖 GDI/HGLOBAL/ISTREAM、格式优先级、畸形输入和输出上限；可解码断言有反例（TA-02），需解码像素；失败清理见 TA-07 | 图片边界定向，需独立解码 | 0.25 |
| 104. `shell_context_menu_invoke` | [shell_context_menu_invoke_tests.cpp](../tests/shell_context_menu_invoke_tests.cpp) | 已评估 / 增强 | 真实工作目录/调用参数构造，保护存储生命周期；依赖实际桌面目录且 ANSI 只判非空，建议补内容断言及环境标签 | Shell 参数定向；环境部分 integration | 0.04 |
| 105. `shell_integration_contract` | [shell_integration_contract_tests.cpp](../tests/shell_integration_contract_tests.cpp) | 已评估 / 改写 | 仅源码文本搜索，正向执行结论可被注释满足（TA-01），还依赖多行排版；保留禁止劫持窗口/绕过注册表桥等负向约束，执行/迁移/恢复改行为证据 | 静态架构与真实集成分别选择 | 0.04 |
| 106. `shell_launch_worker` | [shell_launch_worker_tests.cpp](../tests/shell_launch_worker_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 12.22 |
| 107. `shell_file_operation_worker` | [shell_file_operation_worker_tests.cpp](../tests/shell_file_operation_worker_tests.cpp) | 已评估 / 增强 | 真实 STA 工作器、重命名通知/冲突内容、独立读取队列与异常恢复应保留；复制/移动补内容及逐项结果（TA-04，DND-04）。鼠标钩子安装为注入替身，实际停止唤醒逻辑有价值 | 真实文件系统 integration；钩子定向拆分 | 1.08 |
| 108. `dock_and_window_rules` | [dock_and_window_rules_tests.cpp](../tests/dock_and_window_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.53 |
| 109. `desktop_drop_cache` | [desktop_drop_cache_tests.cpp](../tests/desktop_drop_cache_tests.cpp) | 已评估 / 保留 | 真实搜索缓存/搜索函数及固定格点，覆盖失效、占用、大图标与回退；与待落点 pendingLandingCache 不同，不能覆盖 DND-03 | 搜索/布局定向；异步落点另验 | 0.03 |
| 110. `ui_animation_scheduler` | [ui_animation_scheduler_tests.cpp](../tests/ui_animation_scheduler_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.44 |
| 111. `menu_icon_render` | [menu_icon_render_tests.cpp](../tests/menu_icon_render_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.18 |
| 112. `modern_menu_interaction` | [modern_menu_interaction_tests.cpp](../tests/modern_menu_interaction_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.50 |
| 113. `component_preview` | [component_preview_tests.cpp](../tests/component_preview_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.88 |
| 114. `widget_preview_stage` | [widget_preview_stage_tests.cpp](../tests/widget_preview_stage_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |
| 115. `wallpaper_engine_capture` | [wallpaper_engine_capture_tests.cpp](../tests/wallpaper_engine_capture_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.11 |
| 116. `steam_workshop_manager` | [steam_workshop_manager_tests.cpp](../tests/steam_workshop_manager_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.76 |
| 117. `popup_animation_rules` | [popup_animation_rules_tests.cpp](../tests/popup_animation_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.02 |

<!-- audit-table-end -->

## 当前验证记录

- 规则与说明文档的本地链接、围栏、旧引用和差异检查通过。
- 已重新查询 CTest 清单并对照 CMake 登记；没有运行全量测试或宿主构建。
- TA-02 完成 24 字节反例的字面谓词检查与 Pillow 解码；TA-01 引用此前已保存的隔离实验，并复核当前断言。
- 尚未证明自动选择可靠，也未建立可复用的完整风险映射；当前评估表不能直接启用自动跳过测试。
