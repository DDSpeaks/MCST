# MCST 1.114-R18 Open P/L Placement and Colors

Tracker Bridge internal build: V171

R17 introduced per-currency Open P/L totals based only on unambiguous currency
evidence in the visible Tracker snapshot. R18 improves the presentation of
those totals without changing their calculation.

## R18 changes

- Places each currency total inside the Open Positions table, with its amount
  directly below the individual Open P/L values.
- Colors the complete positive Open P/L value green in HTML reports, including
  its currency symbol/code and optional plus sign.
- Colors the complete negative Open P/L value red in HTML reports, including
  its currency symbol/code and minus sign.
- Applies the same coloring to individual rows and total rows.
- Removes the unnecessary Native Value total explanation line from the report.
- Retains fail-closed currency recognition and ambiguous-row reporting from R17.
- Adds regression tests for total-column placement and both profit colors.

The Tracker Bridge remains V171 and Protocol V2. The R16 Position CCY research
action remains available for diagnostics but is no longer required to calculate
the production Status Report total.
