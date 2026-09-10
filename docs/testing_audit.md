# 现有测试逐项评估

开始日期：2026-09-10。规则基线：`5dc6bc26`。状态：**评估进行中，尚未完成全部条目**。

本轮先评估测试价值、生产入口、替身、断言证据与运行成本，完成全部条目后实施测试修正与优化；当前仍在评估阶段，测试/生产代码均未修改。发现的生产问题另见[问题清单](testing_audit_product_issues.md)，不混入测试修改。表中的优化建议不等于已经实施，实施时另记变更与验证结果。

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

`widget_runtime_diagnostics` 默认入口同时运行快速日志规则、真实文件报告和自动计时停止；`widget_audio_analysis_provider` 同时包含纯信号计算和真实音频设备生命周期。应支持按子场景选择，并给真实环境部分明确标签。`widget_media_task_executor` 已标 integration，但当前媒体动作本身为注入替身；相反，部分设备/时区用例没有 integration 标签。不能单凭测试名称、是否使用线程或目前标签判定覆盖层级。隔离临时文件的短小、确定性模块测试可以留在快速通道，不把所有文件 I/O 一律升级为环境测试。拆分方案优先复用现有可执行文件和参数，先测量配置/编译/执行分别耗时，再决定收益。

### TA-11：多个设置测试重复检查同一段控件源码

`winui_personalization_page_presenter`、`winui_desktop_page_presenter` 等条目只编译标准库文本扫描器；反复搜索同一共享控件的事件名称、33 ms 常量、对齐调用和数值量化表达式，并宣称提交、回滚和布局正确。应把共享控件的行为集中到真实用例，页面保留必要的绑定验证；不再通过多个可执行文件重复搜索相同文本。仍有价值的禁止直接 Shell/网络调用等架构约束可合并保留，但不能代替交互验收。

### TA-12：光标索引增减不能证明按视觉行移动

`widget_text_input_rules_tests.cpp:228–241` 使用真实 DirectWrite 自动换行，但 Down 只断言索引变大、Up 只断言索引变小。错误地前后移动一个字符也可能满足这些断言。应通过布局 hit-test 取得前后视觉位置，核对相邻行与横向位置保持，并保留首末行边界。此为静态断言审查，尚未运行故障注入。

### TA-13：作者测试工具的临时目录可能互相干扰

`widget_author_test_tests.cpp:17–20` 使用未播种的 `std::rand()` 构造临时目录，无进程标识或独占创建；同时启动两次测试可能选择相同路径。另有 RAII 临时目录被 `Check` 中的 `std::exit` 绕过，作者 lint 测试也存在同类失败清理问题。应采用独占创建的隔离目录，并让失败通过正常返回或异常展开完成清理。当前为代码审查，未并发运行该测试。

### TA-14：撤销分支用例没有构造待清除的 redo 历史

`widget_logical_slot_tests.cpp:258–290` 只记录一次变更，Undo 后立即 Redo，再记录新变更并断言不能 Redo。原 redo 在重做时已经消耗，该断言不能证明新分支会丢弃 redo。应在 Undo 后直接提交新变更，先证明 redo 存在再证明被清除；“有界历史”还需实际达到上限并验证淘汰边界。

## 逐项记录

“待评估”表示尚未给出代码审查结论；“评估中”表示已发现部分问题但条目内其他用例尚待阅读；“已评估”是静态价值与边界评估完成，不表示已做完整变异或实机验收。建议运行范围只用于本条目及其依赖发生变化时的定向选择，不能替代完整影响分析。

<!-- audit-table-start -->

当前：已评估 87 项，评估中 0 项，待评估 30 项。

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
| 24. `widget_system_data_provider` | [widget_system_data_provider_tests.cpp](../tests/widget_system_data_provider_tests.cpp) | 已评估 / 增强 | 真实共享工作器/系统采样、释放与确定性防抖/封面转换均有断言；多数设备用例接受不可用错误，只证明状态发布。需分开纯规则与设备、分别报告成功分支覆盖（TA-10） | 数据语义定向；系统采样 integration | 1.04 |
| 25. `widget_audio_analysis_provider` | [widget_audio_analysis_provider_tests.cpp](../tests/widget_audio_analysis_provider_tests.cpp) | 已评估 / 增强 | 正弦输入与独立峰值区间验证真实频谱，投影端点/特性过滤明确；另含真实设备启动/停止，只保证数据或错误状态。宜分开规则与设备场景（TA-10） | 音频规则定向；设备 integration | 0.11 |
| 26. `widget_composition_layer_rules` | [widget_composition_layer_rules_tests.cpp](../tests/widget_composition_layer_rules_tests.cpp) | 已评估 / 改写 | 真实隐藏缓存预算/到期、层级和反馈规则有固定预期；后半源码搜索冒充路由、绘制恢复和顺序执行证据，部分序号比较漏 npos 守卫（TA-01），应改连接处行为验证 | 合成规则定向；宿主图层连接另验 | 0.02 |
| 27. `widget_package_image_cache` | [widget_package_image_cache_tests.cpp](../tests/widget_package_image_cache_tests.cpp) | 已评估 / 增强 | 真实 WIC 缓存、引用计数、负缓存及额度恢复；输入有已知颜色但断言仅尺寸/指针，宜核对实际像素及换图内容。临时目录/COM 的失败清理需加强 | 图片缓存定向；WIC/文件环境明确 | 0.20 |
| 28. `widget_lua_lifecycle` | [widget_lua_lifecycle_tests.cpp](../tests/widget_lua_lifecycle_tests.cpp) | 已评估 / 保留 | 真实生命周期调用受控 Lua 回调，核对上下文、模型、事件/菜单和仅一次 setup/dispose，错误可见；不代表宿主卸载过程中所有资源释放 | Lua 生命周期定向 | 0.01 |
| 29. `widget_runtime_scheduler` | [widget_runtime_scheduler_tests.cpp](../tests/widget_runtime_scheduler_tests.cpp) | 已评估 / 保留 | 真实定时/绝对时间/时间线/帧请求和失效批次；固定时钟、重入与异常路径独立断言，验证无半更新绘制、迟到合并、隐藏和减弱动画；无真实等待 | 定时及刷新调度定向 | 0.01 |
| 30. `widget_permission_state` | [widget_permission_state_tests.cpp](../tests/widget_permission_state_tests.cpp) | 已评估 / 增强 | 真实权限目录/授予交集/必需可选及隔离预览语义，空授权拒绝回退有价值；状态名及旧指纹兼容只做自身往返/函数互比，宜补独立历史常量 | 权限/兼容格式联动 | 0.01 |
| 31. `application_restart_policy` | [application_restart_policy_tests.cpp](../tests/application_restart_policy_tests.cpp) | 已评估 / 保留 | 真实隔离子进程退出与启动取消，watchdog 读取退出码；固定崩溃判定与句柄解析；重启启动器为回调，不证明系统崩溃恢复完整流程 | 启动/退出 integration | 0.11 |
| 32. `widget_layout_context` | [widget_layout_context_tests.cpp](../tests/widget_layout_context_tests.cpp) | 已评估 / 保留 | 真实布局模板/字号迁移/参考尺寸与固定坐标数值；替身仅承载渲染状态，嵌套恢复含重入干扰；宿主设置 span 的时序仍需连接验证 | 布局/字体规则定向 | 0.01 |
| 33. `calendar_service` | [calendar_service_tests.cpp](../tests/calendar_service_tests.cpp) | 已评估 / 增强 | 真实文件服务、固定日期、修订拒绝、跨重载提醒去重和损坏隔离有价值；删除后未重载、隔离文件未核对原字节，宜补持久结果断言及失败清理 | 日历存储 integration；历法定向 | 0.07 |
| 34. `localization_contract` | [localization_contract_tests.cpp](../tests/localization_contract_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.72 |
| 35. `general_settings` | [general_settings_tests.cpp](../tests/general_settings_tests.cpp) | 已评估 / 保留 | 真实设置/外观落盘重载与独立旧版 JSON，异常渐变拒绝后保留原文件，固定渐变坐标有价值；纯默认规则与文件集成可分别选择，非视觉实测 | 设置持久化 integration；外观规则定向 | 0.03 |
| 36. `demo_mode_rules` | [demo_mode_rules_tests.cpp](../tests/demo_mode_rules_tests.cpp) | 已评估 / 增强 | 真实遮蔽规则、映射唯一性/覆盖和资源目录；分类样本标题与内容指向同类，不能区分两种来源。宜补独立分类输入、用类别 ID 代替位置索引预期 | 演示规则/资源定向 | 0.01 |
| 37. `settings_window_open_rules` | [settings_window_open_rules_tests.cpp](../tests/settings_window_open_rules_tests.cpp) | 已评估 / 改写 | 真实请求重试/替换/退出后置动作有价值；多数宿主初始化、IPC、窗口身份和布局绘制断言只是源码搜索，含多行排版及未守卫 npos（TA-01） | 打开状态机定向；宿主连接另验 | 0.02 |
| 38. `deployment_packaging_contract` | [deployment_packaging_contract_tests.cpp](../tests/deployment_packaging_contract_tests.cpp) | 已评估 / 改写 | 大部分构建/发布/迁移行为依赖源码文本，应改脚本调用及产物断言；嵌套 release_publication_tests.ps1 确有真实发布计划、固定 SHA256 和私有清单保留，应保留；未调用远程发布 | 构建/发布边界；嵌套脚本成本需计入 | 0.61 |
| 39. `steam_entitlement` | [steam_entitlement_tests.cpp](../tests/steam_entitlement_tests.cpp) | 已评估 / 增强 | 真实服务、子进程管道及 DPAPI缓存，桥是按文件名输出的可控 EXE；包含重置写保护和撤销复原。应明确 integration，不能证明真实 Steam 账户/菜单入口已验 | Steam 授权 integration/security | 1.42 |
| 40. `launcher_icon_resource` | [launcher_icon_resource_tests.cpp](../tests/launcher_icon_resource_tests.cpp) | 已评估 / 保留 | 以数据资源方式读取实际 Launcher 产物的两个图标组 ID，能发现构建漏打包；不证明图像尺寸/外观或 Shell 图标缓存 | Launcher 资源/构建定向 | 0.02 |
| 41. `steam_runtime_update` | [steam_runtime_update_tests.cpp](../tests/steam_runtime_update_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 4.41 |
| 42. `steam_local_deploy_contract` | [steam_local_deploy_contract_tests.ps1](../tests/steam_local_deploy_contract_tests.ps1) | 已评估 / 增强 | 真实部署脚本操作隔离 Steam 夹具，核对 dry-run 不写、数据隔离、元文件哈希不变和重部署保留；负例接受任意异常，宜核对错误类型/原因以免环境错误冒充拒绝 | 本地部署 integration；无需真实 Steam | 1.00 |
| 43. `deployment_manifest_integration` | [deployment_manifest_integration.ps1](../tests/deployment_manifest_integration.ps1) | 已评估 / 增强 | 真实清单生成/哈希防篡改/复制和 XML 合并；DLL/XBF 为文本夹具，不能证明可运行发行包。硬编码用户 NuGet 默认目录不随实际配置，宜隔离规则输入与 SDK 环境验证 | 打包清单 integration；产物启动另验 | 1.15 |
| 44. `settings_controller` | [settings_controller_tests.cpp](../tests/settings_controller_tests.cpp) | 已评估 / 增强 | 真实 controller 的合并预览、失败保留、重入新值、外部替换禁止旧写有价值；存储/宿主动作为替身，部分保存只核次数。附带真实管道/子进程、失联超时、句柄回收和 IPC 隔离测试，应支持分组；预设/分类归一化为桩 | 设置规则定向；IPC/子进程 integration；大图标门禁联动 | 1.17 |
| 45. `settings_host_actions_semantics` | [settings_host_actions_semantics_tests.cpp](../tests/settings_host_actions_semantics_tests.cpp) | 已评估 / 改写 | 全为源码片段和调用顺序搜索；预览不写系统状态等负向约束有价值，但提交、回滚、布局恢复及高级功能门禁均未执行，部分多行/npos 顺序脆弱。需连接 controller 与可记录宿主动作 | 设置宿主预览/提交/门禁跨模块联动 | 0.02 |
| 46. `settings_search_index` | [settings_search_index_tests.cpp](../tests/settings_search_index_tests.cpp) | 已评估 / 保留 | 真实索引/路由，固定中文/Unicode多词、排序、限制和换语言重建；隐藏、卸载及条件页不泄漏有负向断言；目录为输入，真实设置入口映射需另验 | 搜索/导航/可见性定向 | 0.01 |
| 47. `winui_settings_navigation` | [winui_settings_navigation_tests.cpp](../tests/winui_settings_navigation_tests.cpp) | 已评估 / 改写 | 真实导航状态机覆盖历史、代际、隐藏页及详情父页，应保留；后 800 余行仅源码/XAML 搜索并重复共享控件/宿主测试，巨型合取难定位。删除无契约的排列/尺寸扫描，页面路由与交互改实际连接断言 | 导航状态定向；页面连接和共享控件分别验证 | 0.02 |
| 48. `hotkey_recorder_rules` | [hotkey_recorder_rules_tests.cpp](../tests/hotkey_recorder_rules_tests.cpp) | 已评估 / 保留 | 真实录制状态机，独立按键/IME输入、代际与请求失效、冲突/取消/提交预期；可用性结果注入，不证明实际全局热键注册或 WinUI 焦点事件接入 | 快捷键状态机定向 | 0.01 |
| 49. `winui_personalization_page_presenter` | [winui_personalization_page_presenter_tests.cpp](../tests/winui_personalization_page_presenter_tests.cpp) | 已评估 / 改写 | 全部为源码搜索，字段出现即称真实绑定，事件名/常量即称预览合并与取消；含换行依赖及共享控件重复（TA-01/11），应改真实控件/回调行为 | 外观页面绑定与共享控件；静态约束合并 | 0.01 |
| 50. `winui_desktop_page_presenter` | [winui_desktop_page_presenter_tests.cpp](../tests/winui_desktop_page_presenter_tests.cpp) | 已评估 / 改写 | 全部为源码搜索，量化表达式/提交事件/对齐顺序未被执行；共享控件重复且部分 find 顺序无 npos 检查。保留旧配置兼容风险，改真实绑定/存储验证（TA-11） | 桌面设置绑定/控件与持久化联动 | 0.01 |
| 51. `winui_dock_page_presenter` | [winui_dock_page_presenter_tests.cpp](../tests/winui_dock_page_presenter_tests.cpp) | 已评估 / 改写 | 只扫描源码字段、事件和多行绑定文本，不能证明 Dock 提交、初始化抑制或迁移执行；共享控件检查重复（TA-11）。保留设置更新/旧配置风险，改真实绑定与持久结果断言 | Dock 设置行为定向；共享控件统一验证 | 0.02 |
| 52. `winui_home_about_page_presenter` | [winui_home_about_page_presenter_tests.cpp](../tests/winui_home_about_page_presenter_tests.cpp) | 已评估 / 改写 | 全部为源码搜索，状态代际、商店动作、五次点击、焦点和本地化均未执行；禁止直接网络/Shell 的架构边界可保留，路由/链接改实际模型对照 | 首页/关于状态与动作；架构约束合并 | 0.01 |
| 53. `settings_update_rules` | [settings_update_rules_tests.cpp](../tests/settings_update_rules_tests.cpp) | 已评估 / 增强 | 真实变更分类与 Dock 关联偏好恢复，固定正反样本；AcrylicDarkPreset 是空替身，不能验证外观默认值；Dock 快捷键分类缺正例，宜补单字段变化表 | 设置更新分类/Dock 规则定向 | 0.01 |
| 54. `auto_start_rules` | [auto_start_rules_tests.cpp](../tests/auto_start_rules_tests.cpp) | 已评估 / 保留 | 真实启动状态/归属/迁移决策与固定标记字节，未知状态不误启用；不写注册表或系统任务，不能证明开机实效或不同系统版本兼容 | 自启动规则定向；系统操作另验 | 0.01 |
| 55. `winui_widget_settings_presenter` | [winui_widget_settings_presenter_tests.cpp](../tests/winui_widget_settings_presenter_tests.cpp) | 已评估 / 改写 | 只扫描源码，类型与控件分别存在不能证明映射正确；密码清理、回滚、代际拒绝和异步搜索只有标记。共享控件与布局检查重复，应改真实服务/页面连接断言并保留安全边界 | 组件设置映射/秘密通道/迟到结果联动 | 0.02 |
| 56. `winui_widgets_page_presenter` | [winui_widgets_page_presenter_tests.cpp](../tests/winui_widgets_page_presenter_tests.cpp) | 已评估 / 改写 | 全为源码搜索；命令/权限/代际字段存在不能证明动作被正确门禁，布局、展开和差量刷新大量绑定实现文本。保留已知崩溃边界及禁止直接 I/O 等约束，补真实来源切换、确认和迟到结果行为 | 组件管理页面绑定；共享架构合并 | 0.02 |
| 57. `winui_backup_data_page_presenter` | [winui_backup_data_page_presenter_tests.cpp](../tests/winui_backup_data_page_presenter_tests.cpp) | 已评估 / 改写 | 只扫描确认、取消、代际和重启标记，未执行危险操作门禁或迟到回调；布局常量与排版断言收益低。保留禁止 presenter 直接文件操作等负向边界，补可观察行为 | 备份确认/取消/恢复定向；架构守卫合并 | 0.01 |
| 58. `winui_backup_data_page_backend` | [backup_data_page_backend_tests.cpp](../tests/backup_data_page_backend_tests.cpp) | 已评估 / 改写 | 真实 BackupOperationControl 取消/提交互斥及 64 次竞争应保留；其余存储适配、恢复原子性、代际与重启仅搜索源码。20 ms 未就绪检查缺 worker 到达屏障，需增强确定性；不能据此证明备份恢复成功 | 取消状态机定向；真实恢复连接另验 | 0.04 |
| 59. `winui_widgets_page_backend` | [widgets_page_backend_tests.cpp](../tests/widgets_page_backend_tests.cpp) | 已评估 / 改写 | 真实操作账本覆盖并发项独立释放与身份拒绝，应保留；其余安装文件锁、权限扩展、来源同步和卸载结论来自源码搜索，甚至依赖说明注释。需实际包文件/迟到结果/失败恢复验证 | 包身份与权限/卸载/异步联动 | 0.02 |
| 60. `winui_settings_window_host` | [winui_settings_window_host_tests.cpp](../tests/winui_settings_window_host_tests.cpp) | 已评估 / 改写 | 1350 行纯源码/XAML 搜索，没有创建设置窗口；包含注释、精确尺寸/排版和 catch 数量断言，不能证明激活、标题栏、关闭资源或工作集下降。保留宿主架构负向约束，路由表改实际数据验证，生命周期和显示需对应行为证据 | 设置窗口生命周期/消息/路由；视觉实测 | 0.02 |
| 61. `settings_no_imgui_contract` | [settings_no_imgui_contract_tests.cpp](../tests/settings_no_imgui_contract_tests.cpp) | 已评估 / 合并 | 禁止宿主重引 ImGui 等负向架构约束有价值，可集中保留；正向生命周期名称不能证明执行。CMake 文本截取不等于实际链接图，public API 文件读取失败为空时负向检查会放行 | 设置/组件架构边界；优先实际链接和 API 目录 | 0.02 |
| 62. `slot_contract_matrix` | [slot_contract_matrix_tests.cpp](../tests/slot_contract_matrix_tests.cpp) | 已评估 / 增强 | 真实路由对照独立预期表；几何往返缺少固定坐标判定；4500 组合不代表真实提交（TA-05） | 规则定向；宿主连接另验 | 0.01 |
| 63. `slot_runtime_contract` | [slot_runtime_contract_tests.cpp](../tests/slot_runtime_contract_tests.cpp) | 已评估 / 增强 | 真实控制器、失效、重命名模型、队列合并有价值；外部目标为替身，恢复夹具耦合预期（TA-03/06）。托盘/注册表/快捷方式与拖放混在同一入口，宜细分选择 | 按风险细分；真实 OLE/文件用例保留 integration | 0.32 |
| 64. `widget_interaction_rules` | [widget_interaction_rules_tests.cpp](../tests/widget_interaction_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 65. `widget_interaction_region` | [widget_interaction_region_tests.cpp](../tests/widget_interaction_region_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 66. `widget_draw_geometry` | [widget_draw_geometry_tests.cpp](../tests/widget_draw_geometry_tests.cpp) | 已评估 / 增强 | 真实几何规则含固定图片裁剪、曲线坐标和数值上限；弧线只核部分端点/固定分段数，宜改完整轨迹端点、连续性和每段安全范围，影子检查保留预算。无真实渲染 | 绘图几何与公开预算定向 | 0.01 |
| 67. `widget_view_accessibility` | [widget_view_accessibility_tests.cpp](../tests/widget_view_accessibility_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 68. `widget_accessibility_provider` | [widget_accessibility_provider_tests.cpp](../tests/widget_accessibility_provider_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.18 |
| 69. `widget_view_contract` | [widget_view_contract_tests.cpp](../tests/widget_view_contract_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.05 |
| 70. `widget_author_lint` | [widget_author_lint_tests.cpp](../tests/widget_author_lint_tests.cpp) | 已评估 / 增强 | 调用真实 Lua lint，语法位置/功能声明/多语言键及质量提示有价值；多种错误共置一个样本易掩盖单项漏报，注释忽略应独立正例，JSON 诊断宜解析字段；exit 跳过临时目录析构 | 作者 lint/能力目录/本地化定向 | 0.02 |
| 71. `widget_author_test_runner` | [widget_author_test_tests.cpp](../tests/widget_author_test_tests.cpp) | 已评估 / 增强 | 真实运行受限 Lua 用例和包模块，统计成功/false/异常与诊断；尚未验证跨用例状态隔离或指令限额。临时目录依赖未播种 rand，两进程可撞名，失败 exit 跳过析构（TA-13） | 组件作者测试工具定向；隔离资源改进 | 0.02 |
| 72. `widget_author_tools` | [widget_author_tools_tests.cpp](../tests/widget_author_tools_tests.cpp) | 已评估 / 改写 | 真实权限报告生成有价值，但仅一个输入、输出 JSON 子串共存，不能证明风险/授权字段属于正确权限或任务；改解析 JSON 后逐记录核对必需/可选及域名，复用现有作者工具目标 | 作者权限报告定向 | 0.01 |
| 73. `widget_author_preview_cli` | [widget_author_preview_cli_tests.cpp](../tests/widget_author_preview_cli_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 19.28 |
| 74. `widget_view_tree` | [widget_view_tree_tests.cpp](../tests/widget_view_tree_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.04 |
| 75. `widget_text_input_rules` | [widget_text_input_rules_tests.cpp](../tests/widget_text_input_rules_tests.cpp) | 已评估 / 增强 | UTF-8 完整边界、拒绝写后不变、只读与滚动请求是真实规则；DirectWrite 换行移动只判索引增减，未证明跨到相邻视觉行（TA-12）。需独立 hit-test 行位置/横向保持及字体环境说明 | 文本输入定向；DirectWrite 布局边界 | 0.02 |
| 76. `widget_storage_transaction` | [widget_storage_transaction_tests.cpp](../tests/widget_storage_transaction_tests.cpp) | 已评估 / 增强 | 真实候选事务、最终配额、保留元数据和公开操作预算；跨实例保留只查键，typed 格式正例只自身往返，宜补原值/删除结果及独立旧版文本。无落盘，不能证明持久化原子性 | 事务/typed 格式/公开配额定向 | 0.03 |
| 77. `widget_storage_write_budget` | [widget_storage_write_budget_tests.cpp](../tests/widget_storage_write_budget_tests.cpp) | 已评估 / 保留 | 真实令牌桶配固定时间，覆盖突发耗尽、999 ms 精确重试、长闲置封顶和倒退时间；公开常量作为参数有合理性，测试不证明每个 Lua 写入口实际消费预算 | 存储限频规则；写入口联动 | 0.02 |
| 78. `widget_setting_rules` | [widget_setting_rules_tests.cpp](../tests/widget_setting_rules_tests.cpp) | 已评估 / 保留 | 真实日期时间、URL、数值步长、扩展名和 Unicode 条件校验有固定正反预期；属于公开输入契约，非框架自测。后续新类型按风险补边界，无需为数量新增目标 | 组件设置输入规则定向 | 0.01 |
| 79. `widget_settings_model` | [widget_settings_model_tests.cpp](../tests/widget_settings_model_tests.cpp) | 已评估 / 保留 | 真实类型/通道映射、独立旧布尔文本、数值边界、秘密清除及 ABA 修订拒绝；预期来自固定输入，保护公共数据契约。与较低层规则共享数值样本但新增 schema/通道连接价值 | 组件设置模型/秘密通道定向 | 0.01 |
| 80. `widget_settings_service` | [widget_settings_service_tests.cpp](../tests/widget_settings_service_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.01 |
| 81. `widget_engine_settings_backend` | [widget_engine_settings_backend_tests.cpp](../tests/widget_engine_settings_backend_tests.cpp) | 已评估 / 改写 | 真实声明转换、typed/legacy 编码和身份辅助规则可保留；实际后端事务、秘密/句柄/引用分流及事件发送只有源码扫描。需测试真实后端提交结果与事件，读取未归一化换行，数组断言应先验证长度 | 宿主设置后端/存储与事件联动 | 0.02 |
| 82. `widget_secret_store` | [widget_secret_store_tests.cpp](../tests/widget_secret_store_tests.cpp) | 已评估 / 增强 | 真实隔离文件/DPAPI 重载、包与实例隔离、更新和撤销有价值；Has 未独立改变设置键，最终重载只判 Load 成功，需断言已撤销引用仍不可解及损坏/写失败保护；不覆盖其他 Windows 用户 | secret 存储/security；DPAPI integration | 0.03 |
| 83. `builtin_widget_source_contract` | [builtin_widget_source_contract_tests.cpp](../tests/builtin_widget_source_contract_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.04 |
| 84. `widget_logical_slot` | [widget_logical_slot_tests.cpp](../tests/widget_logical_slot_tests.cpp) | 已评估 / 增强 | 真实声明、绑定/转移身份、撤销重做及拒绝后不变有价值；持久化正例自身往返，所谓有界历史未触达容量。新分支用例在 Redo 后才检查清空，原 redo 已为空（TA-14），需重排并补上限 | 逻辑槽模型/历史/兼容存储定向 | 0.01 |
| 85. `lua_logical_slot_container` | [lua_logical_slot_container_tests.cpp](../tests/lua_logical_slot_container_tests.cpp) | 已评估 / 增强 | 真实 Lua 槽位接收/命中/提交回调；预期表明确，宿主回调为替身。保留失效/键盘覆盖，减少无新增断言的 10000 次重复（TA-03/05） | 组件及交互定向 | 0.01 |
| 86. `folder_self_drop_rules` | [folder_self_drop_rules_tests.cpp](../tests/folder_self_drop_rules_tests.cpp) | 已评估 / 保留 | 真实自包含规则，独立路径样本覆盖分隔符、大小写和相邻前缀；不证明 Shell 别名或整个文件操作 | 路径规则定向 | 0.01 |
| 87. `folder_mapping_rules` | [folder_mapping_rules_tests.cpp](../tests/folder_mapping_rules_tests.cpp) | 已评估 / 保留 | 真实 ChildPath 对照固定中文/根目录/UNC 结果，保护重复分隔符回归；不覆盖实际目录读写 | 映射路径定向 | 0.01 |
| 88. `desktop_namespace_registry` | [desktop_namespace_registry_tests.cpp](../tests/desktop_namespace_registry_tests.cpp) | 已评估 / 增强 | 真实别名解析和 This PC PIDL 匹配，独立 CLSID 预期；含真实 Shell 环境依赖，建议复核 integration 标签 | 命名空间定向及系统边界 | 0.19 |
| 89. `right_click_contract` | [right_click_contract_tests.cpp](../tests/right_click_contract_tests.cpp) | 已评估 / 保留 | 真实菜单路由与独立预期表、保护命名空间和混选删除约束；真实 HMENU 加受控 IContextMenu 核对嵌套/置灰/verb，不调用系统动作，不等于宿主右键和焦点实测 | 右键/选择/命名空间规则联动 | 0.01 |
| 90. `quick_navigation_rules` | [quick_navigation_rules_tests.cpp](../tests/quick_navigation_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 91. `quick_navigation_search_async` | [quick_navigation_search_async_tests.cpp](../tests/quick_navigation_search_async_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.03 |
| 92. `shortcut_application_rules` | [shortcut_application_rules_tests.cpp](../tests/shortcut_application_rules_tests.cpp) | 待评估 / 待定 | 尚无逐项代码结论 | 待影响分析 | 0.29 |
| 93. `desktop_keyboard_rules` | [desktop_keyboard_rules_tests.cpp](../tests/desktop_keyboard_rules_tests.cpp) | 已评估 / 保留 | 真实 Alt+F4 前置条件和按住重复决策，固定正反预期保护桌面接管边界；只验证决策，不弹出系统关机对话框或验证消息链 | 桌面键盘规则定向 | 0.03 |
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
