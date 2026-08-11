# MCST 1.113 Production Release

MCST-Watchdog 1.113 extends the MultiCharts Internal Compatibility Framework to the production Tracker snapshot path and adds the tooling needed to bring a verified profile for a new MultiCharts Tracker build into service without recompiling MCST.

The release also corrects the Tracker Bridge Host compatibility-database path construction used by the new framework and removes a Watchdog variable-shadowing warning. These are build-quality fixes; Bridge Protocol V2 and the Tracker compatibility behavior are unchanged.

## Tracker compatibility profiles

MCST Tracker Bridge now uses the shared `MCST-Compatibility.ini` database for build-dependent `ATOnPTracker.dll` layout data.

A verified Tracker profile is selected only when its exact `ATOnPTracker.dll` PE timestamp and image size match the running module and the required Tracker layout fields are complete and valid.

Unknown builds fail safely. The production Tracker reader does not substitute guessed values for a new module fingerprint.

## Automatic Tracker candidate record

When a new `ATOnPTracker.dll` fingerprint has no complete verified Tracker profile, the Bridge can create a disabled `Candidate.ATOnPTracker-*` section containing the detected fingerprint and empty research fields.

Candidate sections are never selected by production code. Their purpose is to provide a self-documenting starting point for Developer Mode research.

## New detected MultiCharts diagnostics

`[DetectedMultiCharts]` now records both compatibility domains when available:

```ini
charting_pe_timestamp=...
charting_image_size=...
atonptracker_pe_timestamp=...
atonptracker_image_size=...
autotrading_compatibility_profile=...
tracker_compatibility_profile=...
```

The human-readable MultiCharts version remains diagnostic information; exact module fingerprints authorize build-dependent internal values.

## Developer compatibility toolbar

The compact Developer Mode toolbar now provides six actions:

```text
AT Start | AT Capture | AT Finish | Tracker Capture | Open Compat | Reload Compat
```

`Tracker Capture` requests the existing passive Tracker research bundle. `Open Compat` opens the shared compatibility database. `Reload Compat` forces Watchdog to re-evaluate AutoTrading compatibility and requests a new Tracker snapshot; the Bridge resolves Tracker profile data on each snapshot request.

## Clearer Tracker diagnostics

Tracker failures now distinguish conditions such as:

- `ATOnPTracker.dll` not loaded;
- no verified Tracker profile for the detected fingerprint;
- incomplete matching profile;
- CATPTTabView object not found for the selected profile;
- profile layout read failure;
- verified FlexGrid identity mismatch.

This makes a compatibility/read failure distinguishable from an actual critical message found in Recent Logs.

## Tracker Snapshot status

The Tracker Snapshot health row now evaluates Accounts, Open Positions, and Recent Logs together. A partially readable snapshot is shown as a warning rather than being logged as a fully successful Tracker read.

## Bridge identity

```text
MCST Tracker Bridge product version: 1.0
Internal build:                     V156
Bridge protocol:                    V2
```

The Bridge implementation changed because it now consumes Tracker compatibility profiles and publishes Tracker fingerprint/profile metadata. The wire protocol remains V2. Watchdog 1.113 rejects an older Tracker Bridge internal build for production snapshots so the new compatibility safeguards cannot be bypassed by a stale loaded DLL.

## Documentation

README, Installation Guide, User Guide, Developer Guide, Architecture, Compatibility Guide, Coding Standard, build information, changelog, Tracker Bridge README, and release validation have been updated for the generalized compatibility workflow.
