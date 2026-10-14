# Ghidra headless tooling

Canonical project: `work/ghidra/joan.gpr` (gitignored). Exports: `work/ghidra/export/`.

- `run_headless.sh import` — fresh import of `joan.exe`, `JoanofArc.exe`, `VSetting.exe`, `v3d_87.dll` with full auto-analysis, then names + exports (~4 min on 32 cores).
- `run_headless.sh reexport` — reuse the project (`-process -noanalysis`), apply `names.tsv` to joan.exe, re-export everything (~70 s).

Scripts (Ghidra 12 Java postScripts, no PyGhidra needed):

| script | output |
|---|---|
| `ApplyNames.java <names.tsv>` | renames joan.exe functions (`addr<TAB>name<TAB>…`) |
| `ExportRtti.java <outDir>` | `rtti.tsv` (`.?AV` type descriptors + Ghidra vftables) |
| `ExportAll.java <outDir>` | `joan_decomp.c` (every function, `// FUNC <addr> <name>` header), `functions.tsv`, `strings.tsv`, `imports.tsv`; other programs go to `<outDir>/<program>/` with `decomp.c` |

Never open the project while another agent's headless run holds the lock; use a private project under `work/ghidra/<Agent>/` instead.
