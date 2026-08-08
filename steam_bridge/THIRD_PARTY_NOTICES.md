# SnowDesktop Steam Bridge third-party notices

The source files under `steam_bridge/` are licensed under the MIT License in
`steam_bridge/LICENSE`.

When `SNOWDESKTOP_STEAMWORKS_SDK_ROOT` is configured, the bridge includes
Steamworks SDK headers at build time, links the Steamworks import library, and
copies `steam_api64.dll` beside the executable. Those materials are proprietary
materials provided by Valve Corporation under the Steamworks SDK Access
Agreement. They are not licensed under MIT or GPL and are not included in this
repository.

The build configuration rejects a Steamworks SDK root located inside the
SnowDesktop source tree. `.gitignore` also excludes common SDK directory names,
Steam API binaries and headers, local user presets, and `steam_appid.txt` as a
defense against accidental inclusion. These safeguards do not grant any right
to redistribute SDK materials.

Only Steamworks files that Valve designates as redistributable may be shipped
with the built application. Do not publish the SDK, its source or header files,
its tools, or `steam_appid.txt` in a public repository or release package.

Steamworks documentation and terms:

- <https://partner.steamgames.com/doc/sdk/api>
- <https://partner.steamgames.com/documentation/sdk_access_agreement/>

`SnowDesktopWorkshopManager.exe` uses Dear ImGui and its official Win32 and
DirectX 11 backends. Dear ImGui is Copyright (c) 2014-2026 Omar Cornut and
contributors and is distributed under the MIT License. The corresponding
source and license are in `third_party/imgui/`.
