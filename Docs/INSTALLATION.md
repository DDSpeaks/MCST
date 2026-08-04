# MCST Installation Guide

## Runtime installation directory

Install the runtime executable and DLL files directly in:

```text
C:\MCExtras
```

Typical runtime files:

```text
C:\MCExtras\MCST-Watchdog.exe
C:\MCExtras\MCST-TrackerBridge.dll
C:\MCExtras\MCST-Watchdog.ini
```

Any additional runtime DLL dependencies should be placed in the same directory unless a later release explicitly documents another location.

## Source code and documentation

The source tree and documentation are not required to be stored in `C:\MCExtras`.

- The source tree may be placed in any development directory, such as `C:\Users\Administrator\source\repos\MCST-src`.
- Documentation may be extracted and stored anywhere the user prefers.
- `C:\MCExtras` is reserved for the installed runtime EXE and DLL files and their local runtime configuration.

## Portable operation

MCST is designed to avoid mandatory registry entries and `Program Files` installation. The runtime installation can therefore be backed up or moved as one directory, provided that configured paths remain valid.

## Optional Universal Application Mapper

When the separately built Mapper executable is available, copy it to:

```text
C:\MCExtras\UniversalApplicationMapper.exe
```

or change `[DeveloperTools] universal_application_mapper_path` in
`MCST-Watchdog.ini`. The documentation and Mapper source may be stored anywhere.
