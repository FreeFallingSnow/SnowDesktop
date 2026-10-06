# Workshop library skip runtime feedback, 2026-10-05

Validation target: `83e5d43455468ece441a53ebc0c06cd187aa2108`.

- Standard Release build and 128 application tests passed in batch
  `4778ac3035964a3e8e5d57477118074c`.
- The user reported that the path error no longer appeared, but widgets already
  unsubscribed in Steam remained in SnowDesktop.
- Read-only inspection found an unavailable library path alongside a readable
  local application Workshop manifest. The aggregate marked every such scan
  non-authoritative, so history reconciliation could not schedule removals.
- Result: runtime unsubscribe reconciliation **failed**. The successful cache
  query and isolated package tests do not establish runtime synchronization.
- The user requires the existing local manifest as the synchronization source,
  skipping unusable libraries, with no additional client query or protocol.

中文记录：用户实测路径错误已不再出现，但 Steam 退订后的组件仍残留。读取到的本地清单被其他
不可用库的状态整体降为不可信，导致历史退订同步未执行。本次实机验收失败；用户要求沿用本地
清单并跳过不可用库，不引入额外客户端查询或协议。后续调整须保留此失败记录并单独验证。

## Follow-up runtime evidence

Target: `caacfa12b26bcd990fd930578013df531656cb7b`.

- Standard Release build and the merged 12 selected application regressions
  passed in batch `f112a32e992c4325a25bf9a746cb4d37`, without warnings or retries.
- The executable SHA-256 was
  `75033e4d20b8e1beab507aafe4ead43229669bc1a4c71a679681c4382c97bdb1`.
- A read-only probe using production cache/history/planning code observed 11
  subscriptions and 11 downloads. The old authority gate planned no removals;
  the adjusted local flow planned removals for `3804397314`, `3806093751` and
  `3806138954`.
- The new application was observed running from `.build/Release` after the
  build. At 22:55:05–22:55:06 local time, its existing uninstall flow moved those
  three packages into the quarantine directory and removed their registrations.
  The resubscribed `3806202707` remained installed. Subscription history matched
  the 11 local subscriptions, and a subsequent probe found no removal actions.
- The 22:55:06 diagnostic showed a package validation timeout being skipped
  alongside the unavailable library; it did not block those removals. The next
  query at 22:56:04 recorded only the skipped library path.
- Evidence is preserved in `.build/verification/workshop-skip-20261005/`:
  `local-sync-after-installed.log`, `local-sync-runtime-after.log` and
  `local-sync-runtime-evidence.json`. Checks were read-only and used no desktop
  UI automation. Menu appearance was not part of this acceptance.

中文结果：新构建运行后，三个退订组件已按宿主原有卸载流程转入隔离目录并从注册表移除；用户
重新订阅的项目仍保留。实际订阅历史与本地清单一致，补查无待卸载操作。启动时发生的包校验
超时也未阻断退订同步。本地退订残留的数据流程验收通过；本记录未验收桌面菜单视觉效果。

## Steam installation follow-up failure (2026-10-06)

- Target: `caacfa12b26bcd990fd930578013df531656cb7b`, included in packaged source
  `72f09bba624bf394adb7341dc6d8a8c7aa66effc`, internal-dev Build `25731163`.
- The running Steam runtime is `1.0.8.0-487c906757f9f76f`; its executable SHA-256
  is `4051edebd31e06b690c5f1bf6dc51104ebca9d9a69be27bdd9a6e6a40550b851`.
  Git ancestry and the local publication receipt confirm it contains that try.
- Steam uses `D:/SteamLibrary/steamapps/common/SnowDesktop/data/widgets/packages.json`,
  separate from the developer deployment accepted above. Its active-account
  history contains only old item `3780926790`; several installed managed
  Workshop packages absent from the local subscription manifest remain.
- A read-only production cache/planner probe reports authority true, eight
  subscriptions, the unavailable E: library skipped, and zero removal actions.
  Evidence: `.build/verification/workshop-skip-20261005/steam-registry-before-20261006.log`.
- The Steam deployment log at 2026-10-05 23:58:04 reports an ownership conflict
  for local-directory package `fe32b603-6fcf-4bc1-a4cf-367f06d42e17`. Planner
  conflicts do not stop individual actions, but the result-error gate prevents
  persisting the otherwise valid subscription history.
- Result: unsubscribe reconciliation **fails in this Steam installation**.
  The prior developer-data success remains scoped to that dataset. This diagnosis
  does not modify Steam user data, subscribe/unsubscribe, or query Steam online.

中文结论：当前 Steam 版包含上一轮尝试，但 Steam 安装目录使用另一份组件注册表；历史只含一条
旧订阅，使历史遗漏的已安装工坊组件无法进入卸载名单。本地音频频谱与工坊包的 ID 冲突又阻止
有效订阅历史保存。因此此 Steam 数据场景验收失败。上述开发目录的通过记录仅适用于当时数据；
本次检查只读，不修改用户数据，不查询在线订阅。

## Established-account reconciliation candidate (2026-10-06)

- Reconcile managed Steam source bindings alongside the existing account history,
  so incomplete old history does not hide installed packages absent from a valid
  local subscription manifest. New-account baselines, other accounts' remembered
  subscriptions, local packages, built-ins and development packages stay protected.
- An existing local package ID causes that subscribed Workshop item to be skipped
  with a diagnostic warning. Valid subscription identities are persisted independently
  of individual package apply errors; still-installed absent items can be retried.
- Standard Release build and full application suite passed in batch
  `f5a73e34a55f4839b3e07725089bf69b`: 128/128, parallelism 4, 170.38 seconds,
  no test retries, no compile/link warnings, stable input and output checks passed.
  Tools/manual suites were not selected; no build-tool implementation changed.
- Candidate executable SHA-256:
  `9e7be90ac4692770468bffd32424b29820989ce4304a3e7fe9f8eed51f3d3168`.
- The same Steam installation data now yields five removal actions, against zero
  from history-only reconciliation: `3806202707`, `3804397314`, `3806093751`,
  `3806138954`, `3806972908`. The manifest and registry hashes remained stable
  across the read-only probe. No actual Steam user package was removed by the probe.
- Isolated production-source/package-manager regression performs removal with stale
  history and a competing local package ID, checks subscribed/local retention and
  persisted identities, and retries a remaining managed copy after history advances.
- Evidence: `.build/verification/workshop-skip-20261005/steam-registry-after-20261006.log`
  and `steam-registry-evidence-20261006.json`; the batch log and CTest report preserve
  the full suite. No online subscription query or protocol was added.
- Pending: acceptance in a Steam runtime containing this candidate. Build `25731163`
  contains the previous try, and has not been replaced/published by this task.

中文结果：以已安装工坊来源补齐旧历史的遗漏，单项本地 ID 冲突跳过并记录提示，有效本地订阅历史
独立保存。标准 Release 和应用全量 128/128 通过，4 路并行 170.38 秒，无重试、编译或链接警告。
同一份 Steam 数据只读核对从零个卸载操作变为五个，期间数据哈希稳定；隔离回归实际执行卸载，
验证本地/已订阅包保留和再次同步。没有引入在线查询或协议；Steam Build 25731163 尚不含此补改，
待包含该候选的 Steam 宿主实机验收。本次探测未修改 Steam 用户组件数据。

## Steam internal-dev publication (2026-10-06)

- Published source: `ff0813d947192a9901175e03e55121f0a4fa2378`.
- SteamPipe successfully completed App `5080330` Build `25741790` at
  2026-10-06 10:04:08 Asia/Shanghai, using `SetLive internal-dev`; Depot
  `5080331` Manifest `7608319775870335017`.
- Runtime: `1.0.8.0-e32cdcea93ed20f3`; 428 payload files and ZIP entries verified.
  Host SHA-256: `9e7be90ac4692770468bffd32424b29820989ce4304a3e7fe9f8eed51f3d3168`.
  Package SHA-256: `58be4c2e69f1b802d4742784f3a255dc779dddd796de8adf04fbda3b470bf19a`.
  Manifest SHA-256: `808686d82ce4613a05a2230e83461d198d39442a3427dbbb4a63927ad2b1ceb2`.
- Reused the matching standard build/full application validation above. Packaged
  host hash matches the tested host. Public app-info readbacks before/after both
  report Build `25523336`. Private branch assignment evidence is the SetLive VDF
  plus successful app/depot receipts; no independent private-branch readback.
- Desktop Steam was restarted once after all SteamCMD operations, and its new
  connection log confirms login OK at 10:04:31. The local Steam app manifest now
  reports Build `25741790`; installed distribution host hash and runtime manifest
  match the published package. The agent did not start the desktop application.
- Evidence directory: `artifacts/v1.0.8.0/steam-test-20261006-ff0813d9/`, including
  `receipt.json`, `package-verification.json`, `app-build.vdf`, app/depot logs,
  public readbacks, client restoration and installed-payload verification.
- Pending: actual unsubscribe reconciliation after starting the updated Steam
  application, along with desktop interaction/appearance acceptance.

中文发布记录：源码 ff0813d9 于 2026-10-06 10:04:08 发布为 internal-dev Build 25741790，
runtime 和包哈希已核对。复用匹配的标准构建与应用全量结果，公开分支前后仍为 Build 25523336。
私有分支以 SetLive 和成功回执为证据，未独立回查。Steam 已重启并恢复在线，本机已下载新版，
安装载荷与发布包一致。未启动桌面宿主，更新后实际退订卸载及桌面交互仍待用户实机验收。
