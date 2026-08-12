# MCST 1.114-R2 Position Currency Direct Research

MCST-Watchdog 1.114-R2 is a temporary research build based on the 1.114 production source. R2 focuses on the two currency contexts present in Open Positions: the instrument/native currency used by Average Price and the potentially different currency used by Open P/L.

## New focused Position CCY capture

Developer Mode keeps the compact `Position CCY` action. It first writes a fresh Tracker row reference to:

```text
C:\Temp\MCST_Position_Currency_Reference_<pid>.txt
```

It then invokes the new additive Bridge Protocol V2 command 50:

```text
CapturePositionCurrencyDirectResearch
```

Bridge V157 reads the visible Open Positions rows, resolves the previously mapped `ITC_TradeInfo -> +0x98 -> +0x10E0` record-storage candidate, correlates rows using quantity and Average Price, and writes:

```text
C:\Temp\MCST_Position_Currency_Direct_<pid>.txt
```

The direct report inspects the current record hypothesis (`Qty +0x60`, `Average Price +0x68`, `Open P/L +0x70`, currency candidate beginning at `+0x78`) and follows readable short-string pointers. These offsets remain research candidates until live multi-currency captures confirm them.

## Fail-closed fingerprint gate

The direct record hypothesis is enabled only for the researched `ATOnPTracker.dll` fingerprint `0x6A5694FB / 3534848`. A different module build is blocked rather than scanned with stale offsets.

## Two-currency objective

R2 no longer assumes that Average Price and Open P/L share a currency. The report keeps them separate and records an Open P/L currency hint only when the visible text identifies it unambiguously. This prevents double conversion later if MultiCharts already reports P/L in the account/report currency.

## Static extractor evidence

The loaded `ATOnPTracker.dll` contains diagnostic names for `COpenPositionInfoExtractor::AveragePrice`, `OpenPL`, `RealizedPL`, `CurrencyCode`, `CurrencyLetter`, and `CurrencyLetterRPL`. R2 records the diagnostic-string RVAs but deliberately does not call unknown internal functions.

## Multi-currency totals safety

The research build retains the 1.114-R1 safety rule:

- per-position `abs(Qty) * Average Price` is labeled `Native Value`;
- aggregate Position Value is disabled;
- aggregate Open P/L is disabled;
- the report states that totals are not calculated while currency normalization is under research.

## Bridge identity

```text
Product version: 1.0
Internal build:   V157
Protocol:         V2
```

V157 is a real Bridge change: it adds command 50 for focused Position Currency research. The production snapshot boundary and protocol header remain unchanged. Production Tracker snapshots continue to require V156 or newer.

## Recommended capture conditions

Run `Position CCY` while Open Positions contains at least two instruments with different native currencies. Three currencies are preferable. If possible, include a row where Open P/L is visibly in a different currency from Average Price.

See `Docs/POSITION_CURRENCY_RESEARCH.md` for the exact workflow.
