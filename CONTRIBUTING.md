# Contributing and license scope

SnowDesktop is a multi-license repository. By submitting a contribution, you
confirm that you have the right to provide it under the license that applies to
its destination:

- Contributions under `steam_bridge/` are licensed under the MIT License in
  `steam_bridge/LICENSE`.
- Contributions everywhere else are licensed under GNU GPL v3.0 in the root
  `LICENSE`, unless a file contains a more specific third-party notice.

Do not copy code from the GPL core or its TranslucentTB-derived portions into
`steam_bridge/`. Communication between the two programs must remain through a
documented process boundary such as command-line arguments, JSON, or a named
pipe. Any protocol definitions intended for both sides must be original and
carry an explicit permissive license.

The Steamworks SDK and its headers, libraries, tools, and redistributable files
must not be committed to this repository. Configure an external SDK path when
building the Steam-enabled bridge.

Third-party material must retain its original copyright and license notices.
The contribution must also identify its exact upstream source and describe any
modifications.
