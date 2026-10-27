# 4-7-8 Breathing

Official SnowDesktop community component (Steam Workshop) for guided 4-7-8
breathing: a 4 second inhale, a 7 second hold and an 8 second exhale, repeated
for the configured number of rounds.

## Interaction

- Tap the guide circle or the play button in the top-left corner to start, pause
  and resume.
- The arrow in the top-right corner restarts the session from the first inhale.
- During a running or paused session, right-click for **Skip phase**,
  **Restart** and **End session**; the host adds its standard component entries
  to the same menu. Idle only offers **Start**, and the completed state offers
  **Breathe again** and **End session**.
- A session ends automatically after the configured number of rounds and shows
  the completion summary; the top-left play button starts the next session.
- The phase hint sits below the circle while the countdown stays centered in it.
- Moving the component's desktop page out of view pauses the running session at
  the current phase, so it does not finish silently in the background.

## Settings

- **Rounds per session**: 1-12, default 4. Read when a session starts, so a
  change applies to the next session.
- **Breathing circle color**: accent color of the guide circle and progress
  ring. Changes apply immediately, including the uncommitted live preview in the
  settings page.

No descriptions are shown for these controls because the labels, the guide and
the round counter already explain their effect.

## Data and permissions

- Only the two settings above are persisted. The running session lives in the
  instance value model, so unloading or reloading the component returns to the
  idle state.
- The package requests no permissions, data topics or tasks, and performs no
  network or filesystem access.
- Every visible word is translated in all languages the host currently ships;
  the `4 · 7 · 8` numerals and their typographic separator are locale-neutral.
  No fallback placeholder text is used.

## Validation

Run the package-level checks from the repository root:

```powershell
.build\Release\snowwidget.exe lint developer_assets\workshop_widgets\breathing-478
.build\Release\snowwidget.exe test developer_assets\workshop_widgets\breathing-478
# Probe render only; the committed catalog image is workshop-preview.png.
.build\Release\snowwidget.exe preview developer_assets\workshop_widgets\breathing-478 `
  .codex-probes\breathing-preview.png --appearance glass-light --canvas-size 512 `
  --padding 48 --locale zh-CN `
  --background developer_assets\workshop_widgets\community-preview-background.png
.build\Release\snowwidget.exe quality developer_assets\workshop_widgets\breathing-478
.build\Release\snowwidget.exe pack developer_assets\workshop_widgets\breathing-478 `
  .codex-probes\breathing-478.snowwidget
```

`workshop-preview.png` is the 512x512 catalog image generated with the repository
community preview background and the component's real 2x2 span. Desktop pointer
behaviour, context menus and DPI scaling still require an in-host check; the
package checks cannot verify them.
