# 余额小鲸 / Balance Whale

SnowDesktop **官方社区组件**，不随宿主作为内置组件分发。默认“小鲸陪伴”气泡布局，点底栏“查看明细”切换到纯数据卡片，点“返回小鲸”恢复；设置和右键菜单也可切换。一个实例查询一个厂商；添加多个实例可同时看多个账号或厂商。默认 3×3，最小 2×3。宽尺寸保持内容居中，两侧留白；不拉伸金额或强行拆成横向布局。需要支持加密密码与网络密钥引用的 SnowDesktop 1.0.9.0 或更新宿主，并通过清单 feature 检查。

## 使用

1. 导入 `.snowwidget`，或使用仓库的 `scripts/widget-dev.bat developer_assets/workshop_widgets/api-balance-whale -Configuration Release -Once` 同步到开发来源。
2. 在组件设置选择厂商，并在 **API Key 密码框**输入对应平台的密钥；允许 `network.internet`。
3. 默认每 300 秒更新，间隔可设为 60–86400 秒。点刷新或小鲸立即查询；重复点击有 5 秒冷却，失败时退避到最多 15 分钟。401/403 停止自动重试，修改密钥或手动刷新后再试。隐藏时暂停轮询。
4. 点“查看明细”查看接口返回的余额细分或密钥额度。这是显示模式切换，不改变查询账号或数据口径。低余额／低额度只改变状态标记，无声音、消息、费用预测或模型价格表。
5. “打开厂商控制台”只在用户点击后通过可选 `shell.launch` 权限打开官方网页；未授权时打开组件设置。

**密钥隔离：**每个厂商有独立 password 字段，没有明文默认值。宿主加密保存；Lua 只拿 `secret:v1:…` 引用，由网络任务在发送时解引用。组件不把密钥放入 URL、日志、预览或普通 storage。替换密钥时即使引用未变化，也取消旧请求、清空旧结果，防止串账号。查询结果只在当前实例内存中保留，不写磁盘。暂时失败时保留同账号上次结果并明确标记；字段缺失／无效显示错误，不显示虚假的 0。

预览金额使用隔离样本，样本说明保留在组件介绍中；组件画面采用实际运行的更新时间样式，不额外渲染“预览示例”字样。样本不会复制成新实例的真实余额。未配置真实密钥时显示未知和设置提示。

## 官方 API 依据

核对日期：2026-10-06。只使用厂商直接公开的查询接口，不调用推理接口，不读取 DSH 本地数据库，不按 Token 或定价估算费用。

| 预设 | 官方文档 | GET 查询及显示字段 |
| --- | --- | --- |
| DeepSeek | [查询余额](https://api-docs.deepseek.com/api/get-user-balance/) | `https://api.deepseek.com/user/balance`；`is_available`、`balance_infos[].currency/total_balance/topped_up_balance/granted_balance`。CNY/USD 由返回币种决定；设置仅选择优先币种，不做汇率换算。 |
| OpenRouter 账户余额 | [Get credits](https://openrouter.ai/docs/api/api-reference/credits/get-credits) | `https://openrouter.ai/api/v1/credits`；账户余额为实际返回的 `data.total_credits - data.total_usage`，同时显示两项原始金额，USD。**需要 Management Key**。这是返回值相减，不是价格推算。 |
| OpenRouter 密钥额度 | [Get current API key](https://openrouter.ai/docs/api/api-reference/api-keys/get-current-key) | `https://openrouter.ai/api/v1/key`；普通 Key 查询自身 `limit_remaining/limit/usage`，USD。`limit: null` 表示密钥未设置限额，**不代表账户余额无限**。 |
| Kimi / Moonshot 国内 | [查询余额](https://platform.kimi.com/docs/api/balance) | `https://api.moonshot.cn/v1/users/me/balance`；`code: 0`、`status: true`，`data.available_balance/cash_balance/voucher_balance`，CNY。现金可为负数；直接用 available_balance，不重新相加。 |
| Kimi / Moonshot 国际 | [Check balance](https://platform.kimi.ai/docs/api/balance) | `https://api.moonshot.ai/v1/users/me/balance`；同样字段，USD。国内和国际 Key 独立，不能混用。 |
| StepFun / 阶跃星辰 | [获取账户信息](https://platform.stepfun.com/docs/zh/api-reference/accounts/get) | `https://api.stepfun.com/v1/accounts`；`object: account`、`balance/total_cash_balance/total_voucher_balance`。该接口页面未标明币种，显示原始数值，**不擅自加人民币符号**。 |
| Novita AI | [User balance info](https://docs.novita.ai/api-reference/basic-get-user-balance) | `https://api.novita.ai/openapi/v1/billing/balance/detail`；`availableBalance/cashBalance/creditLimit` 按文档除以 10000 转为 USD，信用额度与现金分别标记。 |

所有预设使用 HTTPS 和 `Authorization: Bearer <secret>`。Novita 请求同时发送文档规定的 `Content-Type: application/json`。响应上限 128 KiB、超时 15 秒、带密钥请求不缓存；宿主限制密钥跨源重定向。

MiniMax、GLM/z.ai、Kimi Code、OpenCode Go 未列为首版固定预设：已查看 [MiniMax 官方套餐说明](https://platform.minimax.io/subscribe/coding-plan)、[z.ai 官方文档索引](https://docs.z.ai/llms.txt)、[Kimi Code 官方说明](https://www.kimi.com/code/docs/en/kimi-code/membership.html)与 [OpenCode Go 文档](https://opencode.ai/docs/go/)。这些资料或官方实现未能同时确认当前的**公开 API Key 查询方式与完整返回字段契约**，不能直接沿用参考项目里的私有接口和旧字段。没有使用套餐价格、预估次数或未确认的百分比语义。

## 自定义厂商

可用于其他厂商**官方已经公开**的 HTTPS GET JSON 余额或额度接口。填入 URL、认证头、字段路径和文档单位倍数。路径仅支持对象字段和从 0 开始的 JSON 数组索引，例如 `data.items[0].remaining`，不执行 Lua 或表达式。

- 余额：直接显示剩余字段 × 单位倍数，币种由用户依据厂商文档选 CNY/USD。
- 额度：指定剩余字段和总额度字段。显示原始计量剩余值／总值，以及这两个返回值的比例。不会从使用量、请求次数或价格表补出缺失值。
- 仅支持数值或普通十进制数值字符串；拒绝无效、缺失、非有限值、负剩余额度、总额度 ≤ 0 和剩余额度大于总量。
- 不提供手工假余额、登录 Cookie、网页抓取、OAuth 登录、DSH 任务、模型成本追踪。URL 不含密钥；认证只能通过宿主密码字段。

## 复刻与许可

布局方向与“余额气泡 + 小鲸 / 数据卡片”切换参考 [MeteorNOX/DeepSeek-Balance-Whale-Widget](https://github.com/MeteorNOX/DeepSeek-Balance-Whale-Widget/tree/770d3f55ff40284eb244fff33fc9ed4654be8a45)，代码许可 MIT，保留作者署名。

上游 [PROVENANCE.md](https://github.com/MeteorNOX/DeepSeek-Balance-Whale-Widget/blob/770d3f55ff40284eb244fff33fc9ed4654be8a45/PROVENANCE.md) 明确将 `assets/**` 排除在 MIT 之外。本组件未复制上游角色图片或音频，而是使用新的原创生成插画；见 [ARTWORK.md](ARTWORK.md)。没有引入 DSH 依赖、网页执行环境、模型定价、费用／Token 趋势或动画音效脚本。

## 验证边界

`snowwidget lint/test/permissions/preview/quality/pack/validate` 用于组件级验证。纯 Lua 测试覆盖文档字段与单位、无效数据、密钥引用、旧请求隔离、取消、权限拒绝、退避和销毁。官方接口样本用于解析测试，不等于真实账户联网验证。宿主 password 的加密落盘由已有 SecretStore 回归验证，不在组件中重写加密实现。

真实账号、密码框交互、鼠标／键盘、右键菜单和开发来源激活仍需在 SnowDesktop 实机验收；CLI 渲染图片不代表这些交互通过。禁止把 API Key 发到聊天记录或命令行日志中。

## English

Official community widget, shipped separately from SnowDesktop. Whale mode is the default; the footer's View details / Back to whale actions switch modes, as do settings and the context menu. Wide spans keep the composition centered with side margins. Preview samples are isolated and explained in the introduction; the rendered widget uses the same update-time presentation as runtime. Add separate instances for different accounts. Keys use host-encrypted password settings and opaque secret references; replacing a password cancels pending work and clears previous results. No plaintext key, disk balance cache, price table, billing estimate, DSH database, or inference request is used.

Presets cover the documented DeepSeek, OpenRouter account/key, Moonshot CN/international, StepFun and Novita GET APIs listed above. The OpenRouter account endpoint requires a Management Key; a normal key can query its own spending limit. StepFun's reference does not identify a currency, so its amounts are shown without a currency symbol. Custom HTTPS GET JSON queries accept explicit documented paths, units and authentication headers. Missing or malformed values never become a fabricated zero balance.

Automated package checks and isolated CLI previews are distinct from real-key network and desktop interaction acceptance. The MIT upstream excludes its assets; this package uses a new generated illustration and retains code attribution.
