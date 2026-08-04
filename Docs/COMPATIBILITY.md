# MCST Compatibility Framework

## Purpose

MCST-Watchdog reads the MultiCharts AutoTrading state from internal `Charting.dll` objects. Internal addresses may change after a MultiCharts update. Version 0.588 introduces an explicit compatibility profile database so production monitoring never silently reuses an unverified signature.

## Runtime database

The executable creates the following file next to `MCST-Watchdog.exe` when it is missing:

```text
MCST-Compatibility.ini
```

The database is separate from `MCST-Watchdog.ini`. User monitoring settings remain in the Watchdog configuration file, while verified internal MultiCharts signatures are stored in the compatibility database.

## Exact build matching

A profile is selected only when both values match:

- `Charting.dll` PE timestamp
- `Charting.dll` image size

The selected profile provides:

- strategy vtable RVA
- AutoTrading field offset
- verification note

The initial verified profile is:

```ini
[Profile.MC16-Charting-6A5684BF]
name=MC16 verified Charting.dll 0x6A5684BF
enabled=true
pe_timestamp=0x6A5684BF
image_size=18493440
strategy_vtable_rva=0xA457B8
autotrading_offset=0x142
verification=0.577 research session: 8/8 exact toggle responses
```

## Unknown build policy

The default policy is `reject`. If no exact verified profile exists:

- the AutoTrading reader does not use old addresses;
- the Dashboard shows AutoTrading as `UNKNOWN`;
- the diagnostic message reports the detected timestamp and image size;
- the research tools remain available for controlled verification.

This fail-safe behavior prevents a wrong count from being treated as valid production data.

## Adding a new profile

1. Update MultiCharts and allow MCST-Watchdog to detect the unknown build.
2. Record the reported `Charting.dll` timestamp and image size.
3. Use Developer Mode and the existing AutoTrading research session.
4. Verify ON/OFF behavior over multiple controlled transitions.
5. Add a new `Profile.*` section with the verified values.
6. Restart Watchdog or reload the executable.
7. Confirm that the Dashboard names the selected profile and reports the correct active count.

Profiles must never be copied from another build without verification.

## Installation location

Only runtime EXE and DLL files are intended for installation in:

```text
C:\MCExtras
```

`MCST-Compatibility.ini` is generated next to the executable. Source code and documentation may be stored anywhere selected by the user.
