# MCST Coding Standard

## Language

All source code, comments, identifiers, UI text, log and error messages, project documentation, release notes, and Git commit messages must be written in English.

## Formatting

- Use UTF-8 encoded text files.
- Use four spaces for indentation; do not use tabs.
- Use braces consistently for multi-line control flow.
- Keep functions focused and move reusable behavior into named modules.
- Prefer readable intent over compressed one-line implementation in production code.

## Naming

- Types and classes: `PascalCase`.
- Public functions/methods: follow the established `PascalCase` project convention.
- Local variables and members: `camelCase`.
- Compile-time constants: `kPascalCase`.
- File names should match the primary type or responsibility.

## C++ practices

- Prefer RAII and standard-library ownership types.
- Avoid raw `new` and `delete`.
- Prefer `enum class`, `constexpr`, and strongly typed values.
- Avoid unexplained magic numbers; use named constants or verified profile data.
- Validate external input and fail safely.
- Keep UI work on the UI thread and isolate long-running I/O where appropriate.
- Do not write to MultiCharts process memory.

## Production safety

When data is uncertain, prefer `UNKNOWN` over an inferred healthy state.

Any production read of MultiCharts internal structures must be authorized by an exact verified compatibility profile when the value is build-dependent. Do not reuse an offset, vtable RVA, signature, or other internal value after the relevant fingerprint changes unless it has been verified for that build.

Human-readable product versions are diagnostic information, not internal-memory authorization.

## Compatibility data

Build-dependent internal values belong in the compatibility framework rather than being scattered through production reader code. Research code may contain clearly labelled historical or candidate values for investigation, but those values must not silently become production defaults.

If a future internal reader needs additional profile fields, extend the compatibility schema and document the verification procedure.

## Bridge stability

MCST Tracker Bridge product version, internal build identifier, and protocol version are separate concepts.

Do not change Bridge Protocol V2 for a Watchdog-only feature. Change the bridge protocol only when the information crossing the MultiCharts/Watchdog boundary must change, and document compatibility implications explicitly.

## Configuration rules

MCST uses a self-documenting INI model:

- add every normal known setting to centralized normalization;
- provide a safe built-in default;
- write a missing known setting to the INI file;
- validate/normalize Boolean and bounded numeric settings;
- log configuration normalization without exposing secrets;
- do not invent user-specific addresses, account identifiers, credentials, or verified compatibility data.

Program-generated diagnostic sections must be clearly distinguishable from user settings.

## Secret handling

Treat SMTP passwords, App Passwords, tokens, and similar credentials as secrets.

Do not intentionally include secrets in:

- log output
- diagnostic text
- Status Reports
- email bodies
- crash/startup diagnostics
- research reports

Do not echo a secret value in an error message. Prefer messages that identify the setting or operation only.

## UI separation

Normal production controls and Developer Mode controls must remain visually and functionally distinct. Developer research buttons use the compact Developer toolbar layout and are hidden when Developer Mode is disabled.

New Developer controls should use the centralized Developer toolbar geometry rather than ad-hoc coordinates whenever practical.

## Logging

A useful log entry identifies:

- the subsystem;
- the operation or startup stage;
- the failure or resulting state;
- enough non-secret context to diagnose the problem.

Avoid noisy repeated messages when deduplication or state transitions provide better operational information.

## Error handling

- Guard startup paths so failures can be diagnosed.
- Do not let local archive/report-file failures suppress unrelated email delivery.
- Keep independent schedulers and alert channels independent.
- Preserve last confirmed state when that is more accurate than replacing it with an unsupported assumption.

## Documentation

Public interfaces should use Doxygen-compatible comments when the contract, ownership, safety rule, or failure behavior is not self-evident. Comments should explain intent and constraints rather than restating syntax.

## Release quality

- Build `Release|x64`.
- Keep `/MT` explicit for production projects.
- Do not ship a Debug solution configuration in the production source package.
- Resolve compiler warnings before release unless a specific exception has been reviewed and documented.
- Run `Tools\Validate-Release.ps1` before packaging.
- Perform a real Windows/MSVC rebuild and runtime verification before publishing binaries.
