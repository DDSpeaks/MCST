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

## Broker Monitor state machine

The Broker Monitor consumes the latest Recent Logs rows delivered by Bridge V155.
It maintains four internal states: Unknown, Connected, GracePeriod and Disconnected.
Rows are processed oldest-to-newest and deduplicated, because the Bridge returns a
rolling window of recent rows on every snapshot.

The monitor deliberately does not treat a single disconnect message as an immediate
critical outage. A disconnect or reconnect event starts the configured grace period.
A recovery event cancels the pending alert. Only expiry of the grace period causes a
critical state and an alert decision.

The text patterns are configuration data rather than hard-coded broker knowledge. This
keeps the monitor independent from a specific broker profile and allows future profile
support without changing Bridge V155.

## Universal Application Mapper

The Universal Application Mapper remains a separate application-independent research
tool. MCST-Watchdog stores only its optional executable path:

```ini
[DeveloperTools]
universal_application_mapper_path=C:\MCExtras\UniversalApplicationMapper.exe
```

Do not start the Mapper automatically in production mode. It is intended for Developer
Mode investigations, UI mapping and compatibility research.
