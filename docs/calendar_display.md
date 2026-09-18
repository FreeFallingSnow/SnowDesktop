# Calendar and agenda

Host-owned preferences live in SnowDesktop.general.json. The dedicated WinUI page
uses the existing General domain persistence/IPC and the existing local calendar
service. No external account, network request or component permission is involved
in the host settings page. Component event permissions are unchanged.

The existing month-calendar reads optional calendar.annotations, displays a
secondary line when space permits and full details through accessible labels and
host tooltips. Both extra-calendar and holiday display default off and can be
independently toggled without losing choices/events. Gregorian event keys stay
stable. No non-Gregorian recurrence, month navigation or workday calculations.

Windows 10 2004 is already the target minimum; system ICU C APIs are available
without redistributing an ICU runtime. The names and algorithms therefore track
the user's Windows ICU version. Calendar options use explicit variants rather
than choosing a calendar from language. Holiday regions are independent.

The settings page lists all existing local events. It creates and updates through
CalendarService, preserving reminders/notes and checking revisions. Deletion
checks the selected revision before removal, and asks for explicit confirmation.
The page refreshes while visible without overwriting the editor; stale saves fail
with conflict. All private IPC operations check active settings generation/route.

Offline holiday provenance and regeneration: third_party/holidays/README.md.
Public component contract: widgets/snowdesktop-lua-widget/references/api-v2.md.

Validation: automated date conversion, region isolation, disabled flags, snapshot
bounds, persistence and existing integration suites. Calendar/agenda page and
desktop visuals require real-user acceptance; no desktop-host automation is used.

## Validation record (2026-09-18)

Candidate: `0b0237b7` (Release, Windows SDK 10.0.26100.0, VS 18 MSBuild).
`scripts/build.bat` passed and generated `.build/Release/SnowDesktop.exe`.
`scripts/test.bat full` passed 118/118 (CTest 72.75 s, exit 0), excluding manual
diagnostics as defined by the repository policy. Report:
`.build/Testing/test-run-d4f3d154f346444dbeabe199ad7d6ab6.xml`.
Local build/test transcripts are in `.codex-probes/calendar-build-keys.log` and
`.codex-probes/calendar-tests-keys.log`.

The preceding candidate failed localization checking because a composed key
prefix was interpreted as a literal key; explicit keys resolved that failure.
An isolated leap-month mutation failed at the expected assertion; the same
calendar-service test with production code passed. Existing generated WinRT
`GetCurrentTime` C4002 and unused-localization warnings remain.

`snowwidget lint`, `validate` and `pack` passed for month-calendar. Lint retains
the existing missing package-preview warning. `snowwidget test` reports no test
directory; this is not counted as a pass. Chinese light and English dark 4x4
offscreen previews were inspected with annotations disabled. Source/output
month-calendar Lua and manifest hashes matched after the standard build.

Pending user acceptance: toggle both annotations independently, switch calendars
and holiday regions, inspect smaller month-calendar sizes and hover details,
create/edit/delete events and confirm persistence and reminder behavior. These
settings interactions and enabled desktop visuals are not claimed as verified.

## Display and editor follow-up (2026-09-18)

Runtime candidate `e3698c3b` plus localization-table comments: effective system
language now reaches the annotation service; CN/HK/TW have explicit region labels
in all ten UI languages. Chinese lunar captions use traditional day notation and
month-only labels on day one. Transferred rest-day names are excluded separately
from observed holidays. Month and compact-week layouts fit annotation text without
ellipsis; event dots do not occupy the annotation row.

Calendar settings align controls right and collapse the entire idle editor card.
The event editor uses CalendarDatePicker and TimePicker; all-day events hide the
time controls. Date values are read/written as local Gregorian civil dates.

Standard build passed (`.codex-probes/calendar-revision-final-build.log`). Full
tests passed 118/118, 66.39 s, exit 0; report:
`.build/Testing/test-run-e7ee3313fbe943ab84596b8934606177.xml`.
An earlier full run failed only the literal-localization scanner; locale-specific
data tables now carry the same explicit rationale markers used in the existing
language-name tables. Restoring the old display implementation in an isolated
copy failed the new lunar-caption and region-label assertions as expected.

Enabled annotations were inspected in offscreen 4x4, 6x2, 2x4 and 2x2 previews.
The temporary package used local API fixtures containing real service output
for August and October 2026; the October preview displays 国庆节 without transfer
labels. This proves layout with those samples, not live settings propagation.
Lint and package validation passed with the existing missing-preview warning.
The newly introduced shadowing warning was removed; existing unrelated warnings
remain. Settings controls, picker interactions and live desktop behavior still
require user acceptance. No desktop-host automation was used.
