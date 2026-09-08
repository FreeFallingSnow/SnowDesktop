# SnowDesktop

[简体中文](./README.md) | [English](./README.en.md)

Keep your Windows desktop tidy and make it your own. Automatically group desktop files by type, keep apps and folders together, and use Dock to launch apps and switch windows. Add useful widgets, glass themes, and a separate layout for each monitor to suit the way you work.

## 📦 Installation

<p>
  <a href="https://store.steampowered.com/app/5080330/SnowDesktop/"><img src="https://img.shields.io/badge/Steam-171A21?style=for-the-badge&amp;logo=steam&amp;logoColor=white" alt="Steam Store" width="137" height="44"></a>
  &nbsp;
  <a href="https://apps.microsoft.com/detail/9PLLGJVL4LC3"><img src="https://get.microsoft.com/images/en-us%20dark.svg" alt="Get it from Microsoft" height="44"></a>
</p>

[Official website](https://snowdesktop.com/) | [GitHub](https://github.com/FreeFallingSnow/SnowDesktop)

## 📊 Edition Comparison

Both editions provide the full core SnowDesktop experience.

| Feature | Microsoft Store | Steam |
| --- | --- | --- |
| Desktop organization | ✓ Included | ✓ Included |
| Dock and quick navigation | ✓ Included | ✓ Included |
| Built-in widgets | ✓ Included | ✓ Included |
| Steam Workshop | — Not included | ✓ Included |
| Premium features | — Not included | ✓ Included |
| Updates and fixes | Published with each version update | More timely updates, optional testing channels |

With the Steam edition, you can find and install community widgets through Steam Workshop. The currently supported premium feature is **large app icons**.

See the [official edition comparison](https://snowdesktop.com/compare/) for details.

## ✨ Feature Highlights

- 🖥️ **Give every display its own layout**: Arrange different content on each display and set the grid rows and columns for each page. Adjust icon spacing and widget positions to suit your needs. When you need more room, add pages and flip through them to reach more content.
- 🗂️ **Put files and shortcuts where they truly belong**:
  - **Collections**: Group frequently used apps and shortcuts by purpose, so your desktop tools are easy to find.
  - **Collection groups**: Switch between collections with tabs, keeping tools for work, study, or entertainment in the same desktop area.
  - **Desktop file categories**: Automatically group desktop files by type, making documents, images, and other content easier to find.
  - **Folder mapping**: Show the contents of frequently used folders on your desktop, so you can view and manage files without repeatedly opening folder windows.
  - **File groups**: Bring desktop files and several folders together, and search across them to find the files you need.
- 🚀 **Reach applications and windows from one place**: Dock can attach to any edge of the screen, or appear as a floating Dock only when needed. Add apps, folder stacks, and collections; see running programs; and use window previews to activate, minimize, or close windows.
- 🔎 **Quick Navigation**: Quick Navigation finds desktop items and installed apps by name. Install and run Everything on your PC to also search the files it has indexed.
- 🧩 **Use practical widgets on the desktop**:
  - **Built-in widgets**: SnowDesktop includes analog and digital clocks, a monthly calendar, schedules, reminders, system monitoring, media controls, notes, a Pomodoro timer, an RSS reader, quick launchers, and more.
  - **Widget management and Lua extensions**: Install widgets from local packages, enable or disable them, apply updates, and switch back to a retained older version. You can also write your own widgets in Lua.
- 🎨 **Personalization**:
  - **Themes and styles**: Choose light or dark themes, and adjust widget and Dock colors, transparency, rounded corners, and backgrounds with glass or acrylic effects.
  - **Windows 11 taskbar**: On Windows 11, customize the system taskbar with transparent, dark, light, or glass styles and adjust the light or dark appearance of its icons and text. Set rules to change its look automatically: keep it transparent until an app window is visible or maximized, for example, or apply a chosen style when Start, Search, or Task View opens. App window rules are evaluated separately for each display.
- 💾 **Backup and migration**: Back up and restore SnowDesktop layouts, settings, widget packages, and widget data.

## 🛠️ Build

Requirements: CMake 3.24+, Visual Studio 2022 with the Desktop development
with C++ workload, and Windows 10/11 SDK 10.0.19041.0 or newer. NuGet restores
the pinned Microsoft Windows App SDK 2.4.0 and
Microsoft.Windows.CppWinRT 3.0.260818.1 packages. Neither development nor
target machines need a preinstalled Windows App SDK Runtime.

```bat
.\scripts\build.bat
```

The default build does not stop SnowDesktop or restart Explorer. Its preflight
stops before compilation when the app or hook DLL is active. Exit SnowDesktop
normally first. To clear the lock automatically, use
`.\scripts\build.bat --reload-shell`; it stops SnowDesktop and briefly restarts Explorer.

## 🧱 Technology

- C++20 / MSVC
- WinUI 3 / Windows App SDK 2.4.0 (settings center in a Win32 XAML Island)
- Direct2D + Direct3D 11 + DirectComposition
- Dear ImGui (Workshop Manager only)
- Lua 5.4 (script engine)
- Fluent System Icons Regular (modern context-menu and widget-menu icons)
- Font Awesome 6 Free (backward-compatible widget icons)
- WinHTTP (Lua HTTP runtime)

## 🤝 Contributing

**The project is under rapid development and is temporarily not accepting external pull requests for code integration.**
Feedback is welcome through [Issues](https://github.com/FreeFallingSnow/SnowDesktop/issues),
the [QQ user group 976422547](https://qm.qq.com/q/HyazkCIRig), or by participating in
[Steam test builds](./CONTRIBUTING.en.md#join-the-steam-test-branch).
See the contribution guide in [English](./CONTRIBUTING.en.md) or [中文](./CONTRIBUTING.md)
for participation options, development rules, and licensing information.

## 📄 License

The SnowDesktop core is licensed under GNU General Public License v3.0; see
[LICENSE](./LICENSE). Copyright and license information for third-party
components included in this repository is collected in
[THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md).
Release packages include the complete license and notice files for distributed
third-party components in the `licenses/` directory.

The separate `steam_bridge/` Workshop bridge is MIT-licensed under
[steam_bridge/LICENSE](./steam_bridge/LICENSE). The Steamworks SDK is not
included in this repository and is not covered by either license.
