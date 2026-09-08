# Contributing

[简体中文](./CONTRIBUTING.md) | English

The official repository is [FreeFallingSnow/SnowDesktop](https://github.com/FreeFallingSnow/SnowDesktop).
This guide describes the contribution workflow. Automated agents must also follow the full rules in
[AGENTS.md](./AGENTS.md).

## How to participate now

**The project is currently under rapid development, and we are temporarily not accepting external pull requests for code integration.**
Help improve features, stability, translations, and documentation through these channels:

- [GitHub Issues](https://github.com/FreeFallingSnow/SnowDesktop/issues): Report bugs, suggest features, or point out translation and documentation errors.
- [QQ user group: 976422547](https://qm.qq.com/q/HyazkCIRig): Discuss your experience, ask questions, and share testing feedback.
- [Steam](https://store.steampowered.com/app/5080330/SnowDesktop/) test builds: Try new features and help verify problems; ask in the QQ group about joining.

The development rules below apply to current maintainer work and serve as a reference for when code
contributions reopen. They do not mean that external code PRs are currently accepted.

## Before you start

- Search existing issues and pull requests to avoid duplicate work. Discuss substantial features, architectural changes, or public API changes with the maintainers in an issue before implementation.
- For bug reports, include the SnowDesktop version, distribution channel, Windows version, reproduction steps, expected and actual results, and relevant logs or screenshots. Remove personal paths, credentials, and other private information before sharing.
- Keep each PR focused on one problem. Describe user-visible changes, known risks, and unfinished work; avoid unrelated refactoring or formatting.
- Discussions may be in Chinese or English. Treat participants respectfully and focus feedback on reproducible behavior and specific code.

## Branches and pull requests

1. Create your working branch from the official repository's active `release/vA.B.C.D` development branch. While external code contributions are paused, use the feedback channels above.
2. Target that version branch with your PR. `main` is the stable release branch and does not accept ordinary development PRs directly. If there is no suitable version branch, ask the maintainers to determine the target first.
3. `version.json` is the sole version source. Versions use `A.B.C.0`, with `A` in 1–65535 and `B` and `C` in 0–65535. Do not change the version unless an agreed version update is part of your contribution.
4. Use the [PR template](./.github/pull_request_template.md), link relevant issues, and record validation commands, results, reasons for omitted checks, and scenarios awaiting validation. Use a draft PR for unfinished work.
5. Preserve contributor commits and authorship when integrating into the version branch. Maintainer follow-up changes belong in separate commits; existing external commits are not rewritten merely to standardize their format.

After validation, maintainers publish the version branch to `main` through Squash and merge.
Local version integration and tagging use `scripts/squash_release_to_main.bat`. Remote publication is
a separate action requiring explicit confirmation after the maintainer tests local `main`.
Ordinary contribution PRs do not perform release operations or push to GitCode.

## Commit messages

New development commits on a version branch use this bilingual format, with Chinese and English
describing the same scope:

```text
<type>(<scope>): <Chinese summary> / <English summary>
```

The scope is optional. Types include `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`,
`ci`, `chore`, `try`, `verify`, and `revert`. Use a specific summary rather than “修复问题” or “fix bug”.
For nontrivial changes, include matching Chinese and English descriptions and actual validation:

```text
docs(contributing): 补充贡献流程 / Expand the contribution workflow

中文：
- 说明分支选择与验证要求

English:
- Explain branch selection and validation requirements

验证 / Validation:
- git diff --check（通过 / passed）
- 纯文档改动，未运行构建或测试 / Documentation only; build and tests not run
```

Use `feat`, `fix`, or `perf` only after the corresponding acceptance checks, reproduced-defect
validation, or performance measurements have been completed. If compilation passes but visual,
interaction, or compatibility validation remains pending, use `try`. Include both
“编译通过，待验证” and `build passed, validation pending` in its summary and describe the validation
scope and limitations in the body. Commit each separately successful code-build attempt before
starting another round of changes; do not commit intermediate states that fail compilation.
Record subsequent real-world validation in a separate `verify` commit with the original `try` hash,
steps, and actual result rather than rewriting earlier validation claims.
Widget-only changes use component checks and the corresponding `try(widget)` format described in
[AGENTS.md](./AGENTS.md).

## Building and validation

See the [README build instructions](./README.en.md) for prerequisites and
[scripts/README.md](./scripts/README.md) for script details. Run these entry points from the repository
root, selecting tests that match your change:

```bat
scripts\build.bat
scripts\test.bat list
scripts\test.bat name "<regex>"
scripts\test.bat label "<regex>"
scripts\test.bat core
scripts\test.bat fast
scripts\test.bat full
```

- Native changes require appropriate compilation. Final Release build validation must use `scripts/build.bat` and confirm that `.build/Release/SnowDesktop.exe` was generated. Direct CMake or Ninja commands are diagnostic tools, not substitutes for the standard entry point.
- Check for a running SnowDesktop process and a loaded taskbar hook before building. The default script does not stop processes. When locks need to be cleared, `scripts/build.bat --reload-shell` stops SnowDesktop and briefly restarts Explorer; notify affected users before running it.
- During development, run the smallest sufficient test group. Run `scripts/test.bat full` before final delivery, PR integration, or release of host changes. Build infrastructure, test infrastructure, and cross-module public behavior changes also require the full suite.
- A `try` build awaiting real-world validation may be handed over after the necessary build and targeted tests, explicitly noting that the full suite was not run. Run the full suite once after the user confirms the scenario and before final delivery or a `verify` commit. High-risk boundaries still require full testing.
- For Lua widget package changes that do not affect the host, public APIs, or build scripts, use `snowwidget lint`, component tests, package validation, and packaging checks; host compilation and the full host suite are unnecessary for those changes alone. Documentation-only or comment-only changes may use link, content, and diff checks, explaining why build and tests were not run.
- Tests should protect observable behavior, data or API invariants, or known defects. Extend existing targets where possible. Keep test sources in `tests/` and register them through the `SnowDesktopTests` aggregate target in `CMakeLists.txt`; avoid tests that only check source formatting or repeat the implementation.
- Visual, interaction, and compatibility claims require the original scenario or equivalent observable evidence. Compilation and automated tests do not replace real-world validation. Desktop-host automation is not stable enough for this purpose; have a user validate those scenarios. Independently operable interfaces, such as the settings window, may use suitable automation tools.

## Code, widgets, and localization

- Follow the existing style and reuse established modules and interfaces. Before implementing changes to public widget APIs, manifests, protocols, or capabilities, describe affected callers, compatibility risks, and the plan for `apiVersion`, `minHostVersion`, capability detection, and fallback behavior. A shared version number is not compatibility evidence.
- When changing user-facing text, update every language in `lang/*.json` with real translations. Built-in and official community widget `locales` must match those language files exactly. Run localization contract tests and manually review translation quality.
- `widgets/` contains built-in widgets and the widget-development Skill distributed with the application. Official community widgets belong in `developer_assets/workshop_widgets/`; they are not distributed with the application and must not be moved into the built-in directory without authorization.
- Built-in widgets run from build output. Before handing changes over for real-world validation, copy them through the standard build or precisely synchronize them to `.build/<Configuration>/widgets/<slug>/` for temporary validation as specified in [AGENTS.md](./AGENTS.md). Temporary synchronization is not a host build.
- Before testing official community widgets, perform a standard host build, then run `scripts/widget-dev.bat developer_assets/workshop_widgets/<slug> -Configuration <Configuration> -Once`. Add `-RestartHost` when needed for initial discovery or a source change. Enable “Development version” in widget development settings and confirm the active source before validation.
- See [AGENTS.md](./AGENTS.md) for specific requirements concerning premium-feature entry points, Fluent settings icons, widget previews, and publication order. Reverse operations, such as disabling effects or restoring ordinary behavior, must remain available regardless of Steam bridge availability or unlock status.

## Data and change boundaries

Preserve existing user changes. Do not overwrite or include someone else's uncommitted work.
Inspect the staging area before committing and include only files belonging to this PR.
Do not commit generated output from `.build/`, `.build_debug/`, `artifacts/`, `docs/html/`, or
`.codex-probes/`, or include logs, credentials, certificate private keys, or user data.

**`.build/Release/data/` may contain active desktop layouts, settings, widget storage, and backups.**
Do not delete it as build cache. Before clearing the entire `.build` directory, explain the data-loss
risk, obtain the user's explicit confirmation, and first attempt a data backup. Ordinary contributions
and tests should not depend on clearing user data.

## Contribution licensing and commercial distribution

SnowDesktop is a multi-license repository. Submitting a contribution confirms that you have the right
to provide it under the license applicable to its destination:

| Contribution destination | Applicable license |
| --- | --- |
| `steam_bridge/` | [MIT](./steam_bridge/LICENSE) |
| Project material elsewhere | Root [GNU GPL v3.0](./LICENSE), subject to any specific third-party license notice in a file |

You retain copyright in your original contributions. Contributions use the existing license applicable
to their destination; submitting a contribution does not transfer copyright. SnowDesktop may use these
contributions commercially and distribute them for a fee, subject to the applicable license terms.
Third-party material remains subject to its original licenses and notices.

## Third-party material and provenance

- Submit only material you created or are authorized to provide. If an employer, client, or another rights holder owns relevant rights, first obtain authorization sufficient for the license you are granting.
- For third-party code, images, fonts, data, or other material, identify the exact upstream location, version or commit, license, and modifications. Preserve copyright and license notices and update [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md) when appropriate. Do not present third-party material as your own original contribution.
- Do not copy GPL core code or TranslucentTB-derived material into `steam_bridge/`. The two programs communicate through documented process boundaries such as command-line arguments, JSON, or named pipes. Shared protocol definitions must be original and explicitly permissively licensed. This architectural rule does not replace license review of the actual combination.
- Do not commit the Steamworks SDK or its headers, libraries, tools, or redistributable files. Configure an external SDK path when building the Steam-enabled bridge.
- AI-assisted contributions still require review of provenance, license compatibility, correctness, and sensitive information. Disclose known third-party sources as you would for other contributions.
