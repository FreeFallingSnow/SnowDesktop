# SnowDesktop

[简体中文](./README.md) | [English](./README.en.md)

**A desktop that looks good and works well.**

SnowDesktop is a Windows desktop organization and personalization app built around components. Use different components to organize apps and files and view your schedule, then combine them with Dock, Quick Navigation and appearance settings to make things easier to find, everyday actions more convenient, and the desktop more to your taste.

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
| Dock and Quick Navigation | ✓ Included | ✓ Included |
| Built-in components | ✓ Included | ✓ Included |
| Steam Workshop | — Not included | ✓ Included |
| Advanced features | — Not included | ✓ Included |
| Updates and fixes | Published with each version update | More timely updates, optional testing channels |

With the Steam edition, you can find and install community components through Steam Workshop. The currently supported advanced feature is **large software icons**.

See the [official edition comparison](https://snowdesktop.com/compare/) for details.

## ✨ Feature Highlights

### Keep your apps in order, however many you have

Use a Collection component to organize office, design, gaming and other apps by purpose, so you do not have to search through a screen full of icons. When you have more groups, use a Collection Group component to bring several Collections together. Switch between tabs to access different groups of apps in the same area of your desktop.

Use the Desktop Files component to display documents, images and other files by type. For folders you use often, Folder Mapping displays their contents directly on the desktop. View reference material and open documents without repeatedly navigating into folders; the files stay in their original locations.

### Everyday shortcuts, close at hand

Put frequently used apps and folders in Dock and dock it at the edge of the screen. When you need it, bring Dock to the foreground with a keyboard shortcut or an edge trigger to access your usual content above other windows.

Dock features smooth, lively animations that add movement to app launches and to minimizing and restoring windows.

### Type what you want to find

Use Quick Navigation to find apps, desktop items and organized files by name, without looking through icons one by one. Connect Everything to also search the local files it has indexed.

Besides finding apps and files, Quick Navigation lets you start web searches, find settings and perform simple calculations, reducing the steps needed to find an entry point or switch tools.

### Keep useful information in view

Use the Schedule, ToDo List and Sticky Note components to keep today's plans, tasks and temporary notes on your desktop, ready to see when you return to it.

You can also add clocks, Month Calendar, Media Controls, System Monitor and other components to check the time, control music or follow system activity. Combine and arrange them as needed, keeping only what you use.

### Practical, and pleasing to look at

Use Icon beautification to give app icons a consistent style, combining background colors, glass textures and color filters so icons from different apps work better with your wallpaper. For a more prominent layout, turn your favorite apps into Large software icons, choose their shapes or use your own images.

Adjust the colors, transparency and frosted glass effects of Collections, Dock, the Status bar and desktop components to coordinate the whole desktop. Save styles you like as a Theme, switch whenever you want, and import or export themes without adjusting everything again.

### Find more inspiration in Steam Workshop

Subscribe to community-made components and themes through Steam Workshop to add new tools and decorations to your desktop or try different looks.

If you have an idea of your own, AI can help turn it into a component. SnowDesktop provides a component creation Skill for use with AI coding tools that support Skills. AI can assist with describing requirements, generating the component, previewing and adjusting it, checking and packaging it, and publishing it to Workshop after your confirmation.

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
- [YASB](https://github.com/amnweb/yasb) (MIT; references for tray and system controls; see [pinned version and adaptation scope](third_party/yasb/README.md))
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
