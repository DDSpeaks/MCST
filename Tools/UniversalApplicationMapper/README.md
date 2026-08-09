# Universal Application Mapper Integration

The Universal Application Mapper is an optional developer/research tool associated with the MCST suite. It is not required for normal MCST-Watchdog operation and is not a Watchdog runtime dependency.

## Purpose

The Mapper is application-independent and can be used to investigate Windows application UI structure, including window/control hierarchies and other UI information useful during compatibility research. Within MCST development it can assist investigations such as broker-authentication page/control detection and future UI-based monitoring work.

It is intentionally kept as a separate executable rather than being merged into the production Watchdog.

## Configured path

Watchdog stores the optional executable path in its self-documenting INI configuration:

```ini
[DeveloperTools]
universal_application_mapper_path=C:\MCExtras\UniversalApplicationMapper.exe
```

If this known key is missing, Watchdog writes the default path during configuration normalization. The path may be changed to another location.

## Production behavior

- The Mapper is not required for Watchdog monitoring.
- It must not be started automatically in normal production mode.
- Mapper-related actions belong to Developer Mode.
- The Mapper executable is built/distributed separately from the current MCST-Watchdog solution.
