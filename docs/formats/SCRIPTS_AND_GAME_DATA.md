# Scripts and gameplay data formats

Owner: ScriptsGameplay. Parsers live in `src/formats/{script,scenario,text}_*`; the RES/SET
container and dBase reader (`formats/res_archive.hpp`) and the `Key : value` /
`Type { key:value; }` text readers (`formats/world_text.hpp`) are WorldFormats' and are reused
here, not duplicated. Tool: `joa-script` (`tools/joa-script/`).

Confidence labels: **CONFIRMED** (byte-exact or code-exact evidence), **HIGH CONFIDENCE**,
**HYPOTHESIS**, **UNKNOWN**. Addresses are joan.exe VAs (ImageBase 0x400000; file offset ==
RVA for .text/.rdata/.data). Decompiler output (`work/ghidra/export/joan_decomp.c`) is not source.

## Summary

| Path | Count | Nature | Parser |
|---|---|---|---|
| `script/scenario/*.sce` | 35 | plain-text C-like script, interpreted (no bytecode) — **CONFIRMED** | `parse_script` |
| `script/scenario/*EX.res` | 28 | `[0001N]` string tables (`~`-separated) | `parse_tagged_strings` |
| `script/scenario/*.obj` | 8 | briefing text (Latin-1) | `split_tilde_blocks` |
| `script/scenario/*_map.bm` | 7 | briefing 3D-map scene (`CreateLight`/`CreateMapActor` blocks) | `parse_block_text` (WorldFormats) |
| `script/unit/unitAI.res` | 18 records | Name34 RES; each body is a `.sce`-language script | `ResArchive` + `parse_script` |
| `script/pscript/*.txt` | 266 | particle-system definitions (plain text) | `parse_particle_system` |
| `scenario/scenario_0N.scr` | 8 | `Key : value` campaign step table | `parse_scenario_def` |
| `Resource/GAMEPLAY.RES` | 6 records | Name9 RES; bodies are `{SECTION}[KEY]value` tuning tables | `parse_game_vars` |
| `Resource/STD.SET` | 140 tables | Name9 RES of dBase III tables (game database) | `DbfTable` (WorldFormats) |
| `unit/unit.res`, `unit/recruit.res` | 117 / 38 | Name34 RES; bodies `VERSION 1.0 … END` `Key : value[: value]` | `parse_kv_text` (WorldFormats) |
| `item/item.res`, `item/dropitem.res`, `item/sellitem.res` | 250 / 37 / 47 | same | `parse_kv_text` |
| `item/Object/**/*.pro` | 819 | single item records, same format (placed world objects) | `parse_kv_text` |
| `des/*.txt`, `item_des/*.txt` | 25 / 81 | free text (Latin-1); `des/strategic_N.txt` uses `/T` title, `/N` note lines and `~` page breaks | `split_tilde_blocks` |
| `Resource/help.res` | 1 | `Code\nTITLE\nbody\n~` blocks | `split_tilde_blocks` |

Nothing in `script/` is encrypted or compressed — **CONFIRMED** (`file`, byte dumps, all files
parse as text). Text is Windows-1252/ISO-8859-1 (`Orléans` = byte 0xE9); `latin1_to_utf8` converts.

## Scenario script language (`.sce`, unitAI bodies)

The interpreter tokenises and compiles the text at load into a tree of statement objects
(string `"Unhandled exception in ScriptStatementCompile::compile."` at VA 0x9db79c) and walks
that tree; there is no on-disk bytecode — **CONFIRMED**.

### Tokens — CONFIRMED
Token table at VA 0xa214c8 (44 entries `{char* text; int type}`, `qsort`ed then `bsearch`ed with
`_stricmp` FUN_007c07b0, so keywords are **case-insensitive**). Classifier FUN_00662330:

| type | token |
|---|---|
| 1 / 2 / 3 | int literal (starts with digit or `-`) / float (contains `.`) / `"string"` (no escapes) |
| 4..9 | `{ } = , : \n` |
| 10..32 | `EVENTSYSTEM ID INIT RESET WAIT CHOICE IF ELSE SPEAKER UNIT OBJECT GOTO EXIT FUNC DIALOG CAMERA ACTION XYZ SCRIPT INT FLOAT STRING VECTOR` |
| −50..−36 | `+ - * / ^ < > <= >= == != && || ( )` |
| 0x21 / 0x22 | identifier `[A-Za-z_][A-Za-z0-9_]*` / unrecognised |

Because a token starting with `-` is a number, binary minus must be space-separated; every
shipped use is (`( 180 - FUNC(...) )`). `;` is not a token: the stray `QG_one_recno = 0;` in
`S2L1_tactic.sce` lexes as the number `0` in the original; our lexer skips `;`.

### Grammar (implemented in `script_ast.hpp`)
```
script := STRING '{' stmt* '}'                 -- name, e.g. "S1L1.sce"
stmt   := (INT|FLOAT|STRING|VECTOR) ident      -- declaration, no initialiser
        | INIT block | WAIT block | EXIT
        | IF '(' expr ')' block [ELSE (block | if-stmt)]
        | ident '=' expr | expr
expr   := precedence climbing; primary := number | string | ident | '(' expr ')'
        | XYZ '(' e ',' e ',' e ')' | (FUNC|DIALOG|ACTION|CAMERA|SCRIPT) '(' args ')'
```
- Operator precedence — **CONFIRMED** (FUN_006484b0): `||` 1, `&&` 2, relational/equality 3,
  `+ -` 4, `* /` 5, `^` 6; shunting-yard in FUN_00647c90 pops while top ≥ current → left
  associative. Math sub-expressions start with `(` ("Expecting (!", FUN_00647c90); `&&`/`||`
  produce 0/1 (FUN_006453a0).
- Value types at node+0xc: 1 int, 2 float, 3 string, 0x1b vector (FUN_006439d0) — **CONFIRMED**.
- Shipped corpus uses only `INT` declarations (3991), `if`/`else` (3005/536, 134 `else if`),
  `WAIT` (1452), `init` (35), `exit` (53), `SCRIPT` (16), `FUNC` 16640, `ACTION` 1960,
  `DIALOG` 1016 calls. `GOTO CHOICE SPEAKER RESET ID UNIT OBJECT EVENTSYSTEM CAMERA FLOAT STRING
  VECTOR XYZ` never occur; the parser rejects the statement keywords among them — **CONFIRMED**
  (`script_ast_test`).
- Run-time semantics (scheduling, `init`, `exit`, WAIT polling, SCRIPT sub-scripts, variable
  scopes, arithmetic, RNG) — **CONFIRMED**, see
  [../reverse-engineering/SCRIPT_VM.md](../reverse-engineering/SCRIPT_VM.md): the step script
  root is executed once per 30 Hz logic tick and re-runs from the top every tick unless
  suspended in a `WAIT`; `init` runs once; `exit` ends the current pass; `_des` scripts run
  once at load.
- `[NNNNN]` prefixes on string literals are localisation keys: joan.exe appends `EX.res` to the
  script name (string at VA 0x9da868, FUN_00638aa0) and looks the tag up there. All 1506 tagged
  literals in scripts that have an EX table resolve — **CONFIRMED** (`text_strings_test`).

### Function table — CONFIRMED
Table at VA 0xa1a250: 407 × `{u32 id; char name[52]; u32 return_type}` (stride 60). Lookup
FUN_00661e10 loops `i < 0x197`, `_stricmp`s the name, returns `id` and writes `return_type`
(1 int, 2 float, 3 string). The FUNC executor FUN_00649920 is `switch(id-1)` with a 407-entry
jump table at VA 0x64eca3; argument *N* is fetched with `push N; call 0x401c08` and read from
node+0x10, so **arity = highest N read** in each case (`fld` ⇒ float argument). 11 ids go to the
switch default and are implemented by separate executors: DIALOG 316–322
(`add_dialog del_dialog del_all_dialog add_choice_dialog get_choice dialog_text set_dialog_voice`,
VA 0x65fd70/0x660e60) and ACTION 323/324/326/327 (`give_order stop_all_order give_task
give_team_task`, VA 0x661060; `give_order` resolves its order-name string at compile time,
FUN_00643d40). `stop_curr_task` (325) is a FUNC case but scripts also call it via `ACTION`.

`src/formats/script_funcs.cpp` is generated by `tools/joa-script/gen_funcs.py <game_dir>`
(objdump + the logic above) and regenerates byte-identically. Per-argument signature letters:
`i` int, `s` string, `f` float — `f` from `fld` in the executor or float literals in scripts;
`i`/`s` from shipped literal arguments (the executor loads ints and string pointers
identically) — **HIGH CONFIDENCE**; `?` = no evidence. `format_string` is variadic.

Coverage: all 407 named; 270 used by shipped scripts; every one of the 19,616 calls resolves and
passes ≤ arity arguments (`script_ast_test`). Some scripts pass fewer arguments than the arity
(optional trailing args, e.g. `is_cut_scene_finished`), and a few pass a string literal where an
integer recno is read (`"DF_one_recno"` to `stop_curr_task`) — original script bugs, preserved.

Categories (by name; `joa-script funcs` prints the full table): unit spawning/state ≈120
(`add_unit load_unit search_unit_by_code set_unit_pos set_unit_hp set_unit_team
set_main_character …`), unit orders/AI ≈44 (`set_unit_attack_behaviour is_in_danger
can_rush_attack …`), items/inventory/gold ≈41, scenario state/flags/timers ≈38, nations/
relations/bribery/citizens ≈30, environment/music/minimap ≈24, end conditions ≈18, cut scenes
& camera ≈17, dialog/messages ≈15, misc (facilities/barracks, reputation, calendar, challenge
timers) ≈60.

Task/order names seen in ACTION calls: `give_task` FOLLOW_UNIT IDLE TOWER_ATTACK DEFEND_PLACE
APPROACH_PLACE ATTACK_PLACE LARGE_WEAPON_PUSH RTS_ATTACK_UNIT RTS_ATTACK_ITEM SCOUT RETREAT
SPECIAL1..5; `give_order` APPROACH_PLACE TURN MOVE_TO_TARGET SHOOT.

### unitAI.res — CONFIRMED
18 records (HUMAN_LIGHT_GENERAL, PIG, HUMAN_ARCHER, …, CART). Every body's first statement is
`exit`; the old AI logic below it is commented out. Unit AI is therefore entirely engine code.

## Particle systems (`script/pscript/*.txt`)
```
ParticleSystem " name " 1.00 { Prop = value … EventSequence " name " { Prop = value
  Initial Prop = value   Fade So At t Prop = value … } … }
value := n | random(a,b) | XYZ(s,s,s) | RGBA(s,s,s,s) | " string "
```
Keywords match the second lexer table at VA 0xa21680 (`PARTICLESYSTEM EVENTSEQUENCE RANDOM XYZ
RGBA TEXTURE MODEL FADE SO AT INITIAL FINAL` + 39 property names with type codes 23–30) —
**CONFIRMED**. Properties used: Position SourceBlending DestBlending LifeTime EmitRate EmitRadius
Gravity FollowCamera Orientation SelfRotation SelfRotAxis SelfRotSpeed Has_Rotation Texture
NumParticles Offset OriR IsSmo Model Loop TimeDelay TextureNumber TexturePeriod RChange
Rotation_Speed Rotation_Center Rotation_Axis; per-particle Initial/Fade: Velocity Size Color.
`random(a,b)` keeps operand order as written (`random(10.00,-5.00)` occurs). Semantics of
individual properties (blend enum values, units) — **UNKNOWN**, owned by the renderer port.

## Campaign files (`scenario/scenario_0N.scr`) — CONFIRMED
Loader FUN_004ccfb0 builds `SCENARIO\scenario_` + (`0` if n<10) + n + `.scr`, reads `code`,
`StepNo`, then per step six values into 65-byte fields of a 0x1b2-byte step record:
WorldFileName (`MAP\…\*.wor`), UnitFileName, ScriptFileName, DescriptionFileName,
ObjectiveFileName, ObjectiveMapFileName. All files have `StepNo : 1`. UnitFileName exists only
for scenarios 1–2 (`S1L1.uni`, `S2L1.uni`); later maps ship per world-node unit files
`<stem>_N.uni` — the field is vestigial there (**HIGH CONFIDENCE**; the loader of `.uni` is
WorldFormats' area). `scenario_02.scr` has `code : SCENARIO_03` and vice versa: codes are labels,
the file number is the campaign index.

## GAMEPLAY.RES (`{SECTION}[KEY]value`)
Records ECON, THROWI, UNIT, CHARMOVE, WEATHER, GAME; read by FUN_0051c680 / FUN_00472ee0 via
section lookup FUN_0047cbf0 + key lookup FUN_0047c180 — **CONFIRMED**. Trailing `//` comments
usually hold the original `#define` and old default. **Parsing quirk that changes gameplay —
CONFIRMED**: many keys are read with `atoi` (FUN_007b15cb → FUN_007b1540) even when the text is
fractional, so `STR_DAM 3.0→3`, `DEX_DAM 1.5→1`, `DEF_RAT 0.3→0`. Floats (`AMREXDEF`,
`IDLE*_CH`, `EASY_D`…) use `atof`. The engine must reproduce the truncation. `GAME` defines
`EASY/NORM/DIFF/VDIFF` × `_D/_A/_B`, but the exe never contains the strings `EASY_B`…: its
third block re-reads the `_A` keys into the `_B` globals — **CONFIRMED** (no `EASY_B` string in
`strings.tsv`; FUN_00472ee0).

## STD.SET tables (dBase III, Name9 RES)
140 tables, all parse — **CONFIRMED**. Index tables: `HEADER` (STD, "Standard Game Set"),
`ANIMATE` (82 unit animation codes → per-unit animation table, 21 fields: ANIM_CODE POSITION
MOVEMENT COMBO COMBOAVAIL CHAINAVAIL ITEM_USING SPECIAL ENERGY_PT DAMAGE BLEND_TIME START_TIME
END_TIME UPPERFILE UPPERNAME SCRIPT SOUND SOUND_TIME SOUND2 SOUNDTIME2 SHOW_CRIT), `UPGRADE`
(43 upgrade-list codes → `{LEVEL, CODE}` tables such as JOANUP), `UPITEM` (72 upgrades: TYPE
CODE DESC PARA1 PARA12 PARA3 COST REQUIRE), `LEVEL` (30 levels: EXPERIENCE ATT_POINT SKI_POINT),
`ITEMCLAS` (20), `ITEMTYPE` (39), `NATION` (ENGLISH FRENCH MERCEN), `RACE`, `MODEL`, `MORPH`,
`SDATE`, `SOUND` (505), `MUSIC` (42), `DEFAULTK` (55 default key bindings), `KEYDESC` (115).
Every ANIMATE/UPGRADE DBFNAME exists and every upgrade-list code exists in UPITEM —
**CONFIRMED** (`unitdb_test`). `Resource/RESOURCE` is an older set (AMAZON1, GENERAL1 … with
LOWERFILE columns) not referenced by STD data — **HYPOTHESIS**: leftover from the engine's
previous game. `Resource/INTERFAC.SET` (81 UI layout tables) belongs to the UI port.

## unit.res / item.res records
`VERSION 1.0` … `END`, one `Key : value` per line, extra `: value` fields for list entries
(`recruit.res`: `UNIT_CODE : 1 : 50`; `sellitem.res`: `item : 1 : 10 : 1`). Keys repeat in
order (`ModelName`/`AnimationName` per armour level, `EmitterName` blocks). Original quirk:
`unit.res` ENGLISH_SPY has `CFileName :` with `None` on the next line (the game reads
whitespace tokens). Cross references checked by `unitdb_test` — **CONFIRMED**: unit `Code`
== record name (117/117); `Animation` ∈ ANIMATE; `UpgradeList` ∈ UPGRADE; `DropItemList` ∈
dropitem.res except `NONE` and ALEXANDER's `S2B1_ALEX` (missing in data); drop/shop lists name
item.res codes or record names; recruit lists name unit codes; every item `ItemClass` ∈ ITEMCLAS
(case-insensitive: `.pro` files use `Gate`, `Switch`). Field meanings: see GAMEPLAY_SYSTEMS.md.

## Tool
```
joa-script funcs                       # id name return arity sig executor case_va
joa-script dump script/scenario/S1L1.sce        # pretty-printed AST + per-call id/arity
joa-script dump script/pscript/AttackSword.txt
joa-script dump scenario/scenario_06.scr
joa-script dump script/scenario/S1L1_desEX.res
joa-script dump Resource/GAMEPLAY.RES [UNIT]    # list records / dump one
joa-script dump Resource/STD.SET LEVEL          # dBase table as TSV
joa-script dump unit/unit.res JOAN_OF_ARC
joa-script dump script/unit/unitAI.res PIG
```

## Coordination notes
- WorldFormats owns `ResArchive`, `DbfTable`, `parse_kv_text`, `parse_block_text`; this area
  owns the meaning of GAMEPLAY.RES / STD.SET / unit / item records and everything under
  `script/`, `scenario/`, `des/`, `item_des/`, `item/Object/**/*.pro`.
- Tests: `formats.script_ast`, `formats.script_particle`, `formats.scenario_def`,
  `formats.text_strings`, `formats.text_vars`, `formats.unitdb` (exit 77 without game data).

## Gaps
- Run-time semantics and per-function semantics: see `reverse-engineering/SCRIPT_VM.md`
  (407-row builtin table, one helper level deep).
- `.uni` files, `S*_tactic.RES`, `animscr.res`, `texteff.res`, `recruit.res` UI usage — not
  analysed here.
