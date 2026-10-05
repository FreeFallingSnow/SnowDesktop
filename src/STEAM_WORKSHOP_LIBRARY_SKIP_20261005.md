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
