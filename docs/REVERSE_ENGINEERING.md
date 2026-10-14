# Reverse engineering workflow

Static analysis of the original GOG build (gameId 1421210170) for the Linux port. The original install files are never modified; all analysis output lives in gitignored `work/`.

## Documents

| file | content |
|---|---|
| `docs/ENGINE_IDENTIFICATION.md` | one-page conclusions (engine, compiler, APIs) |
| `docs/reverse-engineering/ENGINE_IDENTIFICATION.md` | evidence for those conclusions |
| `docs/reverse-engineering/EXECUTABLE_ANALYSIS.md` | PE layout, imports, Rich header, toolchain, startup sequence, flag files |
| `docs/reverse-engineering/FUNCTION_MAP.md` | named functions (addr, name, confidence, evidence) + source-file address ranges |
| `docs/reverse-engineering/DATA_STRUCTURES.md` | globals and struct layouts (Sys, D3D wrapper, Config, CameraState, CamMan, View3D, DynArray) |
| `docs/reverse-engineering/RENDERING_PIPELINE.md` | device init, frame loop, scene rendering, shaders, effects, present |
| `docs/reverse-engineering/CAMERA_SYSTEM.md` | camera objects, update/easing, projection constants, FOV-fix mapping |
| `docs/CAMERA_AND_WIDESCREEN.md` | (ModsResearch) projection maths and community fixes |

Confidence labels: **CONFIRMED** (visible in disassembly/decompile: API call, string, constant, data), **HIGH CONFIDENCE** (strong contextual evidence), **HYPOTHESIS**, **UNKNOWN**. Decompiler output is never treated as verified source.

## Tools

- Ghidra 12.1.2 headless (`/opt/ghidra/support/analyzeHeadless`), Java postScripts in `tools/ghidra/` — see `tools/ghidra/README.md`.
- `pefile` (Rich header, imports, sections), `objdump -D -b binary -mi386 -Mintel` for exact instruction checks, Python for string/immediate cross-reference scans.

## Reproduce

```sh
tools/ghidra/run_headless.sh import    # fresh project work/ghidra/joan.gpr + full analysis + exports (~4 min)
tools/ghidra/run_headless.sh reexport  # after editing tools/ghidra/names.tsv (~70 s)
```

Outputs in `work/ghidra/export/`: `joan_decomp.c` (18,802 functions, each headed `// FUNC <addr> <name>`), `functions.tsv` (callers include calls routed through ILT thunks), `strings.tsv`, `imports.tsv`, `rtti.tsv`; sub-directories `JoanofArc.exe/`, `VSetting.exe/`, `v3d_87.dll/` with the same set (`decomp.c`).

## Practical notes for joan.exe

- Incremental linking: almost every call is `call <ILT stub>` → `jmp <function>`. In the decompile these show as `thunk_<name>()`; grep for both `FUN_xxxxxxxx` and `thunk_FUN_xxxxxxxx`.
- `__thiscall` everywhere: Ghidra prints `in_ECX` and drops `this` at call sites. To learn which global object a call acts on, disassemble the call site and read the `mov ecx, imm32` (e.g. `WinMain` → `Sys` `0xa50088`, `Config` `0xa57fb8`).
- Debug-style codegen (`/ZI`): `__LINE__` arguments appear as `DAT_xxx + n` (per-function line base).
- Source attribution: functions that pass `"d:\ww\<file>.cpp"` to `Mem_add`/`Mem_del`/errors identify their object file; ranges are listed in `FUNCTION_MAP.md`.
- Renames are made only through `tools/ghidra/names.tsv` so that a fresh `import` reproduces the annotated project.
- Ghidra projects lock: other agents use private projects under `work/ghidra/<Agent>/`.

## Open gaps

- Unit/character, item and world structures are only partially mapped (DATA_STRUCTURES.md → Unit).
- Exact semantics of config detail fields `+0x92..+0xb4`, shadow/reflection mode values, and the role of camera indices other than 1 need runtime tracing (Wine + logging, or the DynamicRef work).
- Scripted camera FOV values (`CameraState+0x20 ≠ 1.0`) are UNKNOWN.
- Resource container formats (`.RES`, `.SMO`, `.AM*`) are mapped to their loader functions only; formats are documented by the format agents.
