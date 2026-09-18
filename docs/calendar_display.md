# Calendar and agenda

The Calendar & agenda settings page manages a host-owned extra-calendar toggle
and calendar type, and the existing local event service. Holiday display and
region selection have been removed at the user's request, together with the
offline data, generator and related third-party distribution notices.

Windows ICU converts civil dates for the twelve supported additional calendars.
The effective UI language is used even in system-language mode. Chinese lunar
captions use traditional day names and show the month only on day one, including
leap months. Gregorian event keys and navigation stay unchanged.

The built-in month-calendar shows extra-calendar dates in hover tooltips in both
month and compact-week layouts, leaving cells for Gregorian dates and event dots.
Full extra-calendar dates also remain in accessible labels. Immediate-mode host
rendering must paint interaction-region tooltips after component content and clips.
The toggle defaults off; switching it off preserves the selected calendar and
all events. No holiday label is consumed, even when using an earlier host.

Settings controls align right. The whole event-editor card is collapsed until
editing. Native CalendarDatePicker and TimePicker controls edit local Gregorian
dates and times; all-day events hide the time controls. Events preserve revision
checks, local persistence, notes and reminders. Private IPC checks session/route.

The public `calendar.annotations` feature/API v2 remains available. Removed
holiday fields are retained only as false/empty/zero compatibility values so
earlier components do not fail indexing or concatenating them. Persisted legacy
holiday choices are ignored and no longer written. See the component API manual
for the complete contract. Numeric date fields and permission rules are unchanged.

Regression coverage checks extra-calendar conversion and off-state behavior,
ignoring old holiday choices without losing the selected calendar, and absence
of holiday annotations even when callers pass legacy enabled preferences.
Obsolete holiday-accuracy and region-label tests were replaced by these removal
and compatibility checks. Settings/desktop interactions require user acceptance;
offscreen fixtures demonstrate layout only, not live settings propagation.

## Removal validation (2026-09-18)

Candidate `4a417bef`: `scripts/build.bat --reload-shell` passed and generated the
Release executable; existing generated WinRT warnings remain. `scripts/test.bat
full` passed 118/118, exit 0, CTest 67.81 s. Report:
`.build/Testing/test-run-4cea7a366139472ea2872a29a3c8e12c.xml`.
Local transcripts: `.codex-probes/calendar-only-build.log` and
`.codex-probes/calendar-only-tests.log`.

The isolated service/settings checks passed. A controlled reintroduction of
holiday output failed both removal assertions. Package lint/validation passed
(existing missing-preview warning). Source/output Lua SHA256 values matched.
An October 2026 offscreen fixture deliberately supplied older-host holiday data;
the updated widget rendered lunar dates only. Settings and desktop interactions
remain pending user acceptance. No desktop-host automation was used.
