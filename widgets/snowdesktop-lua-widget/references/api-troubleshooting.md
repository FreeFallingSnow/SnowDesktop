# API troubleshooting and upstream feedback

Use this workflow when a documented SnowDesktop API is absent, rejected,
returns a contradictory result or behaves differently in preview and the
desktop host.

## Establish the exact contract

1. Record the SnowDesktop version and run `snowwidget capabilities` with the
   CLI bundled beside this Skill.
2. Export only the relevant live contract with `snowwidget api-contract`,
   `snowwidget view-contract` or `snowwidget system-contract`.
3. Confirm the package's `schemaVersion`, `apiVersion`, feature probe,
   permission declaration and authorization state. A reserved manifest word or
   a mention in prose does not expose an API.
4. Reduce the behavior to the smallest package or Lua fragment that still
   fails. Capture the exact lint, validation, preview and host errors without
   credentials, personal paths or private data.

Do not classify unsupported syntax, a missing permission, a stale copied CLI or
an API advertised only by a newer release as a host defect.

## Inspect the open-source implementation

SnowDesktop is open source at
<https://github.com/FreeFallingSnow/SnowDesktop>. Prefer the tag, release branch
or commit matching the installed version. The default branch can be newer than
the user's executable, so source availability alone does not prove that a
feature exists in that build.

Search the API registry and contract exporters first, then follow the selected
function, view property, system topic or task into its implementation. Compare
four pieces of evidence:

- what `capabilities` says this executable supports;
- what the live contract advertises and which permission it requires;
- what the matching source registers and validates;
- what the minimal package actually observes in preview and, when relevant,
  the desktop host.

Keep a compatibility fallback only when it uses documented capability probes
and preserves honest degraded behavior. Never reach for removed globals,
native binaries, arbitrary filesystem access or fabricated preview data to
hide an API failure.

## Ask the user to report a confirmed problem

When the evidence still indicates a SnowDesktop API defect or documentation /
implementation mismatch, explicitly tell the user and point them to
<https://github.com/FreeFallingSnow/SnowDesktop/issues>. Prepare a concise issue
packet containing:

- SnowDesktop version, installation channel and Windows version;
- `snowwidget capabilities` output and the relevant contract entry;
- a minimal widget package or minimal Lua snippet;
- exact reproduction steps and whether preview, desktop runtime or both fail;
- expected behavior, actual behavior and stable error codes or log excerpts;
- whether the matching source appears inconsistent, with file and commit/tag
  references when known.

Remove secrets, user data and unnecessary absolute paths. Do not claim an
upstream defect solely from inference; distinguish confirmed reproduction,
source evidence and remaining uncertainty.
