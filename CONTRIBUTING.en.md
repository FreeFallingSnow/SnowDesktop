# Contributing

[简体中文](./CONTRIBUTING.md) | English

The official repository is [FreeFallingSnow/SnowDesktop](https://github.com/FreeFallingSnow/SnowDesktop).
This guide describes participation channels and contribution requirements.

## How to participate now

**The project is currently under rapid development, and we are temporarily not accepting external pull requests for code integration.**
Help improve features, stability, translations, and documentation through these channels:

- [GitHub Issues](https://github.com/FreeFallingSnow/SnowDesktop/issues): Report bugs, suggest features, or point out translation and documentation errors.
- [QQ user group: 976422547](https://qm.qq.com/q/HyazkCIRig): Discuss your experience, ask questions, and share testing feedback.
- [Steam](https://store.steampowered.com/app/5080330/SnowDesktop/) test builds: Try new features and help verify problems. Follow the instructions below to join.

### Join the Steam test branch

| Item | Value |
| --- | --- |
| Branch | `internal-dev` |
| Description | SnowDesktop internal development builds |
| Access password | `SnowDesktopDev0927` |

1. Right-click SnowDesktop in your Steam library, open **Properties**, and select **Game Versions & Betas**.
2. Enter and verify the access password above, then select the `internal-dev` branch.
3. Wait for Steam to download the update, then launch the application.

See the [Steam beta branch documentation](https://partner.steamgames.com/doc/store/application/branches) for details.

## Feedback and proposals

Search existing issues before opening a report. Include the SnowDesktop version, distribution channel,
Windows version, reproduction steps, expected and actual results, and relevant logs or screenshots.
Remove personal paths, credentials, and other private information before sharing.

Discuss substantial features, architectural changes, or public API changes with the maintainers in an issue.
Discussions may be in Chinese or English; focus on reproducible behavior and specific suggestions.

## Code contributions

When code contributions reopen, target the active `release/vA.B.C.D` development branch.
`main` is the stable release branch. Use the [PR template](./.github/pull_request_template.md)
to describe the change, link related issues, and record validation and remaining work.

For local builds, see the [README](./README.en.md) and [script guide](./scripts/README.md).
Maintainer and automated-agent workflows are defined in [AGENTS.md](./AGENTS.md).

## Contribution licensing and commercial distribution

SnowDesktop is a multi-license repository. Submitting a contribution confirms that you have the right
to provide it under the license applicable to its destination:

| Contribution destination | Applicable license |
| --- | --- |
| `steam_bridge/` | [MIT](./steam_bridge/LICENSE) |
| Project material elsewhere | Root [GNU GPL v3.0](./LICENSE), subject to any specific third-party license notice in a file |

You retain copyright in your original contributions. SnowDesktop may use them under the applicable
license terms, including commercial use and paid distribution.

## Third-party material and provenance

- Submit only material you created or are authorized to provide. If an employer, client, or another rights holder owns relevant rights, first obtain authorization sufficient for the license you are granting.
- For third-party code, images, fonts, data, or other material, comply with its license and identify the exact upstream location, version or commit, license, and modifications. Preserve copyright and license notices and update [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md) when appropriate. Do not present third-party material as your own original contribution.
- Do not commit the Steamworks SDK or its headers, libraries, tools, or redistributable files. Configure an external SDK path when building the Steam-enabled bridge.
- AI-assisted contributions also require review of provenance, license compatibility, correctness, and sensitive information.
