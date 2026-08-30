# SnowDesktop Steam Bridge

The Steam integration consists of two separate MIT-licensed executables and a
shared `SteamWorkshopCore`. Neither executable links against SnowDesktop or any
GPL-licensed SnowDesktop target. SnowDesktop remains fully usable without
Steam:

- `SnowDesktopSteamBridge.exe` is a headless JSON CLI for subscription
  consumption and automation;
- `SnowDesktopWorkshopManager.exe` is the permanent Dear ImGui Win32/DX11
  authoring application for validating, packaging, binding, creating, and
  updating Workshop components.

The shared core owns the Steamworks boundary for both roles:

- consumer commands enumerate subscriptions, query item metadata, monitor
  downloads, and return the Steam-managed install directory;
- creator operations list the current author's items and create or update a
  Workshop item from one validated `.snowwidget` artifact and a primary
  preview image.

The production Steam identity is App ID `5080330` with Windows depot
`5080331`. `packaging/steam-identity.json` is the repository source of truth.
Every SDK-enabled process verifies the runtime App ID before querying,
downloading, creating, or updating Workshop content; placeholder or foreign
App IDs fail closed.

SnowDesktop must never execute Lua directly from a Steam install directory.
The main application copies `package.snowwidget` into its normal staging area,
validates it, compares permissions and network domains, and only then activates
the package. See [PROTOCOL.md](PROTOCOL.md) for the command contract.

## SDK is deliberately not in this repository

The Steamworks SDK is available only through a Steamworks partner account. Do
not copy its archive, headers, libraries, tools, examples, or documentation into
this repository. Do not commit `steam_appid.txt`.

Extract an authorized SDK to a directory outside the SnowDesktop checkout. Set
the root that contains `public/steam/steam_api.h` either in the environment:

```powershell
$env:SNOWDESKTOP_STEAMWORKS_SDK_ROOT = `
  "C:\external\steamworks_sdk_165\sdk"
scripts\build.bat
```

or for one configure invocation:

```powershell
cmake --preset release `
  -DSNOWDESKTOP_STEAMWORKS_SDK_ROOT=C:\external\steamworks_sdk_165\sdk
cmake --build --preset release
```

CMake rejects an SDK root located inside the source tree. It reads the external
headers and x64 import library, then copies only Valve's `steam_api64.dll`
redistributable beside the built Manager and Bridge. A build with no SDK root
still produces both tools: the Manager retains local inspection and packing,
while Steam operations show an unavailable diagnostic; the Bridge returns exit
code 3.

Never ship `steam_appid.txt`. Local development may use it only as directed by
Valve, outside version control and outside release artifacts. Production
commands must run from the application's Steam launch context under the same
Windows user as the Steam client.

After an SDK-enabled Release build, use the guarded development launcher from
the repository root instead of creating the file manually:

```powershell
scripts\steam-dev.bat manager
scripts\steam-dev.bat bridge status
scripts\steam-dev.bat bridge workshop list-published --page 1
```

The launcher checks the compiled App/depot identity, creates
`.build\Release\steam_appid.txt` only while the selected process runs, and
removes only the file it created. The packaging pipeline independently rejects
the file. The Manager automatically connects to Steam and loads the current
author's first Workshop page at startup; Diagnostics exposes expected and
runtime App IDs for troubleshooting.

The Manager is intentionally limited to Steam Workshop work: selecting a local
project for upload, validating and packaging the upload, managing published
items, handling the Workshop agreement, and showing Steam diagnostics. The
The component development flow lives in SnowDesktop's **Component Developer Tools**
page. Built-in components are never imported as publishable projects; only
`data\widgets\dev` and explicitly added external component directories appear
in the local project list.

The Bridge and Manager intentionally do not call
`SteamAPI_RestartAppIfNecessary`: that API relaunches the product's configured
primary executable rather than the helper that called it. Steam distributions
inherit the launch context from `SnowDesktop.exe`; local helper development uses
the guarded launcher above.

Before testing uploads, enable ISteamUGC file transfer for the app and publish
a nonzero Steam Cloud byte/file quota for Workshop preview images in Steamworks
App Admin. Workshop visibility is configured and published separately there.

## Consumer commands

```powershell
SnowDesktopSteamBridge.exe configuration
SnowDesktopSteamBridge.exe status
SnowDesktopSteamBridge.exe workshop list-subscribed --details
SnowDesktopSteamBridge.exe workshop item-details --item 1234567890
SnowDesktopSteamBridge.exe workshop item-state --item 1234567890
SnowDesktopSteamBridge.exe workshop subscribe --item 1234567890
SnowDesktopSteamBridge.exe workshop unsubscribe --item 1234567890
SnowDesktopSteamBridge.exe workshop download --item 1234567890
SnowDesktopSteamBridge.exe workshop install-info --item 1234567890
SnowDesktopSteamBridge.exe workshop eula-status
```

Discovery, voting, comments, and moderation stay on the Steam Workshop website.
The explicit `subscribe` and `unsubscribe` commands are available to developer
tooling and agent automation. The resident SnowDesktop process reconciles
subscriptions from Steam's local `appworkshop_<AppId>.acf` cache and Workshop
content directories; its periodic check never starts SteamAPI or the Bridge, so
it does not repeatedly mark the application as running in Steam. The Bridge is
started only for an actual Steam operation or creator-identity verification.
Subscribing installs the validated package, updates follow the published package
version, and an item removed from the same Steam account's previous subscription
snapshot removes the managed package. Subscription history is stored per Steam
`ActiveUser`, so switching to a newly observed account cannot remove another
account's components, and a component remembered by any account remains local.
Layouts and per-instance storage are retained across unsubscription. Missing,
partially written, or in-progress cache states never trigger automatic removal.

## Publisher command

The supported graphical workflow is `SnowDesktopWorkshopManager.exe`. It keeps
its schema-v1 project library at
`<SnowDesktop data>\SteamWorkshopManager\projects.json` (and migrates the
former `%LOCALAPPDATA%` store once, recording completion under the new data
root so later launches do not scan the old directory), discovers the
directory supplied with `--development-root`, and runs the separate
`snowwidget.exe` process for authoritative validation and packaging. It never
stores a Steam password or token. Removing a project removes only the local
record. Manager package staging, Steam upload staging, previews, and the
standalone CLI's scratch files all stay below the SnowDesktop `data` directory;
they do not use the system temporary directory.

On the first launch after upgrading, an older
`%LOCALAPPDATA%\SnowDesktop\CreatorProjects` tree is preserved under
`<SnowDesktop data>\CreatorProjects`, while obsolete LocalAppData Hook copies
are removed. A marker below `<SnowDesktop data>\migrations` prevents later
launches from touching that legacy root again.

New items are created private. On update, title, description, and visibility
are left untouched; primary preview and tags change only when selected. After a
successful upload the Manager opens Steam Owner Controls for additional media,
visibility, contributors, legal terms, and deletion.

The CLI remains available for automation.

Validate and pack with the normal channel-independent tool first:

```powershell
snowwidget capabilities
snowwidget validate D:\widgets\my-widget
snowwidget pack D:\widgets\my-widget D:\out\my-widget.snowwidget
```

Create a private Workshop item:

```powershell
SnowDesktopSteamBridge.exe workshop publish `
  --package D:\out\my-widget.snowwidget `
  --preview D:\widgets\my-widget\workshop-preview.png `
  --title "My Widget" `
  --description "A SnowDesktop component" `
  --tag "Widget" `
  --visibility private `
  --open-page
```

Update the content of an existing item:

```powershell
SnowDesktopSteamBridge.exe workshop publish `
  --item 1234567890 `
  --package D:\out\my-widget.snowwidget `
  --change-note "Version 1.1.0"
```

Creation requires a title and a preview. New items default to private. Steam
requires Workshop previews to be smaller than 1 MiB. The bridge uploads a
folder containing exactly `package.snowwidget`; it does not reuse or copy the
GPL package validator. Consumers remain responsible for authoritative package
validation.

Before invoking the publish command, its UI or caller must show that submission
is subject to the [Steam Workshop terms of service](https://steamcommunity.com/sharedfiles/workshoplegalagreement).
The final result includes `needsLegalAgreement` and the Workshop community URL.
Authors must accept the Steam Workshop legal agreement before their item can be
publicly visible. Use `--open-page` to open that item in the default browser
after a successful upload.

## License boundary

- Everything under this directory is MIT-licensed unless explicitly stated.
- Steamworks SDK materials remain under Valve's terms and are not part of this
  repository's MIT grant.
- GPL or TranslucentTB-derived SnowDesktop implementation code must not be
  copied into this directory.
- Release the bridge with `SnowDesktopSteamBridge-LICENSE.txt` and
  `SnowDesktopSteamBridge-THIRD-PARTY-NOTICES.md`; CMake copies both beside the
  executable automatically.

## Author query and Steam distribution

List the current author's projects and statistics with:

```powershell
SnowDesktopSteamBridge.exe workshop list-published --page 1
```

Portable and MSIX packages intentionally omit Steamworks. Run
`scripts\package_steam.ps1` (or `scripts\release.bat package-steam`) for the
Steam-specific payload. The packaging check requires SDK-enabled Manager and
Bridge binaries, includes the only permitted SDK redistributable
`steam_api64.dll`, and rejects headers, import libraries, SDK directories, and
`steam_appid.txt`.
