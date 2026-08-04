# MCST Universal Application Mapper Integration

The Universal Application Mapper is an optional developer tool in the MCST suite.
It is not required for normal MCST-Watchdog operation.

## Runtime location

The expected executable path is configurable in `MCST-Watchdog.ini`:

```ini
[DeveloperTools]
universal_application_mapper_path=C:\MCExtras\UniversalApplicationMapper.exe
```

The Mapper executable itself is not rebuilt by the MCST-Watchdog solution. Copy the
separately built `UniversalApplicationMapper.exe` to the configured location when it
is available.

The Mapper remains a standalone, application-independent research tool. It should not
be started automatically in production mode.
