# SnowDesktop Steam Bridge

`SnowDesktopSteamBridge.exe` is a separate MIT-licensed process for optional
Steam integration. It does not link against SnowDesktop or any GPL-licensed
SnowDesktop target. The intended integration boundary is a small, documented
command-line or local IPC protocol so that SnowDesktop remains fully usable
without Steam.

The initial executable provides:

- `--version` and `status` diagnostics;
- `workshop list-subscribed` for enumerating the current user's subscribed
  Workshop items when built with Steamworks;
- an SDK-free build mode so the public repository and normal builds do not
  require or redistribute the Steamworks SDK.

Publishing, updating, subscribing, downloading, IPC hosting, and achievement
commands will be added on this process boundary as their product behavior is
defined. Until those commands exist, this target is an integration foundation,
not a complete Workshop client.

## Build without Steamworks

The normal Release preset builds an SDK-free executable:

```powershell
scripts\build.bat
.build\Release\SnowDesktopSteamBridge.exe --version
.build\Release\SnowDesktopSteamBridge.exe status
```

`status` reports that Steamworks is not compiled in and returns exit code 3.

## Build with an external Steamworks SDK

Obtain the SDK through your Steamworks partner account and keep it outside this
repository. Point CMake to the SDK root containing `public/steam/steam_api.h`:

```powershell
cmake --preset release `
  -DSNOWDESKTOP_STEAMWORKS_SDK_ROOT=C:\path\outside\SnowDesktop\sdk
cmake --build --preset release
```

The build validates the required header, x64 import library, and redistributable
DLL. It copies only `steam_api64.dll` to the target directory. Never ship
`steam_appid.txt`; use it only for local development as directed by Valve.

Run Steam-enabled commands from the Steam launch context under the same Windows
user as the Steam client:

```powershell
SnowDesktopSteamBridge.exe status
SnowDesktopSteamBridge.exe workshop list-subscribed
```

## License boundary

- Everything under this directory is MIT-licensed unless explicitly stated.
- Steamworks SDK materials remain under Valve's terms and are not part of this
  repository's MIT grant.
- GPL or TranslucentTB-derived SnowDesktop implementation code must not be
  copied into this directory.
- Release the bridge with `SnowDesktopSteamBridge-LICENSE.txt` and
  `SnowDesktopSteamBridge-THIRD-PARTY-NOTICES.md`; CMake copies both beside the
  executable automatically.
