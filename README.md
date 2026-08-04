# MCST-src — MCST-Watchdog 0.578 Verified AutoTrading Reader

This version activates the newly verified AutoTrading reader for the current MultiCharts build.

## Verified compatibility values

- `Charting.dll` PE timestamp: `0x6A5684BF`
- Strategy vtable RVA: `0xA457B8`
- AutoTrading offset: `0x142`
- Verification result: 8 exact responses in 8 controlled ON/OFF transitions

The Dashboard now reads the real active AutoTrading strategy count with these verified values.

- Green when the active count is at least `minimum_active_strategies`
- Red immediately when the active count is below the configured minimum
- Unknown if the passive memory read cannot be completed safely

The reader remains passive. It uses process/module enumeration, `VirtualQueryEx`, and `ReadProcessMemory`; it does not click MultiCharts, send input, or write process memory.

## Configuration

```ini
[AutoTrading]
enabled=true
minimum_active_strategies=65
check_interval_minutes=5
```

The automated research-session buttons are retained for diagnostics and future MultiCharts compatibility work, but they are no longer required for normal AutoTrading monitoring.
