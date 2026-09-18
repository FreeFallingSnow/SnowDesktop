# Offline holiday data

Source: https://github.com/vacanza/holidays, PyPI holidays 0.95, MIT.
Generated using Python with python-dateutil 2.9.0.post0 and six 1.17.0.
Only the generated holiday names/dates and upstream notices are redistributed;
Python and these packages are build-time tools, not application dependencies.

Run `scripts/generate_calendar_holidays.py` to reproduce `src/calendar_holidays.inc`.
It fixes regions, languages, PUBLIC category, observed=False and years 2020–2035.
The source can emit transferred rest days even with observed=False; those entries
are removed by matching its translated `substituted_label` template. Filtering is
per name so a genuine holiday sharing the date is preserved.
No subdivisions or WORKDAY/bank/school categories are included. Multi-day public
holidays are preserved. This is not a comprehensive cultural festival catalog.
Source-provided estimated labels are retained. Future dates are predictions,
not a claim of official confirmation. Dates outside the snapshot are unavailable.
The generator does not scrape websites or query any runtime service.

Names use an available exact UI locale, then its base language, then English,
then an available source language. The settings page states that fallback.
Update the pinned package and snapshot together after reviewing upstream changes.
