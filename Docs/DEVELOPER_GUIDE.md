# MCST Developer Guide

## Build environment

- Visual Studio 2022
- Platform toolset v143
- Windows SDK 10
- x64 configuration
- C++17

Open `MCST.sln` and build `Debug|x64` or `Release|x64`.

## Developer mode

Developer mode is controlled by:

```ini
[Developer]
enabled=false
```

The default is `false`. The centralized configuration normalizer writes this key when it is missing. Research and compatibility tools should remain hidden or inactive in normal production operation whenever practical.

## AutoTrading safety

The AutoTrading reader is passive. It may use process enumeration and `ReadProcessMemory`, but it must not write to MultiCharts memory or inject input.

## Release validation

Run `Tools\Validate-Release.ps1` before packaging a release. A Windows build is still required because static package validation cannot replace Visual Studio compilation.
