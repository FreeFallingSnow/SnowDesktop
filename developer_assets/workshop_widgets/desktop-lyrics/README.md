# Desktop Lyrics / 桌面歌词

SnowDesktop 官方社区歌词组件。默认选用“透明”外观预设，只绘制歌词；当前行位于纵向中央，前后行逐渐淡出。组件高度决定可见行数，字号和行距可独立调整。右键菜单提供带刷新图标的“重新匹配歌词”；显示设置和外观预设使用宿主统一的组件设置入口。

## 使用条件

- 播放器必须通过 Windows SMTC 提供当前歌曲名称、歌手和有效播放进度。没有 SMTC 会话时等待媒体；没有可用进度时明确显示不支持，不按系统时间、音频或歌曲时长猜测进度。
- 为组件允许“读取媒体信息”和“联网”。组件只读取当前 Windows 媒体会话；不提供播放、暂停、切歌、跳转、音量、搜索点播或队列操作。
- `electron.app.*` 这类应用标识会话，以及歌手字段为浏览器/应用占位名的会话不获取歌词，显示静候旋律的占位语。覆盖 Edge、Chrome、Chromium、Firefox、Brave、Opera/GX、Vivaldi、Arc、Zen、Waterfox、Yandex、Whale、QQ、360、搜狗、夸克、UC、傲游及 DuckDuckGo 的名称和常用进程标识；占位名过滤会忽略空白和不可见格式字符。`Brave`、`Opera`、`Zen` 等可能是艺名的短名称须与浏览器来源一致；按完整值判断，不截取作者名。已识别浏览器的作者是 HTTP(S) URL 或以 `www.` 开头时同样不搜索。浏览器先提供标题、尚无真实歌手时等待元数据，不提前搜索；提供真实歌曲信息后仍可使用。请求完成和手动重新匹配前重读当前媒体，已变成占位会话时取消旧请求链。
- 只需 SnowDesktop，无需 Folia、Stage API、Folium、令牌、适配模组或额外本地服务。
- 使用支持清单所列 API v2 features 的宿主；最低声明版本为 1.0.13.0，同版本早期构建仍以 feature 检查为准。

## 歌词获取

参考 Folia 的多源匹配思路，依次尝试 AMLL、网易云、QQ、酷狗和 LRCLIB，共五个歌词源。歌手已知时必须匹配歌手，专辑名不能绕过歌手检查；歌手缺失时才以专辑辅助核对。对提供时长的候选核对不超过三秒的时长差；忽略歌名中不表示版本的附注，保留 Live、翻唱、Remix 等版本标记，支持常见的多歌手分隔方式。

AMLL 先按歌名和歌手搜索，必要时按歌名重查并在本地核对身份。网易云和 QQ 找到匹配的歌曲后，还会按平台歌曲 ID 探测 AMLL 的 TTML；QQ 同时支持 mid 和数字 ID。LRCLIB 精确获取失败后再搜索候选。每次只有一个在途请求，单次请求最多五秒，全部来源失败后退避重试；切歌会取消上一首的请求。

AMLL TTML 和网易云 YRC 保留原生逐字时间；QQ、酷狗和 LRCLIB 的 LRC 按行同步，不伪造逐字时间。候选列表综合歌曲、歌手、专辑与时长选择最高匹配项。身份匹配优先于逐字格式：较低匹配的结果可以先显示，但会继续寻找更高匹配的结果，不能仅凭带逐字时间抢先结束；匹配程度相同时优先逐字歌词。后续来源失败时保留已经取得的最佳结果。歌词及其翻译、音译在当前实例内做有界缓存，组件目录不写入缓存文件。

组件仅向清单列出的六个服务主机发送匹配所需的歌名、歌手、平台歌曲 ID 和时长，不使用播放器账号，不读取音频文件。Folia 的汽水源依赖桌面桥接，Spotify 源需要账号；本组件保持独立，不接入这两类有额外运行条件的来源。第三方接口可能限流、拒绝匿名请求或未收录对应版本，接入来源不保证每首歌曲都能匹配。

只使用公开接口契约，独立实现匹配、解析和绘制，没有复制 Folia 渲染源码。参考来源：

- [Folia 歌词匹配实现](https://github.com/chthollyphile/folia-major/blob/main/src/utils/lyrics/autoMatchBestLyric.ts)
- [Folia AMLL 接口实现](https://github.com/chthollyphile/folia-major/blob/main/src/utils/lyrics/providers/amllApi.ts)
- [Folia 歌词来源及可用条件](https://github.com/chthollyphile/folia-major/blob/main/src/utils/lyrics/lyricSources.ts)
- [Folia QQ 歌词实现](https://github.com/chthollyphile/folia-major/blob/main/src/services/onlineMusic/qq/qqLyricProvider.ts)
- [Folia 酷狗歌词实现](https://github.com/chthollyphile/folia-major/blob/main/src/services/onlineMusic/kugou/kugouLyricProvider.ts)
- [AMLL 原生接口](https://amll.dev/reference/http-api/native)
- [LRCLIB API](https://lrclib.net/docs)

## 时间线与显示

重复的 SMTC 时间线样本不重置本地计时；正常播放中的小幅时间偏差通过有界速度修正平滑收敛，明显跳转或切歌立即跟随。系统没有报告播放倍速时，先等待连续真实位置样本；两个一致的样本间隔建立速率后再平滑推进，不凭空假定 1 倍速。AMLL 的不带单位秒时间、分钟及小时格式均按完整时间轴解析。

YRC、TTML 等逐字歌词取得同曲的可靠逐行对照时，统一按相同原文的多句锚点检查时间差。网易云使用同响应 LRC，AMLL TTML 通过其平台歌曲 ID 获取对照，QQ 通过已匹配的歌曲获取逐行对照。只有足够多的锚点覆盖歌曲不同位置、且偏差表现为稳定常数时，才校准该份歌词的来源偏移；保留原生逐字间隔，不修改用户延迟设置。校准不按歌名、歌手或固定毫秒数特判。对照不可用、文本不一致、锚点不足、时间差不稳定或呈现时钟漂移时保留原时间轴。

媒体快照使用一秒采样容限。短暂过期或缺失时暂停新增网络请求并保留已取得的歌词、请求阶段和校准信息，恢复后继续；专辑字段补全、同歌切换媒体会话不会清空歌词重新搜索。真实换歌、手动重新匹配和失败退避后的重试仍可发起新查询。缺少有效 SMTC 进度时不显示同步歌词。

有有效时间线且正在显示歌词时使用宿主 `animation.frame` 逐帧刷新；暂停、隐藏、无歌词或无有效进度时停止逐帧请求。系统减少动态效果时遵从宿主限制。

“额外歌词延迟”使用毫秒，正数让歌词稍后出现。“透明”是可重新选择的默认组件预设，不开启背景层、玻璃或底色；宿主负责应用预设与保存外观选择。歌词字号由用户设置与宿主行单位决定，不随每句长度变化。长句优先在标点、词边界拆分，再按固定字号换行；原生逐字高亮随文字跨行继续。每句按实际行数和翻译高度排布，组件高度决定能显示的前后歌词数量。绘制宽度另留舍入余量，避免恰好放得下时误省略最后一个字。演示歌词为原创示例，不从网络歌曲中截取。

暂停、停止或没有媒体会话时显示“此刻留白，静候下一段旋律。”；恢复播放后显示同步歌词。权限不足时显示明确原因；播放器未提供可用 SMTC 进度时提示推荐 Folia 播放器。短暂媒体快照过期仅显示等待占位，不误判为播放器不支持。

本组件替代未发布的 Folia Desktop 实验组件，保留 UUID `e2ce8149-ff61-4c2f-9975-8ae4c010e639`；新开发来源为 `data/widgets/dev/desktop-lyrics/`。原布局与组件存储不手工删除，原来的连接字段不再读取。

## English

A read-only community lyric widget for SnowDesktop. It starts transparent and draws lyrics only. The current line stays centered; increasing widget height reveals more surrounding lines. Font scale and line spacing are separate preferences.

A valid Windows SMTC session and timeline are required. Media without a usable timeline is explicitly unsupported, with a recommendation to try Folia. A temporarily stale snapshot displays the waiting message instead. The widget searches five sources: AMLL, NetEase, QQ Music, Kugou and LRCLIB. A known artist must match; an album cannot override an artist mismatch. Album identity is a fallback only when the target artist is absent. Available duration and meaningful version markers are also checked. NetEase and QQ song IDs allow AMLL TTML lookup; QQ tries both its mid and numeric ID. LRCLIB has a candidate-search fallback. One request runs at a time, with a five-second timeout and bounded retry backoff.

Native TTML and YRC word timing is preserved. Synchronized LRC remains line-timed. Candidate lists rank song, artist, album and available duration. Identity confidence takes priority over word timing: a weaker word-timed result does not stop the search or replace a stronger line-timed match. Word timing wins when confidence is equal. Usable lyrics appear while later sources are checked, and the best result remains available if those requests fail. Only six lyric-service hosts are allowed by the manifest. No account cookies, audio files or playback controls are used. Folia's Qishui source needs its desktop bridge and Spotify needs an account; neither is included here. Upstream coverage and anonymous access can vary.

TTML plain seconds such as `19.340` and minute/hour clock forms share the same absolute timeline. When SMTC omits its playback rate, two consistent real sample intervals establish an estimated rate for smooth interpolation; a single sample never implies 1x playback. Duplicate source samples retain their clock anchor. Temporary missing or stale media pauses new requests while retaining lyrics, calibration and the next request stage. Album enrichment or a replacement session for the same song does not restart lyric lookup.

YRC and TTML word-timed lyrics share a generic calibration path when reliable line timing for the same track is available. NetEase uses its accompanying LRC; AMLL uses platform song IDs and QQ uses the matched song to obtain a reference. Matching original-text anchors across the song can establish a stable source offset. Calibration requires enough consistent anchors over different song positions and rejects drifting or ambiguous timing. It preserves native word intervals and never changes the user's delay preference. No song names, artists or fixed per-song offsets are special-cased. Missing references leave the original timing intact.

The default transparent appearance is a selectable component preset. Settings use the host entry; the sole component context-menu command is lyric rematching with a Fluent refresh icon. Paused or stopped playback displays “A quiet pause, awaiting the next melody.” Font size stays consistent between sentences. Long lyrics split at punctuation or word boundaries and wrap without shrinking; native word highlighting continues across rows. Actual block heights, including wrapped translations, determine spacing and visible content. The drawing box includes a rounding margin to preserve the last glyph.

Allow media reading and network access. No Folia installation, adapter mod, token or local service is required. Required features are checked; the declared minimum host version is 1.0.13.0. GPL-3.0-only; see LICENSE.

Application-identifier sessions such as `electron.app.*`, and tracks whose artist is a browser/application placeholder, are excluded before any lyric lookup. Labels and common process identifiers cover Edge, Chrome, Chromium, Firefox, Brave, Opera/GX, Vivaldi, Arc, Zen, Waterfox, Yandex, Whale, QQ, 360, Sogou, Quark, UC, Maxthon and DuckDuckGo. Checks ignore whitespace and invisible format characters. Ambiguous short labels such as Brave, Opera and Zen require the matching browser source. Artist labels are matched as complete values, never truncated. Recognized browser sessions also exclude artists formatted as HTTP(S) URLs or beginning with www. Browser sessions with a title but no real artist wait for metadata before searching; browser sessions with real song metadata remain supported. Task completions and manual rematching refresh the current media first, canceling the old request chain when the session becomes a placeholder.

浏览器标识与来源回退的核对资料 / Browser identity and fallback references:

- [Chromium media metadata fallback](https://chromium.googlesource.com/chromium/src/+/e30962366bf80af4683d3b82b468c7ae1d00f2d5/content/browser/media/system_media_controls_notifier.cc)
- [Firefox Windows SMTC implementation](https://searchfox.org/firefox-main/source/widget/windows/WindowsSMTCProvider.cpp)
- [Brave process path](https://support.brave.app/hc/en-us/articles/360044860011-How-Do-I-Use-Command-Line-Flags-in-Brave), [Vivaldi process path](https://help.vivaldi.com/desktop/privacy/private-windows/), [Zen build documentation](https://docs.zen-browser.app/contribute/desktop/building)
- [360 process names](https://browser.360.cn/se/help/faq-detail_zy_zy.html), [Yandex process path](https://yandex.com/support/browser/en/troubleshooting/media)

以上名称作为占位过滤规则维护；未逐一实机确认所有浏览器都会把名称写入歌手字段。明确 URL 以外的普通带点艺名保留。
These names define placeholder rules; not every browser has been observed publishing its name as the artist. Ordinary dotted artist names remain valid unless they are explicit URLs.
