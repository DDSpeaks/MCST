# MultiCharts Internal Compatibility Framework

## Purpose

MCST reads selected MultiCharts internal structures that are not public stable APIs. Those structures can move when MultiCharts is updated. The compatibility framework prevents a previously valid address, offset, vtable RVA, or layout assumption from being silently reused for a different module build.

The production policy is simple: **exact verified profile or safe failure**.

## When to use Developer Mode

Leave Developer mode off during normal monitoring. Its controls are normally
needed only after a MultiCharts update, after the exact `Charting.dll` or
`ATOnPTracker.dll` fingerprint changes, or when a developer specifically asks
for compatibility evidence. A product-version change is the common trigger,
but the exact module fingerprint is authoritative and can change independently.

An `UNKNOWN` or unavailable reader after such a change is an intentional safe
failure. Research output supports verification; it does not authorize a
candidate automatically.

## Compatibility database

The shared database is:

```text
C:\MCExtras\MCST-Compatibility.ini
```

Watchdog and the Tracker Bridge both use this file. The file is separate from `MCST-Watchdog.ini` because verified internal build data is not ordinary user configuration.

The database metadata is normalized safely:

```ini
[Compatibility]
schema_version=2
unknown_build_policy=reject
```

MCST may create known schema/default metadata, but it never invents verified offsets or addresses for an unknown MultiCharts build.

## Human-readable version versus fingerprint

Watchdog detects the MultiCharts executable product/file version for diagnostics. It also records module fingerprints in the generated `[DetectedMultiCharts]` section:

```ini
[DetectedMultiCharts]
product_version=...
file_version=...
charting_pe_timestamp=0x...
charting_image_size=...
atonptracker_pe_timestamp=0x...
atonptracker_image_size=...
autotrading_compatibility_profile=...
tracker_compatibility_profile=...
```

A product version is **not** sufficient proof for internal-memory compatibility. Profile selection is based on the exact relevant module fingerprint.

## Independent compatibility consumers

A `Profile.*` section can contain data for AutoTrading, Tracker, or both. The consumers evaluate only the fields relevant to them.

### AutoTrading / Charting.dll

Watchdog matches:

```ini
charting_pe_timestamp=0x...
charting_image_size=...
```

A complete AutoTrading profile also supplies:

```ini
strategy_vtable_rva=0x...
autotrading_offset=0x...
```

The currently bundled verified AutoTrading profiles are:

```ini
[Profile.MC16-Charting-6A5684BF]
name=MC16 verified Charting.dll 0x6A5684BF
enabled=true
charting_pe_timestamp=0x6A5684BF
charting_image_size=18493440
strategy_vtable_rva=0xA457B8
autotrading_offset=0x142
verification=Controlled research session: 8/8 exact toggle responses

[Profile.MC17-Charting-6AB57EE6]
name=MC17 verified Charting.dll 0x6AB57EE6
enabled=true
charting_pe_timestamp=0x6AB57EE6
charting_image_size=18624512
strategy_vtable_rva=0xB74CF0
autotrading_offset=0x18
verification=Controlled dynamic research: 4/4 exact responses with both toggle directions
```

Version 1.20.16 retains the MC16 profile unchanged and adds the independently
fingerprinted MC17 profile beside it. The MC17 field was the only candidate to
match every one of four controlled transitions, with two `1 -> 0` and two
`0 -> 1` responses and no unchanged or ambiguous transition. For any later
unknown build, `AT Start` performs the retained passive candidate scan and
records the exact fingerprint. Research output never enables an unknown build
automatically.

Legacy `pe_timestamp` and `image_size` aliases remain readable for compatibility with older databases, but new profile data should use the canonical `charting_*` names.

### Tracker / ATOnPTracker.dll

The Bridge matches:

```ini
atonptracker_pe_timestamp=0x...
atonptracker_image_size=...
```

A complete externally verified production Tracker profile requires:

```ini
tracker_tabview_vtable_rva=0x...
tracker_accounts_page_offset=0x...
tracker_open_positions_page_offset=0x...
tracker_logs_page_offset=0x...
tracker_grid_member_offset=0x...
tracker_rows_offset_1=0x...
tracker_rows_offset_2=0x...
tracker_gettext_slot=...
tracker_flexgrid_vtable_rva=0x...
tracker_gettext_rva=0x...
```

V175 and later can additionally use this optional verified layout value:

```ini
tracker_position_history_page_offset=0x...
```

Its absence does not invalidate an existing production profile; it only makes
the monthly Realized P/L enrichment unavailable.

For the exact verified V147 fingerprint (PE timestamp `0x6A5694FB`, image size
`3534848`), Bridge V176 and newer also embed CATPTTabView primary vtable RVA
`0x1D78C8`. A targeted recovery hit is not sufficient by itself: the object
must pass structural scoring and expose a secondary vtable, a Tracker layout
signature, or at least five credible page pointers. Other fingerprints do not
inherit this anchor.

Bridge V181 adds the independently captured MC17 fingerprint (PE timestamp
`0x6AB58F4C`, image size `3534848`) with CATPTTabView primary vtable RVA
`0x1D78D8`. The passive capture also confirmed that the Accounts and Open
Positions extractor RVAs remain `0x10FAE6` and `0x10FF56`. MC16 and MC17 are
selected strictly by their own fingerprints. A later unknown MC17 subversion
receives no fixed vtable RVA; it must pass bounded RTTI discovery and the same
structural checks, otherwise Tracker data remains unavailable.

Optional research metadata can include:

```ini
tracker_accounts_extractor_rva=0x...
tracker_open_positions_extractor_rva=0x...
```

Example verified Tracker section shape:

```ini
[Profile.MC16-Tracker-EXAMPLE]
name=Verified ATOnPTracker build
enabled=true
atonptracker_pe_timestamp=0x12345678
atonptracker_image_size=1234567
tracker_tabview_vtable_rva=0x...
tracker_accounts_page_offset=0x...
tracker_open_positions_page_offset=0x...
tracker_position_history_page_offset=0x...
tracker_logs_page_offset=0x...
tracker_grid_member_offset=0x...
tracker_rows_offset_1=0x...
tracker_rows_offset_2=0x...
tracker_gettext_slot=...
tracker_flexgrid_vtable_rva=0x...
tracker_gettext_rva=0x...
verification=Describe the controlled verification performed for this exact build
```

The example fingerprint and blank values above are placeholders only and must not be copied as real profile data.

## Candidate sections

If the Bridge sees a new `ATOnPTracker.dll` fingerprint without a complete verified profile, it can create a disabled section similar to:

```ini
[Candidate.ATOnPTracker-12345678-1234567]
candidate_created=true
enabled=false
name=Unverified ATOnPTracker build
atonptracker_pe_timestamp=0x12345678
atonptracker_image_size=1234567
tracker_tabview_vtable_rva=
tracker_accounts_page_offset=
tracker_open_positions_page_offset=
tracker_position_history_page_offset=
tracker_logs_page_offset=
tracker_grid_member_offset=
tracker_rows_offset_1=
tracker_rows_offset_2=
tracker_gettext_slot=
tracker_flexgrid_vtable_rva=
tracker_gettext_rva=
tracker_accounts_extractor_rva=
tracker_open_positions_extractor_rva=
verification=UNVERIFIED - Developer Mode research required before creating an enabled Profile.* section
```

Candidate sections are intentionally **never selected** by the production reader. They are a self-documenting record of the detected build and a workspace for research.

Do not simply rename a candidate to `Profile.*` or set `enabled=true`. Each required value must first be discovered and verified for the exact fingerprint.

## Tracker profile verification workflow

1. Confirm the detected `ATOnPTracker.dll` PE timestamp and image size.
2. Keep the production Tracker reader blocked while the build is unverified.
3. Enable Developer Mode.
4. Use **Tracker Capture** to collect the passive research bundle.
5. Identify the exact CATPTTabView and CFlexGridImpl identities and the required layout values.
6. Verify Accounts, Open Positions, and Recent Logs across controlled states.
7. Verify that row counters, GetText slot/function, page pointers, and grid identity remain consistent.
8. Create an enabled `Profile.*` section for the exact fingerprint.
9. Add a meaningful `verification=` note describing the evidence.
10. Use **Reload Compat**.
11. Confirm the selected Tracker profile is reported and all three sections read without SEH failures or identity mismatches.

A single plausible address or one successful read is not sufficient verification.

## Runtime reload

The Bridge reads Tracker compatibility data from `MCST-Compatibility.ini` when each production snapshot is requested. Watchdog's **Reload Compat** action forces a fresh Watchdog refresh and bypasses the AutoTrading compatibility/read cache.

Therefore, adding a newly verified compatibility profile does not require recompiling MCST. Replacing the Bridge DLL itself still requires MultiCharts to reload that DLL, normally by restarting MultiCharts.

## Unknown-build behavior

When an exact profile is not available:

- AutoTrading remains `UNKNOWN` for an unverified `Charting.dll` build;
- Tracker production reads remain unavailable for an unverified `ATOnPTracker.dll` layout;
- the Dashboard and Status Report expose the detected fingerprint and compatibility diagnostic;
- Developer Mode remains available for controlled research.

MCST must never convert an unknown build into a healthy state by guessing old internal values.

## Scope of the framework

The framework is intended for **production build-dependent values**. Research tools can contain clearly labelled probes or historical research constants when needed to discover a new layout, but those values do not authorize production reads.

Future MC-internal readers should extend the profile schema rather than introduce new silent hard-coded production assumptions.
