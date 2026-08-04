# MCST Coding Standard

## Language

All source code, comments, UI text, log messages, commit messages, and project documentation must be written in English.

## Formatting

- Use UTF-8 encoded text files.
- Use four spaces for indentation; do not use tabs.
- Use braces consistently, including for multi-line control flow.
- Keep functions focused and move reusable behavior into named modules.

## Naming

- Types and classes: `PascalCase`.
- Functions and methods: `PascalCase` for public APIs and existing project conventions.
- Local variables and members: `camelCase`.
- Compile-time constants: `kPascalCase`.
- File names should match the primary type or responsibility.

## C++ practices

- Prefer RAII and standard-library ownership types.
- Avoid raw `new` and `delete`.
- Prefer `enum class`, `constexpr`, and strongly typed values.
- Avoid magic numbers; define named constants.
- Validate external input and fail safely.
- Do not write to MultiCharts process memory.

## Documentation

Public interfaces should use Doxygen-compatible comments when the contract is not self-evident. Comments should explain intent, constraints, and safety properties rather than restating code.
