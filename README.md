# MCST-src — MCST-Watchdog 0.584 Dashboard Polish

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


## Email and AutoTrading alerts (0.580a)

Configure `MCST-Watchdog.ini`:

```ini
[AutoTrading]
enabled=true
minimum_active_strategies=65
check_interval_minutes=5

[Email]
enabled=true
smtp_server=smtp.example.com
smtp_port=587
use_ssl=true
smtp_user=your-user
smtp_password=your-password
from=sender@example.com
to=recipient@example.com
autotrading_alerts=true
autotrading_recovery=true
```

Use **Reload Settings** after editing the INI. Use **Send Test Email** to verify SMTP settings. An AutoTrading alert is sent once when the active count falls below the minimum, and a recovery message is sent once when the count returns to the accepted level.


## Scheduled reports and heartbeat (0.580a)

```ini
[StatusReport]
enabled=true
interval_minutes=60
send_on_startup=false

[Heartbeat]
enabled=true
interval_minutes=60
send_on_startup=false
```

Scheduled reports use the same Dashboard-based status model and are saved to the configured report path. When email is enabled, the report is also sent by email. Heartbeat messages include the current status report so they confirm both that MCST-Watchdog is alive and what MultiCharts currently looks like.


## Architecture cleanup (0.580a)

Email dispatch, AutoTrading alert transitions, and scheduled report/heartbeat timing are separated into dedicated modules. The user-visible behavior remains the same as 0.580, while the malformed multiline strings that prevented compilation are fixed.


## Dashboard polish (0.584)

- All Dashboard status indicators are 20% larger while retaining fixed row positions.
- The overall-status indicator is enlarged from 20 px to 24 px.
- System-status row indicators are enlarged from 14 px to 17 px.
- Recent Activity timestamps use a fixed right-aligned column, so every event description begins at exactly the same horizontal position.
- User-visible version strings are updated to 0.584.

## Centralized INI normalization (0.584)

At startup and whenever **Reload Settings** is pressed, MCST-Watchdog checks every known INI key. Missing settings are written with safe defaults, invalid values are corrected, and numeric values outside supported ranges are clamped. Changes are recorded in `MCST-Watchdog-ConfigNormalization.log`. SMTP credentials and other user-specific strings are never invented.

## Runtime installation

Install the runtime EXE and DLL files in `C:\MCExtras`. Source code and documentation may be stored in any user-selected location. See `Docs/INSTALLATION.md`.

## Engineering policy

All source comments, UI text, logs, and documentation are written in English. See `CODING_STANDARD.md`.
