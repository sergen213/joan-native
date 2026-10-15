# Gameplay systems map

What the original game does and where each rule lives (script, engine code, data). Format
details: [formats/SCRIPTS_AND_GAME_DATA.md](formats/SCRIPTS_AND_GAME_DATA.md). Engine-side AI,
task state machine, pathfinding and end-condition internals: AITroopRE's
`reverse-engineering/GAMEPLAY_AI_TROOPS.md`.

Labels: **CONFIRMED** / **HIGH CONFIDENCE** / **HYPOTHESIS** / **UNKNOWN**. "Manual" =
documentation-sourced (`manual/JOA manual.pdf`, via `pdftotext -layout`), never proof of code.
VAs refer to joan.exe (GOG, PE timestamp 2004-01-19).

## Where logic lives

| System | Scripts | Engine code | Data |
|---|---|---|---|
| Campaign order | — | next mission = current + 1 (FUN_0041cc80 callers pass `DAT_00a58fa7 + 1`; new game passes 1) — CONFIRMED | `scenario/scenario_0N.scr` |
| Mission setup, spawns, events, cut scenes, dialog | `S?L1.sce`, `_tactic*.sce`, `_finish.sce`, `_cut_scene_1.sce` — CONFIRMED | 407 built-in functions | `.uni` placement (WorldFormats) |
| Objectives / win / lose | declared in `_des.sce` via `add_end_condition` …; completion set from scripts (`set_end_condition_finish`) and engine (territory links) — HIGH CONFIDENCE | `end_con.cpp` evaluation, `set_delay_before_win` | `*_desEX.res` strings |
| Unit AI (combat, flee, formation) | none — every `unitAI.res` script is `exit` — CONFIRMED; the AI was compiled to C++ (FUN_005467b0 dispatches on the uscrres class → FUN_00531f90…FUN_00533b10, per AITroopRE) | all | tuning in GAMEPLAY.RES UNIT |
| Unit orders in missions | `ACTION("give_task"/"give_order"/"give_team_task")`, `set_unit_attack_behaviour` | task execution | — |
| Stats, damage, levels | — | formulas (below) | `unit/unit.res`, `item/item.res`, STD.SET LEVEL/UPGRADE/UPITEM, GAMEPLAY.RES |
| Animations / combos | — | combo state machine | STD.SET per-unit animation tables (COMBO, DAMAGE, ENERGY_PT …) |
| Shops, recruiting, drops | scripts open shops/barracks (`barrack_set_recruit_list`, `facility_barrack_add_member`) | — | `sellitem.res`, `recruit.res`, `dropitem.res`, GAMEPLAY.RES `*_DI` drop % |
| Difficulty | — | multipliers on hostile damage | GAMEPLAY.RES GAME |
| Particles / effects | — | particle engine | `script/pscript/*.txt` |

## Campaign and mission table

Campaign index → file → script (CONFIRMED from the `.scr` files; order CONFIRMED from
FUN_0041cc80 callers). Titles: only S1L1/S2L1 set `set_scenario_name` in scripts; the others are
summarised from their objectives/briefings (titles are presumably images, INTERFAC.SET `M1`–`M8`
— HYPOTHESIS). History context from `*_obj.obj`.

| # | `.scr` code | Script | World | Mission (from scripts) | Win (type 0) / lose (1) / side (2) / imperative (3) |
|---|---|---|---|---|---|
| 1 | SCENARIO_01 | S1L1 | scenario1 | "The Arrival" — reach and enter Orléans (29 Apr 1429, `set_game_start_date 1429,4,29`) | 3 / 2 / 2 / 0 |
| 2 | SCENARIO_03 | S3L1 | scenario3 | Defend Orléans: stop the slaughter of citizens, protect the Bastard, defeat William and John de la Pole; >17 citizens killed = loss | 9 / 2 / 1 / 1 |
| 3 | SCENARIO_02 | S2L1 | scenario2 | "Attack at Twilight" — capture Camp du pied fragile and Camp du beau vallon, defeat Alexander de la Pole | 3 / 1 / 0 / 0 |
| 4 | SCENARIO_04 | S4L1 | scenario4 | Capture Tourelles and Saint Augustin; Fierbois priest/councillor, prisoners, six arbalests | 6 / 1 / 7 / 0 |
| 5 | SCENARIO_05 | S5L1 | scenario5 | Loire campaign: clean up Jargeau, Meung, Beaugency; villages north/south of the Loire | 12 / 2 / 19 / 4 |
| 6 | SCENARIO_06 | S9L1 | scenario5 (re-used) | Patay (the tactic script references `patay`; objectives set in `S9L1_tactic.sce`, `S9L1_des.sce` is empty) | — |
| 7 | SCENARIO_07 | S7L1 | scenario7 | Capture Saint-Pierre-le-Moutier, La Charité-sur-Loire, Varzy; kill Matthew Gough and Perrinet Gressart; 100 barrels of gold; bribery limit | 13 / 1 / 11 / 2 |
| 8 | SCENARIO_08 | S6L1 | scenario6 | Coronation march: Auxerre, Troyes, Chalons; escort Charles VII to Reims; reputation ≥ 80 before crowning | 7 / 1 / 6 / 2 |

The number in `S?L1` is not the campaign index (scenario 2 plays S3L1, 6 plays S9L1, 8 plays
S6L1). Each mission script is a stage machine: `init` spawns/loads heroes when
`get_scenario_stage == 0`, `SCRIPT()`s the `_tactic` (event logic) and `_finish` (end
sequences) sub-scripts, and checks `get_end_condition_finish(n)` for rewards — HIGH CONFIDENCE.

### Objectives / victory / defeat
- Declared per mission in `<x>_des.sce`: `add_end_condition` → `set_end_condition_type`
  (0 win/main, 1 lose, 2 side quest — script comments in S1L1_des.sce; 3 = "X must live"
  imperative — HIGH CONFIDENCE, matches manual's "Imperatives"), `_des` text, `_territory_link`
  (town whose capture completes it), `_gold_award`, `_experience_award`, `_enabled`,
  `_para1/_para2`.
- Manual: main objectives are required, side goals optional with gold/XP/item rewards,
  imperative commanders must survive (documentation-sourced).
- `set_delay_before_win(ms)` (15000 in S1L1), `set_game_time_limit`, `call_gameover_msg`,
  `set_game_finish`/`is_game_finish` drive the end — names CONFIRMED, timing HYPOTHESIS.
- Towns are captured when all enemies inside are eliminated (manual).

## Heroes (commanders)

Playable in action mode: Joan of Arc, Jean de Metz, La Hire, Duke of Alençon (manual); RTS mode
controls any French unit. Start stats from `unit/unit.res` (CONFIRMED):

| Code | Class | HP | EP | Str | Dex | Def | Lead | Walk/Run | Weapon / Range | Upgrade list | Morale |
|---|---|---|---|---|---|---|---|---|---|---|---|
| JOAN_OF_ARC | LIGHT_INFANTRY | 60 | 30 | 9 | 10 | 8 | 15 | 3 / 5 | JOAN_SWORD_LV1 / Bow | JOAN (JOANUP) | 70 |
| JEAN_DE_METZ | LIGHT_INFANTRY | 50 | 30 | 10 | 15 | 8 | 5 | 4 / 6 | JEAN_SWORD_LV1 / RAPIDFBOW | JEAN (JEANUP) | 70 |
| LAHIRE | HEAVY_INFANTRY | 90 | 20 | 15 | 7 | 12 | 8 | 3 / 5 | LA_HIRE_MACE_LV1 / Bow | LA_HIRE (LAHIREUP) | 80 |
| ALENCON | HEAVY_INFANTRY | 70 | 30 | 12 | 9 | 10 | 30 | 3 / 5 | PA_Gothic / Bow | ALENCON (ALENUP) | 90 |
| ORLEAN (the Bastard) | HEAVY_INFANTRY | 75 | 30 | 13 | 10 | 12 | 30 | 3 / 4.5 | ORLEAN_SWORD / Bow | ORLEAN | 70 |
| CHARLIE7 (Charles VII) | LIGHT_INFANTRY | 60 | 30 | 10 | 10 | 12 | 50 | 3 / 5 | charles7_sword / — | Charles | 70 |

Scripts load persistent heroes with `load_unit` and fall back to `add_unit` (S1L1.sce), mark the
controlled one with `set_main_character`, and assign `set_unit_ranking` GENERAL/CAPTAIN/INFANTRY.
Full per-unit table: `joa-script dump unit/unit.res <CODE>` (117 records incl. enemy generals,
citizens, siege weapons, horses).

## Troop types

Six combat classes (manual): light infantry, heavy infantry, macemen, polemen (melee); archers,
crossbowmen (range); only light infantry crew siege weapons (manual). Base records (CONFIRMED):

| Code | HP | EP | Str | Dex | Def | Walk/Run | Weapon |
|---|---|---|---|---|---|---|---|
| FRENCH_LIGHT_INFANTRY | 32 | 10 | 4 | 5 | 4 | 3.5 / 6 | f_lisword |
| ENGLISH_LIGHT_INFANTRY | 65 | 20 | 4 | 1 | 4 | 3.5 / 6 | ENG_L_INF_SWORD + BOW |
| FRENCH_HEAVY_INFANTRY | 50 | 10 | 7 | 1 | 7 | 3 / 5 | FR_H_INF_SWORD |
| ENGLISH_HEAVY_INFANTRY | 100 | 10 | 6 | 1 | 5 | 3 / 5 | e_hssword |
| FRENCH_MACEMAN / ENGLISH_MACEMAN | 85 / 85 | 20 | 6 / 8 | 3 / 1 | 5 / 2 | 4 / 6.5 | Frenchmace / MACE_E |
| FRENCH_POLEMAN / ENGLISH_POLEMAN | 85 / 90 | 20 / 10 | 3 / 2 | 3 / 2 | 8 | 2.5 / 4 | f_poleaxe / e_poleaxe |
| FRENCH_ARCHER / ENGLISH_ARCHER | 15 / 47 | 10 | 5 / 1 | 7 / 4 | 2 / 1 | 3 / 5 | FRENCH_BOW / ENGLISH_BOW |
| FRENCH_CROSSBOW_MAN / ENGLISH_CROSSBOW_MAN | 30 / 57 | 0 | 7 / 2 | 5 | 2 | 3 / 5 | FrenchcBow / Engcbow |

Captain (`*_CAPTAIN`), Burgundian (`BUR_*`) and mercenary (`MERCENARY_*`, nation `MERCEN`)
variants exist. Siege weapons (CANNON, TREBUCHET, FAUCONNEAU, BOMBARD, BATTERRAM, crossbow
autobows, SIEGE_TOWER) are units; projectile physics constants are GAMEPLAY.RES THROWI
(`ARW`, `PROJ`, `EXPL`, `STM`, `STC`, `CAN`: time/distance limits, mass, force, average velocity,
gravity). Recruitment: `recruit.res` lists `UNIT_CODE : level : cost` per barracks/workshop
(`LOCATION`, `Population`, `SellingRatio`); `des/*.txt` hold the siege-weapon help texts.

## Items, equipment, upgrades

- Item classes/types: STD.SET ITEMCLAS (GATE OBSTACLE HOUSE WALL TERRAIN WEAPON POTION GOLD
  ENHANCER BOX LADDER WAVE SWITCH PATH MISSION ARMOR MISC KEG CROSS PTERRAIN) and ITEMTYPE —
  CONFIRMED. `item/item.res` (250) holds carried items; `item/Object/**/*.pro` (819) placed
  world objects in the same record format.
- Item fields: ItemClass ItemType Code Model ShadowModel AttachBoneName Name UseImmediately
  Width Height Weight Quantity MaxQuantity Price Range Damage Defense Duration, up to 4
  emitters (EmitterName → pscript particle system), CanTransparent ModifyCamera Pickable.
- Hero weapons are per-hero levelled items: JOAN_SWORD_LV1..LV9 damage 10,20,…,90 (price
  400 → 12450), LV10 base 40 plus 31 suffix variants `LV10A…LV10ABCDE` with damage 50–100
  (HYPOTHESIS: A–E are five collected relics combined into the sword); JEAN_SWORD_LV1..5
  (15–80), LA_HIRE_MACE_LV1..5 (24–95). Armour JOAN/JEAN/LAHIRE/ALENCON_ARMOR_LV1..3: Defense
  1–4, and `unit.res` ModelLevel/ModelName lists swap the hero mesh per armour level
  (joan_armorB/C) — CONFIRMED data, mesh-swap rule HIGH CONFIDENCE. Item descriptions:
  `item_des/<code>.txt`.
- Enhancers: amulets (AT_*), gemstones (GS_*), rings (RG_*) — Damage field 9999 (sentinel,
  HYPOTHESIS), effects not decoded (UNKNOWN; engine applies bonuses through FUN_00497e10
  slots 7/8 in the damage code below — HYPOTHESIS).
- Potions/food: Small/Medium/Big Red/Green/Blue/Yellow potions (Damage/Defense/Duration fields
  = restore amounts — HYPOTHESIS), food items `fd_*` (bread, apple, fish, meat, chicken).
- Drops: `DropItemList` → `dropitem.res` (`ITEM : percent`); per-class drop chance GAMEPLAY.RES
  `LINF_DI 10, HINF_DI 12, ARC_DI 8, CRO_DI 8, POLE_DI 10, MACE_DI 8`, captains 100.
- Shops (CONFIRMED decomp, f_shop.cpp / sitemres.cpp / i_buy.cpp): `sellitem.res` records (`S3L1_POTION`, …) =
  `Type POTION|WEAPON`, `SellingRatio`, lines `ITEM : price factor : quantity (-1 unlimited) : level (-1 MinLevel)`.
  `shop_set_sell_item_list` copies a record onto the shop facility, leaving out UniqueItem entries already obtained
  (Game+0x71c, filled by Unit_add_item 0x495f40 → FUN_00472620 whenever a pickup, script gift or hand-over stores a
  UniqueItem; shop purchases do not add to it). The stock is the facility's, kept between visits and saved with it.
  Price = Price × factor × (level − MinLevel + 1).
  A POTION-type or `set_can_regen_sell_item` shop restocks every game hour: round(listed / 2) + left, capped at the
  listed quantity. Sale (whole stack, CanSell items; food only to POTION shops, the rest only to WEAPON ones):
  Price × SellingRatio × quantity / Quantity × level factor when dragged to the seller, Price × SellingRatio × level
  factor (whatever the stack holds) when double-clicked (UI_SYSTEM.md Shop); sold items are not restocked.
  Native: `Facility::stock`, `game::shop_buy` / `shop_sell` (engine buy screen and campaign bot).

## Experience, levels, combos

- Levels 1–30: STD.SET LEVEL `EXPERIENCE` thresholds (1000, 3200, 5800, … 4,065,260),
  `ATT_POINT` attribute points per level (5→12), `SKI_POINT` combo points (1, 2 from level 16)
  — CONFIRMED data; manual: points are spent on six attributes and on combos.
- Attribute gain per point (GAMEPLAY.RES UNIT): `HP_PER 4`, `EP_PER 3`, `STR/DEF/DEX/LDS_PER 1`.
- Kill XP: KILL `EP_NOR 6`, `EP_CAP 15`, `EP_GEN 30`; team share EXP `MEM_EXP 0.4`, `LD_EXP 0.4`;
  critical kill `CRITEXP 3.0` — names CONFIRMED (FUN_0051c680), exact use HYPOTHESIS.
- Upgrade catalogue STD.SET UPITEM: `STRENGTH+1..10`, `DEXTERITY+1..10`, `DEFENSE+1..7`,
  `LEADERSHIP+1..5`, `HP+n`, `EP+n`, `NEW_COMBO_<A/B string>` (REQUIRE = prerequisite combo),
  DOWN_ATTACK, RUSH_ATTACK, ENERGY_ATTACK_UP, RANGE_ATTACK, HORSE_ATTACK_A/B, JUMP_ATTACK_B;
  `COST` in points.
- Per-character plans (UPGRADE → `{LEVEL, CODE}` tables): JOANUP/JEANUP unlock combos by level
  (BB 1, RUSH_ATTACK 3, BBB 6, JUMP_ATTACK_B 8, BBBB 10, HORSE_ATTACK_B 12, AAAAA 14,
  AAAAB 17, ENERGY_ATTACK_UP 19, BBBBB 22, AAAAAB 24). The manual says Rush needs level 4 and
  Energy level 20 → table LEVEL is probably 0-based (level − 1) — HYPOTHESIS. LAHIREUP, ALENUP
  and the troop tables (E_INFUP …) also list per-level stat increments; with UNIT `UPG_TIME
  600000` ("time before ai use upgrade points") these are the AI's automatic spending plan —
  HYPOTHESIS. Manual: ordinary troops level up but cannot change weapons/armour.
- Combos: A = light, B = heavy attack; base set A AA AAA AAAA B AB AAB AAAB, down attack,
  range attack, horse attack A (manual). Per-animation combo data (COMBO, COMBOAVAIL,
  CHAINAVAIL, ENERGY_PT, DAMAGE, START/END_TIME) in the STD.SET per-unit animation tables;
  `CHARMOVE CHAINPRO 200` chain timing. Energy attack costs `EGATK_MP 40` EP and `EGATK_HP 20`
  HP, heavy attack `HATK_MP 6` EP (GAMEPLAY.RES; manual confirms energy attack costs HP+EP).
  Regeneration: `RECOV_MP 1` per `RECOV_T 10000` ms; HP recovery `DEF_HP_R`, `RECOV_HP 10000`.

## Damage and defence (engine)

Read from FUN_005104f0 (melee attack value), FUN_00510bf0 (ranged attack value: range weapon +
ammunition, ÷200 when both present), FUN_00509e90 (ranged hit resolution: distance falloff,
× FUN_00510e40 = (skill_0xe + 10)/11 and × FUN_00510f10) and the melee hit resolution
FUN_00509950 / FUN_0049d810 (same defence formula). Globals loaded by FUN_0051c680 from
GAMEPLAY.RES. Structure HIGH CONFIDENCE (decompiler reading, not executed); role names of
stats/bonus slots HYPOTHESIS where noted:

```
weapon   = Σ equipped weapon Damage (slots 0,1) × (100 + bonus7)/100       // bonus7/8: FUN_00497e10 (enhancers?)
melee    = (Str × STR_DAM + (weapon − WP_DAMG) + 100) × animDamage / 100 × (100 + bonus8)/100
ranged   = (Dex × DEX_DAM + 100) × bowDamage / (100, or 200 if two range items)  × (100 + bonus8)/100
if attacker is hostile to the player: × {EASY_D 0.7, NORM_D 1.0, DIFF_D 1.5, VDIFF_D 2.0}[difficulty]
atk      = (attackerMorale + 100 − MOR_NOR) × raw / 100 × modifiers
def      = (defenderMorale + 100 − MOR_NOR) × (Def × (100 − p) × DEF_RAT / 100 + armourLevel × AMR_DEF) / 100
damage   = atk/2 + max(0, (atk − def)/2)            // at least half of the attack always lands
ranged falloff: full until FALL_S 800, linear to (1 − DIST_P 0.5) at FALL_E 2400
armour level 3 vs explosive (0x40/0x2000 damage types): × AMREXDEF 0.5
friendly bonus: attacker/defender on the player's side × F_ATTACK/100, F_DEFEND/100 (scenario 8 uses /250 for some cases)
```
With the shipped data and the `atoi` truncation (CONFIRMED, see formats doc) `STR_DAM = 3`,
`DEX_DAM = 1`, **`DEF_RAT = 0`**, `WP_DAMG = 0`, `AMR_DEF = 1`, `MOR_NOR = 50`,
`F_ATTACK 130`, `F_DEFEND 70`: the Defense attribute contributes nothing to `def`; only armour
level does. The port must keep this. Other combat constants: block `BLOCKPER 50` % within
`BLOCKANG 70`°, knock-down `T_FALLF/V_FALLF/F_FALLR`, `ARR_FALL 25`, `FELL_DUR 2500` ms,
invincibility after getting up `INVC_DUR 6000` ms, `INDANGER 0.2` HP fraction, siege damage to
units `SIE_UDAM 1.5`, arrows stuck in a body `ARW_ATH 10` for `ARW_TIME 30000` ms.
Difficulty `_A` multipliers scale the native AI's attack chance (source: AITroopRE, HIGH
CONFIDENCE, not re-verified here): FUN_00511f80 = base chance (unit+0x2df, from FUN_00511e70:
30 archer/crossbow, 50 others, 90 for unit-type+0x149 ∈ {4,5}) × {EASY_A 0.5, NORM_A 1.0,
DIFF_A 1.5, VDIFF_A 2.0}[difficulty byte 0xa57fc2], consumed by FUN_00535c20/FUN_00541270
(`rand(100) < chance`). The `_B` globals receive the `_A` values (formats doc); their use —
UNKNOWN.

## Command, morale, leadership

`MAX_COM 2000` command points, `COM_LIM 200`, consumption `COM_CONS 1` per `COM_TIME 1000` ms;
morale normal `MOR_NOR 50` (old define 70), change to all units `MOR_CH_U 1.0`, to troop
`MOR_CH_T 0.3`. Leadership = number of units a commander can lead (manual). Bribery:
`BRI_COMM 5` / `BRI_CAPT 10` gold per unit (UNIT), script functions `set_can_bribe_flag`,
`get_enemy_bribed_cost`, mission 7 objective limits bribe spending.

## AI hooks available to scripts

`give_task`: FOLLOW_UNIT, IDLE, TOWER_ATTACK, DEFEND_PLACE, APPROACH_PLACE, ATTACK_PLACE,
LARGE_WEAPON_PUSH, RTS_ATTACK_UNIT, RTS_ATTACK_ITEM, SCOUT, RETREAT, SPECIAL1–5;
`give_team_task` (same set, per team); `give_order` (APPROACH_PLACE, TURN, MOVE_TO_TARGET,
SHOOT); `stop_all_order`, `stop_curr_task`; queries `is_in_danger`, `is_facing_enemy`,
`can_rush_attack`, `can_energy_attack`, `is_enemy_exist_for_unit_team`; `set_unit_retreat`,
`set_unit_team`, `set_unit_attack_behaviour` ("NEAREST" in the disabled unit AI). Usage counts
are in the formats doc. The decision logic behind them is engine code (AITroopRE).

## Open gaps
- `_finish.sce` reward application path. (Script scheduling/WAIT semantics: CONFIRMED in
  `reverse-engineering/SCRIPT_VM.md`.)
- Enhancer/potion effect decoding; meaning of FUN_00497e10 slots.
- Uses of the difficulty `_B` globals; `CRITICAL`, `C_BOUNS` sections.
- Mission titles for missions 2, 4–8 (likely bitmaps).
