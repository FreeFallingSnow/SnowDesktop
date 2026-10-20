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
- WinHTTP (Lua HTTP runtime)

## 🧩 Third-Party Code and Assets

The entries below are grouped by their use in SnowDesktop. Local links describe their provenance and integration; complete copyright and license notices are in [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md).

### Libraries, SDKs and Runtimes

| Project | Use | License |
| --- | --- | --- |
| [Everything SDK](third_party/everything/README.md) | Everything file-index queries in quick navigation | MIT |
| [Dear ImGui](third_party/imgui/README.md) | Workshop Manager interface | MIT |
| [Lua](third_party/lua/README.md) | Widget script engine and authoring tools | MIT |
| [MinHook](third_party/minhook/README.md) | Taskbar and Wallpaper Engine capture hooks | BSD 2-Clause |
| [Windows App SDK](https://github.com/microsoft/WindowsAppSDK) | WinUI 3 settings interface and private application runtime | Microsoft Software License Terms |
| [Windows ML](https://learn.microsoft.com/windows/ai/new-windows-ml/) | Runtime dependency deployed with Windows App SDK | Microsoft Software License Terms |
| [WebView2 SDK](https://developer.microsoft.com/microsoft-edge/webview2/) | Dependency in the Windows App SDK deployment manifest | BSD 3-Clause |
| [C++/WinRT](https://github.com/microsoft/cppwinrt) | WinRT interface projections for the build | MIT |

### Data

| Project | Use | License |
| --- | --- | --- |
| [pinyin-data](third_party/pinyin-data/README.md) | Full-pinyin and initial-letter matching for Chinese names | MIT |

### Fonts, Icons and Textures

| Project | Use | License |
| --- | --- | --- |
| [MiSans Regular](third_party/misans/README.md) | Optional interface font | MiSans font license agreement |
| [HarmonyOS Sans SC](third_party/harmonyos-sans/README.md) | Optional interface font | HarmonyOS Sans Fonts License Agreement |
| [Fluent System Icons](third_party/fluentui-system-icons/README.md) | Menu, status-bar and control-center icons | MIT |
| [Font Awesome Free Solid](third_party/font-awesome/README.md) | Backward-compatible widget icon font | SIL OFL 1.1 |
| [WinUI acrylic noise](third_party/microsoft-ui-xaml/README.md) | Noise texture for glass materials | MIT |

### Referenced and Adapted Projects

| Project | Reuse | License |
| --- | --- | --- |
| [YASB](third_party/yasb/README.md) | Tray protocol, callbacks and panel organization; Bluetooth, power-mode and notification-state references | MIT |
| [TranslucentTB](THIRD_PARTY_NOTICES.md#translucenttb-derived-portions) | Portions of taskbar integration and desktop backdrop composition | GPL-3.0-only |
| [DeskMakeover](THIRD_PARTY_NOTICES.md#deskmakeover-derived-shape-geometry) | Icon shape control points and continuous corner geometry | MIT |
| [OpenHarmony GraphicsEffect](third_party/graphics-effect/README.md) | Glass edge-light, fan-mask and inner-shadow calculations | Apache-2.0 |

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
