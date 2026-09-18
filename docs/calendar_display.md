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
