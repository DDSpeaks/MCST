# Position Currency Research — 1.114-R16

> Retained diagnostic in the 1.114-R31 package. R31 does not continue the search
> for Average Price currency. Production Status Reports total only Open P/L rows
> whose displayed currency is already unambiguous.

## R15 result carried into R16

R15's broad search found one interface that uniquely matched the extractor ABI
and the live position count:

```text
module:       ATCenterProxy.dll
vtable RVA:   0x44D518
live objects: 10 in the reference capture
```

The verified interface layout is:

| Vtable slot | Meaning | Object field |
| ---: | --- | ---: |
| `+0x40` | Quantity | `+0x1A8` (`int32`) |
| `+0x48` | Average Price | `+0x1B0` (`double`) |
| `+0x58` | Open P/L | `+0x1B8` (`double`) |
| `+0x70` | CurrencyCode / CurrencyLetter source | `+0x308` (`std::wstring`) |
| `+0x88` | CurrencyLetterRPL source | `+0x328`, falling back to `+0x308` |
| `+0x90` | Realized P/L | `+0x1C8` (`double`) |

PriceScaleCode is not read from this interface. Its extractor first obtains a
different interface and then calls that interface's `+0x60` slot.

## R16 verification gate

Bridge V171 refuses inferred layout reads unless all of the following match:

- ATOnPTracker timestamp `0x6A5694FB` and image size `3534848`;
- ATCenterProxy image size `7303168`;
- vtable RVA `0x44D518`;
- expected target RVAs at slots `+0x40`, `+0x48`, `+0x58`, `+0x60`,
  `+0x70`, `+0x88`, and `+0x90`;
- exact bounded code signatures proving the four numeric field offsets and the
  two currency getter implementations.

This is a fail-closed research fingerprint, not a general cross-version ABI.

## Object search and row correlation

R16 first searches regions reached from `ITC_TradeInfo`, `CATPTTabView`, the
Open Positions page, and its grid for the exact verified vtable value. If all
rows are not uniquely covered, it searches pointer references in the same graph
and then scans other readable private/mapped regions for that exact vtable only.

Objects are matched to the visible grid using absolute Quantity and Average
Price. Open P/L is reported as a delta but is not a stable key because it can
change between the grid read and the memory read.

## Currency decoding

Both currency fields are decoded as bounded MSVC x64 `std::wstring` values:

- primary position currency: object `+0x308`;
- RPL/P&L currency override: object `+0x328`;
- effective RPL currency: use `+0x328` when non-empty, otherwise `+0x308`.

The reader validates size, capacity, SSO/heap storage, terminator, printable
characters, and a strict three-uppercase-letter code. It never calls the getter.

## Expected files

Build `Release|x64`, replace both binaries in `C:\MCExtras`, restart
MultiCharts, and run Developer → `Position CCY`. Return:

- `C:\Temp\MCST_Position_Currency_Dynamic_<pid>.txt`
- `C:\Temp\MCST_Position_Currency_R16_Checkpoint_<pid>.txt`

An `OK` checkpoint means every parsed visible row had one unique verified
object and both currency fields had a valid layout. A second capture with a
different native instrument currency is still required before production use.
