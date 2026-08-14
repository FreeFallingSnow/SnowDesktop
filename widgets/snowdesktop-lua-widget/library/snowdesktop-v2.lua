---@meta SnowDesktop API v2

---@alias SnowWidgetSizeClass 'small'|'medium'|'large'
---@alias SnowWidgetSurfaceKind 'desktop'|'preview'
---@alias SnowResourceState 'pending'|'ready'|'error'
---@alias SnowDateStyle 'none'|'short'|'long'
---@alias SnowTimeStyle 'none'|'short'|'long'
---@alias SnowDurationStyle 'short'|'clock'
---@alias SnowStateValue nil|boolean|number|string|SnowStateValue[]|table<string, SnowStateValue>

---@class SnowSize
---@field width number
---@field height number

---@class SnowRect: SnowSize
---@field left number
---@field top number
---@field right number
---@field bottom number

---@class SnowDpiContext
---@field x integer
---@field y integer
---@field scaleX number
---@field scaleY number

---@class SnowGridContext
---@field columns integer
---@field rows integer

---@class SnowMonitorContext
---@field available boolean
---@field primary boolean
---@field pixelBounds SnowRect
---@field pixelWorkArea SnowRect
---@field logicalBounds SnowRect
---@field logicalWorkArea SnowRect

---@class SnowThemeContext
---@field mode 'dark'|'light'
---@field background integer RGB color
---@field border integer RGB color
---@field accentToken 'systemAccent'
---@field accentColor integer RGB color
---@field highContrast boolean

---@class SnowAccessibilityContext
---@field highContrast boolean
---@field reducedMotion boolean
---@field textScale number

---@class SnowWidgetContext
---@field logicalSize SnowSize
---@field pixelSize SnowSize
---@field dpi SnowDpiContext
---@field sizeClass SnowWidgetSizeClass
---@field grid SnowGridContext
---@field monitor SnowMonitorContext
---@field theme SnowThemeContext
---@field accessibility SnowAccessibilityContext
---@field locale string BCP-47 locale
---@field region string ISO 3166 region when available
---@field timeZone string Windows time-zone key
---@field utcOffsetMinutes integer
---@field inputLanguage string BCP-47 input language when available
---@field visible boolean
---@field preview boolean
---@field focused boolean Host-managed input focus in the current build
---@field selected boolean
---@field surface SnowWidgetSurfaceKind

---@class SnowWidgetInfo
---@field id string
---@field width number
---@field height number
---@field selected boolean
---@field selectedPackageId string

---@class SnowWidgetTheme
---@field bg integer
---@field border integer
---@field alpha number
---@field borderAlpha number
---@field gradientEndA number
---@field cornerRadius number
---@field contentTheme integer

---@class SnowSettingField
---@field key string
---@field label string
---@field type 'text'|'bool'|'int'|'float'|'select'|'color'
---@field default? string|number|boolean
---@field min? number
---@field max? number
---@field options? string[]

---@class SnowSettingPreset
---@field id string
---@field label string
---@field default? boolean
---@field values table<string, string|number|boolean>

---@class SnowWidgetSettings
---@field fields? SnowSettingField[]
---@field presets? SnowSettingPreset[]

---@class SnowWidgetDefinition
---@field name? string
---@field render fun(context: SnowWidgetContext, model: any) Exactly one of render or view is required in API v2; view is not available yet.
---@field setup? fun(context: SnowWidgetContext): any Runs once and returns the instance model passed to render and dispose.
---@field event? fun(context: SnowWidgetContext, model: any, event: SnowWidgetEvent) Receives host surface events; declarative node events are not available yet.
---@field dispose? fun(context: SnowWidgetContext, model: any, reason: 'unload'|'hotReload'|'shutdown'|string) Runs at most once before the instance VM is released.
---@field useCustomStyle? boolean
---@field followPersonalizationDefault? boolean
---@field showTitle? boolean
---@field bottomBarHover? boolean
---@field bg? integer
---@field border? integer
---@field alpha? number
---@field borderAlpha? number
---@field gradientEndA? number
---@field glassEnabled? boolean
---@field settings? SnowWidgetSettings

---@class SnowWidgetEvent
---@field kind 'visibility'|'resize'|'pointer'|'timer'|'schedule'|'action'|'selection'|'environment'|'panel'
---@field action? 'click'|'doubleClick'|'pointerDown'|'pointerMove'|'pointerUp'|'wheel'|'opened'|'closed'|string
---@field id? string
---@field name? string
---@field missed? integer
---@field coalesced? boolean
---@field visible? boolean
---@field selected? boolean
---@field columns? integer
---@field rows? integer
---@field x? integer
---@field y? integer
---@field button? integer
---@field delta? integer
---@field surface? 'desktop'|'panel'
---@field reason? string

---@class SnowApiInfo
---@field current integer
---@field supported integer[]
---@field features string[]

---@class SnowCapability
---@field id string
---@field available boolean
---@field version? integer
---@field reason? 'unsupported'|string

---@class SnowCapabilities
---@field apiVersion integer
---@field features SnowCapability[]

---@class SnowSystemInfo
---@field osFamily 'windows'
---@field osBuild? integer
---@field processArchitecture 'x86'|'x64'|'arm64'|'unknown'
---@field nativeArchitecture 'x86'|'x64'|'arm64'|'unknown'
---@field hostVersion string
---@field apiVersion integer
---@field packaged boolean
---@field portable boolean
---@field deploymentMode 'packaged'|'portable'

---@class SnowSystemUptime
---@field milliseconds integer
---@field includesSleep boolean

---@class SnowDateTimeParts
---@field year integer
---@field month integer
---@field day integer
---@field wday integer 1 is Sunday
---@field hour integer
---@field min integer
---@field sec integer
---@field millisecond integer
---@field timeZone string

---@class SnowTimeFormatOptions
---@field timeZone? 'local'|'utc'|string
---@field locale? string
---@field dateStyle? SnowDateStyle
---@field timeStyle? SnowTimeStyle

---@class SnowTimeDelta
---@field years? integer
---@field months? integer
---@field days? integer
---@field hours? integer
---@field minutes? integer
---@field seconds? integer
---@field milliseconds? integer

---@class SnowTimeZoneOptions
---@field timeZone? 'local'|'utc'|string

---@class SnowNumberFormatOptions
---@field locale? string
---@field minimumFractionDigits? integer
---@field maximumFractionDigits? integer
---@field grouping? boolean

---@class SnowBytesFormatOptions
---@field locale? string
---@field base? 1000|1024
---@field maximumFractionDigits? integer

---@class SnowDurationFormatOptions
---@field locale? string
---@field style? SnowDurationStyle

---@class SnowRelativeTimeFormatOptions
---@field locale? string
---@field unit? 'auto'|'second'|'minute'|'hour'|'day'
---@field numeric? 'auto'|'always'

---@class SnowLocaleOptions
---@field locale? string

---@class SnowImageResource

---@class SnowFontResource

---@class SnowWidgetPanelOptions
---@field title? string
---@field width? integer
---@field height? integer

---@class SnowInlineTextEditOptions
---@field storageKey string
---@field x number
---@field y number
---@field width number
---@field height number
---@field multiline? boolean
---@field initialValue? string
---@field selectAll? boolean
---@field textColor? integer
---@field fontSize? number
---@field backgroundColor? integer

---@class SnowDesktopIcon
---@field path? string
---@field title? string

---@class SnowResourceStatus
---@field state SnowResourceState
---@field type 'image'|'font'
---@field name string

---@class SnowTextMetrics
---@field width number
---@field height number

---@class snow.widget
widget = {}

---Validate and freeze an API v2 immediate-render definition.
---@param definition SnowWidgetDefinition
---@return SnowWidgetDefinition
function widget.define(definition) end

---@return SnowApiInfo
function widget.apiInfo() end

---@param feature string
---@return boolean
function widget.hasFeature(feature) end

---@return SnowWidgetContext
function widget.context() end

---@return SnowWidgetInfo
function widget.info() end

---@return SnowWidgetTheme
function widget.theme() end

---@param permission string
---@return boolean
function widget.hasPermission(permission) end

---@param title string
function widget.setTitle(title) end

function widget.openSettings() end

---@param options? SnowWidgetPanelOptions
function widget.openPanel(options) end

function widget.closePanel() end

function widget.invalidate() end

---@param level 'debug'|'info'|'warn'|'error'
---@param message string
function widget.log(level, message) end

---@param name string
---@param intervalMilliseconds integer
---@param repeatTimer? boolean
---@return boolean
function widget.setTimer(name, intervalMilliseconds, repeatTimer) end

---@param name string
---@return boolean
function widget.cancelTimer(name) end

---@class snow.schedule
schedule = {}

---@class SnowScheduleOptions
---@field whenHidden? 'pause'|'throttle'|'continue'

---Create or replace a coalescing repeating schedule.
---@param id string
---@param milliseconds integer
---@param options? SnowScheduleOptions
---@return boolean
function schedule.every(id, milliseconds, options) end

---Create or replace a one-shot schedule.
---@param id string
---@param milliseconds integer
---@param options? SnowScheduleOptions
---@return boolean
function schedule.after(id, milliseconds, options) end

---@param id string
---@return boolean
function schedule.cancel(id) end

---@alias SnowDataHiddenPolicy 'pause'|'throttle'|'continue'

---@class SnowDataSubscribeOptions
---@field maxAgeMs? integer Requested sampling interval and stale threshold.
---@field whenHidden? SnowDataHiddenPolicy

---@class SnowCpuDataValue
---@field usagePercent number
---@field logicalProcessors integer
---@field name string

---@class SnowMemoryDataValue
---@field totalBytes integer
---@field usedBytes integer
---@field freeBytes integer
---@field usagePercent number

---@class SnowGpuAdapterDataValue
---@field id string Opaque adapter identifier.
---@field name string
---@field usagePercent number
---@field dedicatedMemoryBytes integer
---@field dedicatedUsedBytes integer
---@field sharedMemoryBytes integer
---@field sharedUsedBytes integer

---@class SnowGpuDataValue
---@field adapters SnowGpuAdapterDataValue[]

---@class SnowPowerDataValue
---@field acPower boolean
---@field charging boolean
---@field saver boolean
---@field batteryPercent number
---@field estimatedRemainingSeconds? integer

---@class SnowNetworkStatusDataValue
---@field connectivity 'none'|'local'|'internet'
---@field transport 'none'|'ethernet'|'wifi'|'cellular'|'other'
---@field costKnown boolean
---@field metered boolean
---@field roaming boolean
---@field overLimit boolean

---@class SnowNetworkTrafficDataValue
---@field connected boolean
---@field receivedBytes integer
---@field sentBytes integer
---@field downloadBytesPerSecond integer
---@field uploadBytesPerSecond integer

---@class SnowStorageVolumeDataValue
---@field id string Opaque volume identifier; never a filesystem path.
---@field displayName string
---@field mountPoint string Display-only mount point such as C:\\.
---@field kind 'fixed'|'removable'|'network'|'optical'|'ramdisk'|'unknown'
---@field capacityBytes integer
---@field freeBytes integer Free bytes available to the current user.
---@field capacityAvailable boolean False when capacity lookup is intentionally skipped for a remote or unready volume.
---@field removable boolean
---@field readOnly boolean

---@class SnowStorageVolumesDataValue
---@field volumes SnowStorageVolumeDataValue[]

---@class SnowStorageIoDataValue
---@field readBytesPerSecond integer Aggregate physical-disk read rate.
---@field writeBytesPerSecond integer Aggregate physical-disk write rate.
---@field busyPercent number Aggregate physical-disk busy percentage, clamped to 0..100.

---@class SnowDisplayRect
---@field x number
---@field y number
---@field width number
---@field height number

---@class SnowDisplayPixelRect
---@field x integer
---@field y integer
---@field width integer
---@field height integer

---@class SnowDisplayDataValue
---@field id string Opaque display identifier.
---@field name string User-visible display name when Windows provides one.
---@field primary boolean
---@field bounds SnowDisplayRect Logical bounds.
---@field workArea SnowDisplayRect Logical work area.
---@field pixelBounds SnowDisplayPixelRect Physical-pixel bounds.
---@field pixelWorkArea SnowDisplayPixelRect Physical-pixel work area.
---@field dpiX integer
---@field dpiY integer
---@field scale number Effective DPI divided by 96.
---@field refreshHz number Zero when unavailable.
---@field orientation 'landscape'|'portrait'|'landscapeFlipped'|'portraitFlipped'|'unknown'
---@field hdrKnown boolean
---@field hdrSupported boolean
---@field hdrEnabled boolean

---@class SnowDisplayTopologyDataValue
---@field displays SnowDisplayDataValue[]

---@class SnowDisplayCurrentDataValue
---@field display SnowDisplayDataValue Display containing this widget surface.

---@class SnowAudioOutputDefaultDataValue
---@field id string Opaque default render endpoint identifier.
---@field name string User-visible endpoint name when Windows provides one.
---@field state 'active'|'disabled'|'unplugged'|'notPresent'|'unknown'

---@class SnowAudioOutputVolumeDataValue
---@field endpointId string Opaque endpoint identifier matching audio.output.default.
---@field volume number Master volume scalar in the minimum..maximum range.
---@field muted boolean
---@field minimum number Currently 0.0.
---@field maximum number Currently 1.0.

---@class SnowAudioOutputAnalysisDataValue
---@field waveform number[] 128 normalized mono points in -1.0..1.0.
---@field spectrum number[] 64 normalized magnitude bins in 0.0..1.0.
---@field rms number Normalized RMS level in 0.0..1.0.
---@field peak number Normalized peak level in 0.0..1.0.
---@field silent boolean
---@field deviceChanged boolean
---@field endpointId string Opaque endpoint identifier.
---@field sampleRate integer
---@field channels integer Source mix channel count; waveform is always downmixed.

---@class SnowMediaControlsDataValue
---@field canPlay boolean
---@field canPause boolean
---@field canPlayPause boolean
---@field canStop boolean
---@field canNext boolean
---@field canPrevious boolean
---@field canSeek boolean
---@field canChangePlaybackRate boolean
---@field canToggleShuffle boolean
---@field canChangeRepeatMode boolean

---@class SnowMediaTimelineValue
---@field sessionId string Opaque media session identifier.
---@field positionMs integer Position relative to the session start.
---@field durationMs integer Bounded non-negative duration.
---@field minimumSeekMs integer
---@field maximumSeekMs integer
---@field updatedAtMs integer Epoch milliseconds reported by Windows.

---@class SnowMediaSessionValue
---@field id string Opaque media session identifier.
---@field sourceName string Bounded source display identity supplied by Windows.
---@field title string
---@field artist string
---@field album string
---@field playbackStatus 'closed'|'open'|'changing'|'stopped'|'playing'|'paused'
---@field current boolean
---@field controls SnowMediaControlsDataValue
---@field timeline SnowMediaTimelineValue

---@class SnowMediaSessionsDataValue
---@field currentSessionId string Empty when Windows has no current session.
---@field sessions SnowMediaSessionValue[] At most 32 entries.

---@class SnowMediaCurrentDataValue
---@field session SnowMediaSessionValue

---@class SnowMediaTimelineDataValue
---@field timeline SnowMediaTimelineValue

---@class SnowDesktopItemDataValue
---@field id string Stable host reference; never an absolute path.
---@field title string
---@field source string
---@field type string
---@field selected boolean

---@class SnowDesktopItemsDataValue
---@field items SnowDesktopItemDataValue[] At most 2048 entries.
---@field revision integer

---@class SnowDesktopSelectionDataValue
---@field items SnowDesktopItemDataValue[] At most 512 entries.
---@field revision integer

---@class SnowDesktopChangesDataValue
---@field revision integer
---@field reason string Bounded host change reason.

---@class SnowDataSnapshot<T>
---@field available boolean
---@field value? T
---@field timestamp integer Epoch milliseconds, or zero before a sample exists.
---@field stale boolean
---@field warmingUp boolean
---@field error? string

---@class SnowDataSubscription<T>
---@field value fun(self: SnowDataSubscription<T>): SnowDataSnapshot<T>
---@field unsubscribe fun(self: SnowDataSubscription<T>): boolean

---@class snow.data
data = {}

---Subscribe to a host-shared on-demand data topic.
---@overload fun(topic: 'system.cpu', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowCpuDataValue>
---@overload fun(topic: 'system.memory', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowMemoryDataValue>
---@overload fun(topic: 'system.gpu', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowGpuDataValue>
---@overload fun(topic: 'system.power', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowPowerDataValue>
---@overload fun(topic: 'system.network.status', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowNetworkStatusDataValue>
---@overload fun(topic: 'system.network.traffic', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowNetworkTrafficDataValue>
---@overload fun(topic: 'system.storage.volumes', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowStorageVolumesDataValue>
---@overload fun(topic: 'system.storage.io', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowStorageIoDataValue>
---@overload fun(topic: 'system.display.topology', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowDisplayTopologyDataValue>
---@overload fun(topic: 'system.display.current', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowDisplayCurrentDataValue>
---@overload fun(topic: 'audio.output.default', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowAudioOutputDefaultDataValue>
---@overload fun(topic: 'audio.output.volume', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowAudioOutputVolumeDataValue>
---@overload fun(topic: 'audio.output.analysis', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowAudioOutputAnalysisDataValue>
---@overload fun(topic: 'media.sessions', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowMediaSessionsDataValue>
---@overload fun(topic: 'media.current', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowMediaCurrentDataValue>
---@overload fun(topic: 'media.timeline', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowMediaTimelineDataValue>
---@overload fun(topic: 'desktop.items', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowDesktopItemsDataValue>
---@overload fun(topic: 'desktop.selection', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowDesktopSelectionDataValue>
---@overload fun(topic: 'desktop.changes', options?: SnowDataSubscribeOptions): SnowDataSubscription<SnowDesktopChangesDataValue>
---@param topic string
---@param options? SnowDataSubscribeOptions
---@return SnowDataSubscription<table>
function data.subscribe(topic, options) end

---Compatibility editor retained by the host; new v2 widgets should wait for
---the declarative control tree instead of building new interaction on it.
---@param storageKey string
---@param x number
---@param y number
---@param width number
---@param height number
---@param multiline? boolean
---@param initialValue? string
---@param selectAll? boolean
---@param textColor? integer
---@param fontSize? number
---@param backgroundColor? integer
function widget.editText(storageKey, x, y, width, height, multiline, initialValue, selectAll, textColor, fontSize, backgroundColor) end

---@class snow.system
system = {}

---@return SnowSystemInfo
function system.info() end

---@overload fun(feature: string): SnowCapability
---@return SnowCapabilities
function system.capabilities() end

---@return SnowSystemUptime
function system.uptime() end

---@class snow.time
time = {}

---@return integer epochMilliseconds
function time.now() end

---@return integer monotonicMilliseconds
function time.monotonic() end

---@param epochMilliseconds? integer
---@param timeZone? 'local'|'utc'|string
---@return SnowDateTimeParts
function time.parts(epochMilliseconds, timeZone) end

---@param epochMilliseconds? integer
---@param options? SnowTimeFormatOptions
---@return string
function time.format(epochMilliseconds, options) end

---@param epochMilliseconds integer
---@param delta SnowTimeDelta
---@param options? SnowTimeZoneOptions
---@return integer
function time.add(epochMilliseconds, delta, options) end

---@param left integer
---@param right integer
---@return -1|0|1
function time.compare(left, right) end

---@class snow.l10n
l10n = {}

---@param key string
---@param ... string|number|boolean
---@return string
function l10n.tr(key, ...) end

---@return string
function l10n.language() end

---@param value number
---@param options? SnowNumberFormatOptions
---@return string
function l10n.formatNumber(value, options) end

---@param bytes number
---@param options? SnowBytesFormatOptions
---@return string
function l10n.formatBytes(bytes, options) end

---@param milliseconds integer
---@param options? SnowDurationFormatOptions
---@return string
function l10n.formatDuration(milliseconds, options) end

---@param deltaMilliseconds integer
---@param options? SnowRelativeTimeFormatOptions
---@return string
function l10n.formatRelativeTime(deltaMilliseconds, options) end

---@param values string[]
---@param options? SnowLocaleOptions
---@return string
function l10n.formatList(values, options) end

---@class snow.module
module = {}

---Load one package-relative .lua module during entry evaluation.
---@generic T
---@param path string
---@return T
function module.require(path) end

---@class snow.resource
resource = {}

---@param name string
---@return boolean
function resource.exists(name) end

---Create a declared image handle during entry evaluation.
---@param name string
---@return SnowImageResource
function resource.image(name) end

---Create a declared package-private font handle during entry evaluation.
---@param name string
---@return SnowFontResource
function resource.font(name) end

---@param handle SnowImageResource|SnowFontResource
---@return SnowResourceStatus
function resource.status(handle) end

---@class snow.draw
draw = {}

---@param x number
---@param y number
---@param text string
---@param size? number
---@param color? integer
---@param maxWidth? number
---@param bold? boolean
---@param singleLine? boolean
---@param maxHeight? number
---@param alpha? number
---@param font? SnowFontResource
function draw.text(x, y, text, size, color, maxWidth, bold, singleLine, maxHeight, alpha, font) end

---@param text string
---@param size? number
---@param maxWidth? number
---@param bold? boolean
---@param font? SnowFontResource
---@return SnowTextMetrics
function draw.measureText(text, size, maxWidth, bold, font) end

---@param image SnowImageResource
---@param x number
---@param y number
---@param width number
---@param height number
---@param alpha? number
function draw.image(image, x, y, width, height, alpha) end

---@param x number
---@param y number
---@param width number
---@param height number
---@param color? integer
---@param radius? number
---@param alpha? number
function draw.rect(x, y, width, height, color, radius, alpha) end

---@param x number
---@param y number
---@param width number
---@param height number
---@param color? integer
---@param radius? number
---@param thickness? number
---@param alpha? number
function draw.strokeRect(x, y, width, height, color, radius, thickness, alpha) end

---@param x number
---@param y number
---@param width number
---@param height number
function draw.pushClip(x, y, width, height) end

function draw.popClip() end

---@param x1 number
---@param y1 number
---@param x2 number
---@param y2 number
---@param thickness? number
---@param color? integer
---@param alpha? number
function draw.line(x1, y1, x2, y2, thickness, color, alpha) end

---@param centerX number
---@param centerY number
---@param radius number
---@param color? integer
---@param alpha? number
function draw.circle(centerX, centerY, radius, color, alpha) end

---@param glyph string
---@param x number
---@param y number
---@param size? number
---@param color? integer
function draw.fa(glyph, x, y, size, color) end

---@param glyph string
---@param x number
---@param y number
---@param size? number
---@param color? integer
function draw.fluent(glyph, x, y, size, color) end

---Requires desktop.read. This compatibility call is present in the current
---v2 draw table, but desktop query APIs are not exposed to the v2 sandbox yet.
---@param itemOrPath SnowDesktopIcon|string
---@param x number
---@param y number
---@param size? number
---@param alpha? number
function draw.icon(itemOrPath, x, y, size, alpha) end

---@class snow.layout
layout = {}

---@return number
function layout.width() end

---@return number
function layout.height() end

---@return integer
function layout.columns() end

---@return integer
function layout.rows() end

---@return SnowWidgetSizeClass
function layout.sizeClass() end

---@return integer
function layout.cellWidth() end

---@return integer
function layout.cellHeight() end

---@return number
function layout.cellScale() end

---@param value number
---@return integer
function layout.cu(value) end

---@param value number
---@return number
function layout.fontCu(value) end

---@return integer
function layout.cellGap() end

---@return integer
function layout.barHeight() end

---@class snow.storage
storage = {}

---@param key string
---@return string?
function storage.get(key) end

---@param key string
---@param value string
function storage.set(key, value) end

---@param key string
function storage.remove(key) end

---@return string[]
function storage.keys() end

---@class snow.state
state = {}

---Return a deep copy of transient instance state or the supplied default.
---@generic T: SnowStateValue
---@param key string
---@param default? T
---@return T?
function state.get(key, default) end

---Store a deep copy. Returns false when the value is unchanged.
---@param key string
---@param value SnowStateValue
---@return boolean changed
function state.set(key, value) end

---@param key string
---@return boolean changed
function state.remove(key) end

---@param key string
---@return boolean
function state.has(key) end

---@return string[]
function state.keys() end

---@return boolean changed
function state.clear() end

---@type string
widgetId = ''
