# Position Currency Research

This document describes the temporary MCST-Watchdog **1.114-R2 Position Currency Research** workflow.

The R2 goal is narrower than the R1 memory sweep: determine whether **two independent currency contexts** can be associated with each Open Positions row directly from MultiCharts Tracker data:

- the instrument/native currency used by `Average Price` and `Native Value`;
- the currency used by `Open P/L`, which may already be the account/report currency.

The loaded `ATOnPTracker.dll` build contains diagnostic method names for `COpenPositionInfoExtractor::AveragePrice`, `OpenPL`, `RealizedPL`, `CurrencyCode`, `CurrencyLetter`, and `CurrencyLetterRPL`. R2 does not call these unknown internal functions. Instead it uses the previously mapped live Open Positions storage to determine which fields can be read safely and correlated with visible rows.

## Safety model

The research action is read-only. It:

- reads the normal Tracker status snapshot through MCST Tracker Bridge V157;
- writes that snapshot as a row-reference file;
- invokes Protocol V2 command 50, `CapturePositionCurrencyDirectResearch`;
- reads the visible Open Positions grid through the already verified Tracker reader;
- resolves `ITC_TradeInfo -> +0x98 -> +0x10E0`, the previously mapped Open Positions record-storage candidate;
- correlates records using quantity and Average Price;
- inspects candidate fields beginning at the statically identified record offsets;
- does not place, modify, or cancel orders;
- does not write to MultiCharts process memory;
- does not simulate mouse or keyboard input;
- does not call unknown MultiCharts functions.

## Exact research fingerprint

The temporary record-offset hypothesis is authorized only for the exact `ATOnPTracker.dll` build captured in the preceding research session:

```text
PE timestamp: 0x6A5694FB
Image size:   3534848
```

If either value differs, R2 writes a `BLOCKED_FINGERPRINT_MISMATCH` report and does not reuse the candidate record offsets.

## Before capture

For the strongest result, keep at least two open positions whose instruments use different native currencies. Three currencies are even better. If possible, include positions where the displayed `Open P/L` currency is visibly different from the instrument currency. Example:

```text
AAPL:xnas       USD instrument
MILDEF:xome     SEK instrument
SAP:xetr        EUR instrument
```

Use actual positions available in the test environment.

## Run the capture

1. Build the solution as `Release|x64`.
2. Copy the new `MCST-TrackerBridge.dll` V157 to `C:\MCExtras`.
3. Restart MultiCharts so V157 is loaded by the PowerLanguage Bridge host.
4. Start MCST-Watchdog 1.114-R2 and enable Developer Mode.
5. Verify that the Dashboard can read Open Positions.
6. Press **Position CCY**.
7. Wait for the completion dialog. On success, `C:\Temp` opens automatically.

## Files to return

The two primary files are:

```text
C:\Temp\MCST_Position_Currency_Reference_<pid>.txt
C:\Temp\MCST_Position_Currency_Direct_<pid>.txt
```

The direct report already includes the visible rows and the focused record correlation, so the large R1 Targeted Structure Probe bundle is no longer the primary output.

## What the direct report contains

For every visible Open Positions row it records:

```text
Profile
Account
Symbol
Side
Qty
Average Price
Open P/L
Open P/L currency hint from the displayed text, when unambiguous
Last Update
```

It then searches the mapped Open Positions record storage using the current static record hypothesis:

```text
quantity       +0x60
average_price  +0x68
open_pl        +0x70
currency candidate begins at +0x78
```

The `+0x78` and later fields are **research candidates**, not production facts. R2 prints their raw values and follows readable short-string pointers so values such as `SEK`, `EUR`, or another currency representation can be observed without guessing.

The report also records whether the internal `+0x70` value matches the displayed `Open P/L`. This is important because the displayed P/L may be in a different currency from Average Price.

## Static extractor evidence

R2 also searches the loaded `ATOnPTracker.dll` image for diagnostic method-name strings including:

```text
COpenPositionInfoExtractor::AveragePrice
COpenPositionInfoExtractor::OpenPL
COpenPositionInfoExtractor::RealizedPL
COpenPositionInfoExtractor::CurrencyCode
COpenPositionInfoExtractor::CurrencyLetter
COpenPositionInfoExtractor::CurrencyLetterRPL
```

The report labels these addresses explicitly as **diagnostic string RVAs**. They confirm that the named methods exist in that module build, but they are not treated as callable function addresses.

## Verification rule

A field is not promoted into the production reader from one matching row. We want at least two different instrument currencies and preferably three. A good result would look conceptually like:

```text
MILDEF:xome  Average Price currency -> SEK
AAPL:xnas    Average Price currency -> USD
SAP:xetr     Average Price currency -> EUR
```

while separately demonstrating the currency semantics of `Open P/L`.

If the same candidate field follows the instrument currency across rows and a separate field follows the P/L/account currency, the next production step can read both directly rather than introducing a PowerLanguage symbol-currency provider.

## Temporary totals policy

This research build deliberately does **not** aggregate Position Value or Open P/L across Open Positions. `Native Value` remains the per-position `abs(Qty) * Average Price` amount in the instrument currency.

The report continues to state:

```text
TOTALS NOT CALCULATED - position and P/L currency normalization is under research.
```

Production totals will be restored only after both currency contexts are verified.

## Bridge identity

```text
Product version: 1.0
Internal build:   V157
Bridge Protocol: V2
```

V157 is required for this R2 capture because command 50 is new. Production Tracker snapshot compatibility remains V156-or-newer.
