# Camera, projection and widescreen

Scope: how the original `joan.exe` builds its 3D projection, how the community FOV fix and Camera Fix Patch change it, and the rules our native renderer must implement.
All addresses are for **our GOG `joan.exe`** (6,677,952 bytes, SHA-256 `b25e2154af3ffc876545af60a2c8b90380d4327541cd38841ef327411c3b2fa5`, ImageBase `0x400000`). For `.text` file offset = VA − `0x400000` (section VA == raw offset == `0x1000`).
Decompiler output quoted here is Ghidra 12 output from `work/ghidra/export/joan_decomp.c`; it is not verified source.

Tools used: `objdump -d -M intel` on `joan.exe`, Python byte search (`re.finditer` over the raw file), Ghidra export, the scratch VCDIFF dumper `work/third_party_ref/vcdiff_dump.py`.

## 1. Original projection path

### 1.1 `0x7674A4` — perspective matrix builder (D3DXMatrixPerspectiveFovLH equivalent)

HIGH CONFIDENCE (instruction-level reading; the game links D3DX8 statically, so there is no import to name it).

`stdcall (D3DMATRIX* out, float fovy, float aspect, float zn, float zf)`, `ret 0x14`:

```
7674aa fld  [ebp+0xC]          ; fovy
7674b0 fmul [0x9E2FF0]         ; 0.5f (CONFIRMED: bytes at VA 0x9E2FF0 = 0.5f)
7674cb fsincos
7674cd fstp [edx]              ; edx = &arg fovy  -> arg slot now holds cos(fovy/2)
7674cf fstp [eax]              ; eax = &local[-4] -> sin(fovy/2)
7674d1 fld  [ebp+0xC]          ; cos(fovy/2)                     <- FOV fix hook #1
7674d7 fdiv [ebp-4]            ; yScale = cot(fovy/2)
7674da fld  [ebp+0x10]         ; aspect                           <- FOV fix hook #2
7674dd fdivr st,st(1)          ; xScale = yScale / aspect
7674df fstp [eax]              ; m11 = xScale
...    m22 = yScale, m33 = zf/(zf-zn), m34 = 1, m43 = -zn*zf/(zf-zn), all other 0
```

Siblings in the same static D3DX block: `0x767410` (right-handed variant: `m34 = -1`, `zn-zf`; no callers), `0x767723` (orthographic, callers `0x5E5729`, `0x61440D`).

Callers of `0x7674A4` (CONFIRMED by scanning every `E8 rel32` in `.text`; there are no thunk or pointer references):

| Call site | Function | Arguments |
|---|---|---|
| `0x6143BF` | `FUN_006141A0` — per-view camera update | `fovy = atan(this[+0xC0])` (CRT `atan` at `0x7B2184`: `fld1; fpatan`), `aspect = (float)this[+0x20] / (float)this[+0x24]`, `zn = this[+0x44]`, `zf = this[+0x48]` (copied from `+0x4C`) |
| `0x5BECE5` | `FUN_005BEC90` — thiscall `SetPerspective(fov, aspect, zn, zf)`; stores the four floats at `+0xBC..+0xC8`, matrix at `+0xCC` | only reached from `0x5BEB06` (via ILT thunk `0x404E17`) with `(0x3F490FDB = π/4, 1.0, 1.0, 1000.0)` |

`FUN_006141A0` details (HIGH CONFIDENCE, disassembly `0x61436A..0x614440` + decompile):
- `this[+0xA0]` selects the projection: `0` perspective (above), `1` orthographic with `width = this[+0x20]*this[+0xC4]`, `height = this[+0x24]*this[+0xC4]`, `zf = this[+0x50]`.
- `this[+0x10], [+0x14], [+0x20], [+0x24]` are copied into a viewport block at `+0x2AE` together with `MinZ = 0`, `MaxZ = 1.0f` → they are viewport X, Y, Width, Height in pixels (HIGH CONFIDENCE; layout matches `D3DVIEWPORT8`).
- The projection is stored at `this+0x12E` and `this+0x26E`; view and inverse-view matrices at `+0x1EE`, `+0x22E`, `+0xEE`.

So the game's projection rule is: **vertical FOV = `atan(k)` with `k = this[+0xC0]`; aspect = viewport W/H.** The FOV fix's constant check (below) shows that in gameplay `cos(fovy/2) == 0.9238795f`, i.e. `fovy = 45°` (`k = 1.0`). Native default: **vertical FOV 45°** (CONFIRMED for the value the fix keys on; HYPOTHESIS that no other gameplay camera uses a different `k` — needs a runtime trace of `this[+0xC0]`).

### 1.2 Hard-wired 1024×768 display profile

HIGH CONFIDENCE. `.data` VA `0xA016A0` is an array of exactly one 0x78-byte record: accessors `0x4B08A0`, `0x4B08F0`, `0x4B0930` accept only index 0 (`-1 < i && i < 1`) and `0x4B0930` copies it into the live global `0xA48858`.

| Offset | VA | Value (ours) | Value (HD Update exe) | Meaning |
|---|---|---|---|---|
| +0x04 | `0xA016A4` | 1024 | 1920 | screen width |
| +0x08 | `0xA016A8` | 768 | 1080 | screen height |
| +0x0C | `0xA016AC` | 16 | 16 | HYPOTHESIS: colour depth |
| +0x18/+0x1C | `0xA016B8` | 1023 / 767 | 1919 / 1079 | max x / max y |
| +0x30/+0x34 | `0xA016D0` | 1023 / 767 | 1919 / 1079 | max x / max y |
| +0x38/+0x3C | `0xA016D8` | 1024 / 768 | 1920 / 1080 | width / height |

The remaining fields (`814, 662, 1013, 761, 200, 100, …`) are unchanged by the HD patch; HYPOTHESIS: fixed UI rectangles in 1024×768 pixel space.
UI art is also authored for this size (strings `MENU1024`, `LD1024BG`, `LD1024CP`, directory string `INT8X6\` at `0x9D9BE4`).
Consequence: the unmodified game always renders a 4:3 frame (aspect `1024/768 = 1.3333334f`, exactly the value the FOV fix tests for). Widescreen on Windows is obtained by forcing the output resolution in a wrapper (dgVoodoo 2), which **stretches** the 4:3 frame.

## 2. AlphaYellow "Wars and Warriors: Joan of Arc – FOV Fix" v1.0

- Source: <https://github.com/alphayellow1/AlphaYellowWidescreenFixes/tree/main/source/fixes/WarsAndWarriorsJoanOfArcFOVFix> (`dllmain.cpp`, 287 lines), cloned at commit `99a5f300685bb9a505eea7cf12e7dad3ee61875f` (2026-09-19) into `work/third_party_ref/AlphaYellowWidescreenFixes/`. License: MIT (repo `LICENSE`, "Copyright (c) 2024 AlphaYellow").
- Binary: GitHub release tag `warsandwarriorsjoanofarc`, asset `Wars.and.Warriors.Joan.of.Arc.-.FOV.Fix.v1.0.rar` (323,040 bytes, SHA-256 `3f5918f9757c0461b1ae1e1ce351b5be681e8cdd585f756e6fabc7020df4597a`, published 2025-02-28), also on PCGamingWiki: <https://community.pcgamingwiki.com/files/file/3309-wars-and-warriors-joan-of-arc-fov-fix>. Contents: `WarsAndWarriorsJoanOfArcFOVFix.asi` (1,072,128 bytes, PE i386, link time 2025-02-27, SHA-256 `716c884b6a3b94c8c7f3b2e2e17970125d6380dd9d8db595d5644ce2c75be3b4`; contains the same two signature strings as the source — CONFIRMED with `strings`) and `WarsAndWarriorsJoanOfArcFOVFix.ini`:
  ```ini
  [FOVFix]
  Enabled=true
  [Settings]
  Width=0        ; 0 = desktop resolution (Util::GetPhysicalDesktopDimensions)
  Height=0
  FOVFactor=1.0
  ```
- Mechanism: **runtime mid-function hooks** (safetyhook `create_mid`) installed from a thread spawned in `DllMain`; loaded by ThirteenAG's Ultimate ASI Loader as a 32-bit `winmm.dll` proxy (the game imports WINMM). No file on disk is modified. Game detection is by executable **name only** (`joan.exe`, case-insensitive); there is no hash or version check. Dependencies: safetyhook, spdlog, inipp, Ultimate ASI Loader.

### 2.1 Hooks and their location in our `joan.exe`

| # | Signature (first match, `Memory::PatternScan`) | Hook address | Our `joan.exe` | HD Update `joan.exe` (6,418,432 B) |
|---|---|---|---|---|
| 1 | `D9 45 0C 8B 45 08 D8 75 FC D9 45 10 D8 F9 D9 18` | match + 0 | **CONFIRMED** unique: file `0x3674D1`, VA `0x7674D1` | CONFIRMED unique: file/VA-offset `0x366403` (VA `0x766403`) |
| 2 | `FC D9 45 10 D8 F9` | match + 1 | **CONFIRMED** unique: match at file `0x3674D9`, hook VA `0x7674DA` | CONFIRMED (inside the hook-1 match) |

Both hooks fall inside the single function `0x7674A4` (§1.1). The fix therefore affects **every** perspective matrix the game builds through it — both callers in the table above.

Hook 1 (`0x7674D1`, original instruction `fld dword [ebp+0xC]`): the 3 bytes are overwritten with `90 90 90`, then a mid-hook is placed at the same address. The callback reads `c = [ebp+0xC]` (which at this point is `cos(fovy/2)`, not the FOV angle) and executes inline `fld` of
```
c' = (c == 0.9238795042f) ? c / FOVFactor : c
```
replacing the NOP-ed load. Effect: `yScale' = cot(fovy/2) / FOVFactor`, i.e. `tan(vfov'/2) = FOVFactor · tan(22.5°)` — only for the 45° camera.

Hook 2 (`0x7674DA`, original `fld dword [ebp+0x10]`): the callback rewrites the stack argument in place before the original `fld` runs:
```
aspect' = (aspect == 1.333333373f) ? Width / Height : aspect
```

### 2.2 Resulting behaviour

- **Resolution:** the fix does not change the game's render resolution or window. `Width`/`Height` only describe the final display (normally a dgVoodoo-forced output resolution). PCGamingWiki lists exactly that combination for widescreen/ultrawide/4K/multi-monitor.
- **Aspect / horizontal FOV:** projection `xScale = yScale / (W/H)`. After the wrapper stretches the 4:3 frame to W:H the geometry is undistorted and the result is **Hor+**: vertical FOV kept at 45°, horizontal FOV = `2·atan(tan(22.5°)·W/H)`.
- **Vertical FOV:** unchanged unless `FOVFactor ≠ 1`; then `vfov = 2·atan(FOVFactor·tan(22.5°))` (1.1 → 48.99°, 1.25 → 54.75°).
- **Projection matrix fields touched:** `m11` (via aspect and FOV) and `m22` (via FOV). `m33`, `m34`, `m43` (depth) untouched.
- **Camera state / position / orbit:** untouched.
- **Mouse interaction:** untouched. HYPOTHESIS: picking stays consistent because the game stores and reuses the same patched matrix (`+0x12E`/`+0x26E`) for unprojection; the wrapper rescales cursor coordinates.
- **Viewport scaling / UI placement:** untouched. HUD and menus remain 1024×768 art stretched by the wrapper.

### 2.3 Limitations and risks (from the source)

- Exact float equality: any camera whose `cos(fovy/2)` or aspect differs by one ULP is skipped. With the HD Update's 1920×1080 exe the aspect is already `1.7777778f`, so hook 2 does nothing there (HYPOTHESIS: then the HD exe is already Hor+ for 16:9 but still hard-wired to one resolution).
- `FOVFactor` also applies to the ctor projection set at `0x5BEB06` (45°, aspect 1.0), whose consumer is UNKNOWN.
- Hook 1 leaves its result on the x87 stack from inside the C++ callback via `_asm fld`; correctness relies on the compiler and safetyhook not touching x87 state after that instruction. Not reproducible on non-x87 code paths and irrelevant to a native port.
- Requires an ASI loader and a D3D8 wrapper for widescreen; Windows/Wine only.

## 3. Camera Fix Patch (Urielik / Beniamin Gajecki)

- ModDB: <https://www.moddb.com/mods/camera-fix-patch> (released 2025-07-14). Files: <https://github.com/bgajecki/joa-camera-fix-patch> (cloned to `work/third_party_ref/joa-camera-fix-patch/`, commit `e62e9fc1`). **No license file** → all rights reserved by default.
- Mechanism: **static executable patch** distributed as two VCDIFF/xdelta3 files (LZMA secondary compression).
- Decoded with `work/third_party_ref/vcdiff_dump.py` (both patches = copy source, then 6 bytes of `0x90`, then copy rest):

| Patch | Intended source | Patched file offset | Original bytes | Instruction |
|---|---|---|---|---|
| `joan-636.xdelta` | `joan.exe` 6,672,428 B | `0x31893..0x31898` | `D8 0D B4 FB 9C 00` (ours) | VA `0x431893` `fmul dword [0x9CFBB4]` (0.2f) |
| `joan-612.xdelta` | `joan.exe` 6,418,432 B | `0x31883..0x31888` | `D8 0D 94 6B 7E 00` (HD Update exe) | VA `0x431883` `fmul dword [0x7E6B94]` (0.2f) |

- **CONFIRMED** for our build: 6,672,428 = 0x65D000 (end of sections) + 44-byte CodeView `NB10` record, i.e. our GOG exe without its Authenticode signature. Taking the first 6,672,428 bytes of our `joan.exe`, zeroing the security data directory (header offset `0x1B8`, 8 bytes) and the PE checksum (`0x178`), and writing the six NOPs gives Adler-32 `0x3162CAF7`, exactly the target checksum stored in `joan-636.xdelta`. The patch therefore applies to an unsigned copy of the GOG exe (xdelta3 itself does not check the source).
- `joan-612`: offset, opcode and constant match the HD Update `joan.exe`, but applying it there does not reproduce the stored Adler-32 (`0x512FA177` vs `0xC8B19F45`) — HIGH CONFIDENCE same code, source file differs elsewhere.

Patched code (`FUN_00431790`, called from `0x43118D` via thunk `0x40437C`; HIGH CONFIDENCE reading):
```
t     = (FUN_004322D0() + 1.0) * 0.7 + 0.2      ; FUN_004322D0 returns float at obj+0xE1 (HYPOTHESIS: mouse-sensitivity option)
k     = flag(this+0xC4) ? t * 0.4 : t * 0.2     ; patch removes "* 0.2" -> k = t in that branch (5x)
delta = (int)(mouseDelta[0xA516F4] * k)         ; __ftol truncation, clamped to ±45
yaw   = (yaw ± delta) wrapped to [0,360)        ; applied for unit action states 0x1C/0x1D/0x49
```
So the patch multiplies the default-mode turn rate by 5. Because the per-update yaw step is an **integer number of degrees** truncated from `mouseDelta × k`, small mouse deltas (high polling rates) produce zero rotation in the original; with `k` five times larger far fewer deltas truncate to zero. This matches the PCGamingWiki issue "Unresponsive camera controls — reduce the mouse polling rate". HIGH CONFIDENCE on the arithmetic; the meanings of `0xA516F4` (int mouse delta) and the `+0xC4` flag are HYPOTHESIS.

## 4. Native reimplementation requirements

Projection (both OpenGL and Vulkan back ends, one shared function):
1. Base vertical FOV `v0 = atan(k)` with `k` the camera's original parameter (`1.0` → 45°). Keep the original near/far (`this+0x44`, `this+0x48`) per camera.
2. **Hor+**: `aspect = viewport_width / viewport_height` of the real swapchain viewport; `tan(h/2) = tan(v0/2) · aspect`. Never stretch a 4:3 render.
3. Optional user FOV scale `s` (default 1.0) applied as `tan(v/2) = s · tan(v0/2)`, matching the community fix's `FOVFactor` semantics; clamp horizontal FOV ≤ 120°.
4. Orthographic views (`this[+0xA0] == 1`) scale width with aspect the same way (`width = height · aspect`), height from the original.
5. Expected horizontal FOV at default settings:

| Aspect | Ratio | Horizontal FOV | `m11` (= `xScale`) |
|---|---|---|---|
| 4:3 (original) | 1.3333 | 57.82° | 1.810660 |
| 5:4 | 1.2500 | 54.75° | 1.931371 |
| 16:10 | 1.6000 | 67.07° | 1.508883 |
| 16:9 | 1.7778 | 72.73° | 1.357995 |
| 21:9 (2560×1080) | 2.3704 | 88.95° | 1.018496 |
| 21:9 (3440×1440) | 2.3889 | 89.40° | 1.010601 |
| 32:9 | 3.5556 | 111.65° | 0.678998 |
| custom W×H | W/H | `2·atan(0.414214·W/H)` | `2.414214·H/W` |

   `m22 = 2.414214` (cot 22.5°) for every aspect. These values are the acceptance check for a unit test of the projection helper.
6. Aspect ratios below 4:3 (5:4, portrait) must not shrink the vertical FOV: Hor+ already handles them (horizontal narrows). Do not switch to Vert−.

UI / 2D:
7. Original UI is authored in a 1024×768 pixel space. Render it in a 4:3 "safe rectangle" of height = window height, centred horizontally (pillarbox for menus and full-screen art; letterbox for aspect < 4:3).
8. In-game HUD elements are anchored to the nearest edge/corner of the full viewport (left-anchored stays left, right-anchored moves to the right edge) and scaled by `window_height / 768`; full-screen overlays (fades, loading screens, movies `movie/*.wmv`) fill the safe rectangle or the whole screen with correct aspect, never stretched.
9. Mouse: convert window coordinates to UI space with the same transform; unproject picks with the actual Hor+ projection (no hidden 4:3 assumptions).

Camera input (from §3):
10. Accumulate yaw in floating point (sub-degree) from raw mouse deltas; reproduce the original sensitivity curve `k = ((s + 1)·0.7 + 0.2) · (flag ? 0.4 : 0.2)` degrees per count for parity, but without integer truncation so high-polling-rate mice work. Keep the ±45°/update clamp.

Test matrix (each independently): main menu, in-game HUD, tactical/overview camera, scripted cutscenes, map screen, movies — at 4:3, 5:4, 16:10, 16:9, 21:9, 32:9 and one custom ratio. Compare against reference captures of the original at 1024×768 (vertical framing must match).

The native implementation needs no ASI loader, wrapper or exe patch; it does not reuse code from either community fix (the FOV fix is MIT and could be cited, but only its formulas are needed).

### Native mouse yaw (`src/engine/game_input.cpp`, item 10)

- The game side keeps the original step: the player controller (0x431790, `controller_player.cpp`) turns `d = int(mouse_dx · k)` whole degrees per update, `k = ((s + 1)·0.7 + 0.2)·0.2` (0.18°/count at the default sensitivity), ±45° clamp in ATTACK.
- `GameInput::take()` (once per 33 ms tick) adds `raw_dx · k` to a float accumulator, hands the game the whole degrees `n = trunc(acc)` encoded as `mouse_dx = (n ± 0.5) / k` (so the game's truncation yields exactly `n`) and keeps `acc − n` for the next tick. No counts are lost: 1 count per tick for 60 ticks turns 10° (float 10.8°) instead of 0°. The game state stays deterministic (it only ever sees integer degrees); the remainder is engine state, cleared by `GameInput::reset()` (pause/load).
- Right-stick X feeds the same accumulator at 150°/s × `[controller] sensitivity` at full deflection (after the radial deadzone).
- Option `camera_fix` (default off) reproduces the Camera Fix Patch (§3): the factor `0.2` is dropped from the engine accumulator, i.e. 5× turn rate (0.9°/count at default sensitivity). Mouse only; the stick rate is unchanged.
- Riding (`+0xC4`, factor 0.4) is not implemented by the game controller yet; the accumulator uses 0.2.
