# MultiCharts Internal Compatibility Framework

## Purpose

Some MCST features read MultiCharts internal process data that is not a stable public API. Internal addresses and layouts may change after a MultiCharts update. The compatibility framework prevents production readers from silently reusing unverified build-dependent values.

## Two different version concepts

MCST records two different kinds of identity.

### Human-readable MultiCharts version

Watchdog reads the active MultiCharts executable Windows version resource and exposes product/file version information in diagnostics and Status Reports.

This information is useful to a human operator, but it is **not sufficient to authorize internal-memory offsets**.

### Internal module fingerprint

Production compatibility is selected from an exact module fingerprint. The current AutoTrading reader uses:

- `Charting.dll` PE timestamp
- `Charting.dll` image size

A profile is accepted only when both values match the verified profile.

## Runtime compatibility database

The database is stored beside `MCST-Watchdog.exe`:

```text
MCST-Compatibility.ini
```

It is intentionally separate from `MCST-Watchdog.ini`. Normal operational settings may use safe defaults; verified internal compatibility data must not be invented.

## Current verified profile

The production database contains the verified profile required by the current AutoTrading reader:

```ini
[Profile.MC16-Charting-6A5684BF]
name=MC16 verified Charting.dll 0x6A5684BF
enabled=true
pe_timestamp=0x6A5684BF
image_size=18493440
strategy_vtable_rva=0xA457B8
autotrading_offset=0x142
verification=Controlled research session: 8/8 exact toggle responses
```

The `verification` text records provenance. It is not a substitute for the exact fingerprint check.

## General compatibility principle

The framework is not conceptually limited to AutoTrading. Any future production feature that depends on MultiCharts internal layout must treat its build-dependent data as compatibility-profile data.

Examples include, when applicable:

- vtable RVAs
- structure or field offsets
- interface signatures
- internal object-family signatures
- semantic locators tied to a specific binary build
- any other value obtained by reverse-engineering a particular MultiCharts module

The current production schema exposes the AutoTrading values because those are the build-dependent values used by the current Watchdog internal reader. Future readers should extend the profile/schema rather than introduce unrelated hard-coded production offsets.

## Unknown-build policy

The production policy is fail-safe `reject` behavior. If an exact verified profile is unavailable:

- old internal addresses are not reused;
- the affected reader reports `UNKNOWN`;
- diagnostics report the detected fingerprint;
- Developer Mode research remains available for controlled verification.

A matching human-readable MultiCharts version alone does not override this policy.

## Missing profile values

When a production reader needs a build-dependent value, that value must exist and be verified for the selected build. A missing value must not be substituted from another build. The affected reader should remain `UNKNOWN` until the profile is complete.

## Detecting a MultiCharts update

A MultiCharts update may change the executable product version, internal module fingerprints, both, or neither. Watchdog therefore records the visible version for diagnostics and independently fingerprints the relevant internal module before selecting a profile.

`[DetectedMultiCharts]` in `MCST-Watchdog.ini` is automatically maintained diagnostic output. When available it includes both the human-readable executable version and the detected `Charting.dll` PE timestamp/image size. It is not a compatibility authorization database and should not be edited to force a match.

## Verifying a new build

A safe verification workflow is:

1. Allow Watchdog to detect the new MultiCharts build.
2. Record the exact relevant module fingerprint reported by diagnostics.
3. Confirm that production readers remain `UNKNOWN` rather than using an old profile.
4. Enable Developer Mode in a controlled environment.
5. Use the appropriate research tool for the internal value being investigated.
6. Change one controlled observable state at a time where possible.
7. Repeat transitions to distinguish stable structure from incidental values.
8. Record candidate RVAs/offsets/signatures together with the exact module fingerprint.
9. Re-run the controlled experiment after process or workspace restart when practical.
10. Add a new profile only after the evidence is repeatable.
11. Rebuild/reload as required and confirm that the exact new fingerprint selects only the intended profile.
12. Verify the production reader against independently known state before deployment.

Profiles must never be copied to a different build merely because the product version looks similar.

## AutoTrading research safety

AutoTrading research is passive. It may enumerate processes and memory regions and use `ReadProcessMemory`. It must not write to MultiCharts process memory or inject input.

Research candidates are evidence, not automatically trusted production values.

## Self-documenting INI versus compatibility trust

The normal Watchdog INI deliberately writes missing known settings with safe defaults. **That rule does not mean verified compatibility addresses are generated by default.**

`MCST-Compatibility.ini` contains trust-sensitive build data. A profile may have schema defaults for non-sensitive metadata, but a verified internal address or offset must come from actual build verification, not configuration normalization.
