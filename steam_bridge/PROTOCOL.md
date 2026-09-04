# SnowDesktop Steam Bridge protocol v1

The bridge is a one-shot command-line process. It does not host an always-on
server, accept arbitrary executable paths, change subscriptions without an
explicit command, or load component code.

`configuration` is the only command that never initializes Steam. It reports
the compiled version, expected App ID (`5080330`), Windows depot ID (`5080331`),
protocol version, and whether the binary was built with Steamworks. Packaging
and local-development scripts use it to reject stale or placeholder binaries.

`entitlement status` initializes Steam for App ID `5080330`, requires an
online signed-in user, and returns `owned` from `ISteamApps::BIsSubscribed()`
for that current account and App ID. It does not treat installation as proof of
ownership and does not relaunch the process through Steam.

## Transport

- Arguments use the native Windows Unicode command line.
- Successful output is UTF-8 JSON on stdout.
- Errors are UTF-8 JSON on stderr and use a nonzero process exit code.
- Long-running commands emit newline-delimited progress objects on stdout,
  followed by one final object containing `"ok": true`.
- All Steam identifiers are JSON strings so JavaScript consumers do not lose
  64-bit integer precision.
- `status` reports both expected and runtime App IDs. Every runtime command
  rejects a Steam context whose actual App ID differs before accessing
  Workshop content.
- `entitlement status` returns `loggedOn`, `owned`, and the current `steamId`.
  `owned: false` is an authoritative successful query, while initialization or
  sign-in failures are errors and must not be interpreted as ownership.

Exit codes:

| Code | Meaning |
|---:|---|
| 0 | Success |
| 3 | Bridge was built without Steamworks |
| 4 | Steam initialization or interface failure |
| 5 | Steam operation or requested item failure |
| 6 | Download or upload timeout |
| 64 | Invalid command line |

## Content contract

One Workshop `PublishedFileId` represents one SnowDesktop package. Its content
folder contains exactly one authoritative artifact named `package.snowwidget`.
The default developer metadata is:

```json
{"format":"snowdesktop-widget","artifact":"package.snowwidget","protocolVersion":1}
```

Metadata, Workshop ownership, and Steam's install folder are untrusted inputs.
The SnowDesktop process must:

1. verify the returned consumer App ID and reject banned/incompatible items;
2. canonicalize the Steam install path without following reparse points;
3. require exactly one regular `package.snowwidget` artifact;
4. copy it into the normal component staging directory;
5. run the full archive and manifest validator;
6. bind `(providerId, PublishedFileId, ownerSteamId)` to the package UUID;
7. require confirmation for source, permission, or network-domain expansion.

The resident host obtains subscription state from Steam's local
`appworkshop_<AppId>.acf` cache instead of periodically launching this Bridge.
It treats that state as authoritative only after a complete parse with the
expected App ID. A newly subscribed package is copied, validated, and installed
automatically; a changed package is updated automatically unless permissions or
network domains expand. The host persists the last authoritative subscription
set separately for each Steam `ActiveUser`. An item missing from the same known
account's next complete snapshot is an unsubscription, unless another remembered
account still subscribes to it. A newly observed account only establishes its
baseline and cannot remove packages installed by another account. Unsubscription
unloads live instances but must not delete their layout records or per-instance
storage, so subscribing again can restore them. Missing, partially written, or
in-progress cache states must never trigger removal. Bridge commands remain the
authoritative boundary for explicit Steam operations and creator verification.

## Progress objects

Downloads emit:

```json
{"event":"download-progress","publishedFileId":"123","available":true,"downloaded":4096,"total":8192,"state":16}
```

Uploads emit:

```json
{"event":"publish-progress","publishedFileId":"123","status":3,"processed":4096,"total":8192}
```

Callers must ignore unknown fields and event names for forward compatibility.
`protocolVersion` changes only when an incompatible transport or content
contract is introduced.

## Component publishing workflow

The higher-level creator commands share the Workshop Manager project store and
package pipeline:

```text
workshop component-plan --source DIR --data-directory DIR [policy options]
workshop component-publish --source DIR --data-directory DIR [same options]
    (--confirm-create|--confirm-update)
```

`--data-directory` is mandatory so an Agent cannot silently create a second
project store. Both commands use its `SteamWorkshopManager` child for project
associations and staging. `component-plan` performs local inspection,
validation, and packaging, but never initializes Steam or submits an item. Its
single JSON result includes the action, content hash, localizations, preview,
tags, visibility, and the exact required confirmation flag.

`component-publish` rebuilds the plan and refuses to proceed unless the caller
passes the matching confirmation. New items require `--confirm-create` and are
always private; bound items require `--confirm-update`. It emits
`component-plan` and `component-publish-progress` JSON Lines before the final
result. A newly allocated PublishedFileId is persisted as soon as Steam returns
it, including when upload or a later localization fails.

Source policies are persistent per local project. Package text submits all
supported manifest localizations; Steam text preserves the listing during an
update; manual English requires an explicit title. Local preview/tags submit
the corresponding project values, while Steam preview/tags preserve the
remote values. Steam-managed sources are invalid for creation. When the packed
SHA-256 is unchanged, listing, preview, tag, and visibility changes use a
metadata-only update unless `--force-content` is specified.

## Author query and association

`workshop list-published --page N` returns the current Steam user's published
items, 50 items per page. Each result includes owner and App IDs, metadata,
primary preview URL, tags, timestamps, visibility and ban state, plus
subscriptions, favorites, website views, and comments. Steam IDs and
PublishedFileIds remain JSON strings.

The Manager writes additive association fields into developer metadata:

```json
{"format":"snowdesktop-widget","artifact":"package.snowwidget","protocolVersion":1,"packageId":"UUID","version":"1.2.3"}
```

Association is accepted only when the current user owns the item, its Consumer
App ID matches, and `packageId` matches the validated local manifest UUID.
