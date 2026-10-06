# Third-Party Notices

SnowDesktop includes or redistributes the following third-party software and
assets. These notices apply only to the named components. The SnowDesktop core
is licensed under the GNU General Public License v3.0; the separate Steam bridge
under `steam_bridge/` is licensed under the MIT License.

## Corresponding source / 对应源码

SnowDesktop's corresponding source, including its modifications to third-party
code and the scripts needed to build the application, is maintained in the
official repository and can be obtained free of charge:

<https://github.com/FreeFallingSnow/SnowDesktop>

Build instructions and pinned dependency versions are provided in `README.md`,
`README.en.md`, `CMakeLists.txt`, `CMakePresets.json`, and `scripts/README.md`
in that repository.

SnowDesktop 的对应源码（包括对第三方代码的修改及构建应用所需的脚本）可从上述官方
仓库免费获取。构建说明和固定依赖版本见仓库中的 `README.md`、`README.en.md`、
`CMakeLists.txt`、`CMakePresets.json` 和 `scripts/README.md`。

The upstream project links below identify third-party origins. SnowDesktop's
changes to those components are included in the corresponding source above.
The separately obtained Steamworks SDK remains subject to Valve's terms, as
described under "Optional Steamworks dependency" below.

下列上游项目链接用于说明第三方来源；SnowDesktop 对相关代码的修改包含在上述对应
源码中。另行获取的 Steamworks SDK 仍受 Valve 的条款约束，详见下文
“Optional Steamworks dependency”。

## Third-party components

| Component | Version | License | Copyright / source |
| --- | --- | --- | --- |
| Microsoft Windows App SDK | 2.4.0 NuGet package | Microsoft Software License Terms | Copyright (c) Microsoft Corporation; <https://github.com/microsoft/WindowsAppSDK> |
| Microsoft Windows ML Runtime | 2.1.74 NuGet package | Microsoft Software License Terms | Copyright (c) Microsoft Corporation; <https://learn.microsoft.com/windows/ai/new-windows-ml/> |
| Microsoft Edge WebView2 SDK | 1.0.3719.77 NuGet package | BSD 3-Clause | Copyright (c) Microsoft Corporation; <https://developer.microsoft.com/microsoft-edge/webview2/> |
| Microsoft.Windows.CppWinRT | 3.0.260818.1 NuGet package | MIT | Copyright (c) Microsoft Corporation; <https://github.com/microsoft/cppwinrt> |
| Dear ImGui | 1.92.5 WIP | MIT | Copyright (c) 2014-2025 Omar Cornut and contributors; <https://github.com/ocornut/imgui> |
| Lua | 5.4.7 | MIT | Copyright (C) 1994-2024 Lua.org, PUC-Rio; <https://www.lua.org> |
| Everything SDK client | bundled source | MIT | Copyright (C) 2016, 2022 David Carpenter; <https://www.voidtools.com/support/everything/sdk/> |
| pinyin-data | bundled data | MIT | Copyright (c) 2016 mozillazg; <https://github.com/mozillazg/pinyin-data> |
| MinHook | bundled source | BSD 2-Clause | Copyright (c) 2009-2017 Tsuda Kageyu; <https://github.com/TsudaKageyu/minhook> |
| Font Awesome 6 Free Solid | 6.5.2 font | SIL Open Font License 1.1 | Copyright (c) Font Awesome; <https://fontawesome.com/license/free> |
| Fluent System Icons Regular | upstream commit `21d5d02f724be2aaf586564775fff73a18a76eb6` | MIT | Copyright (c) 2020 Microsoft Corporation; <https://github.com/microsoft/fluentui-system-icons> |
| Microsoft WinUI acrylic noise texture | upstream commit `7b68d3e0b771a57d80098799406234efee479517` | MIT | Copyright (c) Microsoft Corporation; <https://github.com/microsoft/microsoft-ui-xaml> |
| MiSans Regular | 4.009 original OTF | MiSans font intellectual property license | Copyright © 2020-2025 Beijing Xiaomi Mobile Software Co., Ltd.; <https://hyperos.mi.com/font/zh/> |
| HarmonyOS Sans SC | 1.0 original Regular TTF | HarmonyOS Sans Fonts License Agreement | Copyright 2021 Huawei Device Co., Ltd.; <https://developer.huawei.com/consumer/cn/design/resource/> |
| DeskMakeover shape catalog | upstream `main` as referenced in 2026 | MIT | Copyright (c) 2026 Jinming Yang; <https://github.com/nicepkg/deskmakeover> |
| OpenHarmony GraphicsEffect adapted rim calculations | upstream commit `da8e11652a705ea2141c35de1a1fff501148740e` | Apache-2.0 | Copyright (c) 2025-2026 Huawei Device Co., Ltd.; <https://github.com/openharmony/graphic_graphics_effect> |
| TranslucentTB-derived portions | upstream commit `322e2b7395a51975150126276308b415970e080b` | GPL-3.0-only | Copyright (c) TranslucentTB contributors; <https://github.com/TranslucentTB/TranslucentTB/tree/322e2b7395a51975150126276308b415970e080b> |

The complete Everything SDK, Dear ImGui, Lua, pinyin-data, Font Awesome,
Microsoft Fluent System Icons and MinHook license texts are retained under
their corresponding `third_party/` directories. All are copied into the
`licenses` directory of release packages. The Font Awesome font is distributed
under the SIL Open Font License 1.1 with the reserved font name
"Font Awesome".

The Microsoft Windows App SDK runtime is redistributed self-contained under
the license terms and third-party NOTICE installed by the pinned 2.4.0 NuGet
package. The build records those files in `SnowDesktop.deployment.json`, and all
release packagers copy them to the payload `licenses` directory. The pinned
Windows ML 2.1.74 and WebView2 1.0.3719.77 licenses and third-party notices are
copied the same way, along with the Microsoft.Windows.CppWinRT 3.0.260818.1 MIT
license.

## Optional interface fonts / 可选界面字体

SnowDesktop uses MiSans and HarmonyOS Sans as optional interface fonts. They are
separate licensed assets, not GPL-licensed SnowDesktop code. Their original OTF/TTF
files are bundled unchanged with SnowDesktop; no glyph subsetting, conversion,
or font redevelopment is performed. The full agreements are retained in
`third_party/misans/LICENSE.pdf` and `third_party/harmonyos-sans/LICENSE.txt` and
copied into release payloads as `MiSans-LICENSE.pdf` and
`HarmonyOS-Sans-LICENSE.txt`. These fonts may not be redistributed as standalone
font products. The font settings and About page identify their use and owners.

SnowDesktop 使用 MiSans 与 HarmonyOS Sans 作为可选界面字体，字体资源分别适用
自己的许可，不适用 SnowDesktop 源码的 GPL 许可。仅随应用分发原始 OTF/TTF 文件，
不进行字形裁剪、转换或字体再开发；保留完整协议并随发行包交付。禁止将这些字体
作为独立字体产品再次分发。字体设置与关于页面标明使用的字体和权利人。

## YASB references and adapted portions

YASB references and adapted portions are documented separately in
[`third_party/yasb/README.md`](third_party/yasb/README.md), pinned to
`d6d1e6d553b0aac34fd5fb34928d3ca82b8d055f`. Copyright (c) 2024 amnweb and
Copyright (c) 2021 denBot. Its MIT notice is retained in
[`third_party/yasb/LICENSE`](third_party/yasb/LICENSE) and distributed as
`YASB-LICENSE.txt`. The adapted private tray wire structures and Bluetooth
control approach are identified there; SnowDesktop's IPC, task service and
native presentation are separate implementations. The About page links to
the pinned upstream source.

## TranslucentTB-derived portions

SnowDesktop contains modified portions derived from TranslucentTB at upstream
commit `322e2b7395a51975150126276308b415970e080b`. The incorporated material is
copyright TranslucentTB contributors and remains licensed under GPL-3.0-only,
the same license used by the SnowDesktop core. SnowDesktop modified the material
in 2026 for its own taskbar integration and desktop backdrop implementation.

The affected files are:

- `src/taskbar_dynamic/ShellViewCoordinator.idl`
- `src/taskbar_hook/taskview_visibility.h`
- portions of `src/taskbar_hook/taskbar_hook.cpp`
- portions of `src/app/render/desktop_backdrop_compositor.cpp`

The complete GPL v3 license text is retained in the repository root `LICENSE`.
The upstream source and history remain available at the pinned TranslucentTB
commit linked above.

## DeskMakeover-derived shape geometry

`src/icons/icon_beautify.cpp` adapts normalized shape control points and continuous
corner geometry from DeskMakeover's `dm-icon-core` shape catalog. The upstream
visual-language reference is available at
<https://github.com/nicepkg/deskmakeover/blob/main/docs/specs/02-visual-language.md>
and the upstream project source is available at
<https://github.com/nicepkg/deskmakeover>.

DeskMakeover is distributed under the following MIT License:

Copyright (c) 2026 Jinming Yang

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## OpenHarmony GraphicsEffect-derived rim calculations

`src/theme/flat_glass_rim.h` contains modified CPU adaptations of the SDF edge-light,
opposing fan-mask and exponential inner-shadow calculations from OpenHarmony
GraphicsEffect at commit `da8e11652a705ea2141c35de1a1fff501148740e`.
Copyright (c) 2025-2026 Huawei Device Co., Ltd. SnowDesktop modified these
portions in 2026 for its existing opacity masks and chose its own visual
parameters. This is not an assertion that Honor MagicAnimation uses the same
implementation. The source paths and adaptation boundaries are documented in
`third_party/graphics-effect/README.md`. The Apache-2.0 license is reproduced
below so release packages containing this notices file retain the full text.

`src/theme/flat_glass_rim.h` 包含上述官方边缘光、对向扇区遮罩和指数内阴影计算的修改版。
SnowDesktop 于 2026 年将其移植到现有透明度遮罩并独立调校视觉参数，未声称与荣耀
MagicAnimation 使用相同实现。来源和移植边界见上述 README；完整许可证附于本文末尾。

## Optional Steamworks dependency

`steam_bridge/` can optionally be built against a separately obtained
Steamworks SDK. The SDK, its headers, import libraries, and redistributable
binary are not included in this repository and are not covered by either the
SnowDesktop GPL license or the bridge MIT license. See
`steam_bridge/THIRD_PARTY_NOTICES.md` for the distribution boundary.

## Apache License 2.0 — OpenHarmony GraphicsEffect

```text

                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS
```

## Microsoft WinUI acrylic noise texture

The original 256×256 noise PNG is embedded in the host and author preview.
Source and regeneration details: `third_party/microsoft-ui-xaml/README.md`.

    MIT License

    Copyright (c) Microsoft Corporation. All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE
