# MCST-Watchdog 1.051

## Status Report Delivery Build Fix

This maintenance release fixes the C++ compilation failure in version 1.05.

- Corrects malformed multiline wide-string literals in the manual Status Report dialog.
- Uses explicit `\r\n` line breaks.
- Removes the local variable shadowing warning in the corrected block.
- Keeps local report archiving independent from HTML email delivery.
- Keeps the scheduler reliability protections introduced in 1.05.

Bridge Protocol V155 and the current AutoTrading compatibility profile are unchanged.
