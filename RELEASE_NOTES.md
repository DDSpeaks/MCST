# MCST 1.114-R3 Position Currency Dynamic Research

MCST-Watchdog 1.114-R3 is a temporary research build based on the 1.114 production source. It continues the Open Positions currency investigation while removing the fixed record-root assumption that R2 proved invalid on the live system.

## What R2 proved

The normal Tracker reader successfully returned the visible Open Positions rows, including separate Average Price and Open P/L values. In the R2 live capture, Open P/L was visibly denominated in EUR, but the research-only `ITC_TradeInfo -> +0x98 -> +0x10E0` record-root candidate was null. R3 therefore does not use `+0x10E0` as a prerequisite.

## New R3 dynamic correlation

The Developer Mode `Position CCY` action still writes a fresh row reference:

```text
C:\Temp\MCST_Position_Currency_Reference_<pid>.txt
```

Bridge V158 then writes:

```text
C:\Temp\MCST_Position_Currency_Dynamic_<pid>.txt
```

R3 searches readable private/mapped data memory for each visible Average Price and keeps a memory candidate only when the same visible Quantity occurs nearby. A nearby numeric match for the displayed Open P/L increases the candidate score.

No fixed record base or fixed `Qty/Average Price/Open P/L` offsets are required for candidate discovery.

## Two-stage bounded scan

R3 first follows readable pointers from verified Tracker-related roots:

```text
ITC_TradeInfo
ITC_TradeInfo + 0x98 target
ITC_TradeInfo + 0x88 target
CATPTTabView
Open Positions page object
Open Positions grid object
```

If those regions do not correlate all parsed rows, R3 performs a bounded fallback scan of readable private/mapped process data regions. Both stages have explicit byte and runtime limits.

The capture remains read-only. It does not write MultiCharts memory, simulate UI input, or call unknown MultiCharts internal functions.

## Section-by-section extractor evidence

R2 attempted to read the full `ATOnPTracker.dll` image as one contiguous block. That could fail even when the relevant diagnostic strings were present. R3 parses the PE section table and scans readable sections separately for:

```text
COpenPositionInfoExtractor::AveragePrice
COpenPositionInfoExtractor::OpenPL
COpenPositionInfoExtractor::RealizedPL
COpenPositionInfoExtractor::CurrencyCode
COpenPositionInfoExtractor::CurrencyLetter
COpenPositionInfoExtractor::CurrencyLetterRPL
COpenPositionInfoExtractor::PriceScaleCode
```

The report also records raw RIP-relative references to those diagnostic strings where found. These references are evidence only; they are not treated as callable function addresses.

## Candidate neighborhood output

For each retained Quantity + Average Price candidate, R3 records:

- memory address and source region;
- Quantity matches relative to Average Price;
- displayed Open P/L matches relative to Average Price;
- small positive integer fields near Average Price, useful for discovering numeric currency codes;
- readable direct, indirect, and second-level string pointers;
- currency hints from strings such as EUR, USD, SEK, DKK, NOK, GBP, CHF, JPY, CAD, AUD, NZD, HKD, and SGD;
- nearby finite double values;
- cross-row frequency of recurring relative offsets.

This should make it possible to distinguish the instrument/native currency from the Open P/L/reporting currency without guessing from the ticker.

## Fail-closed fingerprint gate

R3 remains scoped to the researched `ATOnPTracker.dll` fingerprint:

```text
PE timestamp: 0x6A5694FB
Image size:   3534848
```

A different module fingerprint is blocked.

## Multi-currency totals safety

The research package keeps the safety rule introduced in R1:

- per-position `abs(Qty) * Average Price` is labeled `Native Value`;
- aggregate Position Value is disabled;
- aggregate Open P/L is disabled;
- the status report says totals are not calculated while currency normalization remains under research.

## Bridge identity

```text
Product version: 1.0
Internal build:   V158
Protocol:         V2
```

Command 50 keeps its existing protocol identifier/name for compatibility, but Bridge V158 changes its implementation from fixed-root R2 research to dynamic R3 correlation.

See `Docs/POSITION_CURRENCY_RESEARCH.md` for the capture workflow.
