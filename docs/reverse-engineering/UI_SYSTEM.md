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
(3.073) -> buttons once stripe=1 (3.573). SE: see "Sound effects" below.
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

#### Sound effects (SE_play_by_name, SOUND table) — RE
Port seam: `Ui::sound(code)` / `Ui::sound_at(code, t, at)` queue SOUND codes per frame (`Ui::sounds`, cleared by
`Ui::begin`); `Client` and `UiRunner` (`--ui-screen`) play them 2D after drawing (`AudioSystem::play_sound`, Sfx bus).
Test: `tests/engine/ui_sounds_test.cpp` (no GPU; every queued code must exist in SOUND).
- `IF_BUTTON_MOUSE_DOWN`: en2d button input 0x6218c0 plays it on activation (release inside, key shortcut,
  joystick button) when widget +0x90 is set (last init arg of 0x621540/0x6211d0 = 1: every menu/HUD/options
  button, slider and combo arrows). Port: `Ui::clicked()` (at most once per frame; `clicked(r, false)` for
  non-buttons) and `Ui::shortcut(key)` for Esc on BACK/CANCEL buttons.
- Main menu tick 0x70acb0: MENU_SLIDE_IN as "main" starts its fade (opening, t = 1.0 s); BUTTON_SLIDE_MANY when
  the first main/difficulty button starts sliding in (3.573 s / 1.2 s back from Load-Options / 0 from Difficulty)
  and when the buttons start sliding out (the click); MENU_SLIDE_OUT when a difficulty pick starts the stripe/Joan
  fade (0.566 s after the click).
- Credits music: none of its own. Music_process 0x4e6fe0 maps states 0/1/3/0xf (menus and credits) to MUSIC type M,
  so the menu theme keeps playing (port: unchanged `GameAudio::menu()` music); credits start 0x6b56b0 plays nothing.

#### Gaps
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
  Not reproduced: RemapKey_assign's duplicate handling.
- Joystick capture (GamepadPlay): in the Gamepad/Joystick scheme (input method 2 = key scheme 1, listed on the Input
  page only while a pad is connected as 0x7435e0) a pad button press binds through RemapKey_assign(1, action, device
  1, J_JOYPAD-1) — J_JOYPAD n = SDL positional button table `joypad_button` (game_input.hpp; DInput button numbers
  of the original pads are not recoverable, HYPOTHESIS mapping: 1..4 South/East/West/North, 5/6 LB/RB, 7/8 D-pad
  up/down, 9/10 L3/R3, 11/12 D-pad left/right), which also clears J_KEY like the original's pad field write. Start
  (fixed Main Game Menu, refused by RemapKey_assign) cancels; in other schemes East = Escape cancels and other pad
  buttons are ignored. Test `engine.gamepad`.

#### Pad focus navigation (GamepadPlay, port extension; the original menus are mouse/keyboard only)
`Ui::nav_modal(scope)` (ui_core.hpp) turns the rects a modal screen passes to hover()/clicked() into a focus ring; the
focused rect answers hover() so every screen draws its original hover sprite/colour/marks for it (verified 1:1 against
the mouse hover and the original hover captures: `engine.gl_gamepad` / `engine.vk_gamepad`). Slider/combo rows take
pad left/right through `nav_adjust()`; LB/RB page through `nav_page()`. Scopes: menus (ui_screens.cpp, per screen /
options page / popup state), brief pages + message log + quick-load popup (hud_game.cpp), dialog box, game over,
victory, de-brief (game_ui_bridge_screens.cpp).

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
- Root leave: buttons back to y = 62 at 0/50/100/150 ms in widget order (bottom button first, RE via 0x73db40), fade out.
- Page enter: labels from x = -612 at 500 ms, controls drop from (448, 62) bottom first 550 + 50 k ms, buttons
  500 ms instant; FRAME to its page y at 500 ms. Page leave: labels out at 0, controls 50 k ms (order INF),
  FRAME to -448 at video 400 / audio 200 / input 250 / game 100 ms.
- Remap enter: buttons 500 ms, rows bottom first 550..1000 ms (slots from y = 70, key boxes from x = 1024),
  category combo 1050 ms; leave 0x748830: rows 50 + 50 i ms.
- MenuModel::options_prev = page being left (5 = remap, -1 = just opened); the leaving page is drawn on a
  scratch model copy.
- Close (BACK / Esc on the root, 0x73ca80 -> 0x73cc00(6), MenuModel::options_closing): root leave (buttons to
  y = 62 at 0/50/100/150 ms in widget order, bottom button first, fade 6/s) + BACK fade 6/s at once + LONGBARs to
  x = -1024 / +1024 after 200 ms. Tick 0x73db40 ends the menu once the bottom root button is at y = 62 with alpha 0
  and both bars are out: 0.2 + 0.3137 = 0.514 s, then `previous` (main: Joan/stripe fade back; in game: the
  in-game menu). Input ignored meanwhile. RE; checked by ui_sounds_test (±1 frame).

#### Sound effects — RE
- IF_SETTING_BUTTON_CLICK: only the four root buttons (0x73d830, before 0x73cc00(page)); value changes play the
  arrow buttons' IF_BUTTON_MOUSE_DOWN.
- BUTTON_SLIDE_MANY: every leave (root case 0, page leaves 0x74c2e0/0x7420e0/0x7480e0/0x7433d0/0x748830, close
  case 6 — close plays it twice, case 0 + case 6), opening (0x73c9d0 runs case 0 leave), and when the entered
  page's watched widget delay expires (0x73db40 root bottom button 500 ms; page ticks 0x74b550 / 0x741960 /
  0x742ee0 / 0x746ff0: 500 ms, the first enter delay of every page; remap row 0: 1000 ms — which widget each
  tick watches is INF).

#### Gaps
- Not reproduced: a second BUTTON_SLIDE_MANY when a page-tick widget's
  leave delay expires (INF whether it happens).
- Opened in game by its hotkey (previous = none) the original slides INGAME_B out (0x6f26e0, MENU_SLIDE_OUT)
  before resuming; the port resumes once the close slide ends (same as Load/Save).

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
  cancels / leaves; SAVE / SAVE NEW → entry (IN_FRAME (228,660+lc), IN_BG (286,670+lc), text (286,671+lc)).
  "Enter description here" (SAVEMENU 8) and the delete question (SAVEMENU 7) are never referenced by the exe →
  not shown. Entry copies the selected slot's description into obj+0x449d (SAVE NEW: an out-of-range slot read,
  shown empty here), then FUN_006ca000 + FUN_0045bf80 make the text box visible and focused. Return (checked before
  the text box) saves: GameFile_save, thumbnail FUN_004731e0(camera), list refresh FUN_00753060(1) (page 1),
  state 0 — the screen stays open (no FUN_0074dbc0). Escape cancels. HIGH.
- Description text box = en2d txtbox2d (`d:\ww\en2d\txtbox2d.cpp`): init FUN_0062d4f0(x1 0x11e, y 0x2d3, x2 0x2e2,
  buf, size 0x40, font 0xa51608 Goudy14(22) white, align 1 = left), moved to y 0x29f + lc on entry. Keys
  FUN_0062dd10: chars 0x20..0xff (cp1252 WM_CHAR) insert at the caret replacing the selection, refused when the text
  has 63 bytes, for '~' (0x7e) or when the result is not narrower than x2 - x1 + 1 = 453 px; Backspace 8 / Delete
  0x5300 delete the selection or one char; Left 0x4b00 / Right 0x4d00 / Home 0x4700 / End 0x4f00 move the caret
  (+0x87), Shift keeps the selection anchor (+0x8d); held keys repeat (FUN_00595fb0). Mouse FUN_0062e490: press in
  the box puts the caret at the last index whose x <= mouse (0x62e9a0), dragging (+0x84) moves only the caret
  (selection), double click selects all (0x62e950). Draw FUN_0062d730: caret = 1 px line at x1 + width(prefix)
  from y1 to y1 - 1 + font height (FUN_00632100, text colour) while 200..399 ms into a 400 ms cycle; every handled
  key restarts the cycle at 200 ms (visible); selection = line at the bottom. HIGH (code in menu_saveload.cpp).
- Thumbnail (FUN_004731e0, decompile failed; argument = the camera DAT_00a48ccc, render texture 512x256 from
  D3D_create_render_texture 0x603400, skipped by `no_save_preview_pict.sys`): the original Save/Mission1-AutoSave.BMP
  holds the 4:3 view at 320x240 in the top-left, black elsewhere (measured). Native: the flow captures the world
  frame without UI when the in-game menu or the Save screen opens (Esc / F7; the world is paused until the save,
  so this equals the original's render at save time) and at F5, Session::save writes `<stem>.bmp` next to the
  `.jsav` (same 512x256 24-bit layout, 4:3 centre crop of the viewport box-filtered to 320x240); DeleteSave removes it.
  Shown for the selected slot only, 320x240 of 512x256 at (601,253+lc) (Ui::bitmap reads the absolute path).
- `list_saves` (game_ui_bridge.cpp): native `*.jsav` (META) + Save/*.SAV newest first (GameFile_enum_saves 0x4770a0,
  comparator 0x477bd0 on saved FILETIME); `SaveSlot::file` = full path. GameFlow re-scans the folder whenever the
  Load/Save screen opens (F7/F8, main menu LOAD, in-game LOAD/SAVE) and after a save or delete.

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
- SE (RE, port via `Ui::sound_at` / `Ui::sound`): MENU_SLIDE_IN at the backdrop slide-in only when it was down
  (0x6f24d0 checks its +0 flag); MENU_SLIDE_OUT at the backdrop slide-out (0x6f26e0); BUTTON_SLIDE_MANY at the
  in-game menu slide-in (0x6f43e0 state 1) and slide-out (0x6f38a0, the click), the Load/Save first slide step
  (0x751750) and slide-out (0x74dbc0). Esc on BACK/CANCEL is a button shortcut (0x6218c0 key 0x1b) and clicks too.

#### Gaps
- No reference shots for a selected slot / delete confirm / description entry / in-game confirm box / any
  mid-animation frame: those use decomp values only (gl_/vk_save_thumbnail check the selected-slot thumbnail 1:1
  against its BMP and the surrounding screen against s04_07_load_menu).
- Text box: Shift+arrow selection and double-click select-all are not ported (UiInput has no modifiers / click
  count); the IN_FRAME / IN_BG / text slide (500 px/s from y 0x2c8 / 0x2d2) is drawn at its end position.
- Autosaves (Mission::autosave, game layer) still write no thumbnail. (The capture itself is an off-screen frame:
  begin_frame -> submit -> read_back without end_frame, nothing extra presented.)

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
  centre (912.5,110) exactly). Tiles `<world>` z<zoom>r<row>c<col> 200x200 px, table named after the World (World +6,
  FUN_0071bf10; `S5L1W` for S9L1, which plays scenario 5's world); tile world size = World+0x54/+0x58 (world w/h floats,
  disasm 0x71baa4..) / max col / max row; world y up (row 1 = top). World+0x54/+0x58 = `<world>.wor` CreateGrid X/Y x
  2048 (FUN_0044e8c0, `(float)(n << 0xb)`): S1L1 8x8 = 16384, S5L1 16x40 = 32768x81920 - one z1 tile = 8192 units on
  every map. Outside the map: tile `z<zoom>b`. Tiles carry alpha 170 in the texture (no extra blend).
- Rotation (minimap +0x2b = DAT_00a4f3ac, set_minimap_rotation; 1 in S3L1, S4L1, S5L1, S9L1, S6L1, S7L1): tiles
  (FUN_007141a0 blits with the rotation), icons (FUN_00716010), dots (FUN_0070fbe0), waypoints (FUN_00718540), fog and
  clicks (Minimap_screen_to_world) turn r x 90 degrees counter-clockwise about the view centre: view pixel (u, v) ->
  r1 (v, 200-u), r2 (200-u, 200-v), r3 (199.5-v, u); rotation 1 = world +x up. The player arrow angle loses r x 90.
  Port: `hud_minimap_turn`, UiQuad::turns for the tiles.
- Icons: every world level's `<node>.MAP` (FUN_0071bf10 loop over World +0x72 levels), position + level origin
  (Level +0x44/+0x48).
- icons centred, drawn only when fully inside the view; player = MAPICON "cursor" rotated by heading (type 2).
  ponytail: rotation done with per-texel quads (UiQuad::turns only does 90 degree steps).
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
- Fog / vision areas: see "Minimap vision" below (hud_mapobj.cpp).

#### Name bars — FUN_00520e80 (text billboard) / FUN_00520270 (LifeBarre billboard 16x2 world units at head+95).
Text "%s (%d)" (name, level +0x124), font obj 0xa5cdc0 = verdana12_reg_outline, colour friendly (0x4c,0x7f,0xff),
hostile (0xf5,0x41,0x21); bar texture bar_french / bar_english inner UV (0..0.78125, 0..0.625).
ponytail: drawn in 2D at the reference distance size (frame 54x9, fill 50x5, text top = anchor-18, measured in
s04_04_mission1_hud), all scaled by HudUnit::bar_scale = ref camera distance / camera-to-unit distance (a world-space
billboard shrinks as 1/distance). MEDIUM.

#### Objectives — FUN_00704810: LIGHTBAR/DARKBAR alternate at (0, 0x94+20i), text (5, 0x97+20i) font obj 0xa5d3c8
"Goudy12 Bold Black" drawn white (TextOp::Diffuse). "LINE" (MAINGAME 384x2) at (0, 0x93+20n) before the first side
row when main rows were drawn (FUN_00704810 second pass, &DAT_009deba4 = "LINE"). Fade, highlights and suffixes: see
"Objective panel" below.

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

#### Interact indicator, target HUD, party effects (HudGaps) — `hud_effects.{hpp,cpp}`, `hud_game.cpp` party()
- Indicator FUN_006d3590 (targets per tick; show 0x6d3450 / hide 0x6d3480 = MissionEvent::InteractPrompt a) + draw
  FUN_006d23b0 (Sys_disp_frame, late): GMSG `BG` then `BORDER` (288x72) at (384,100), text Goudy12(18) yellow (font obj
  0xa58ac0, asm 0x6d2d39) wrapped and centred both ways (0x626e00 flags) in (392,106)-(652,162). Text priority: tied
  prisoner GMSG 4, talk GMSG 2 (unit name +0x3d), recruit GMSG 12, item ItemRes +0x20 `Description` (gate: format
  (key, state +0xd1) "%2?{close|open}"; box/ladder (key); else raw), mount GMSG 11, barrack GMSG 6, workshop GMSG 7 /
  8 wood / 9 iron, GMSG 10 when the resource needs Alencon. `%%1s` texts are formatted twice (args, then the key =
  KeyMap_get_name(scheme, 0x11)). CONFIRMED (decomp + asm); `%N?{a|b}` = a when N != 0 is INF.
- Bribe line (same function, flag ebp-0x1c): no box, 0x626320 in (820,220)-(1000,320), same font: GMSG 3 (name, cost,
  gold) + key, or GMSG 5 (name, cost - gold) when gold is short; name = class name if class ranking < 3 else unit name.
- Interact results: only bribe_no_gold has UI feedback — Unit_bribe 0x4e8500 posts attention message MESSAGE 2 once
  (FUN_004ad710; the bridge posts it in draw since it needs t_interf); bribe's RTS_OTHER_NARRATOR_001 is a game Sound
  event. free_prisoner / talk / recruit / item handlers (0x43bef0, 0x43bd40, 0x43c210, 0x43bc50) draw nothing else.
- Target HUD (TUI set, head of FUN_006f9b60; `hud_target`): set by FUN_006fe280 (player hits a unit: Melee_tryHit,
  Ranged_tryHit, Unit_onMessage = Ev::Hit other == player) / FUN_006fe340 (Gate_on_event / Wall_on_event, sender ==
  player = new MissionEvent Ui "item_hit"), cleared 3000 ms after the last set; shown HP chases the real one by <= 40
  per HUD update (FUN_006fd8b0). Unit (class +0x118 0/1): portrait FUN_00703880 table at (0,0x15), SWORD (0,0x68),
  SWORDBG (0x2e,0x72), EHPBAR width ftol(hp/max*94) (min 1) at (0x2f,0x72) + EHPTIP after it; siege / structures:
  EHPBAR *78 at (0x43,0x72), SBASE (0,0x50), icon (8,0x3c) (TOWER/WALL/GATE/WOODGATE/DOOR/TRUNK by StructionType; gate
  max = item +0xc6). Text "<class name> (<level>)" (or the billboard name when the team leader's rank is 4) at (8,0x82),
  font "Goudy12(18) target unit" (0xa56da0). Matches s08_combat.mp4 @37 s (ENGLIC, "English Light Infantry Captain (1)").
- Party loop: hit (status id 1, Unit_onMessage) -> 1000 ms DICON (t+244, H-0x8c) + DSWORD1..9 (100 ms frames) +
  CHIT1..3 (t+224, H-100) / MHIT1..3 (t+230, H-0x15) for frames 1..3; HP below {DANGER} INDANGER (0.2): CPTOP, DICON
  hidden every 3rd 100 ms frame, DSWORD loop; CMORAL pips (morale*6/100 at DAT_009de6cc {60..15} + t+224, H-0xd),
  morale = FUN_006ffb40 average of leader + troops. Positions CONFIRMED (decomp).
- HudFinish (CONFIRMED, decomp + asm): CSELECT 1..3 = (DAT_00a500b2 real ms - DAT_00a4d75f) / 100 % 8; the start is
  set when the character window opens / switches unit (FUN_006f7d80, +0x6ae open / +0x6af unit). CSELECT on the
  window's unit (+ MSELECT when it has troops); with the window closed, in RTS mode CSELECT on ViewCtrl +0x1238
  (Rts_select_commander / Rts_click_select; `GameCamera::rts_commander()`). Every party sprite and the troop count
  (not the HOLD/RESUME buttons) use 0x80ffffff when unit +0x2f (control) or Unit_is_task_locked (+0x2e1) or
  FUN_00717a90 fails: |unit - player| >= half the minimap view (+4 -> +0x21/+0x25 = world size of one 200 px tile;
  true without a minimap). RTS branch of FUN_006d23b0: the same GMSG box and font with t_interf RTS 1 ("%1d light
  infantry %1p{unit|units} %1p{is|are} required to operate the %2s.", class +0x1ad crew slots, class name) for the
  siege weapon Rts_pick_target stores in DAT_00a5704d (unit_class 8 with +0x38c, any selection;
  `GameCamera::rts_hover_siege()`), unless it is hostile to the player or all +0x378 crew slots hold live units.
  HUD tick FUN_006fd8b0 (Game_process_tick, per game tick = `GameUiBridge::hud_tick`): panel bars +0x20 HP (step
  <= 9), +0x24 EP (<= 3), +0x28 EXP (+500, smaller gaps exact) chase the player (FUN_00703750 snaps them on a player
  change); the bar texts and level % use the real values; target +0x2c snaps to the real HP on a new target
  (FUN_006fe280 / FUN_006fe340), then chases by <= 40 per tick (0 when the target's HP is 0).
- In game: `work/PlayableGame2/HudFix/play_interact.txt` (--mission 7, `warp french recruit` into the S7L1 barrack
  interact box): GMSG 6 "Press R to enter the barrack and recruit units." in the box at (384,100), GL = VK by eye.
  (S1L1 warps to BreakBoxPlace / Horse Message / Castle Gate Place / JeanMessage / Event3 Message showed no prompt.)
- Gaps: the hit flash restarts at Ev::Hit (the exe holds it 1000 ms after the status-1 flag clears, one frame late);
  the RTS crew box was checked headlessly only (S1L1 has no siege weapon, no in-game shot); no original capture shows
  an interact prompt (s04/s08 shots and videos checked).

#### Critical-attack HUD — Crit_drawHUD 0x45fae0 (+ kill interface FUN_006df180) — `hud_crit.{hpp,cpp}`
- HUD object 0xa4f820 (CONFIRMED, field use in Crit_drawHUD / Crit_key 0x462810 / Crit_startSequence 0x462970 /
  init 0x45f860): +0xc/+0x10/+0x14 sets CRITICAL / CRITI_F / CRITI_K, +0x18 sequence shown, +0x19 CA, +0x1a CF,
  +0x1b CK, +0x1d progress (short), +0x1f combo (6 chars), +0x25 key count = 'A'/'B' chars, +0x27 sequence start,
  +0x2f CRITATT time (doubles, real ms DAT_00a500ba), +0x38 enable (`set_critical_attack_enabled`). Drawn by
  FUN_006f9b60 right after the panel text, before the party (CONFIRMED). Everything is skipped while +0x38 is clear.
- Trigger: Player_critCheck 0x416680 → Crit_startSequence when HP trail - HP > CRI_DIFF (10); the combo is a random
  free AnimGrp record; sequence hidden after CRI_TIME (10000 ms). Crit_key: LMB 'A' / RMB 'B' (first attack resets,
  follow-ups continue), wrong key → progress 0, complete → sound CRITATT, +0x19 = 1, +0x2f = now, player +0x1b4 (crit
  flag: next melee hit x2). Melee_applyDamage 0x509950: crit hit that kills → sound CRITKILL + FUN_00509d70
  (ECX 0xa4f820, asm 0x509cb6) +0x1b = 1, keeping +0x2f. Model: `HudModel::crit` from World::crit (fill) and
  Ev::Crit CRITSEQ / CRITATT / CRITKILL (on_world_events). Mission 1 enables it only after 600 s of scenario count time
  (S1L1_tactic.sce), so no reference capture shows it.
- Key icons: i-th key at (0x12 + 0x2d i, H-0xa1) (BottomLeft), 40x40 `LIGHT`/`HEAVY` for i < progress, `LIGHTOFF` /
  `HEAVYOFF` after (CONFIRMED).
- Banner (CONFIRMED, asm 0x45fe12..0x460846 for CF; CA/CK identical code): priority CF > CK > CA; CA centred on
  (W/2, H/2-100), CK/CF on (W/2, H/2); t = now - +0x2f, GAMEPLAY.RES {CRITICAL} FADE_IN1/FADE_IN2/GLOW_IN1..3 = 200 ms
  (globals 0xa3ccfc/0xa3ccf8/0xa3cbfc/0xa3cbf8/0xa3cbf4; FADE_DUR / FADE_OUT are loaded but never read).
  t ≤ F1: r = min(t/F1,1), rect = centre ± r·1.1·size/2 (each corner __ftol, inclusive), alpha ftol(255 r);
  t ≤ F1+F2: k = 1.1 - min((t-F1)/F2,1)/10, alpha 255; then natural size at (W/2 - w/2, cy - h/2).
  Glows (natural size, same centre, alpha 255·p): (F, F+G1] `<p>GLOW01` p; (.., +G2] GLOW01 1-p + GLOW02 p; (.., +G3]
  GLOW02 1-p + GLOW03 p (p = phase fraction, float); after F+G1+G2+G3 (1000 ms) the flag is cleared. Prefixes "" /
  CK / CF. CK's branch also clears +0x19 every frame. All draws are plain alpha blends (op 3); the faded ones use
  stage selectors (.,0,2,1/0) = diffuse alpha modulated, static ones (0,0,0) = texture only. One CA GLOW02 call uses
  layer 2 instead of 10 (draw-list order; not modelled).
- Never drawn (CONFIRMED by byte search of joan.exe): CF — no code sets +0x1a (CRITI_F is loaded, `hud_crit` keeps the
  branch for banner = 3); `Slash` / `CFSlash` / `CKSlash` (no name string); HURT `hurt_bg` (only the set name "HURT"
  is referenced, by the loader "main_game_hurt_interface_res").
- Kill interface (set KILL; object 0xa50518; CONFIRMED): `start_kill_interface(_2)(mode)` (366/369, MissionUi::kill)
  shows panel 0 / 1; FUN_006df180 draws `panel` + icon at (0x1ac, 8), panel 1 at y 0x33 when panel 0 is shown (else 8),
  text in (500, y+4)-(0x250, y+28) centred (0x626130, selectors 0,0,0,0 = atlas colours) with font obj 0xa48f70
  "Celtic 10pt English". Mode → icon / value: 0 `icon` citizens killed (DAT_00a5918d), 1 `ficon` female citizens
  (0xa59199), 2 `hicon` houses burnt (0xa591a5), 4 `icon` citizens saved (0xa591c8), 5 `gold` gold crates (0xa591b6),
  each "count/limit" (limit = `set_kill_citizen_limit(_2)`, obj +10 / +0xe, FUN_0058fca0 plain itoa); 3 `ticon`
  "%d:%d" of limit - DAT_00a59171/1000 s (m:s, unpadded). Hidden while DAT_00a5d384 (pause/cut-scene). Used by S3L1,
  S4L1, S5L1, S7L1, S9L1. Drawn TopLeft (x 0x1ac is an absolute constant, centred only at 1024).
- Tests: `engine.hud_crit` (headless: model, icon positions, CA rects/alpha at 100/300/500/700 ms, CK swap, clear after
  1000 ms, kill panels), `engine.gl_hud_crit` / `engine.vk_hud_crit` (placement at 4:3 + 21:9 anchors, HUD panels vs
  s04_04 masked SSIM). Autoplay `work/PlayableGame2/HudGaps/play_crit.txt` (seed 1: idle 19100 frames past the 600 s
  gate, run to the camp, HP 60→40 starts "AB" at ~19775, LMB lights Light at ~19844, RMB follow-up 19850 → CRITATT;
  shots 19848 icons, 19856 fade-in, 19864..19880 glows; GL = VK by eye in work/PlayableGame2/HudGaps/CritHud/<r>/).
  Short version: `work/PlayableGame2/HudFix/play_crit.txt` (autoplay test hook `crit` at frame 100 = the state after
  600 s: S1L1_tactic re-runs `set_critical_attack_enabled(get_scenario_count_time > 600000)` every pass, so the hook
  sets scenario count time 1 past 600000 ms besides critical attacks on + HP trail > CRI_DIFF; the next tick's
  Player_critCheck starts "AAAA", chained LMB lights the icons (f140: 3 of 4), CRITATT banner at f172; GL = VK).
- Clocks (CONFIRMED, Sys_main_loop asm 0x499cae: DAT_00a500b2 += real ms, DAT_00a500ba += real ms x game speed):
  banner and the CRI_TIME timeout both use DAT_00a500ba = game time, so the port advances `banner_ms` with ui.dt()
  only while the game runs (`HudModel::sim_paused`), and World::crit_check (per tick, game time) is equivalent to the
  draw-time check (the exe hides at now - start >= CRI_TIME, the port at > : at most one tick later).
- Gaps: no original capture of the crit HUD or kill panel exists (verified only against the decomp + sprite tables);
  the kill interface's per-unit billboard flag (unit +0x31d +0x52 for nation-0 units in modes 0/1/4) is not modelled.

#### EXP counter / level-up / combo HUD (MGEXP) — FUN_006fe660, FUN_006ffed0, ComboHUD_update 0x702740 — `hud_exp.{hpp,cpp}`
- Drawn by FUN_006f9b60 right after the PLVLBAR (param&1), in that order (CONFIRMED); all skipped in RTS mode
  (DAT_00a5b9f8 == 4; the combo HUD also resets). Table MGEXP (HUD object +0x14), en2d sprite draw 0x633500 (trailing
  right/bottom args = stretched rect). Clock: game ms DAT_00a500ba → `HudModel::exp.now` advanced by `ui.dt()` in
  `hud_exp()` unless `sim_paused`; events are stamped with the clock of the last drawn frame. Float constants from the
  disassembly.
- EXP queue add FUN_006fe520 (CONFIRMED): called by Unit_add_exp 0x5229a0 for the player only (also by debug key 0x6c
  with a random value); up to 9 entries {value +0x3c, strlen(itoa) +0x60, rolling value +0x69 = 0, state +0x8d = 0,
  timer +0x96 = 0}; the slide timer +0xba restarts only when the queue was empty. Port feed: `game::Ev::XpGain` of the
  player (`hud_exp_events` from `GameUiBridge::on_world_events`).
- Draw FUN_006fe660, first entry only (CONFIRMED): `EXPTEXT` at (W-125t, H-0x104), t = min(dt,200)/200 since +0xba
  (alpha t*255 is computed but unused), `EXPTEXT2` once t = 1 / in states 1-3 at (W-0x7d, H-0x104); the last entry
  slides back out in state 4 (t = 1 - ...). Digits right to left at (W-0x50-0x50i, H-0x140), names "E<d>_<k>":
  state 0 roll (`_1`): digit i is drawn only once digits < i match; the unmatched one steps +10^i (+2·10^i while ≥ 2
  short) per call — the exe steps once per drawn frame of its fixed 30 Hz loop (FrameTimer 30/30), so the port steps
  once per 1000/30 ms of real time (equivalent; also while paused, as the exe keeps drawing); state 1 (`_2`, one frame), 2 (`_3`, hold until ≥ 1500 ms, cmp 0x5dc), 3 (`_2`, one frame; 2→3→4 happen in
  one frame when the value has ≥ 2 digits, quirk kept), 4 (`_1`, alpha 255 - min(dt,300)/300·255; digits shrink to
  s = alpha/255 (80s × 52s) and fly from the block centre ((0x400 + left)/2, H-0x126) toward (0x3fc, 0x212)); alpha 0
  drops the entry. The roll / fly-out never shows in a reference: positions CONFIRMED by decomp, look INF.
- Level-up FUN_00703680 (from Unit_check_level_up, player, level +0x124) / FUN_006ffed0 states +0xc2 (CONFIRMED):
  1 LEVELUP0 alpha = scale = min(dt,500)/500; 2 LEVELUP1, 3 LEVELUP2 (one frame each); 4 LEVELUP3 hold ≥ 2200 ms
  (0x898); 5 LEVELUP3 shrink/fade out + LEVEL1 grow/fade in over 300 ms; 6 LEVEL2, 7 LEVEL3 hold 2200 ms (7 is a
  separate `if`: drawn in the frame 6 ends), 8 LEVEL2, 9 LEVEL1 shrink/fade out 300 ms. Banners scaled about
  (0x400 - w/2, H-400 + h/2) (natural rect (776, H-400) 248x32); LEVEL<k> at x = (0x400 - w/2 - sw/2) - n·80·s and the
  level digits "L<d>_<k>" at x = 1024 - (i+1)·80s, 80s wide, squeezed to LEVEL's height 36s (exe passes that bottom).
  Port feed: `Ev::LevelUp` (value = new level) of the player.
- Combo: Unit_onMessage 0x75d540 (melee / down-attack hit by the player): ++DAT_00a5ba62 and, if > 1 and
  DAT_00a4d6e4 == 0 (never written in the decomp; ignored), ComboHUD_show 0x7036f0 = state +0xcd 1, count +0xce, timer
  +0xd0 — every hit restarts the fade-in. ComboHUD_update: 1 COMBO0 scale/alpha min(dt,500)/500; 2 COMBO1 one frame;
  3 COMBO1 + digits "C<d>-1" at x = 864 - (i+1)·80, 80 × 62 (top-0x1f .. top+0x1f, stretched from 52), count follows
  DAT_00a5ba62 while it grows, ≥ 2000 ms without growth (0x7d0) → 4 (count and DAT_00a5ba62 zeroed); 4 COMBO0 one
  frame; 5 COMBO0 shrink/fade out 300 ms. Banners about (0x400 - w/2, H-0x1c7 + h/2) → COMBO 824..1024 × H-455..H-427.
  Port feed: `World::combo_hits` read after each tick (it increments on the same hits; World resets it 2 s after the
  last hit, `world_sim.cpp`), so the HUD does not write it back. REF: s08_combat.mp4 @ 34-39 s shows white COMBO0
  fading in, then red "2 COMBO" at the decomp position (digit 790..850 × 282..344, COMBO 830..1020 × 315..340).
- Literal x constants: the exe uses 0x400 / 0x3fc / 0x212 (not W/H) for the banners and the EXP fly target — equal to
  the right edge / H-238 only at 1024x768. The port draws the whole group in the bottom-right frame (keeps the
  1024x768 look on every aspect ratio).
- Tests: `engine.hud_exp` (headless: queue cap/digits, roll sequence 247 → 2,4,6,7,27,47,247, hold 1500 / fade 300,
  queued entry follow-up, level-up state durations, combo show/grow/hide, RTS reset, live feed: player XP / level-up
  / combo_hits > 1 through `on_world_events`, other units ignored); `engine.gl_hud_exp` / `engine.vk_hud_exp`
  (mid-roll EXP + LEVELUP3 + "2 COMBO", "LEVEL 12", combo alone; 21:9 bottom-right group vs 4:3, combo vs the video
  frame `work/PlayableGame2/HudGaps/ExpCombo/ref_combo_39s.png`). GL = VK: 21:9 group vs 4:3 SSIM all 0.964 / level
  0.932 / combo 0.951; combo vs the video frame 0.211 (world behind vs black) with the best-matching shift (1,1) px.
  Autoplay `work/PlayableGame2/HudGaps/play_exp.txt`: combo fight (f600/f620 "2 COMBO"), then `win` → objectives
  award the hero EXP 1000 (process_end_conditions → add_xp) → "1000 +EXP" + LEVEL UP (f652..f712), "Level 2" (f740..);
  button-mashing alone kills nobody before Joan dies (~f1000), as in the original capture.
- Gaps: no reference for the EXP counter / level-up banner (no XP gained in the
  captures); DAT_00a4d6e4 meaning unknown.

#### Minimap vision (fog of war) — FUN_0070ec50 / FUN_00719900 / FUN_00719b40 / FUN_00719e60. HIGH
Code `src/engine/hud_mapobj.cpp` (`hud_mapobj_fill`, `hud_vision_mask`, `hud_minimap_fog`, `hud_vision_visible`);
tests `engine.hud_mapobj`, `engine.gl_/vk_hud_mapobj`.
- Minimap object 0xa4f3..: +0x478 = 200x200 byte mask (operator new 40000 in init 0x70e1d0), +0x47c = 256x256 texture
  (FUN_00605e20(0x100,0x100,1,0xb)). FUN_0070ec50 (HUD tick FUN_006fd8b0, every frame while the minimap is on and the
  player's world exists) clears the mask, sets 1 for: (a) every live unit (not Unit_is_dead) of the player's nation
  (unit +2) or allied to it (relation +3 == 4) whose class is not citizen (+2 == 0), HORSE (+0x118 == 2) or special 7:
  circle of radius unit +0x3c1 around ftol(pos + 0.5) (GAMEPLAY.RES {MINIMAP} VISION_R = 2000, DAT_00a3cc44, is only
  the +0x3c1 default; set_unit_(team_)vision_range change it); (b) every area of territories (FUN_00420ff0 array)
  owned by that nation or an ally (+0x38 > 0; shapes +0x48[i], coords +0x4c/+0x5c/+0x6c/+0x7c.., radius +0xcc);
  (c) the ten script areas (+0x2f8 enabled, +0x302 shape, coords +0x30c.., radius +0x44c). Then the texture texels =
  mask ? 0 : 0x80000000.
- Raster (view px = (x - ox)·sx, (oy - y)·sy, origin +0x1d/+0x21, sx/sy = 200 / world span of the zoom): circle rows
  ftol(dy·sy ∓ R·sy), per row d = row/sy - dy, half = sqrt(R² - d²)·sx/sy, span ftol((dx ∓ half)·sx + 0.5) (disasm
  0x70f04c..0x70f188); rectangle cols/rows from the corners; quadrilateral scanline (FUN_00719e60): edges going down
  on screen give the left bound, edges going up the right bound, so only quads counter-clockwise in the world fill.
- Draw (FUN_0070e500 param&1): tiles, icons (FUN_00716010), unit dots, player, then the mask texture at (W-0xd3, 10)
  src (0,0)-(199,199) colour (0,0,0,0xff) = black alpha 0x80 outside the vision, then the RTS frustum, FRAME, waypoints
  (FUN_00718540, not fogged) and territory alerts. FUN_0070fbe0 draws a unit's dot / hero icon only if its mask pixel
  is set (0x70fdf6): enemies (and citizens / horses outside friendly vision) disappear from the minimap.
- Port: the bridge collects the areas in world units (`HudModel::minimap_vision`), the draw rasterises the same mask
  in view pixels and fills the fogged runs (black 0x80); the dot test is the world-space equivalent
  (ponytail: no pixel rounding at circle edges). Waypoints moved to `HudModel::minimap_waypoints` (drawn after FRAME).
- Measured: s04_06_rts_mode (zoom 1) circle radius 48.5 px = 2000 units at 400 px / 16384; s04_06_minimap_zoom
  (zoom 0) ~24 px; the lit area is the union of Joan's and Jean de Metz's circles (centres 6 px apart); fogged texels
  half as bright. Every later in-game shot (s04_05..s04_08, quick-load done, both videos) has the same mask.
- Initial state: init 0x70e1d0 sets +0x474 = 0, allocates the mask uncleared and creates the texture. The build
  (HUD tick FUN_006fd8b0 from Game_process_tick, UI mode 9, every tick while the controlled unit lives) needs only
  +0xe minimap on, +0x13 player, a world, and the game clock DAT_00a500ba > +0x474; the draw has the same gates
  minus the clock. So there is no unrevealed start state: the first tick builds the mask. s04_04_mission1_hud (5 s after
  the loading screen) shows no mask, enemy dots everywhere, and a different camp icon than every later shot (the
  S1L1_tactic init picks fcampS1L1 / ecampS1L1). Jean's opening dialogue (tactic event 1, run on the first script
  passes) only starts ~5 s later (s04_04_mission1_tutorial_msg). So the shot predates the first world/HUD tick
  (INFERENCE from those three facts; why the original idled after loading is unknown). Our session ticks from frame 0,
  so `engine.gl_/vk_session_hud` reproduce the unmasked minimap with the original console cheat (below); the `hud` UI
  sample keeps `minimap_fog = false`.
- Script areas: builtins 393-396 (FUN_0065f740.. -> FUN_007196e0 / 00719770 / 007197f0 / 007198d0) write
  +0x2f7 + n, so area n = 1..10 is slot n-1 (unchecked; the port had 0..9). Console cheats (Script_cheat_exec):
  `enable_fog_of_war [n]` (FUN_00456990, n defaults to 1): n == 0 -> area 10 = the world rect (DAT_00a45f3e+0x48,
  everything visible), else area 10 off; `toggle_fog_of_war` (FUN_00456a40) flips DAT_00a4f3ae (0 at start) the same
  way. Port: autoplay `cheat enable_fog_of_war <n>` (session_autoplay.cpp; toggle not ported).
- Gaps: FUN_007190b0 (RTS mode: hostile unit
  models hidden outside the mask) is a 3D-world effect, not done.

#### Objective panel — FUN_00704810, HUD object 0xa4d618. HIGH
Code `hud_mapobj.cpp` (`hud_mapobj_event`, `hud_objective_show/toggle/alpha`, `hud_objectives`).
- Fields: +0x14d shown, +0x14e pinned, +0x14f state (0 hidden, 1 fade in, 2 fade out, 3 shown), +0x153 time,
  +0x1f7 count, +0x299[i] end-condition recno (list order), +0x157[i] prefix index, +0x1a7[i] highlight, +0x1f9[i]
  time. Times are real ms DAT_00a500ba; the port advances ms counters with ui.dt(). In a cut scene (Game+0x744)
  FUN_006fd8b0 adds the frame time to +0x153, freezing the panel clock (`objective_paused`).
- Alpha (disasm 0x70491d..0x704a8d): state 1 = min(255, Δ/2000·255) → 3 at 255; state 3 = 255, and unless pinned
  after Δ > 11999 → state 2 with t = now; state 2 = max(0, 255 - Δ/3000·255), at 0 hidden. The state only advances
  inside the row loop (no rows → frozen).
- List: Game_add_end_condition → FUN_007078c0 append; set_end_condition_enabled 0 / del → FUN_007072c0 remove;
  enabled 1 or finish NORMAL → FUN_00707400 insert first (no-op if present) with "New!"; des update → FUN_007076c0
  "New!"; finish WIN → FUN_00707960 "COMPLETED!", LOSE → FUN_00707b20 "FAILED!" (both also t = now). "New!" is skipped
  in scenario 6 stage 1. All of them then show the panel like FUN_00707ce0 (script show_objective): state 0 → 1 with
  t = now; state 2 → 1 with t = now - (255 - Δ/3000·255)·2000/255 (resume at the same alpha).
- O key (action 0x2e, PlayerCtrl_process 0x42e603): 0 → 1 (shown); 3 → 2; 1 → 2 with t = now - (255 - Δ/2000·255)/255·3000;
  2 → 1 as above; always pinned = 1 (no auto-hide until O again).
- Rows: main (type 0) then side (type 2), enabled (+0x0f) records only, at most 10. Highlight prefix MAINGAME 1/2/3
  ("COMPLETED!" gold f2d531 / "FAILED!" red / "New!" green) + " ", length L = strlen + 1, index starts at L; while
  state 3 the shown prefix is the string from index-1 (types in from the right) and the index drops every > 100 ms;
  colour per channel 255 + (target - 255)/L·(L - index) (int division). At index 1: "New!" row forced to alpha 255,
  cleared 2000 ms after the last step; COMPLETED!/FAILED! row alpha = 255 - Δ/2000·255, at 0 the line is removed and
  the draw returns. Bars/text/LINE colour alpha = row alpha.
- Suffix after the description: para1 (+0x0b) 1 → MAINGAME 5 " (Time left: %1d %1p{minute|minutes})" with
  (DAT_00a591cc objective count time s - DAT_00a59171 count time 1 ms / 1000) / 60; 2 → MAINGAME 6
  " (%1d/%2d rescured)" with DAT_00a591d0 / DAT_00a591d4 (rescue count / limit). MAINGAME 7 "(Time expired)" is not
  referenced by FUN_00704810.
- Gaps: the objmenu Objectives page (FUN_00734a30) also appends the suffixes — `BriefObjective::text` still has none;
  the exe's minutes expression mixes int and unsigned (wraps when the time is over), the port keeps it signed.

#### Out of scope / gaps
Character-details window: own section "Character details window (CharDetails)" below. EXP / level-up / combo (MGEXP, ComboHUD), critical HUD (Crit_drawHUD) and
minimap vision / objective fade: own subsections (ExpCombo, CritHud, MinimapObj slices) where landed. The cut-scene dialog
box is in `GameUiBridge` (below).
HUD entry is now `hud_game(Ui&, HudModel&, std::vector<UiCommand>&)`: HoldResume{value = party index} from the
HOLD_*/RESUME* button (hover/press states), MinimapClick, QuickLoad, QuickLoadCancel.

### RTS overlay (RtsComplete) — RE notes
Code `src/engine/hud_rts.{hpp,cpp}` (drag box, cursor, portrait clicks), `src/engine/world_view_rings.cpp` (rings),
`src/engine/game_camera_rts.cpp` (input, pick, commands, cursor animation). Behaviour spec: GAMEPLAY_AI_TROOPS.md Part B
§5 2.3-3.8. Tests `engine.rts_mode` (headless), `engine.gl_rts_mode` / `engine.vk_rts_mode` (pixels); autoplay
`work/PlayableGame2/RtsComplete/play_rts.txt`. No original capture shows RTS (`s04_06_rts_*` were taken in mission 1,
where F2 is gated to scenario >= 5, so they show action mode): visuals are checked against the original assets only.
- Selection ring — CONFIRMED (decomp). Not a sprite: unit +0x325 is a second Actor created with the unit actor
  (model handle unit +0x217) and parented to the unit node (FUN_0060ec70(actor +0x8c)): it follows position and heading.
  Model by class Special (+0x118) from UnitArray +0x70..+0x7c = `MODEL\select`, `select2`, `select3`, `select4`:
  4/5/6/9 (cannon, fauconneau, crossbow, bombard) select2, 8/11 (siege tower, ram) select3, 10 (trebuchet) select4,
  else select (r 25). Texture Slash_01 (DXT5). Shown by Unit 0x519250(1) for the active group (Rts_init, box/click
  select, recall), never for Special 2/7 (horse, animal), hidden while dead (Unit_process 0x52c490). No per-state colour.
- Drag box — CONFIRMED: Rts_draw 0x4450f0 while +0x1235 (dragging): FUN_00633070 = four 1 px FUN_00632a70 quads from
  the press point (+0x123c/+0x1240) to the mouse, colour DAT_00a33724 = FUN_0042a260(255, 255, 255, 0xcc) (r, g, b, a
  byte order, FUN_0043f780).
- Cursor — CONFIRMED: INTERFAC.SET table RTSICON (texture rtsicon.dds, 64x64 cells, POINT/GRIP 32x32). Idle
  (+0x1254 == 0) name by hover type +0x1248: 0 POINT00, 1/2 INFATT00 / HVYATT00 (class 2-4) / ARCATT00 (class 5-6)
  by the single selected unit, 6 ASSTO_00, 7 ASSIGN01, 8 FOLLOW01, others keep the previous name. Command families
  (Rts_process 0x442ef0): prefix + 2 digits, next frame when +0x124c + 50 ms < now, after the last frame back to idle
  (POINT 0..5, HVY/INF/ARCATT 0..8, MOVTO 0..10 (move, attack-move), CMOVTO 0..10 (cannot move), ASSTO_ 0..10,
  ASSIGN 0..15, FOLLOW 0..17, CASSTO 0..10). Hotspot = mouse for POINT*, else the sprite centre. Drawn instead of the
  CURSOR.RES arrow while the RTS overlay is up; the port draws cursor and drag box in the TopLeft frame (no 4:3 clip),
  so the cursor also shows over the 16:9 minimap right of the design canvas (RtsFinish).
- Portrait clicks — HIGH: Rts_process_input step 4, portrait under the cursor (0x6ffd90, CommanderPanel 0x6fd670) ->
  Rts_select_commander 0x451530 by recno, double click = centre; the hold/resume button keeps its own click.
- Minimap — CONFIRMED: FUN_00717240 left click = camera pivot x/y to the point, distance stepped -200 until
  Rts_camera_view_valid; right click = Minimap_screen_to_world 0x717700 -> Rts_cmd_move(pos, Ctrl). Port deviation
  (RtsFinish): the jumped pivot takes the ground height + 40-unit eye clearance at once (the original keeps z and lets
  Camera_rts_edge_scroll settle it; with the kept z mid-map jumps put the eye inside hills).
- Orders in play (RtsFinish, CONFIRMED ViewCtrl_process_key 0x42e1a0 mode switch): in mode 4 the per-mode handler is
  PlayerCtrl_rts 0x440b30 instead of the keyboard/mouse controller, so `World::tick_units` skips the player controller
  (before, RTS left clicks were light attacks and the controller froze the AI-driven hero in RUN); the interact prompt
  is hidden in mode 4. Map items are picked by their model's bounding sphere (ITEM\<dir>\<Model>, heading, scale).
- RtsHero (coded; tests `engine.rts_mode` part 8/9, `engine.hero_switch`; demo `work/PlayableGame2/RtsHero/play_rts_hero.txt`).
  Hero switch = `hero_switch()` (game_camera_rts.cpp, Ctrl_switch_commander 0x432300, args from the disassembly:
  action 24 -> 0, 25..28 -> 1..4): action mode keys in GameCamera::after_tick (PlayerCtrl_process gates: mode != 4,
  not range mode +0xac, scenario > 1 or +0x79 = `--rts-unlocked`, player not climbing / H_MNT_L..H_DMNT_R unless
  dead; Shift + 25..32 = Unit_toggle_hold first); cycle candidates alive, +0x2f == 0, not task-locked, +0x114 != -1,
  inside the minimap view (FUN_00717a90, `hud_minimap_scale`) unless the player is dead; effects: range mode off,
  ride mode = mount, World::set_player (HUD panel/party and camera follow player_id; camera 0 turns to the hero),
  outside RTS new hero's task parked + team tasks dropped, old hero resumes its parked task; mode 4 = swap only.
  Rts_select_commander second press / portrait double-click (`!add && (last == idx || dbl)`) on a playable hero
  (Unit_is_playable_hero 0x42ff90) -> voice 001 + switch; Rts_deinit -> `Session::on_rts_exit` -> switch to +0x1237
  (cleared by Rts_clear_highlight: click / box select, recall). Rts_first_selected_leader / count use group 0.
  Over capacity: `rts_join_target/args` -> HudModel::rts_join_prompt (ui_confirm_popup, GameFlow paused); popup
  args from 0x44f750 (name +0x3d, cap, joining, size - max(0, cap - joining), Joan). Voices: MissionEvent Sound,
  b = 0 -> GameAudio 2D; per-command tables decoded from the string pushes (Rts_click_select, Troop_cmd_formation_move
  — a single troop moving says RTS_LTINF_005..007, not its class prefix —, Rts_cmd_attack_unit/item, Rts_cmd_assign,
  PlayerCtrl_rts, Unit_toggle_hold 006/007, dismiss RTS_SIEGE_007); random variants from a cosmetic RNG. F3/F4 in
  rts_tick. Buildings: type 10 + BARR/WEAPON/IRON/WOOD loops (Rts_process families 0xf..0x12), RMB -> MissionEvent
  Interact "recruit_screen"/"workshop_screen" (a = leader, 0 for a barracks without one). Save: native section
  "VIEW" (GameCamera::save_view: groups 0..9, active, +0x1237, +0x1238, RTS distance + camera), not hashed; loaded
  after GameCamera::reset. Gaps: the box-select voice ignores the "already playing" test (0x473140); FUN_00717910
  (non-hero commander visibility) not modelled; script quit_rts_mode skips the Rts_deinit switch.
- RE for these items (RtsFinish): voices = "RTS" + 0x453da0 prefix (_LTINF_/_HVINF_/_POLE_/
  _MACE_/_ARCHER_/_CROSS_/_SIEGE_ by class for rank <= 3, _JOAN_/_METZ_/_LAHIRE_/_CHARLES_/_ALENCON_/_ORLEANS_/
  _RICHEMONT_ for rank 4-5) + 3 digits, played by 0x492c40 (SOUND entry, 2D). Exactly one commander selected -> his
  fixed index, else "RTS_LTINF_" + Misc_random_range; a single selected unit overrides: select 008 / troops 001
  (siege RTS_LTINF_001), random 2..4; move 003 / random 5..7 (siege: M slots full RTS_SIEGE_003 else RTS_SIEGE_005 +
  CMOVTO); cannot move 015 / random 11..13 (unit prefix kept; siege RTS_SIEGE_004); attack unit 003 / random 8..10
  (siege: F slots full -> generic, else RTS_SIEGE_005); attack item: commander 005, siege RTS_SIEGE_006 / generic 5..7,
  several units 8..9; join: target prefix + 005 (troop leader) / 012 (general); follow: per follower 013 (014 when
  following CHARLIE7) or generic 5..7; siege crew RTS_SIEGE_001/002; cannot assign 11..13; commander key 001 (second
  press) / 002; recall group random 2..4; store group RTS_NARRATOR_001; hold 006 / resume 007 (rank > 3). F3/F4 in RTS
  (0x453790): Ctrl_use_hp_item / Ctrl_use_ep_item on every live selected unit with rank > 3. Second commander press
  (and Rts_deinit on +0x1237) = ViewCtrl_switch_character 0x432300 for JOAN/JEAN/LAHIRE/ALENCON (mode 4: player swap
  only). The ViewCtrl is saved raw (0x4857c0: 0x126e bytes + camera 0x28; load 0x485930), so groups 1..9 ARE saved.
  Building hover (Rts_pick_building 0x452610): facility kind 3 / 0x10 whose AABB holds the terrain hit, owner nation =
  first selected leader's (or the player's), barracks +0x97 == 1 and within 512, workshop within 512 (IRON/WOOD only for
  a selected ALENCON within 1024); cursors BARR01..18 / WEAPON01..09 / IRON01..09 / WOOD01..17 loop while hovering.
  Over-capacity: popup t_interf CHARMOVE|1 (name, cap, n_new, removed = size - max(0, cap - n_new), is Joan), game
  paused, +0x1269; OK -> Rts_cmd_assign(target, 1).

### Character details window (CharDetails) — RE notes
Code `src/engine/hud_character.{hpp,cpp}` (class CharacterWindow, owned by the Client, drawn before `ui_hud`); flow hooks in
`session_flow.cpp` (actions 35/36, Esc, `paused()`), `HudModel::show_character`; game layer `World::use_item` /
`World::unequip` / `World::transfer_item`, `ItemDef::description/detail_file`, `UpItem::desc`. Tests `engine.character_details` (headless clicks),
`engine.gl_/vk_character_details` (`character_details_shots.hpp`); autoplay `work/PlayableGame2/CharDetails/play_chardetails.txt`.
- Open/close: FUN_006f7d80 (open: unit +0x6af, game speed 0 = sim paused, HIGH: Game_process_tick skips the sim when
  speed == 0), FUN_006f7e90 (close, speed restored). Input FUN_006f7780: Esc closes; FUN_006f5a70 maps actions 34..41 to
  interfaces 1..7 (35 and 36 both = this window): the same key toggles it, another menu key switches. Mirrored into
  `MissionUi::inventory_open` (script close_inventory_interface). CONFIRMED.
- Left panel FUN_006f6280 (IUNITATT): bg (8,8); row table DAT_00a277d8 (6 x 16 B, read from joan.exe): highlight at
  (8+8, y+8) for y = 233/257/292/318/342/366, values right-aligned at x 262, y+8; ADD (COMMELE add_*) at (266, y+8)
  on the hovered row while attr points > 0 → Unit_spend_stat_point 0x50d640 types 6,7,1,3,2,5 (= World::allocate_point).
  EXPBAR (0x4a,0xa8) clipped to 211·(xp−LEVEL[l−1])/(LEVEL[l]−LEVEL[l−1]) (none at level 30, NEXT LEVEL = UNITATT 21 MAX);
  coins1..4 (0x18,0xcc) by gold <2000/4000/6000; name block (0x24,0x1f)-(0x108,0x66) ≤2 lines centred vertically; level
  centred in (0x1a,0x7b)-(0x44,0xad); EXP/NEXT (0x108,0x7a/0x8f), gold (0x108,0xd2), kill boxes centred in
  (0xd8,0x1a2/0x1be/0x1da/0x1f5)-(0x11b,+0x15), available exp/combo (0x108,0x21c/0x230); CB/WP page button (0xf6,0x13),
  CB_*G blink every 500 ms while combo points > 0 (heroes only). Label tooltips (UNITATT pairs 1..20) after 250 ms rest.
  Fonts chosen by measurement (object addresses are ECX of thunks): values 18pt attribute numbers out/over (hovered
  row), gold/level 24pt large numbers light, name Goudy_Bold_Gold_24. CONFIRMED layout, MEDIUM fonts.
- Inventory page FUN_006d6740 / input FUN_006d8ac0: PANEL (0x144,8), INVENTBG (0x154,0x20); icons ITEM\<code>(.dds,
  "#M" while hovered) at cell−8: melee0 (0x157,0x34), melee1 (0x1a7,0x34), ranged (0x157,0x122), ammo (0x1cc,0x128)
  + count (0x1d4,0x130); grids personal 6x8 (0x21a,0x3e), troop 6x4 (0x21a,0x1a6), enhancers 4x1 (0x15f,0x220), 40 px
  cells; counts / "Lv%1d" (INVENT 1) at the cell. Drop areas weapon (0x15f,0x3c)-(0x1ff,0xdc) types 8/9/10, ranged
  (0x15f,0x12a)-(0x1af,0x1cc) 0xb/0xc, ammo (0x1d4,0x130)-(0x1fc,0x1d0) 0xd..0x12 with bow/crossbow compatibility;
  UsableUnit +0x255 checked. Right click: red 0x15 / blue 0x17 / all 0x19 potion → 0x50b4f0/0x50ba00/0x50bcd0 (force 1,
  team overflow) = World::use_item; ARMOR → equip. Item tooltip: name + Description with %Nd values (0x5a4c30) +
  INVENT 12..15 hint. Saint-Catherine-de-Fierbois socket swords use JOAN_SWORD_LV10_<n> icons. CONFIRMED.
- Party portraits / commander hand-over (ItemTransfer; manual p. 35 "SHARING ITEMS AMONG YOUR COMMANDERS": left-click
  the item, then the commander's portrait). CONFIRMED (decomp + asm):
  - Hit test FUN_006ffd90 on the main game interface 0xa4d618 (party list +0x125, count +0x145): the circle of radius
    36 (sqrt ≤ 36) around (DAT_009de66c[i + 8 − n] − 800 + W, H − 0x3a); table (read from joan.exe) 115, 207, 299,
    391, 483, 575, 667, 759 = frame x (DAT_009de64c) + 46.
  - FUN_006f7780, before the page input: a left press (Mouse_single_click_in_rect, event 0, click count 1) inside the
    party panel rect DAT_00a48868..74 on a portrait whose unit has rank +0x104 > 3 sets the shown unit +0x6af, restarts
    the CSELECT blink (DAT_00a4d75f) and returns; a non-playable hero forces the inventory page (FUN_006f7f20(1)).
  - Pointer item state (+8): 1 pressed on an item, 2 after a < 200 ms click (FUN_00594d30), 3 = held ≥ 500 ms (drag),
    4 = the next click. On a left release (event 4, FUN_00594a10) in state 3/4, 0x6db54a: portrait under the release
    point → target must have rank ≥ 4 (asm `cmp [ecx+0x104],4; jl`), not Unit_is_task_locked, +0x2f == 0 and pass
    FUN_00717a90 on the minimap 0xa4f381 (its position within half the minimap view of the player); then
    Unit_add_item 0x495f40(item, count, level, target, extra.., 0) on target +0x1c1. Success: FUN_006de4d0 item sound
    (IE_SWORD types 8..10, IE_BOW 0xb/0xc, IE_ARROW 0xd..0x12), pointer cleared. Failure: state 4 keeps the item on the
    pointer, state 3 puts it back (switch on the source +0x11). The window's own unit must pass the same three tests
    for any inventory input (0x6d8c21). So a click on another portrait shows that hero (press) and hands him the item
    (release); a drag onto a portrait hands it over while the window stays.
  - Port: `CharacterWindow::party` (after the page; the press switches `unit`, the release calls World::transfer_item
    with `hud_minimap_scale`); the port keeps the pointer item in its source (Held::owner) until it lands, so a
    refused item held from another hero can only go to a portrait or back. Placement rules: GAMEPLAY_IMPLEMENTATION.md
    "Commander hand-over". The HOLD / RESUME button area keeps its own clicks (as ui_rts). PadWindows: the portraits
    join the focus ring; South on one = press + release.
- Drop on the ground (manual "DISPOSING ITEMS FROM YOUR INVENTORY": click the item, click outside the inventory).
  CONFIRMED (decomp FUN_006d8ac0 after the portrait test): state 3/4, left release, no portrait unit under the
  pointer, pointer outside (0, 0)-(800, 600) (FUN_00594900, screen pixels: the minimap / HUD side) and ItemRes +0x25c
  CanDrop (ItemRes_parse default 1; 41 records say 0: hero armours, letters, keys, Orleans' / Charles' / Arthur's
  weapons via the case-insensitive key) → FUN_00512830 on the window's unit: 10 tries at Misc_random(360) degrees,
  class radius + item radius + 15 from the unit, onto a floor polygon (Phys_track_poly, height + 0.2) with a floor
  marker; no floor = the item is destroyed. Failure keeps a clicked item, puts a dragged one back. Port:
  `CharacterWindow::party` → World::discard_item → World::drop_item (item radius = 15 as the pickup's, so it lands
  15 beyond the pickup reach); PadWindows: North (Y / Triangle) drops the held item.
- Combo page FUN_006ef9e0 / FUN_006f0a90 / list FUN_006f0fe0 (IATTCOMB): learned skill upgrades (types 9..16,
  unlocked == applied) in 10 LIST rows (0x15d, 0x69+0x1c i), DESC at (0x160, 0x6e+0x1c i) + " (key)" in red for types
  0xb..0xe, level centred (0x269..0x2e4); ADD (0x2b7, 0x6c+0x1c row) → spend type 8; New Attacks (unlocked, REQUIRE
  learned) 3 ATT rows (0x15c, 0x1d4+0x28 i), cost ATTCOMB 1 right at 0x2c2, LEARN (0x2d6, 0x1d7+0x28 i) →
  Unit_apply_upgrade 0x4949f0; SBUP/SBDW (0x1c0/0x288, 0x1a4); frame BG (0x158,0x20) drawn last. HIGH.
- Shop (i_buy.cpp: init FUN_006c2120, open copy FUN_006c3230, draw FUN_006c4070, buy FUN_006c5a00, commit
  FUN_006c3540): BUY main 800x600 centred; stock = the shop facility's stock (f_shop +0x79, see GAMEPLAY_SYSTEMS.md
  Shops; open copies the entries with quantity ≠ 0, close writes the quantities back and deletes sold-out entries) in
  a 6x13 grid (+0x100,+0x38); player bag 6x8 (+0x210,+0x38); coins (+0x10,+0x1d0),
  gold (+0xc9,+0x1d4); BUY 1/2 titles; tooltips BUY 3/4/5 (stock), BUY 6/10 (own items); BUY 8 / BUY 9 message box;
  every close (Esc / Enter / c_* button (+0x67,+0x218)) commits, so purchases go straight to MissionState::gold +
  World::buy_item + Facility::stock (`game::shop_buy`). Buy (CONFIRMED decomp): price (0x6c5ac6) = trunc(Price ×
  factor × (level − MinLevel + 1)); gold < price → BUY 8; stock −1 (unless unlimited); a free personal cell
  for the footprint (FUN_006c9d20, column by column) or MaxQuantity room in same-item stacks of the copied personal
  bag (FUN_006c9f10), else stock +1 and BUY 9; FUN_00457ba0 tops up stacks from the last one and puts the rest in
  the cell (no equipping, no troop grid); a ShowArmoryInterface item joins hint list +0x728. Selling (FUN_006c6550):
  a single click on a bag stack picks it up (+0x245); dropped in the seller's grid (0x6c7319) the whole stack sells for
  trunc(Price × SellingRatio × quantity / Quantity × level factor) if CanSell (+0x25d) and food types 0x15/0x17/0x19 ↔
  POTION shop, others ↔ WEAPON shop, else "Cannot sell" (BUY 10) and it stays held; a click in the bag puts it down.
  A double click (Mouse_double_click_in_rect: the second press of a Windows double click, which no single-click test
  sees) in the bag with nothing held (+0x245 == 0) sells the stack under the pointer at once (0x6c7860) for
  trunc(Price × SellingRatio × quantity / quantity × level factor), one Quantity's worth whatever the stack holds.
  So double-clicking a resting stack only picks it up (its first press did); putting a stack down and clicking it again
  at once sells it at that value. Engine: same clicks (0.5 s / 4 px). Sim paused while open (windowed runs).
- ARMORY (armory_interface, layout FUN_006b9870; trigger at the end of Unit_auto_pickup 0x49da30: a player pickup of a
  ShowArmoryInterface (+0x1e8) item not yet in the hint list 0x472a90 → add 0x4729c0 → open 0x6bbc90 / 0x6ba4b0):
  `CharacterWindow::on_world_events` on Ev::Effect "PICKUP", hint list MissionState::item_hints_b (Game+0x728),
  HudModel::armory_item
  (pauses). bg1 armory_b (a black translucent veil over the world), title + arrows, ARMORY 2 + item name, frame_bg /
  frame (0x1c,0xf8), fancybar DESCRIPTION, item_des/<DetailDescriptionFilename> text, longbar + BACK TO MISSION
  (ARMORY 3; Esc / Enter too). HIGH layout, fonts by font-object table where known (0xa48d48 Goudy_bold_18).
- Cosmetics checked: the name's darkest text pixels equal the reference ((51,31,11) both, Goudy_Bold_Gold_24);
  HELPBOX 1-texel strips sampled at texel centres (en2d point-samples them); unit name bars over the panels are
  dropped while the window is open (the original's bars are world billboards, i.e. under the 2D window).
- SSIM vs s04_06_character_details (panels above the attention message, black behind): GL 0.952, VK 0.952 (min 0.93).
- Grid positions: each bag item keeps its cell (ItemInstance bag/col/row = the original item +0x15 col / +0x16 row in
  bag 0 enhancers / 1 personal / 2 troop; hud grids 0 personal, 1 enhancers, 2 troop 6x4 at (0x21a,0x1a6), 3 shop
  stock); unplaced items go first-fit (enhancer grid, personal, troop) and keep the cell. Saved in the optional native
  save section INVG (outside WRLD, so World::hash and old saves are unaffected; profile heroes as id −1 − index, so
  the hero carry-over in MISN keeps them), carried through Player.dat export/import (campaign.cpp place() honours the
  stored cell) and imported from original .SAV unit records (savegame_import.cpp, records after the 0x432 dump).
- Gaps: troop inventory items are not used by the troop AI;
  shop buys / sells by click instead of drag, buy price = Price (the exe's FPU expression is unrecovered), hourly
  restock (FUN_00469710) not modelled, stock kept for the window's lifetime only; the armory's rotating 3D item and its
  slide-in animation are not drawn; letter viewing (type 0x23) and cross socketing (0x6de590) are not wired; the
  shop's 3D seller / player portraits (View3D) are not drawn.

### Recruit and siege workshop screens (RecruitWorkshop) — RE notes
Code `src/engine/hud_recruit.{hpp,cpp}` (FacilityWindow, shared chrome + barracks), `hud_workshop.cpp`; game side
`src/game/facility_build.cpp` (Places::recruit / build_siege / set_workshop_list, tick_recruit / tick_workshop).
Tests `engine.recruit_workshop` (headless: mouse, pad, commit, spawn, grid save/load), `engine.gl_/vk_recruit_workshop`
(images, no reference exists). Autoplay `work/PlayableGame2/RecruitWorkshop/play_recruit.txt` (mission 7).
- Open: MissionEvent::Interact "recruit_screen" (0x43bfe0: DAT_00a48959 unit, DAT_00a48955 facility, 0x6c0310(1)) /
  "workshop_screen" (0x43c080, 0x6ed3e0(1)) -> HudModel::recruit_screen / workshop_screen; game speed 0 while open
  (GameFlow::paused). Close 0x6c0310(0): rows slide out, state 3 restores the speed.
- Layout (1024x768, BARRACK set; ICON_1/ICON_2 and class icons FLINF..FCROSS from MESSAGE, arrows COMMELE2): bg (0,0);
  title (0x138,7)-(0x2c4,0x23) TITLE + BARRACK 15 / WORKSHOP 16 centred (Goudy14(22) white 0xa51608) with L_/R_ARROW;
  rows (0x24, 0x68+100 i)-(+0x200, +100): I_OT / I_OV, graybar (+0x5c,+0x28), labels (+0x60/200/0x130/0x198, +0x30)
  Goudy12 Bold Black (0xa5d3c8) BARRACK 7..10 / WORKSHOP 8..11, "%s (%d %s)" name / cost / BARRACK 3 at (+0x60,+0x10),
  bars at y +0x40 step 0x12 (barracks: Strength att_r, Defense att_y at +200, Dexterity att_b at +0x130, one per 2
  points; workshop: Dexterity / 5 (melee) or / 20 (ranged) att_r, Defense / 5 att_y, WalkSpeed att_b, max 5 each),
  MELEE / RANGE (SRangeWeaponId +0x177 < 1) black at (+0x198,+0x40); hover test (x1, y1, x2 + 0x4c, y2) slides ia_ov
  (0x1d8 -> 0x224) with aw_r (+0x14) / aw_l (+0x34) out (300 px/s). Rows slide in from y -100 (3500 px/s, -1500 px/s²,
  30 ms stagger, last row first), the info FRAME (360x580) from x 0x400 to (0x27b,0x5b). Buttons B_OT/B_OV/B_DW
  (0x28b,0x2be) RECRUIT|BUILD + CHECK and (0x32e,0x2be) CANCEL + CROSS (label at +0x28,+10); workshop HELP H_*
  (0x26,0x2be) WORKSHOP 18.
- Barracks info frame (FUN_006c1180): titlebars at +0xf/+0x7f/+0x103/+0x11f/+0x1a6/+0x1e2 (labels +0x17,+2:
  LOCATION, ABLE CITIZENS, player name, TOTAL UNITS (MAX: troop max), RECRUITMENT COST, GOLD), goldbars
  +0x73/+0xf7/+0x19a/+0x1d6; RECRUIT.RES Location (+0x2b) in (+0x16,+0x2a)-(+0x152,+0x5c); POPULATION %d (owner pop)
  (+0x16,+0x5e); CITIZEN icons (+0x16+0x1e i, +0x96/+0xbe); troop c_* then new n_* from (+0x16,+0x13b), one wrap to
  +0x163; cost / gold "%d GOLD" (+0x16,+0x1c2) / (+0x16,+0x1fe), gold icons (+0x100,+0x19c/+0x1d8).
- Barracks data (BarrUI_open / count_troop / handle_input / confirm): rows = recruit list 1 entries of the facility;
  gold copy; able citizens clamp(pop − 0.95 pop_max, 0, 20); troop = the player's team members + spawn-barrack groups
  of his troop id + this facility's queue (stops at the first other troop) by unit_class (1 linf, 2 hinf, 3 pole,
  4 mace, 5 arch, 6 cross). aw_r: refused at the player's max troop size, without citizens, or cost + class Gold >
  gold; aw_l undoes. Enter / RECRUIT: RecruitFacility_queue_order per class (player troop id, 9999 without player),
  gold −= cost, owner pop −= recruited; Esc / CANCEL: nothing. RecruitFacility_process 0x468600 then feeds the spawn
  barrack one unit per free tick (add member group type/team/1/recruit level + barrack order IDLE prio 5 at the
  facility point, 5000 ms); the barrack spawns it into the player's troop (join the leader of that troop id).
- Workshop data (FUN_006ea020 / FUN_006ec480 / FUN_006ea1a0, facility f_work.cpp): products from RECRUIT.RES
  (Workshop_set_recruit_list 0x46b730: ≤ 4, cost = class Gold, crew = int field, build ms = float × 1000); capacity
  +0x7d = 12; money = gold (NORMAL) or the facility's WORKSHOP_RESOURCE (IRON / WOOD, cost = class Resource +0x165).
  aw_r refused when built + new ≥ capacity or money short. BUILD / Enter: 0x46ab40 per weapon (needs the player, the
  crew count of builders and < 20 queued; team 9999), money −= cost even when a queue call fails (as the original).
  Process 0x46a040: oldest queued starts (RTS_SIEGE_008), after its ms spawns at the first free spot 200 units around
  the rect (0x46b150, facing out), built +1, SMS WORKSHOP 17 + RTS_SIEGE_009. Info frame (FUN_006ee790): LOCATION,
  TOTAL SIEGE WEAPONS grid (built N_*, new R_*, empty ICON frames to the capacity, 4 per line at +0x1e + 0x4b k,
  +0x9d/+0xed/+0x13d), COST (+0x16,+0x1b4), GOLD / RESOURCE (+0x16,+0x204), gold/iron/wood icons (+0x100,+0x184/+0x1d4).
- Pad: nav_modal 4100 / 4200; rows and buttons are the focus ring, left/right on a row = aw_l/aw_r (nav_adjust),
  South clicks, East = Esc.
- Help: G_POPUP text box (FUN_006d3cb0 flags 4, width 0x262, title WORKSHOP 19 goudy_bold_14, body Goudy12 Bold
  Black, ≤ 0x3ff chars, CLOSE; `ui_text_popup`, shared with the brief's siege help). The box grows to fit the text;
  the original has no scrolling there.
- Build progress bar (FUN_0046ba20 / 0x46c4d0 / 0x46c540): LifeBarre frame + bar_workshop fill at facility + 200 z
  while the current weapon builds, fill = clamp(elapsed / build ms); drawn with the name bars (HudUnit::workshop_bar).
  RTS_SIEGE_008 is skipped while an RTS_SIEGE_009 voice plays (0x473140; GameAudio). Default order for new weapons:
  0x46a040 hands workshop +0x81 (OrderRec) to OrderQueue_add, but nothing writes +0x81 (ctor FUN_00469e90 zeroes it,
  no builtin / loader / UI sets it), so new weapons never get one (absent in practice).
- Row icons: the class codes are looked up case-insensitively (0x5270b0; unit.res spells "fauconneau" /
  "Batterram"), which is why L_FC / L_BR now draw.
- Gaps: free-spot test = walkable terrain + cylinder overlap (0x6932d0 uses path space + unit shapes); builders
  (+0xf0) never register (crew 0 in all shipped lists); fonts of the row/info text by font-object address
  (0xa51608 / 0xa5d3c8), alignment of the rect texts HYPOTHESIS (centred). Mission 7's NORMAL workshop1 belongs to
  the English until the S7L1 script captures it (cut scene in "workshop cutscene", then no Mercen left in "workshop1
  place" -> French); its IRON / WOOD workshops open only for Alencon as the controlled unit (0x43c080).

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
- MISSION_BRIEF_<1..8> (SE, the narrated background) plays when 0x7246f0 opens the Background page with the
  .OBJ loaded (port: `background()` at brief_time 0). MAP_BUTTON = the minimap toggle key (0x70eb20, action 45;
  played by `Client`).
- Gaps: commander / siege 3D actors, battleground camera controls, 2D icon rotation, user key names in
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
