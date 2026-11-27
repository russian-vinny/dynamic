# Dynamic source — agent instructions (Vinny's Afterflow vault memory)

This repository is the source for the **Dynamic** Geometry Dash mod (`dynamic`, v2.2.7).

## Memory vault

Before changing code, read:

`C:\Users\fried\OneDrive\Desktop\Vinny's Afterflow\Afterflow.md`

The vault keeps its historical folder name `Vinny's Afterflow` (mandatory global memory path) even though the released mod identity is now `dynamic` / `Dynamic`. It holds architecture, constants, stage definitions, decisions, bug history, user preferences, and the build/install runbook. It is the source of truth for context.

## Mandatory vault update

Every time you learn something new — behavior, constant, bug, root cause, build result, gotcha, user preference — update the vault in the same turn before finishing. Follow `C:\Users\fried\OneDrive\Desktop\Vinny's Afterflow\AGENTS.md`.

At minimum:

1. Append a dated line to `C:\Users\fried\OneDrive\Desktop\Vinny's Afterflow\09 Log\Learning Log.md`.
2. Update the affected reference/architecture/bug note.
3. Add or update a session note in `05 Sessions/`.
4. Update `changelog.md` in this repo and `09 Log/Changelog.md` in the vault in the same turn. Keep the real mod: no recreation, no cube changes.

## Build and install

```bat
cmd /c C:\Users\fried\AppData\Local\Temp\opencode\build-dynamic-clang.bat
cmake --build build-dynamic --target Dynamic_PACKAGE
```

- Artifact: `build-dynamic\dynamic.geode`
- Install while GD is closed, then purge `geode\unzipped\dynamic`.
- Record the new SHA-256 in the vault's `01 Project/Build and Install.md`.

## Constraints

- Do not trade accuracy for speed.
- The selected tick value must be the runtime value.
- Keep the released identity `dynamic` / `Dynamic` / `vinny` / `@v1nny0` / `2.2.7` unless a release bump is requested.
- Read the vault's `07 User Preferences/User Preferences.md` and `01 Project/Requirements.md` before changing behavior.
