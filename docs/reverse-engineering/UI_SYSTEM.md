# Original 2D interface (en2d + interfac) — reconstruction notes

Owner: UiFrontend. Code: `src/engine/ui_{font,core,model,screens,runner}.*` (core), `menu_*.cpp`, `hud_*.cpp` (screens).
Tests: `tests/engine/ui_{font,canvas}_test.cpp` (no GPU), `tests/engine/{gl,vk}_ui_*_test.cpp` + `ui_cases_*.hpp` (screenshots,
harness `tests/engine/ui_shot.hpp`). Addresses are `JoanofArc.exe` VAs from `work/ghidra/export/joan_decomp.c`.

## 1. Where the layout comes from

| Data | Source | Confidence |
|---|---|---|
| Sprite atlas rects (what to draw) | `Resource/INTERFAC.SET`: 81 dBase III tables (Name9 RES index), one per screen/widget family (`MENU1024`, `MENU_B`, `OPTMENU`, `SAVEMENU`, `INGAME_B`, `MAINGAME`, `MINI_MP`, …). Fields `BITMAP` (sprite name), `TEXTURE` (`int8x6/<name>.dds`), `SRC_X1..SRC_Y2` (inclusive texel rect), `ROTATION` (1 = stored rotated; only `OPTMENU remap_fr`), `WIDTH`, `HEIGHT` (drawn size). `MINIINFO` (minimap world↔map rects per zoom) and `HEADER` are not sprite tables. Loaded with `FUN_006099c0("MENU1024",1,0)`. | CONFIRMED (data + loader) |
| Screen positions, draw order, animation | Hard-coded in the exe: each image widget is created with `FUN_00629660(x, y, set, "name", colour, …)` (stores x/y at widget+9/+0xb, rect end = x−1+WIDTH unless given). Widget flag +6 = coordinates authored for 640×480 and scaled to the screen (`FUN_00633e60(…, 0x280, 0x1e0)`); otherwise 1024×768 pixels. | CONFIRMED |
| Strings | `Resource/t_interf.txr` (`id|SECTION|n|text`, cp1252), fallback `t_basic.txr`. `Ui::text("MAINMENU", 1)` = "New Campaign". | CONFIRMED |
| Fonts | `FONT/*.fin` + `int8x6/<same>.dds`, and "grid" fonts registered in `Sys_init_subsystems` 0x4d6fe0 (§2). | CONFIRMED |
| Cursor | `Resource/CURSOR.RES` (dBase: FILENAME, HOTSPOT_X/Y = 2,2, BITMAPPTR = offset) → `Resource/I_CURSOR.RES` 32×32 paletted (`pal_std.res`, index 0xFF transparent). Records NORMAL, WAIT. | HIGH |
| Key names | `Resource/STD.SET` `DEFAULTK` / `KEYDESC` (see GAMEPLAY_CHARACTER_COMBAT.md). | HIGH |

## 2. Fonts (en2d/font2d.cpp 0x623e90..0x625c50)

- `.fin` fonts (loader 0x624810, called `(name, 1, 0)`): spacing (font+0x38) = 1, width adjust (+0x3a) = 0; metrics from the file
  (space width = header metric_d, +0x36).
- Grid fonts (0x623e90 `(name, cell_w, cell_h, columns, first, last, pad, spacing, adjust)`): cells start at (pad, pad), step
  `pad + cell`, wrap when `x + cell_w > atlas width` or `columns` reached. Each glyph = tight box of texels with alpha ≠ 0 inside
  its cell (scanner 0x628490: first/last opaque row, then first/last opaque column; an empty cell collapses to the 1×1 bottom-right
  texel). `y_offset` = glyph top − minimum top over the font; line height (+0x30/+0x34) = tallest glyph; +0x32 = top..bottom
  extent; space width (+0x36) = width of `'1'`; then `adjust` is subtracted from every width. The full parameter table is in
  `grid_font_specs()` (`ui_font.cpp`).
- Layout (measure 0x624f00, draw 0x6255d0): advance = glyph width + spacing; space, characters outside first..last and glyphs with
  width < 1 advance by the space width, TAB by 8×; glyph drawn at `(x, y + y_offset)` with its full atlas rect; measured width
  omits the trailing spacing. Centred text uses `x − floor(w/2)`.
- Verified against the original main menu: Goudy_bold_24 "New Campaign" / "Load a Campaign" / "Options" measure 205 / 231 / 103 px
  and span x 154..358 / 141..371 / 205..307 centred on x = 256 in `s06_main_menu.png` — exact (`ui_font_test`).
- Not RE'd: the txtbox2d word wrapper (we wrap greedily on spaces) and 0x6255d0's "..." truncation.

## 3. Canvas, widescreen, high-DPI, hit testing

- Design space 1024×768 (the `MENU1024`/`LD1024*` art; `MENU800`/`LD800*` sets exist for 800×600 and are not used).
- `Ui::frame(Anchor::Center)`: the 4:3 canvas centred in the viewport with black pillar/letterbox bars (the original only ran
  4:3 modes). Corner/edge anchors keep each design coordinate's distance to that real screen edge — the HUD corners on 16:9/21:9.
- Scale = canvas height / 768 for every frame (any resolution, high-DPI: SDL window created high-pixel-density; mouse points ×
  pixel density). Quads are snapped to whole pixels, so 1024×768 is texel-exact.
- Mouse is mapped back through the inverse of the current frame transform; buttons act on left-button release inside the rect
  (en2d hit test `FUN_00629a40`: rect x..x+w−1).
- Colour: the D3D8 original blended the 2D layer in gamma space, and much of the art is large semi-transparent dark
  vignettes (MENU1024 `alab_s`/`sphere`, INGAME_B `sphere`, `credits_border`). Blending those in linear space made them up to
  ~30/255 too bright, so the UI pass blends sRGB-encoded values: GL draws it with `GL_FRAMEBUFFER_SRGB` off and
  `ui_frag.glsl` encoding its output; Vulkan renders it in a second dynamic-rendering pass into an `R8G8B8A8_UNORM` view of the
  (MUTABLE_FORMAT) sRGB colour image with the same shader encode. `UiQuad::color` stays a linear multiplier. Main menu after the
  change: mean absolute difference to `s06_main_menu.png` 0.45/255 (GL), GL vs VK 0.1/255.

## 4. State and commands

`ui_model.hpp`: `MenuModel` (screen, previous screen for BACK, options page, `GameOptions` = CONFIG.DAT fields, key-binding rows,
save slots, page/selection, screen time) and `HudModel` (player/party units, minimap, messages, overlays). Screens only change
navigation fields and append `UiCommand`s: `NewGame(difficulty)`, `Load(file)`, `Save(file, description)`, `DeleteSave`,
`OptionsChanged`, `Resume`, `RestartMission`, `QuitToMain`, `Quit`, `MissionBrief`, `MessageLog`. `UiRunner` (used by
`joan-native --ui-screen NAME` and the tests) honours Quit/Resume/QuitToMain and logs the rest until a game layer consumes them.

## 5. CLI

`joan-native --ui-screen NAME [--scenario S1L1] [--screenshot out.png --frames 4]`, names: main difficulty credits options video
audio input game remap load save ingame ingame-options hud msglog brief quickload. Sample states reproduce the reference shots
(`ui_sample_state`, `ui_screens.cpp`). With `--scenario` the HUD draws over the 3D world.

## 6. Per-screen RE notes (merged from the screen owners; positions, addresses, confidence, SSIM, gaps)

Screenshot tests score masked SSIM (tests/engine/ui_shot.hpp: luma, 8x8 windows, stride 4) against 1024x768 captures of the original; 21:9 runs are resampled back to 1024x768 and must reach the 4:3 threshold - 0.10. Thresholds sit just below the measured value; HUD cases are low because the 3D world behind translucent panels is not rendered in the test (only opaque panel areas are scored). GL and Vulkan score identically.

### Main menu, difficulty, credits (MenuFrontEnd)

Code: `src/engine/menu_main.cpp` (menu_main, menu_difficulty, menu_frontend_backdrop, menu_canvas_bars),
`src/engine/menu_credits.cpp`. Tests: `tests/engine/ui_cases_frontend.hpp`, `{gl,vk}_ui_frontend_test.cpp`.
Model: `MenuModel::main_entry` (0 full opening, 1 back from Load/Options, 2 back from Difficulty).
Scratch scripts: `work/ui/MenuFrontEnd/{cmp,shift,tmatch}.py`.

#### Main menu object (FUN_00709090 init, this = menu object)

Widget offsets from the disassembly (`add $off,%ecx` before each call; x at +9, y at +0xb, alpha float at +0x11,
fading flag +0x5e, moving flag +0x35). Image widgets (stride 0xb7, set handle +0x31 = MENU1024):

| off | sprite | start pos | final | animation |
|---|---|---|---|---|
| 0x2ac7 | main | 0,0 | 0,0 | alpha 0->1 at 3/s |
| 0x2b7e / 0x2c35 | alab_l / alab_r | -188,0 / 1024,0 | 0,0 / 836,0 | 1200 px/s |
| 0x2cec | alab_s | 0,0 | 0,0 | alpha 4/s |
| 0x2da3 / 0x2e5a | gold_l / gold_r | 45,0 / 755,0 | same | alpha 4/s |
| 0x2f11 / 0x2fc8 | lea_l / lea_r | -264,0 / 1024,0 | 0,0 / 760,0 | 1600 px/s |
| 0x307f | sphere | 32,0 | same | alpha 4/s |
| 0x3136 | stripe | 240,744 | same | alpha 2/s |
| 0x31ed | logo_bar | 1024,16 | 0,16 | 7680 px/s |
| 0x32a4 | logo | 288,26 | same | alpha 4/s |
| 0x335b | joan | -45,150 | same | alpha 1/s |

All 8 faded sprites start at alpha 0 (`FUN_0061f1b0(0,..,instant=1)`). Confidence: high (decomp + 0 px measured).

Text-box buttons (FUN_00621540, stride 0x346, start x -512, slide to 0): main (font Goudy_bold_24 = DAT_00a48d08,
draw callback FUN_00707e30) 0x6c5 New Campaign y224, 0xa0b Load y300, 0x13dd Options y376, 0x1097 Credits y452,
0x2781 Quit y528 (h 72); difficulty (Goudy_bold_20 = DAT_00a47dc8, callback FUN_00708760) 0x1723..0x243b at
y 224/276/328/380/432 (h 48). Fonts: Sys_init_subsystems 0x4d7538/0x4d755e.

Button art (MENU_B): normal `*_OT` + black text; hover (mouse in x1..x2,y1..y2) `*_OV` + marks + white text;
held `*_DW` + marks + white text offset +1,+1. Text x = x1 + (x2-x1)/2 - w/2; y = y1+11 (L) / y1+5 (M).
Marks: L_MARK at centre-w/2-40 and centre+w/2, y1+12; M_LA at centre-w/2-73, M_RA at centre+w/2+5, y1+14.

Copyright (draw FUN_0070a8b0 tail): MAINMENU 10, font "Goudy12 Bold Black" (0xa5d3c8), colour black with the
stripe's alpha, FUN_00626130 centred in (0, stripe.y2-8)-(1024, stripe.y2+12), stripe.y2 = 751.

Draw order (FUN_0070a8b0): black fill, main, sphere, joan, lea_l, lea_r, gold_l, gold_r, [Load page
FUN_0073d2c0 / Options page FUN_0074e170], logo_bar, logo, 5 main buttons, 5 difficulty buttons (if +0x3414),
alab_s, alab_l, alab_r, stripe, copyright.

#### Opening sequence (tick FUN_0070acb0)

Waits 1000 ms (+0x342a += frame ms), then each step starts when the previous one's condition holds:
main 3/s (t=1.0) -> alab slide (1.333) -> lea slide + alab_s fade (1.490) -> gold fade once lea in and alab_s=1
(1.740) -> logo_bar slide (1.990) -> logo (2.123) -> sphere 4/s + joan 1/s (2.373) -> stripe 2/s once joan>0.7
(3.073) -> buttons once stripe=1 (3.573). SE: MENU_SLIDE_IN at the main fade, BUTTON_SLIDE_MANY at the buttons.
Buttons: FUN_0061e020(0,y, 3500 px/s, accel -1500), accel set to -4000 (FUN_0061e2a0(0xc57a0000)) once
x >= -409.6; next button starts when the previous x > -301. Closed form: 0.15745 s per button, stagger
0.061461 s. Move/fade engine: FUN_0061f450 (pos += dt*speed, speed += dt*accel, clamp at target),
FUN_00620810 (alpha += dt*rate, delay in ms). Input ignored while any button moves (+0x341e).
Escape on the main menu does nothing; 'S' key + "coronation" typing are cheats (not ported).

Returning: from Load/Options (+0x3415/+0x3416) Joan and the stripe had faded out at 4/s, so they fade back
(joan 1/s, stripe after joan>0.7) then the buttons slide in (main_entry 1). From Difficulty CANCEL only the
buttons slide (main_entry 2). Credits leaves via FUN_004d7e70(3) which rebuilds the menu: full sequence.

Exit animations (ported, closed form on model.time via MenuModel::frontend_exit / frontend_exit_time; input
ignored meanwhile). Buttons slide out FUN_0061e020(-512,y, 10 px/s, accel 12000): 0.2913 s each, next when
x <= -51.2 (0.0915 s stagger), last main button out at 0.6575 s. Then:
- New Campaign -> Difficulty (its buttons slide in); Quit -> UiCommand Quit (FUN_006e4c00(1)), both at 0.6575 s.
- Load / Options: Joan + stripe fade out 4/s, page opens at 0.9075 s.
- Credits: Joan + stripe 4/s, then the reverse sequence from 0.9075 s: sphere 4/s, logo 4/s, logo_bar -> 1024 at
  7680 px/s, gold 4/s, lea out 1600 px/s + alab_s 4/s, alab out 1200 px/s, main 4/s; credits open at 2.447 s.
- Difficulty EASY..VERY HARD: buttons out; once the 4th is out (0.566 s) stripe 4/s + joan 1/s (SE MENU_SLIDE_OUT);
  sphere once stripe = 0 and joan < 0.3 (+0.7 s); reverse sequence; DAT_00a57fc2 = 0..3 and
  Battle_start_scenario(1,1,0) when main reaches 0 -> UiCommand NewGame{0..3} at 2.806 s (fires once).
- CANCEL / Escape (FUN_006218c0(0x1b,..)): buttons out, Main at 0.6575 s (main buttons slide back in).
Checked by tests/engine/ui_frontend_flow_test.cpp (no GPU, ±2 frames).

#### Credits (state 0xf: start FUN_006b56b0, tick FUN_006b5900, draw FUN_006b5e60)

- Fade widget +0x32 (alpha +0x43) shows the current bg; stage 0: black + bg1 fading in at 4/s, border/logo/bar/
  title with the same alpha. Stage k=1..3: bgk + bg(k+1) fading in at 0.33099/s after a 13000 ms delay; stage++
  when it reaches 1. Stage 4: bg4. Stage period 13 + 3.021 s; stage 1 at 0.25 s.
- Strips t1..t5 (+0xe9 + i*0xb7), x = (W-510)/2, stacked from y=768, move to y = 136-h at 60 px/s; they only
  tick from stage 1 (so scroll time = t-0.25); drawn only while moving. Last strip stops at t = 61.32 s ->
  stage 5/6 -> main menu (FUN_004d7e70(3)).
- Over the strips: bgk_mask (stage 0: none). During a cross-fade the original draws bgk_mask with alpha a and a
  second texture stage holding bg(k+1)_mask (FUN_005d5650); we draw bgk_mask (1-a) then bg(k+1)_mask (a): exact
  at both ends, slight coverage dip mid-fade.
- border 0,0; logo 372,24; bar 316,112; CREDITS 1 in goudy_bold_14 (0xa47d88) centred in (316,112)-(707,135).
- Measured (template match of the strip scroll + least-squares cross-fade alpha): s04_02_remap_0..5,
  remap_6, game_options, main_menu_again, credits_3s, credits_10s are all credits (names desynced): strip times
  5.7, 9.1, 12.45, 15.8, 19.1, 22.5, 34.1, 42.03, 47.42, 54.7 s; fitted cross-fade alpha remap_4 0.945
  (model 0.927), credits_3s 0.784 (model 0.788) — confirms the 0.25 s strip delay.
- Escape or a click leaves early (input handler not RE'd; matches the capture log's Escape).

#### Widescreen

`menu_canvas_bars` blacks out everything outside the 1024x768 canvas (slide-in start positions, Joan's lower
150 px and strips below 768 reach past it; the original's 4:3 screen clipped them).

#### SSIM (gamma-space UI blending, 4 frames)

| case | reference | GL 4:3 | GL 21:9 | VK 4:3 | VK 21:9 |
|---|---|---|---|---|---|
| main (cursor corner masked) | s06_main_menu | 1.000 | 0.886 | 1.000 | 0.886 |
| difficulty (EASY hover, mouse 268,262) | s06_difficulty | 0.999 | 0.886 | 0.999 | 0.886 |
| credits t=42.28 | s04_02_main_menu_again | 1.000 | 0.892 | 1.000 | 0.892 |

Thresholds 0.98 (21:9 = 0.88, limited by the harness resampling). Mean abs diff vs reference: main 0.45,
difficulty 0.48 (cursor and text AA), credits 0.15.

#### Gaps
- Sound effects (MENU_SLIDE_IN/OUT, BUTTON_SLIDE_MANY) and credits music are not triggered here.
- Credits mask cross-fade approximated (above).

### Options menu (interfac/optmenui.cpp) — RE notes (MenuOptions)

Code: `src/engine/menu_options.cpp` (screens), `src/engine/menu_options_store.{hpp,cpp}` (options.ini, STD.SET key
tables). Tests: `tests/engine/ui_cases_options.hpp`, `gl_ui_options_test.cpp`, `vk_ui_options_test.cpp`,
`ui_options_store_test.cpp`. Scratch (decomp extracts, disassembler `disx.py`): `work/ui/MenuOptions/`.

Confidence: **RE** = constant read from the decomp/disassembly; **REF** = also matched against an original
screenshot; **INF** = inferred.

#### Object / functions
| fn | role |
|---|---|
| 0x73ba80 | owner init: FRAME (COMMELE2 `frame`) + 2 LONGBARs + 4 root buttons + BACK; calls page inits |
| 0x73c740 | loads OPTMENU, OPTMENUF, OPTMENUB, COMMELE2, COMMELE3 (+ MENU_B in game) |
| 0x73cc00 | page switch: leave(old)/enter(new) = slide targets. Pages 0 root, 1 video, 2 audio, 3 input, 4 game, 5 remap, 6 close |
| 0x73d2c0 | draw: [0x592c30 bg, in game 0x6f21c0 IGAMEMNU backdrop], FRAME, top LONGBAR, title, bottom LONGBAR, 5 root buttons, pages |
| 0x73d830 | input: root buttons -> pages (see below); Esc/BACK -> 0x73ca80 close (applies input method, saves config) |
| 0x748b10/0x74bc50/0x74c2e0/0x74a580/0x74a700 | video init/enter/leave/draw/input |
| 0x740440/0x741cd0/0x7420e0/0x7412e0/0x7413e0 | audio |
| 0x7435e0/0x747bc0/0x7480e0/0x745720/0x745b10 | input + remap (remap enter 0x748540, leave 0x748830, row list 0x7478a0) |
| 0x742470/0x743170/0x7433d0/0x742bf0/0x742c80 | game |

Page ids by title (0x73d2c0 case n prints OPTMENU 34+n): 1 VIDEO, 2 AUDIO, 3 INPUT, 4 GAME, 5 remap ("OPTIONS").
The handler's button checks run in a hidden ECX order (1, 2, 4, 3); mapping by title/content: VIDEO->1,
AUDIO->2, INPUT->3, GAME->4 (RE + content).

lc = -0x30 when DAT_00a500c2 == 9 (in game), else 0: every y below is +lc (RE).

**Unused data (RE):** no record of OPTMENU (tabs `*_ot/_ov/_dw`, `*tit`, `remap_*` incl. rotated `remap_fr`),
OPTMENUB (`bkpanel`, `topbar_*`) or OPTMENUF (`frtpanel`) is referenced by name anywhere in joan.exe (strings
absent from the decomp/strings.tsv; only the three table loads exist). They are an older, unshipped layout.

#### Fonts (Sys_init_subsystems 0x4d6fe0: `mov ecx,<obj>` before each registration)
- 0xa51608 = `Goudy14(22) white`: all buttons, row labels, combo values, remap rows (RE).
- 0xa48d48 = `Goudy_bold_18`: title in the top LONGBAR (RE, 0x73d3b8). 0xa489c8 = `Goudy12(18) black` (combo type 0/1, unused here).

#### Root list (page 0) — REF (s04_07_options_ingame, SSIM 0.991)
- Backdrop: in game `ingame_backdrop_back` under the page and `ingame_backdrop_front` (stone, In-Game Menu bar) over it, else `menu_frontend_backdrop`
  with MENU1024 logo_bar/logo/alab_* redrawn on top (0x70a8b0 draws the options page between gold_r and logo_bar).
- LONGBAR (COMMELE2 1024x60) at (0, 180+lc) and (0, 712+lc); title centred in (0,180+lc)-(1024,210+lc) (0x626130) —
  `menu_heading_bars` (MenuSaveLoad helper).
- Buttons: rect (256, y)-(768, y+64), y = 320/400/480/560 + lc (0x73cc00 case 0 targets); draw 0x73ee40:
  `cs_ot` / `cs_ov` hover / `cs_dw` held (512x48, COMMELE3 texture, COMMELE2 table) at (x, y); text centred in
  (x, y-5)-(x2, y2-20); hover adds MENU_B `diamond` at (x-32+(512-w)/2, y+17) and (x+10+(512+w)/2, y+17).
- BACK: text button rect (462, 712)-(562, 742) (0x73e130: label centred in the rect raised 1 px; hover/held adds
  COMMELE2 `mark` at (x1+x2-w)/2-20 and (x1+x2+w)/2+6, y+8; held = black text). Esc = BACK. -> model.previous.

#### Pages (RE, confirmed REF against tools/reference/capture_ui.py shots)
Common widgets:
- Row label (0x73e990): MENU_B `s_ot` (512x56) at (-100, row) — clipped at x=0 (off the 4:3 screen); text
  right-aligned ending at x2-40 = 372 (0x626080), top y+8; highlighted row adds `diamond` at (377, y+14) and
  (412-w-67, y+14). Row highlight = mouse in (0, ctrl_y)-(1024, ctrl_y+52) (handler 0x74a700 0x593980 calls).
- Row y (label) = 243, 295, 347, 399, 451, 503, 555, 607; control y = label-7 (enter targets, controls in
  reverse order 600..236); control x = 448 (0x1c0).
- Slider (0x760050 init, 0x760b70 draw): box 464x52 `page_ot` / `page_ov` (highlighted) (0x75fba0), arrows
  `aw_l_*` (x+35, y+11) / `aw_r_*` (x+390, y+11) only while highlighted, scroll bar (x+80,y+12)-(x+380,y+36),
  range 0..steps-1; `hl_bar` drawn with src rect (0,0, v*300/range+3, 24) at (x+76, y+10) (disasm 0x760c39),
  `hl_mark` at (x+74+v*300/range, y+8) (0x760aa0). Arrow hold-repeat after 300 ms not reproduced (click = ±1, drag on bar).
- Combo type 2 (0x6b29b0, draw 0x6b3ec0, page 0x6b23b0, input 0x6b4280): same box, value centred at
  (x+232-w/2, y+13), arrows always drawn; left = previous (wraps), right = next (wraps).
- FRAME (COMMELE2 `frame`, 440x552, ROTATION=1) at (460, y): video 108, audio -100, input -48, game -204,
  root/remap -448 (hidden) — its bottom edge follows the last control row. Thin outline only.

| page | rows (label: control) | buttons (y 712..742) |
|---|---|---|
| Video | Brightness s101, Contrast s101, Gamma s101, Draw Distance s21, Dynamic Grass c(Off,On), Dynamic Grass Draw Distance s101, Dynamic Grass Draw Density s101, Shadow c(No/Low/Medium/High) | DEFAULT 387..487, BACK 537..637 |
| Audio | Music Volume s101 (0x3A), Environmental Volume s101 (0x3C), Sound Volume s101 (0x41), 3D Sound c(Off,On) (0x43) | DEFAULT 387..487, BACK 537..637 |
| Input | Input Method c(Keyboard, Keyboard & Mouse [, Gamepad/Joystick only with a pad]), Mouse Sensitivity s21, Invert Mouse c(Normal,Inverted), Riding Camera Rotation Angle s180, Riding Mode c(Mode A, Mode B) | REMAP KEYS 212..362, DEFAULT 437..587, BACK 662..812 |
| Game | Game Level c(Easy/Normal/Hard/Very Hard), Melee Auto-Targeting c(On,Off) | BACK 462..562 |

Label/control pairing = the control created immediately before each label (draw-distance s21, the two combos
confirm it). Video/audio/input/game have no CONFIRM/CANCEL: the original writes config on every change and
saves when the menu closes; the port mutates `model.options` live and emits `OptionsChanged` on BACK/Esc.
DEFAULT restores that page's GameOptions{} defaults (original: Config_set_defaults / scheme 3, sens 0, 90°).

Value mappings (0x74a700, 0x745b10; config = 0xA57FB8 + offset):
- brightness 0xA8 = (s-50)*0.02; contrast 0xAC = s<51 ? s*0.01+0.5 : (s-50)*0.04+1;
  gamma 0xB0 = s<51 ? s*0.01+0.5 : s<76 ? (s-50)*0.08+1 : (s-75)*0.12+3.
- draw distance 0x98 = s*0x3c00/20 + 0x401; grass distance 0xA0 = s*20 (0 also turns Dynamic Grass off,
  else on); density 0xA4 = s; shadow 0x9C = combo-1; dynamic grass 0x9F.
- mouse sensitivity 0x6F = (s-10)/10; riding angle 0x76 = s; invert 0x73 = combo-1; riding mode 0x74 = combo-1;
  melee auto-target 0x75 = combo-1 (0 = On); input method 0x6B = {1:0, 2:3, 3:1}[combo] (0x73ca80).

#### Remap (page 5) — RE + REF (ui_options_remap_0..6)
- Category combo (type 2) at (116, 236): Movement, Control, Command, Menus, Onscreen Indicators, Camera Control,
  Miscellaneous (OPTMENU 40..44, 50, 56). DEFAULTK id ranges (0x7478a0): 1-10, 11-21, 22-32, 35-41, 42-46,
  47-51, 53-55; rows whose current-scheme name is "Not Available" or "Escape" are skipped; max 10.
- Rows i = 0..9 at y = 300 + 40 i: action slot (116, y) rect 464x30, `slot_ot` black text / `slot_ov` white
  (hover, selected) / `slot_dw` held, text centred in (x, y-2)-(x2, y2-2) (0x73f490); key box (580, y) rect
  326x40 `tb_ot` / `tb_ov` selected, white text (0x73fde0). Selected row: text OPTMENU 45 "Press a valid key or
  button" + `mark`s. No FRAME on this page.
- Capture (0x745b10): Esc cancels; key -> scheme key field (DIK via SDL->DIK table); mouse button (keyboard &
  mouse scheme) -> FPS_MOUSE. Buttons CONFIRM 212..362 (RemapKey_save_config, -> input page, `OptionsChanged`),
  DEFAULT 437..587 (RemapKey_reset_scheme), CANCEL 662..812 / Esc (KeyMap_apply_config = restore).
  Not reproduced: RemapKey_assign's duplicate handling, joystick capture.

#### Key tables (remapkey.cpp, KeyMap_load 0x4cae70, KeyMap_get_name 0x4cb5b0) — RE
DEFAULTK hex fields, empty -> 0xffff (keys) / 0xff (J_JOYPAD, FPS_MOUSE atoi). Name by scheme: 0 -> K_KEY;
1 (pad) -> J_JOYPAD (device 1) unless 0/0xff then J_KEY; 3 (kbd+mouse) -> FPS_KEY unless 0/0xffff then FPS_MOUSE
(device 2). KEYDESC lookup (0x4cb9b0) by (device, code). CONFIG.DAT 0xB4 custom flag + 0x30-byte entries at
0xB5 (+0x22 pad, +0x23 mouse, +0x24 K, +0x28 J, +0x2c FPS) override (KeyMap_apply_config 0x4cc220).

#### Persistence
`$XDG_CONFIG_HOME/joan-native/options.ini` (else ~/.config): `name=value` per GameOptions field +
`keyN=K,J,JP,FPS,FPSM` (hex) per DEFAULTK record. Load order: GameOptions{} + DEFAULTK, CONFIG.DAT
(`options_from_config`), options.ini. UiRunner loads at start / QuitToMain and saves on OptionsChanged.

#### Capture (tools/reference/capture_ui.py) — worked
Wine sandbox from the main menu; instead of fixed sleeps it polls screenshots until the screen matches
shots/s06_main_menu.png (Escape only while clearly a movie), waits for slide-ins to settle, then clicks.
Writes work/reference/shots/ui_options_{root,video,audio,input,game,remap_0..6}.png (~2 min). The gauntlet
cursor positions were template-matched (hotspot 2,2): root (253,407), video (507,338), audio (507,416),
input (507,507), game (510,585), remap (285,722) — the test cases reproduce those hovers.
Captures confirmed: all page layouts, FRAME outline, label clip at x=0, slider fill (volumes 9/15/16 from the
sandbox CONFIG.DAT), hover diamonds/arrows, remap rows (keyboard & mouse scheme), CONFIRM marks.
Measured fixes: hl_bar is 25 rows (not 24); slider init truncates (`__ftol`, double math 0x748d8c).

#### SSIM (masked SSIM, tests/engine/ui_shot.hpp)
| case | GL 4:3 | GL 21:9 | VK 4:3 | VK 21:9 |
|---|---|---|---|---|
| ingame-options vs s04_07_options_ingame | 1.000 | 0.908 | 1.000 | 0.908 |
| options vs ui_options_root | 0.987 | 0.888 | 0.987 | 0.888 |
| video vs ui_options_video | 0.988 | 0.855 | 0.988 | 0.855 |
| audio vs ui_options_audio | 0.988 | 0.885 | 0.988 | 0.885 |
| input vs ui_options_input | 0.988 | 0.882 | 0.988 | 0.882 |
| game vs ui_options_game | 0.988 | 0.896 | 0.988 | 0.896 |
| remap vs ui_options_remap_0 | 0.987 | 0.877 | 0.987 | 0.877 |
Remaining pixel diffs: front-end ring edges (MenuFrontEnd backdrop), slider rows (fixed after measuring).

#### Slider repeat (0x760df0) — RE
Arrow click (release) = 1 step and resets the repeat accumulator; while an arrow is held the hold time
(+0x95) grows to 300 ms, then the accumulator (+0x91) adds the frame ms and steps once per
ftol(2000 / ((max-min) * 0.4)) ms (101 steps: 50 ms, 21 steps: 250 ms, 180 steps: 27 ms). Port: MenuModel
slider_hold_ms / slider_repeat_ms, reset whenever the button is up.

#### Remap conflicts (RemapKey_assign 0x4cba40) — RE, store `remap_assign`
Refuses action 34 (Main Game Menu) and (device, code) pairs absent from KEYDESC (buttons stored +1). Sets the
scheme's key (+0x24 keyboard / +0x28 pad / +0x2c kbd+mouse) and button (+0x22 pad / +0x23 mouse; the other one
becomes 0). The last other action holding the same key or button gets its DEFAULTK value back if no other
action holds that default (keyboard scheme: test skips the assigned action; pad/kbd+mouse: skips the
conflicting one), otherwise it receives the replaced binding (swap). Capture 0x745b10: Shift/Ctrl/Alt are
detected as modifier edges and always assigned through scheme 3 with DIK 0x2a/0x1d/0x38 (original quirk, kept);
left/right click bind mouse buttons in the kbd+mouse scheme; the selection ends even when refused.

#### Slide animations — RE (closed form on model.time since the page switch)
Shared en2d motion (`ui_slide`/`ui_fade`): 3500 px/s, -1500 px/s^2 after a delay, snap at target; fades 4/s
(rows) or 6/s (buttons). Start positions = reset functions 0x73ded0/0x74b6b0/0x741a70/0x742f90/0x747200.
- Opening: LONGBARs from x = -1024 / +1024 after 250 ms; root buttons drop from (256, 62) bottom first at
  500/550/600/650 ms; BACK appears at 500 ms (instant, fade 6/s).
- Root leave: buttons back to y = 62 at 0..150 ms (order INF), fade out.
- Page enter: labels from x = -612 at 500 ms, controls drop from (448, 62) bottom first 550 + 50 k ms, buttons
  500 ms instant; FRAME to its page y at 500 ms. Page leave: labels out at 0, controls 50 k ms (order INF),
  FRAME to -448 at video 400 / audio 200 / input 250 / game 100 ms.
- Remap enter: buttons 500 ms, rows bottom first 550..1000 ms (slots from y = 70, key boxes from x = 1024),
  category combo 1050 ms; leave 0x748830: rows 50 + 50 i ms.
- MenuModel::options_prev = page being left (5 = remap, -1 = just opened); the leaving page is drawn on a
  scratch model copy. Not ported: the close animation (case 6) — the port switches screens at once.

#### Gaps
- Not reproduced: sound effects (IF_SETTING_BUTTON_CLICK, BUTTON_SLIDE_MANY), the options close animation,
  joystick capture on the remap page.

### MenuSaveLoad notes: Load / Save / In-Game Menu / INGAME_B backdrop / G_POPUP box

Code: `src/engine/menu_saveload.{hpp,cpp}`, `src/engine/menu_ingame.cpp`. Tests: `tests/engine/ui_cases_saveload.hpp`,
`gl_ui_saveload_test.cpp`, `vk_ui_saveload_test.cpp`. Model fields added to `MenuModel`: `save_state`,
`save_click_time`, `ingame_confirm` (comments in ui_model.hpp).

#### INGAME_B backdrop (high confidence)
- Init FUN_006f1940 (start positions), slide-in FUN_006f24d0 (targets): left (0,0), right (512,0), top (0,0),
  bottom (0,704); woodtex (0,0), sphere (40,24) (igamebg4: alpha 64 white→black shading), ring (0,0),
  titlebar (0,0), title (312,48). Widget offsets in the object: 0x30 left, 0xe7 right, 0x19e top, 0x255 bottom,
  0x30c woodtex, 0x3c3 titlebar, 0x47a title, 0x531 label, 0x5e8 sphere, 0x69f ring, 0x756 l_arrow, 0x80d r_arrow.
- Draw FUN_006f21c0: layer 10/5 = woodtex, sphere, ring, left, right; layer 1 = titlebar, title, label, l_arrow,
  r_arrow, top, bottom. en2d layer 1 is IN FRONT of the menu pages (in-game menu layer 5, savemenu layer 2):
  verified in s04_07_ingame_menu (bottom stone covers the BACK longbar). API: `ingame_backdrop_back/front`.
- Label "In-Game Menu" (IGAMEMNU 15) in Goudy14(22) white centred in (312,50)-(708,68); arrows y 54,
  l_arrow x = (396-w)/2 - 68 + 310, r_arrow x = (396+w)/2 + 314 (FUN_006f2ac0).

#### In-game menu (high confidence)
- FUN_006f2d70 init, FUN_006f43e0 final slide targets: COMMELE2 frame (ROTATION=1, CCW) at (292,110);
  8 buttons 512x64 at x 256, y 114 + 68 i: Load (F8), Save (F7), Mission Brief (F12), Message Log (M),
  Restart Mission, Options (F9), Quit To Main Menu, Quit To Windows (IGAMEMNU 1,2,3,13,4,5,6,7; keys =
  KeyMap_get_name of DEFAULTK records 38,37,40,41,39 via MenuOptions' `action_key_name`). LONGBAR (0,712),
  BACK rect (462,712)-(561,742) (FUN_006f5200: white label, MARKs on hover, black when pressed).
- Button draw FUN_006f4c40 (thunk 0x407658): CM_OT / CM_OV hover / CM_DW pressed (COMMELE2 table,
  commele3 texture); label raised 2 px, black / white on hover. Font: decomp passes 0xa489c8 (Sys_init pairs
  it with "Goudy12(18) black") but the reference glyphs match Goudy14(22) metrics exactly → Goudy14(22).
- Input FUN_006f3c90: Escape/BACK → resume; restart/quit open the G_POPUP box with body IGAMEMNU 12 or 10 +
  "\n" + 11, OK → RestartMission / QuitToMain / Quit.

#### G_POPUP box FUN_006d3cb0 (`ui_confirm_popup`, medium-high)
- No title → width 0x1c8 (456); body wrapped at 361 px; h = 0x30 + n*lh - 2 + 2n + 0x14 + 0x24 (lh = 16 for
  Goudy14(22) → 120 for one line, confirmed by HudGame in s04_07_quickload_prompt: box (284,324,456,120)).
- Frame tl/t/tr/l/c/r/bl/b/br, buttons b_ot/b_ov/b_dw 172x36 at x = (-189 n + 473)/2 + 284 + 189 i,
  y = box_y + h - 60, labels G_POPUP 1/2 (OK/CANCEL), fonts 0xa51608 Goudy14(22). Gap: frame_l/r are stretched
  (original clips/tiles); body line alignment centred (FUN_00626e00 not fully RE'd).

#### Save / Load (savemenu, high confidence)
- lc = -48 in game (DAT_00a500c2 == 9), 0 from the main menu. Final targets FUN_00751750 / init FUN_0074cb10:
  FRAME (128,152+lc); slots 10 x (116..580, 300+lc+40 i, h 36) SLOT_OT / SLOT_OV (hover or selected, + MARKs) /
  SLOT_DW, label Goudy14(22) black / white (FUN_007538b0); page widget FUN_006b29b0 type 2 at (116,236+lc):
  page_ot, aw_l (151,+11), aw_r (506,+11), "PAGE %1d OF %2d" (SAVEMENU 22) — label 5 px above the rect
  centre (measured); arrows wrap (0x6b4280). PIC (580,232+lc); save thumbnail Save/<name>.BMP (320x240 of
  512x256) at (601,253+lc); INFOBAR (612,520+lc) + "FILE INFORMATION" (Goudy12 Bold Black, diffuse white) +
  DIAMONDs at mid - w/2 - 28 / mid + w/2 + 8, y+8 (measured); INFO (612,560+lc); LONGBARs (0,180+lc)/(0,712+lc),
  heading Goudy_bold_18 white centred in (0,y)-(1024,y+30) (0x74e314): SAVEMENU 20 / 6.
- File info (FUN_0074e170, original pairing kept): y 560 "SAVED TIME:" (25), 574 date (DATE|1 "October 13,
  2026"), 600 "SAVED DATE:" (24), 614 time ("%d:%.2d %s" 12 h), 640 "MISSION:" (21), 657..713 mission name
  SAVEMENU 10 + mission (header value_c, 0 = Free Mission) word-wrapped.
- Bottom buttons (rects x..x+150, y 712+lc, labels measured Goudy14(22); init passes Goudy_bold_18):
  load none-selected BACK(437); save none-selected SAVE NEW(262) + BACK(612); selected LOAD|SAVE(237),
  DELETE(437), CANCEL(637); delete confirm / description entry CONFIRM(262) + CANCEL(612).
- Input FUN_0074f230: click selects, double click loads (load mode), empty slot deselects, Escape deselects /
  cancels / leaves; SAVE / SAVE NEW → entry (IN_FRAME (228,660+lc), IN_BG (286,670+lc), text (286,671+lc),
  63 chars, Enter = confirm). "Enter description here" (SAVEMENU 8) and the delete question (SAVEMENU 7)
  are never referenced by the exe → not shown.
- `list_saves` (game_ui_bridge.cpp): native `*.jsav` (META) + Save/*.SAV newest first (GameFile_enum_saves 0x4770a0,
  comparator 0x477bd0 on saved FILETIME); `SaveSlot::file` = full path, thumbnail Save/<stem>.BMP for .SAV only.

#### Font object map (Sys_init 0x4d6fe0, `mov ecx` + name push): work/ui/MenuSaveLoad/fonts.txt
Two pairings disagree with the screenshots (0xa489c8, 0xa48d48 as button labels); every final choice above is
measured. en2d 0x626130 text = TextOp::Diffuse (core).

#### Results (masked SSIM, GL = VK)
| screen | reference | 4:3 | 21:9 canvas |
|---|---|---|---|
| load (main menu) | s04_03_load_campaign (cursor masked) | 0.997 | 0.862 |
| save (in game) | s04_07_save_menu | 0.999 | 0.867 |
| ingame (Options hovered) | s04_07_ingame_menu | 0.999 | 0.912 |
Mean abs pixel diff vs reference (CLI, no mouse): ingame 0.87 (only the hover row), save 0.39, load 1.03.

#### Animations (closed form on MenuModel::time, `ui_slide` / `ui_fade` in menu_ingame.cpp; check tests/engine/ui_anim_test.cpp)
- en2d move-to 0x61e020 + update 0x61f450: straight line, velocity v0 px/s, acceleration a px/s^2 after a delay
  (menus 3500 / -1500; backdrop halves 5120/0, top 920/0, bottom 320/0); fade 0x61f1b0: rate/s after delay.
- Backdrop in FUN_006f24d0 / out FUN_006f26e0 (0.3 s each), driven by `ingame_backdrop_closed` (set again
  when the menu resumes / restarts / quits). In-game menu in FUN_006f43e0 (rows from 600 px above, delay
  44 ms * (8 - i)), out FUN_006f38a0 (delay 44 ms * i, FRAME 352 ms, LONGBAR 150 ms), then the chosen page /
  command; Resume also slides the backdrop out first. Restart/quit confirm boxes reset instantly (FUN_006f4a10).
- Load/Save in FUN_00751750 (event chain turned into start times: FRAME+LONGBARs, slot 9 at FRAME-4px, each
  slot i-1 at half of slot i, PIC with slot 5, INFOBAR/INFO with slot 1, page bar at slot 0 + 115 px; buttons
  fade in at 6/s once settled; input/file info only when settled), out FUN_0074dbc0 (0.91 s) then BACK.
- G_POPUP borders are tiled/clipped at the sprite size like 0x633500's clip extent (no stretching).

#### Gaps
- No reference shots for a selected slot / delete confirm / description entry / in-game confirm box / any
  mid-animation frame: those use decomp values only.

### HUD / HUD overlays (HudGame) — RE notes

Code: `src/engine/hud_game.cpp` (`hud_game`), model `HudUnit/HudMapIcon/HudMessage/HudModel` in `ui_model.hpp`,
samples `sample_hud` in `ui_screens.cpp` (`--ui-screen hud|msglog|brief|quickload|objective|attention`).
Tests: `tests/engine/ui_cases_hud.hpp`, `gl_ui_hud_test.cpp`, `vk_ui_hud_test.cpp`. Tools: `work/ui/HudGame/tm.py`
(alpha-masked template matching of INTERFAC.SET sprites vs shots), `pe.py` (read joan.exe data tables).
All positions are 1024x768 pixels; the exe writes them as `H-n` / `W-n` (DAT_00a48860 = H, DAT_00a4885c = W), so
each group is anchored to that edge (BottomLeft panel, BottomRight party, TopRight minimap, TopLeft objectives).

#### Player panel — FUN_006f9b60 (param&1 sprites, param&4 text). Confidence: CONFIRMED (decomp + match SSD≈5)
| item | pos | evidence |
|---|---|---|
| MAINGAME panel 272x108 | (0, H-0x6c=660) | 0x6f9b60; match (0,660) |
| portrait `<CLASS>` + "1" (Joan1 58x90) | (15, 660) | 0x5a11a0/0x5a1550("1"); match SSD 5.5 |
| PHPBAR / PEPBAR / PLVLBAR, width = 100*v/max | (0x8d, H-0x3a/-0x2a/-0x14) | PLVLBAR skipped at level 30 |
| "hp/max", "ep/max", "xp%" right edge 0xf2 (left = x-w+1, 0x626080) | y H-0x3d / H-0x2d / H-0x17 | font obj 0xa50048 = verdana_12_regular_black_outline |
| ammo count centred in (0x68,H-0x18)-(0x7b,H-0xd) | | same font, "0" when none |
| level centred in (0x69,H-0x38)-(0x7b,H-0x2d) | | font obj 0xa52848 = verdana_12_gold_black_outline |
Font objects resolved from the font init 0x4d7000.. (`push name; mov ecx,obj; call`), table in this file's history.

#### Party portraits — same function. CONFIRMED (portrait/button SSD 0.9..2.9)
slot k = 8 - count + i; tables (joan.exe): frame/portrait x DAT_009de64c/0x9de68c = {69,161,253,345,437,529,621,713},
CHP x DAT_009de6ac = {83,...,727}, count x DAT_009de6e4 = {131,...,775}, morale offsets DAT_009de6cc; screen x = t-800+W.
CPTOP (x, H-100); CHP 45x8 at (hp_t, H-0x1d) drawn from the right (src x0 = 45-filled); portrait JOANC*/JEANC* 70x88
at (x, H-0x78) chosen by armour item (JOAN_ARMOR_LV1 → "JOANC2", LV2 → "JOANC1", LV3 → "JOANC3"; Jean LVn → JEANCn);
HR_FRAME + RESUMEOT/HOLD_OT at (x, H-0x5a) for non-player units (or RTS mode); CMBASE (t-0x31e+W, H-0x19) and troop
count in (cnt_t, H-0x23)-(cnt_t+16, H-0xc) when the unit leads troops.

#### Minimap — FUN_0070e500 / FUN_00716010 / FUN_0071b6a0. HIGH
- view 199x200 at (W-0xd3, 10), centred on the player at (W-0xd3+99.5, 110) (template match of the z1 map vs ref: player
  centre (912.5,110) exactly). Tiles `<scenario>W` z<zoom>r<row>c<col> 200x200 px; tile world size = World+0x54/+0x58
  (world w/h floats, disasm 0x71baa4..) / max col / max row; world y up (row 1 = top). S1L1 world = 16384 (STG grid
  256x256 cells x 64). Outside the map: tile `z<zoom>b`. Tiles carry alpha 170 in the texture (no extra blend).
- icons centred, drawn only when fully inside the view; player = MAPICON "cursor" rotated by heading (type 2).
  ponytail: rotation done with per-texel quads (UiQuad has no rotation).
- FRAME (MAPICON frame 216x216): decomp (W-0xd7, 6) but that clips; measured (806, 6) = W-218.
- sample: player (14971,12698) (cutscene camera of S1L1_tactic.sce: 14988,12783), camps from map/scenario1/S1L1.map
  (fcastle 8116,726; fcamp/ecamp 12000,14000 — lands within 2 px of the ref tent), enemy dots measured.
- Unit dots (FUN_0070fbe0): filled quad (x-1,y-1)-(x+1,y+1) via 0x632a70 (half size = float 1.0 at 0x9cf784), colours
  set up at 0x70de70..0x70e0b7: DAT_00a6a5a8 red (255,0,0,0xcc), 0xa6a59c green, 0xa6a5b4 blue, 0xa6a5a0/0xa6a5b8 white.
  The ref's 5x5 red blobs = several overlapping 3x3 dots (core 254,1,1 = stacked alpha; edge 234 = 0.8 red over map).
  One branch draws "!ENEMY" (sprite) instead — mode at unit +0x82 == 4; which mode that is was not resolved.
  Model: HudMapIcon::dot (None = sprite). Who gets which colour is the game layer's call (friendly/siege/etc. per branch).
- Clicks (RTS only, FUN_00717240 / Minimap_screen_to_world 0x717700): left = camera jump, right = move order →
  UiCommand::MinimapClick {world, value 0/1}.
- Gap: fog/vision areas (lighter circle in later shots; set_minimap_*_vision_area).

#### Name bars — FUN_00520e80 (text billboard) / FUN_00520270 (LifeBarre billboard 16x2 world units at head+95).
Text "%s (%d)" (name, level +0x124), font obj 0xa5cdc0 = verdana12_reg_outline, colour friendly (0x4c,0x7f,0xff),
hostile (0xf5,0x41,0x21); bar texture bar_french / bar_english inner UV (0..0.78125, 0..0.625).
ponytail: drawn in 2D at the reference distance size (frame 54x9, fill 50x5, text top = anchor-18, measured in
s04_04_mission1_hud), all scaled by HudUnit::bar_scale = ref camera distance / camera-to-unit distance (a world-space
billboard shrinks as 1/distance). MEDIUM.

#### Objectives — FUN_00704810: LIGHTBAR/DARKBAR alternate at (0, 0x94+20i), text (5, 0x97+20i) font obj 0xa5d3c8
"Goudy12 Bold Black" drawn white (TextOp::Diffuse). MAINGAME "line" 384x2 drawn under the main objectives at
y = 0x94+20n-1 (measured in s04_06_objective; not found in FUN_00704810, condition = HudModel::objectives_main). Gap: fade
in/out and the "(Time left: %1d minutes)" suffixes.

#### Attention message — FUN_004d8c10. CONFIRMED (corn_tl SSD 0.09, tmess 0.03)
PANEL (12,548), CORN_TL/TR (4|0x3d5, 540), CORN_BL/BR (4|0x3d5, 621), ICON_1 / icon / ICON_2 (28,564) (48x48 M000x
icons centred), text Goudy14(22) white (0xa51608) in (0x68,H-0xcc)-(W-0x1c,H-0x84), `_` toggles red, line gap 2,
last 3 lines shown. Icon 'TMESS' for tutorial messages, 'M0000' otherwise (SCRIPT_VM 344). Typewriter 40 chars/s =
FUN_004ace90 typed += dt·0.04 (CONFIRMED). Highlight red (220,32,32) measured.

#### Message log — FUN_006e1570 / FUN_006e22e0 over the in-game backdrop (FUN_006f21c0). CONFIRMED
cover (0x48,0x88); page_l (0x68,0x98), page_r (0x200,0x98); "Page %1d/%2d" Goudy14(22) at (0xa4,0x28c) left /
right edge 0x359; title "Mission %1d" Goudy_bold_18 (0xa48d48) at (0x7c,0xa0) / right edge 900; entries 4 per page at
y = 0xca+0x70 i, x 0x7c / 0x214: ICON_1, icon at (x,y) (M000* at +10,+10), ICON_2, text Times_10 (0xa56ff0) in
(x+0x50,y)-(x+0x15e,y+100); colour (0,0,0,0xbb); arrows COMMELE2 aw_l/aw_r (0x78|0x364, 0x28c). Pages per mission
ceil(n/4), total rounded to even. BACK = in-game BACK text button.

#### Mission brief — objmenu over the in-game backdrop, heading OBJMENU 15 upper-cased. MEDIUM
COMMELE2 cs_ot/ov/dw/ds 512x48 at (256, 210+63i) (measured), labels OBJMENU 4..8, 44, 63 centred in (x,y-5)-(x+511,y+27)
(0x73ee40), font chosen visually (Goudy14(22) white); OBJMENU tc boxes (8,24)/(876,24) with MISSION/TOTAL TIME (76/77)
and h:mm:ss. Gap: topic pages (Background text, objectives, township/commander lists, 2D/3D battleground) not built;
clicking a topic does nothing. The pages are objmenu.cpp (init FUN_0071d2d0: LONGBARs slide to (0,0x84)/(0,0x298),
TXT_FV at (0x19,0xb8), list rows (0x17, 0xdc+0x28 i)-(0x17f, ..+0x27); townships IMG_F/TXT_FH from x 0x400 (FUN_00653363 area),
commanders SCR_FH (0x186,0x1e2)) — a separate subsystem with no reference shots of the pages.

#### Quick-load prompt — G_POPUP box via `ui_confirm_popup` (menu_saveload.hpp, FUN_006d3cb0): body QSAVE 1, buttons
G_POPUP 1/2; measured frame (284,324) 456x120, buttons (331|520, 384). Result → UiCommand QuickLoad / QuickLoadCancel.

#### SSIM (masked; 3D world absent behind the HUD, translucent parts excluded), GL = VK
| case | 4:3 | 21:9 |
|---|---|---|
| hud (bars strip, party bars/shields, minimap frame edges) | 0.754 | 0.612 corners |
| objective | 0.552 | 0.516 |
| attention | 0.417 | 0.405 |
| quickload (popup) | 0.442 | 0.616 |
| msglog | 0.930 | 0.807 canvas |
| brief | 0.971 | 0.880 canvas |
Low values for objective/attention/quickload come from their translucent bodies over the (missing) world; visual
side-by-side is near-identical (work/ui/HudGame/c_*.png).

#### Out of scope / gaps
Character-details window (F1, s04_06_character_details); combo/level-up/+EXP floating numbers (MGEXP, ComboHUD);
critical HUD (Crit_drawHUD); siege/gate HP (TUI SWORD/EHPBAR/SBASE); hit/select flashing frames (CHIT/CSELECT/MSELECT,
DICON/DSWORD damage blink); morale pips (CMORAL); fog of war on the minimap. The cut-scene dialog box is now in
`GameUiBridge` (below).
HUD entry is now `hud_game(Ui&, HudModel&, std::vector<UiCommand>&)`: HoldResume{value = party index} from the
HOLD_*/RESUME* button (hover/press states), MinimapClick, QuickLoad, QuickLoadCancel.

### Mission brief pages (MissionBrief) — RE notes
Full notes (layout tables, data sources, SSIM, gaps): `work/ui/notes_MissionBrief.md`; per-page decomp facts:
`work/ui/MissionBrief/re_*.md`. Code `src/engine/hud_brief.{hpp,cpp}`; tests `ui_cases_brief.hpp`, `gl_/vk_ui_brief_test.cpp`,
`ui_brief_flow_test.cpp`; references `tools/reference/capture_ui.py brief` → `work/reference/shots/ui_brief_*.png`.

#### Summary
- objmenu: init FUN_0071d2d0, page switch FUN_00721a60, draw FUN_0071f690 (pass 1 / View3D / pass 2), input 0x720ca0.
  Pages +0x80: 1 Background, 2 Objectives, 3 Townships, 4 Commanders, 5 Battleground, 8 Strategic Command, 9 Siege
  Weapon; BACK/Esc → topic menu; Strategic/Siege topics only for chapter ≥ 5 (0x7226a0).
- File-backed content (`brief_content`): scenario_%02d.scr → .obj Background text, .bm 3D-map scene, .ter territories;
  unit.res + des/<weapon>.txt siege data; des/strategic_<n>.txt on selection. Objectives/commanders/weapons present are
  game-filled `HudModel` lists.
- The View3D map scene is CPU-rasterised into `Ui::baked_texture` (perspective, z-buffer, D3D per-vertex lighting).
- SSIM 4:3 (GL = VK): background 1.000, objectives 0.981, townships 0.981, townships-2 0.961, commanders 1.000,
  commanders-2 1.000 (actor rect excluded), battleground 0.966.
- Gaps: commander / siege 3D actors, battleground camera controls, 2D icon rotation, voice-over, user key names in
  Strategic text, no references for Strategic / Siege / 2D battleground.

### Game UI bridge (UiBridge) — live HudModel + in-game overlays
Code `src/engine/game_ui_bridge.{hpp,cpp}` (fill / events / handle / list_saves / speaker portraits),
`game_ui_bridge_screens.cpp` (overlays). Tests `game_ui_bridge_test.cpp` (headless, mission 1), `gl_/vk_ui_bridge_test.cpp`
(`ui_bridge_shots.hpp`). Fact sheet behind this section: work/ui/UiBridge (set.py dumps INTERFAC.SET tables).
- HUD fill: party = FUN_006ff950 (live units of the player's nation, rank > 3, class in DAT_00a6a570 table JOAN_OF_ARC,
  JEAN_DE_METZ, LAHIRE, ALENCON, ORLEAN, CHARLIE7, arthur_de_richemont, FLAVY; slot order = table order) CONFIRMED;
  panel portrait = unit.res CFileName + "1" (class +0x100) CONFIRMED; party portrait <CFILE>_ARMOR_LVn → JOANC2/1/3,
  JEANCn, LAHIRECn, ALENCOCn, else CFileName CONFIRMED; PLVLBAR (xp − LEVEL[l−1]) / (LEVEL[l] − LEVEL[l−1]) CONFIRMED.
  Minimap: <stem>.map records (0xa4 bytes: +2 key, +0x44 sprite, +0x92/+0x96 x/y) shown unless
  set_minimap_icon_display_enable(key, 0) HIGH; waypoints MAPICON wp_<type>; FUN_0070fbe0 hero icons "!joan_M"…,
  other friendly = green dot, hostile = red dot (siege / captain / "_H" mounted branches not modelled: HYPOTHESIS).
  Name bars: head + 95 projected with view_proj, bar_scale from the projected size of 16 world units (MEDIUM).
  Commanders 0x4cdd40, objectives = enabled end conditions with a description (Win first). Attention messages:
  states 3..5 of MissionUi, age = typed / 40, logged when they leave (FUN_00499e40); speaker portraits
  FUN_004ab330 (class) / FUN_004aa3c0 (name) tables, METZ/LAHIRE/ALENCON 1..3 by armour CONFIRMED.
- Letterbox FUN_006ca230: bars (W·55/640)·fade/255 = 88 px, black alpha = fade; wipe status 6/8 cut_scene_texture
  at cs.wipe, 7 black CONFIRMED. Dialog FUN_0063b830 (cs.status 2, fade 255): ICON_1/portrait/ICON_2 (28,689), text
  Goudy14(22) white (asm 0x63bae5) in (119,693) 848x36, 40 chars/s, click/Space/Enter finishes typing; done when
  typed and the voice stopped (deviation: voiced lines also type out). Choices below the text at x 134, 5 px apart;
  hovered = red (HYPOTHESIS), click answers. Continue indicator (dot<n>_up/_hl, art not in INTERFAC.SET): not drawn.
- Title FUN_00761340: black (bg alpha) + font_verdana_12_bold ×5 centred, fade-in / hold / fade-out CONFIRMED.
- Loading FUN_00756430: M<n> scr (0,194) / title (416,72) / name (276,352), SL_BG1 vignette, SL_COM bar (fill
  408,710; frame 410,712; head 405+2%,708), "LOADING" (SCELOAD 1) Goudy_bold_18 (asm 0x75987d) at (450,744); stages
  3 s black, 1 s title, 1 s name, 1 s reveal, bar 20 %/s, wait for 100 %, 2 s bar fade, 0.5 s to black. SSIM 1.000
  (loading_7s, stage 2) / 1.000 (mission1_cutscene, stage 5 at 45 %), GL.
- Win FUN_006e6dc0: VICT G100 0.9..0.6 → G50 (256,320) + GGLOW pulse (50 ms steps), text VICTS n Goudy18(27) white
  in (172,450)-(852,586), then MBUT "Next Mission" (412,694) → game-end menu: in-game backdrop titled OBJMENU 14,
  time boxes, RESTART MISSION / LOAD GAME / CONTINUE (VICT 5) → RestartMission / LoadMenu / ContinueCampaign.
  Gap: the menu's topic list (De-brief page 7 …) and the button positions (HYPOTHESIS: bottom row).
- Game over FUN_006b7160: GAMEOVER G100 1080x144..720x96 → G50 (212,344) + GGLOW (192,324), call_gameover_msg text,
  MBUT buttons (412, 564/604/644) GAMEOVER 1..3 → RestartMission / LoadMenu / QuitToMain CONFIRMED. Scenario 6's
  "restart = load <text>.SAV" variant not modelled.
