# 开发组件默认来源 / Default development source

新发现且验证通过的开发组件 UUID 默认激活为开发来源。注册表保存 knownDevelopmentIds 和 developmentOverrides；用户停用后再次扫描、同步或重启不重新激活。旧注册表迁移时保留现存开发组件的原选择，因为旧数据不能区分默认未激活与人工停用。只新增之前未发现的 UUID 时自动激活；不自动改变日程等敏感权限的决定。

New valid development UUIDs become active sources on discovery. Persisted knownDevelopmentIds and developmentOverrides preserve opt-outs across rescans, synchronization and restarts. Legacy registries retain the selection of existing candidates because old state cannot distinguish defaults from manual opt-outs. Sensitive permission decisions remain independent.
