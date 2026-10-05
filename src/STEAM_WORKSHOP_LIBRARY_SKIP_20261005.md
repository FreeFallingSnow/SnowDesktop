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
