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
