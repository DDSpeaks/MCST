# Position Currency Research — 1.114-R3

## Goal

The purpose of this temporary research build is to determine whether MCST can obtain both currency contexts for each Open Positions row directly from MultiCharts:

```text
Average Price / Native Value  -> instrument currency
Open P/L                      -> P/L or report/account currency
```

The values may be in different currencies. R3 therefore keeps them separate throughout the investigation.

## Why R3 exists

R2 verified that the production Tracker reader can read the visible rows, but its research-only `ITC_TradeInfo -> +0x98 -> +0x10E0` storage pointer was null in a live capture. R3 removes that assumption completely.

## Safety boundary

The R3 capture is passive:

```text
Process-memory writes:       no
Unknown internal calls:      no
UI input simulation:         no
Order operations:            no
Production totals enabled:   no
```

The Bridge reads memory that is already readable inside its own MultiCharts process and writes diagnostic text files under `C:\Temp`.

## Required Bridge

```text
Product version: 1.0
Internal build:  V158
Protocol:        V2
```

Production Tracker snapshots still require only V156 or newer. The `Position CCY` R3 capture specifically requires V158.

After replacing `C:\MCExtras\MCST-TrackerBridge.dll`, restart MultiCharts so the new DLL is loaded.

## Recommended live data

The most useful capture contains at least two open positions whose instrument currencies are known to be different. Three currencies are better.

Examples:

```text
US stock       -> USD
Swedish stock  -> SEK
Euro-area stock -> EUR
```

A row where Open P/L is displayed in EUR while Average Price is in USD or SEK is especially useful.

## Running the capture

1. Build `Release|x64`.
2. Copy the R3 `MCST-Watchdog.exe` and `MCST-TrackerBridge.dll` to `C:\MCExtras`.
3. Restart MultiCharts.
4. Start the Bridge host and Watchdog.
5. Enable Developer Mode.
6. Verify that Open Positions contains useful live rows.
7. Press `Position CCY`.
8. Wait for the completion dialog. The bounded fallback scan can take longer than R2 because it may inspect a substantial amount of readable data memory.

The action writes:

```text
C:\Temp\MCST_Position_Currency_Reference_<pid>.txt
C:\Temp\MCST_Position_Currency_Dynamic_<pid>.txt
```

## How R3 finds candidates

R3 parses the visible row values first:

```text
Quantity
Average Price
Open P/L
```

It then discovers readable private/mapped regions reachable from Tracker roots and scans them for Average Price.

An Average Price hit is retained only when the visible Quantity occurs within a bounded nearby window.

A displayed Open P/L numeric match in the same neighborhood is additional evidence.

R3 does not require:

```text
record base + 0x60 = Quantity
record base + 0x68 = Average Price
record base + 0x70 = Open P/L
root + 0x10E0      = record storage
```

Those were R2 hypotheses, not production facts.

## What to inspect in the R3 report

For each row, look first at:

```text
candidate_count=
CANDIDATE ... score=
quantity_i32_offsets_from_average=
quantity_i64_offsets_from_average=
displayed_open_pl_offsets_from_average=
```

Repeated relative offsets across several unrelated rows are stronger evidence than a one-row coincidence.

Then inspect:

```text
small_integer_fields_near_average:
nearby_pointer_strings:
nearby_double_fields:
```

A stable small integer field that changes with USD/SEK/EUR may be a currency code candidate. A readable nearby `USD`, `SEK`, `EUR`, `$`, `€`, etc. string may be even stronger.

## Static extractor evidence

The report separately scans PE sections for the diagnostic names of:

```text
CurrencyCode
CurrencyLetter
CurrencyLetterRPL
```

and related Open Positions getters.

`diagnostic_string_rva` and `raw_rip_reference_rvas` are static evidence only. R3 does not invoke these functions.

## Success criteria

A field should not be promoted to production merely because it appears plausible.

For native/instrument currency, require repeated live captures showing the same field/relationship with at least two different known instrument currencies.

For Open P/L currency, require a repeated relationship that agrees with the visible P/L currency and distinguishes cases where native currency differs.

Until then:

```text
currency = UNKNOWN
totals   = NOT CALCULATED
```

## Files to return for analysis

The two most important files are:

```text
MCST_Position_Currency_Reference_<pid>.txt
MCST_Position_Currency_Dynamic_<pid>.txt
```

No raw process-memory binary dump is required by R3.
