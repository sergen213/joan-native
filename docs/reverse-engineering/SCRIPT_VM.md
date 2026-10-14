# Script VM — runtime semantics of the mission-script interpreter

Owner: ScriptVM. Implementation: `src/game/script_vm.{hpp,cpp}` (VM, `IScriptHost`),
`src/game/script_vm_builtins.cpp` (category table), `src/game/script_host.{hpp,cpp}`
(`RecordingHost`), tool `tools/joa-script-run/`, tests `tests/game/script_{vm,corpus}_test.cpp`
(`ctest -R game`). Language/lexer/grammar and the builtin name table: [../formats/SCRIPTS_AND_GAME_DATA.md](../formats/SCRIPTS_AND_GAME_DATA.md).

Labels as elsewhere: **CONFIRMED** = read in joan.exe code (decompile checked against the
disassembly where it matters), **HIGH**, **HYPOTHESIS**. VAs are joan.exe (GOG, 2004-01-19).
Everything in sections 1–7 is CONFIRMED unless marked.

## 1. Where scripts run

| What | Where |
|---|---|
| Load scenario step | `FUN_0041cc80`: reads `SCRIPT\SCENARIO\<ScriptFileName>.SCE`, compiles it into a new script object (`FUN_0063f190`, 0x70 bytes) stored at step+0x186, loads `<ScriptFileName>EX.res` (`FUN_0063f580`). Then compiles `<DescriptionFileName>.SCE`, loads its EX table, **executes it once** (`exec(0)`) and deletes it — `_des` scripts only declare objectives. |
| Per tick | `Sys_main_loop` (30 Hz frame timer) → `Game_process_tick 0x4e5fc0` (only when game speed ≠ 0, not suspended, in game, screen mode `DAT_00a500c2` ≠ 5) → after the unit/item/AI processes → scenario tick `FUN_00471620` → **one `exec(0)` of the step script root per tick** unless the game is lost (`DAT_00a59298 == 2`). Non-campaign maps run the script at scenario+0x1b1 instead. |
| Game clock | `DAT_00a500ba` (Sys+0x32) += `ftol(frame_ms × game_speed)` every frame; `DAT_00a500be` is that per-frame delta. The fixed-step debug mode uses 0x21 = 33 ms. The VM advances 33 ms per tick (`Vm::kTickMs`). |
| Order inside `FUN_00471620` | challenge timers count down (training mode: challenge duration, else the 4×5 restart durations; unsigned `x > dt ? x-dt : 0`) → script exec → `FUN_004cd5d0`: count times += dt and time limit −= dt (lost at < 1) unless finished / cut scene / clock stopped → end-condition rewards. |
| Unit AI | `unitAI.res` scripts are compiled with the same compiler but every body starts with `exit` (formats doc); they are never needed. |

## 2. Compiled tree

`Script_open_compile`-style recursive descent (`FUN_00649280` → node `compile` = vtable slot 4)
builds C++ node objects (factory `FUN_00640db0`). Vtable slots: [3] exec, [4] compile, [6]
find variable, [12] call executor, [13] WAIT poll. Children live in a DynArray with a **cursor**
(`FUN_00585f20` current, `0x63ee20` next, `0x4b9d40` reset to first) — the cursor is the resume point.

| type | token | node | exec | compile |
|---|---|---|---|---|
| 1 | (root) | script, 0x70 B, vtable 0x9da9c8 | `FUN_00649060` | `FUN_00649280` |
| 2 | ID | labelled block | `0x63f790` | `0x63f8a0` |
| 3 / 4 | INIT / RESET | once-block, done flag +0x70 | `0x63fb80` / `0x63fde0` | `0x63fca0` / `0x63ff00` |
| 8 | WAIT | started flag +0x20 | `0x640370` | `0x6404e0` |
| 9 / 0xb | GOTO / CHOICE | | `0x640690` (returns 2) / `0x640bb0` (returns 1) | |
| 10 | EXIT | | `0x6409d0` returns −1 | |
| 0xc / 0xd | IF / ELSE marker | | `0x6456f0` / `0x648560` | `0x645a40` / `0x648630` |
| 0x10 | argument / rhs wrapper | var ref (+0x14) or sub-node (+0x18) | `0x644900` (always 1) | `0x644b10` |
| 0x11 | INT/FLOAT/STRING/VECTOR declaration | value at +0xc/+0x10 | `0x6439b0` (1) | `0x6439d0` |
| 0x12 | `ident = …` | +0x14 var, +0x18 rhs (0x10) | `0x644530` | `0x644680` |
| 0x13 | constant | | `0x643800` | `0x643820` (atoi / atof→float / string) |
| 0x14 | call statement | +0x14 call node | `0x643f80` | `0x644190` |
| 0x15–0x18 | FUNC / DIALOG / CAMERA / ACTION call | id at +0x20, ret type +0xc | `0x643b60` | `0x643c00` (`0x643d40` for ACTION: resolves order/task name at compile time into +0x22) |
| 0x19 | XYZ | | | |
| 0x1a | SCRIPT | | `0x645d40` | `Script_open_compile 0x645de0` |
| 0x1b | math expression | RPN list | `0x6463f0` | `0x647c90` |

Token → statement node mapping `FUN_006488d0`; declarations go to the enclosing node's
variable list (`0x63ef50`), not its children. Statements can only be keywords, calls, SCRIPT
or `ident = …` — a bare expression is a compile error.

## 3. Execution (return codes 1 = done, 0 = suspended, −1 = exit, 2 = goto restart)

- **Root / block** (`FUN_00649060`, `0x63f790`): run children from the cursor; 0 → return 0
  keeping the cursor (resume here next tick); 1 → next child, at the end reset and return 1;
  −1 → reset, return −1. The driver calls the root every tick, so **the whole body re-runs
  every tick** from the top unless it is suspended inside a WAIT.
- **init** (`0x63fb80`): skipped once its done flag is set; the flag is set only when the body
  completes (an `exit` inside `init` leaves it unset, so `init` re-runs).
- **exit**: −1 propagates through IF/blocks to the root, which resets and ends this tick's pass.
  Inside a WAIT body it is ignored; inside a SCRIPT sub-script it only ends the sub-script.
- **WAIT** (`0x640370`): first entry executes every body statement once, ignoring their
  results, sets the started flag. Then (same tick and every following tick) it polls each child
  that is a call statement (0x14) or an assignment (0x12, polls `rhs->sub-node` = the call):
  `call->vtable[13]`. Any false → reset cursor, return 0. All true → clear flag, return 1.
  The poll does **not** re-evaluate the call or its return value: it asks the builtin whether
  the action it started is finished. Only these builtins can be unfinished (everything else
  returns true, i.e. WAIT{ FUNC("x") } completes in the same tick):
  FUNC (`0x6509e0`): 296/297/302 cut scene (`0x65af50`), 301 (`0x653a90`), 303 (`0x65b0e0`),
  304 buy interface (`0x653c20`), 329 title screen (`0x65c470`), 330 pause_script, 385 brief
  (`0x65eea0`), 390 is_cut_scene_finished (`0x65f600`), 398 letter (`0x65f950`);
  DIALOG (`0x660e60`): 316 add_dialog, 319 add_choice_dialog; ACTION (`0x6612e0`): 323 give_order.
  A non-call rhs in a polled assignment dereferences null in the original (not in shipped data).
- **pause_script(ms)**: `DAT_00a6a254 = game_ms + ms`; done when `DAT_00a6a254 < game_ms`
  (strict). One global wake time. With 33 ms ticks, `pause_script(n)` resumes on tick
  `floor(n/33) + 2` counting the issuing tick as 1.
- **IF** (`0x6456f0`): children = condition, then-statements, ELSE marker, else-statements
  (`else if` = one nested IF). The condition is evaluated only when the cursor is on it, so a
  branch that suspended **resumes without re-testing**. Truthiness: int ≠ 0, float ≠ 0.0,
  string `strlen > 0` (vector ≠ 0; other types return −2).
- **Assignment** (`0x644530`): the rhs wrapper always reports 1, so `x = SCRIPT(…)` never
  suspends. Value is copied **raw by the value's type** — the variable keeps its declared
  type: `INT x; x = (7 / 2)` stores the float bits 0x40600000 and later reads as 1080033280.
  Strings copy with `strncpy(…, 400)`.
- **SCRIPT("path")** statement (`0x645d40`): executes the sub-script root; 0 → suspend the
  caller; 1/−1 → continue. Its value is the raw code (as an expression it never suspends).
  Sub-scripts are compiled **when the parent compiles**, looked up first in a global cache by
  `_stricmp` path (`FUN_00648c50`, refcounted, inserted before compiling so recursion ends).
  One shared instance per path: cursors, init flags and variables are shared by every
  `SCRIPT()` naming the same file. Each sub-script has its own EX table and its own variable
  namespace (root parent pointer = 0).
- GOTO/ID/CHOICE/SPEAKER/UNIT/OBJECT/EVENTSYSTEM/RESET/vectors: compiled by the original but
  unused by shipped scripts; the parser (formats) and VM reject them.

## 4. Expressions (`FUN_00647c90` compile, `FUN_006463f0` evaluate)

- An expression starts with `(`; operands are constants, variables, `FUNC(…)`, `XYZ(…)` or
  `SCRIPT(…)` — `DIALOG/ACTION/CAMERA` inside one is "Unrecognized entity in math expression!".
  A builtin argument or rhs that is not parenthesised is a single operand. Shunting-yard with
  precedence `||` 1, `&&` 2, comparisons 3, `+ -` 4, `* /` 5, `^` 6 (`FUN_006484b0`), equal
  precedence pops → left-associative, parentheses flatten into one RPN list.
- **Evaluation order**: operands are evaluated lazily when their operator is reached, left
  then right. So sub-expressions come first: `( f(3) + f(4) * f(5) )` calls 4, 5, 3.
  `&&` evaluates its right *operand* only if the left is a non-zero int/float (a right
  sub-expression was already evaluated); `||` always evaluates both. Literals are not executed.
- **Int ⊕ int** (`0x646908..`): `+ - *` 32-bit wrapping; **`/` gives a FLOAT**
  (`fild a; fidiv b; fstp float`, so 1/0 = +inf); **`^` is power**: `ftol(pow(a, b))` (int,
  truncation, out of range → 0x80000000); comparisons and `&& ||` give int 0/1.
- **Int/float mix or float ⊕ float**: both converted to float32, `+ - * /` float, `^` =
  `(float)pow`, comparisons int 0/1.
- **String ⊕ string**: `+` concatenates (`strncat` to 400), `< > <= >= == !=` by `strcmp`;
  other operators → error −2.
- Mixed string/number: only `&&` (→ 0); anything else is error −2 (node value left stale).
- No unary minus: `-5` is a literal (lexer), `a - 5` needs spaces.

## 5. Values, variables, strings

- Every node carries a type at +0xc (1 int, 2 float, 3 string, 0x1b vector) and a payload at
  +0x10. Constants: `atoi`, `atof` stored as float32, strings verbatim.
- Variables: `INT/FLOAT/STRING/VECTOR name`, initialised 0 / 0.0 / "" at compile time and never
  re-initialised (the declaration node is not executed). Scope = the declaring node (root,
  INIT, WAIT, IF incl. both branches); lookup `0x63ee70`: own list newest first, then parent;
  names `_stricmp` case-insensitive; must be declared before use ("Variable not found !").
  All variables are therefore persistent across ticks; there are no globals between scripts
  (state shared between scripts goes through builtins: stage, bit flags, counters).
- Builtin argument reads are raw: the executors load `node+0x10` with `mov` (int / char*) or
  `fld` (float) regardless of the type tag. Missing trailing arguments: optional per builtin.
- String buffers are 0x191 bytes: copies truncate to 400 characters.
- Localisation `FUN_006380b0`: a string argument starting with `[` is looked up as
  `atoi(s+1)` in the calling script file's EX table (`<path up to '.'>EX.res`); FUNC builtins
  copy `min(len−2, 400)` chars (drop CRLF), DIALOG ones `min(len, 400)`; no match → the raw
  string including the tag (dialog_text has no fallback). Done per builtin (call_gameover_msg,
  set_scenario_name/description, attention_call_message*, format_string, DIALOG 316/319/321/322…).

## 6. Calls

- Call node exec (`0x643b60`): **arguments are evaluated last to first**, then the keyword's
  executor runs; the call statement always returns 1 (a call never suspends by itself).
- Executors are per keyword and know only their ids: FUNC switch `FUN_00649920` (ids 1–407
  minus the 11 below), DIALOG `FUN_0065fd70` (316–322), ACTION `FUN_00661060` (323, 324, 326,
  327; sets type int), CAMERA `0x661020` (empty). **`ACTION("stop_curr_task", …)` — 21 calls in
  the shipped scripts — therefore does nothing**, and `FUNC("add_dialog")` would do nothing.
- Return type = the table's (`FUN_00661e10` writes it at compile time); the executor stores its
  result at +0x10. `get_character_recno` (63) returns `*(short*)exec_param`; the driver passes 0.
- RNG (`FUN_00590540`, object `0xa45fa8`, seed at +0x7a): Borland LCG
  `seed = seed·0x15a4e35 + 1; return (u32)(n · ((seed >> 16) & 0x7fff)) >> 15`. Seeded once with
  `time(NULL)` in WinMain (`FUN_005904e0`); the same stream is used by 79 engine functions
  (combat, drops, AI), so the original is not reproducible — the VM takes an explicit seed and
  exposes `Vm::rng()` for the world layer to share.

## 7. joa_game API

```cpp
joa::game::Vm vm(loader, host, seed);   // loader: game path -> source (vfs_source_loader)
vm.load_scenario(step);                  // main + _des once, like FUN_0041cc80
vm.tick();                               // one 30 Hz logic tick (FUN_00471620 order)
vm.state();                              // ScenarioState: bit flags, stage, mode, timers, game_finish
class IScriptHost {                      // world binding
  Value call(const BuiltinCall&);        // every non-VM builtin; BuiltinCall: fn (id/name/sig/ret),
                                         // kw, args (raw i()/f()/s()), localized(n), script, line, site, tick
  bool  poll(const BuiltinCall&);        // WAIT predicate of the blocking builtins above
  void  trace(const BuiltinCall&, const Value&, bool vm_builtin);  // observer
};
```
`builtin_category(id)` / `is_blocking_builtin(kw, id)` / `is_vm_builtin(id)` classify ids.
`RecordingHost` returns 0 / 0.0 / "" (spawn builtins: fresh handles 1, 2, …), finishes blocking
builtins after `poll_delay` polls and logs `t=<tick> <file>:<line> KW name(args) = result`.

```
joa-script-run <1..8 | scenario_NN.scr | S1L1 | script\scenario\x.sce> [--ticks N] [--seed S] [--poll-delay N] [--game-dir DIR]
```

**Builtins implemented by the VM** (world-independent, exact): random_number 1;
set_game_finish/is_game_finish 204/206; game time limit 207/208; objective/scenario count times
219–226; challenge restart durations/flags/duration 254–259; scenario/event bit flags, reset,
stage, mode 277–288; backup/restore_bit_flag 291/292; pause_script 330;
get_scenario_passed_real_time 338; format_string 363 (`%Ns`, `%N[,W]d`, `%Nz`, `%%` — the
letters f r ? p q j of `FUN_005a31e0` are reported as errors; shipped text uses only s and d).
Bit flags: bit `(n−1)&31`, set writes `(v & 0xff) << bit` (v = 3 sets two bits).

## 8. Builtin table

Survey of every executor case (one level into its helper). Categories: query 114, unit-state 74, objective 47, world 42, ui 28, flags 23, spawn 16, timer 16, item 15, camera 13, unit-order 8, dialog 7, math 2, sound 2.
**W** = WAIT-blocking, **[VM]** = implemented in the VM. kw = executor that owns the id.
Arg letters i/s/f are corrected from the case disassembly. conf: C = CONFIRMED, H = HIGH,
Y = HYPOTHESIS. Globals: unit array `0xa47c08` (`UnitArray_get`), item
array `0xa460a0`, scenario object `0xa58f90`, step records `DAT_00a58fb7 + (stage−1)·0x1b2`.

| id | name | cat | kw | args | returns | wait | semantics | VA | conf |
|---|---|---|---|---|---|---|---|---|---|
| 1 | `random_number` **[VM]** | math | FUNC | i n | i random 0..n-1 |  | Borland LCG on obj 0xa45fa8 (+0x7a seed): seed=seed*0x15a4e35+1; return (n*((seed>>16)&0x7fff))>>15 (FUN_00590540) | 0x651010 | C |
| 2 | `set_snow_level` | world | FUNC | i level | i always 1 |  | Weather obj 0xa50298: +0x20 (short) = level (FUN_00652a10) | 0x651100 | C |
| 3 | `start_weather_effect` | world | FUNC | i effect | i always 1 |  | If weather obj 0xa50298 byte[+effect+1]==0: effect 1 -> DAT_00a4839c=DAT_00a47bb4; effect 2 -> DAT_00a483ac=DAT_00a47bb4 (record effect start time); else no-op | 0x651130 | H |
| 4 | `call_gameover_msg` | objective | FUNC | s msg | i always 1 |  | Inline case: '[N' msg -> script text table entry (max 400ch); FUN_0065ebd0 sets game-over text (obj+0x9e8, 255ch), opens screen if not shown; player DAT_00a5b9e8 FUN_00508b90(0,1) | 0x65ebd0 | H |
| 5 | `search_for_item` | query | FUNC | i unit_recno, s filter, i radius, i flag | i item recno (0 = none) |  | If unit alive: FUN_00503680(filter,radius,flag) on unit; filter letters T(target)/E[G[C\|O]\|C\|W\|K]/F[G\|W] pick enemy/friendly gates (C=closed hp>0,O=open), walls etc within radius | 0x6519d0 | Y |
| 6 | `get_item_rotation` | query | FUNC | i unit_recno, i item_recno | i degrees |  | (int)(unit facing +0x1a rad->deg - relative angle unit->item pos +0x56 (FUN_00505a00)) = absolute bearing to item; no deleted check on item | 0x653000 | H |
| 7 | `get_item_dist` | query | FUNC | i unit_recno, i item_recno | i distance (0 if either missing) |  | (int)\|item.pos(+0x56..) - unit.pos(+0xe..)\| 3D length | 0x653060 | C |
| 8 | `get_read_letter_once` | flags | FUNC |  | i flag |  | Returns (char)DAT_00a46059 (read-letter-once flag) | 0x653110 | C |
| 9 | `item_code_to_id` | query | FUNC | s item_code | i item type id (1-based, 0 = not found) |  | Lookup item type by code in item-type table 0xa58f50 (stride 0x2bf, FUN_004a36c0, stricmp) | 0x651a30 | C |
| 10 | `add_item_ref` | spawn | FUNC | s item_code | i always 1 |  | If type found: item type refcount++/load resources (FUN_004a39f0) and append id to scenario preload list obj 0xa58fa7 (+0x27d/+0x281, FUN_004cd960) | 0x651a60 | H |
| 11 | `add_item` | spawn | FUNC | s item_code, s nation | i item recno |  | Create item of type (FUN_0049ea90 on item array 0xa460a0); nation name->recno (0xa47bf8 FUN_004b91a0, 0xa57f80 FUN_004b67e0) -> item+8; if type+0x1c4 flag, attach 0x52-byte marker obj at item+0xa2 | 0x651ad0 | C |
| 12 | `del_item` | spawn | FUNC | i item_recno | i always 1 |  | Delete item (FUN_0049f370): deletes linked gate/structure obj (0xa50530, id item+6), calls item dtor, frees slot | 0x651e70 | C |
| 13 | `del_item_by_name` | spawn | FUNC | s name | i always 1 |  | Repeatedly search_item_by_name(name) and delete each match until none (FUN_0049f370) | 0x651ea0 | C |
| 14 | `set_item_pos` | item | FUNC | i item_recno, f x, f y, f z | i item_recno (0 if deleted) |  | Set item map = DAT_00a45f3e (current map, FUN_004994b0) and pos +0x56/+0x5a/+0x5e (FUN_0041ff50, updates render/attached objs) | 0x651f00 | C |
| 15 | `set_item_dir` | item | FUNC | i item_recno, i degrees | i 1 ok / 0 deleted |  | item+0x62 = degrees*0.017453292 (rad) (FUN_00499fd0, updates render obj) | 0x651fa0 | C |
| 16 | `set_item_nation` | item | FUNC | i item_recno, s nation | i nation recno (0 = not found/deleted) |  | Nation name->recno; item+8 (short) = recno or 0 | 0x652010 | C |
| 17 | `add_item_box` | spawn | FUNC | s item_code, s name, f x, f y, f z, i degrees, s content_code | i item recno (0 = fail) |  | Create item, set map DAT_00a45f3e, pos, dir; strncpy name->item+0xc (64); if box (type class 10): content from drop table 0xa50288 (FUN_00463ed0) or item code -> box obj +0xc2/+0xc4 | 0x651c90 | C |
| 18 | `search_item_by_name` | query | FUNC | s name | i item recno (0 = none) |  | Scan item array 0xa460a0 from top for item name (+0xc) == name (stricmp) (FUN_0049f420) | 0x65b570 | C |
| 19 | `search_item_by_code` | query | FUNC | s item_code | i item recno (0 = none) |  | Scan item array for item whose type (+0xa) code (type+0x10) == code (FUN_0049f4b0) | 0x65b5b0 | C |
| 20 | `search_item_sell_list_by_code` | query | FUNC | s item_code | i sell-list index (1-based, 0 = none) |  | Find code in sell list table 0xa501e0 (stride 0x1c, code at +2) (FUN_004ce900) | 0x65b5f0 | C |
| 21 | `is_item_deleted_by_name` | query | FUNC | i item_recno, s name | i 1 = deleted or name differs, 0 = exists with name |  | item deleted -> 1; else stricmp(item+0xc,name)!=0 | 0x65b630 | C |
| 22 | `is_item_box` | query | FUNC | i item_recno | i bool |  | item alive && item+0x4e (class) == 10 (box) | 0x65b6a0 | C |
| 23 | `is_item_gate` | query | FUNC | i item_recno | i bool |  | item alive && item+0x4e == 1 (gate) | 0x65b700 | C |
| 24 | `is_item_wall` | query | FUNC | i item_recno | i bool |  | item alive && item+0x4e == 4 (wall) | 0x65b760 | C |
| 25 | `search_recruit_list_by_code` | query | FUNC | s unit_code | i recruit-list index (1-based, 0 = none) |  | Find code in recruit list table 0xa51278 (stride 0xb4, code at +2) (FUN_004cad00) | 0x65b7c0 | C |
| 26 | `get_gate_hp` | world | FUNC | i item_recno | i hp (0 if not gate) |  | Gate obj = item vfunc+0x4c; returns gate+0xc2 (hp) if item+0x4e==1 | 0x650b90 | C |
| 27 | `get_gate_max_hp` | world | FUNC | i item_recno | i max hp (0 if not gate) |  | Gate obj (item vfunc+0x4c) +0xc6 | 0x650c10 | C |
| 28 | `set_gate_hp` | world | FUNC | i item_recno, i hp | i always 0 |  | gate(vfunc+0x4c)+0xc2 = hp; no class check | 0x650c90 | C |
| 29 | `set_gate_max_hp` | world | FUNC | i item_recno, i max_hp | i always 0 |  | gate(vfunc+0x4c)+0xc6 = max_hp; no class check | 0x650cf0 | C |
| 30 | `gate_open` | world | FUNC |  | i state char (-1 if missing) |  | No args: structure obj #1 in array 0xa50530 -> vfunc+0x4c -> (char)+0xd1 (gate open state); does not open anything | 0x652440 | C |
| 31 | `get_gate_state` | world | FUNC | i item_recno | i state (-1 = not gate/deleted) |  | (char)gate+0xd1 (1 = open) | 0x6524a0 | C |
| 32 | `set_gate_need_key` | world | FUNC | i item_recno, i flag | i flag (-1 deleted) |  | (char)gate+0xce = flag (needs key) | 0x652520 | C |
| 33 | `get_gate_need_key` | world | FUNC | i item_recno | i flag (-1 deleted) |  | (char)gate+0xce | 0x652590 | C |
| 34 | `set_gate_invincible` | world | FUNC | i item_recno, i flag | i flag (-1 deleted) |  | (char)gate+0xcf = flag (invincible) | 0x6525f0 | C |
| 35 | `get_gate_attacked` | world | FUNC | i item_recno | i flag (-1 deleted/no gate) |  | (char)gate+0xd0 (attacked flag) | 0x652660 | C |
| 36 | `get_wall_hp` | world | FUNC | i item_recno | i hp (0 deleted) |  | Wall obj = item vfunc+0x58; returns +0xc6 | 0x650d50 | C |
| 37 | `set_wall_hp` | world | FUNC | i item_recno, i hp | i always 0 |  | wall(vfunc+0x58)+0xc6 = hp | 0x650db0 | C |
| 38 | `get_wall_max_hp` | world | FUNC | i item_recno | i max hp (0 deleted) |  | wall(vfunc+0x58)+0xca | 0x650e10 | C |
| 39 | `break_wall` | world | FUNC | i item_recno, i attacker_unit | i always 0 |  | If unit and item alive: FUN_004eef00(0x40,2,item,1,&item.pos(+0x56),unit,...) on obj 0xa5ccd0 with 1000.0f, DAT_00a500ba - posts damage/break event at wall | 0x650e70 | Y |
| 40 | `set_wall_invincible` | world | FUNC | i item_recno, i flag | i flag (-1 deleted) |  | (char)wall(vfunc+0x58)+0xe6 = flag (invincible) | 0x650f40 | C |
| 41 | `unit_code_to_id` | query | FUNC | s unit_code | i unit type id (1-based, 0 = not found) |  | Lookup in unit-type table 0xa48d88 (stride 0x203, code at +4, stricmp) (FUN_005270b0) | 0x65b160 | C |
| 42 | `add_unit_ref` | spawn | FUNC | s unit_code | i always 1 |  | If type found: unit type refcount++/load sprites (FUN_00527520) and append id to scenario preload list 0xa58fa7 (+0x277/+0x27b, FUN_004cd8b0) | 0x65b190 | H |
| 43 | `search_unit_by_code` | query | FUNC | s unit_code | i unit recno (0 = none) |  | Type id by code, then scan UnitArray 0xa47c08 from top for unit+0x3b (type id) == id (FUN_0051fd50) | 0x65b200 | C |
| 44 | `search_unit_by_name` | query | FUNC | s name | i unit recno (0 = none) |  | Scan UnitArray from top for unit name (+0x3d) == name (stricmp) (FUN_0051fdd0) | 0x65b240 | C |
| 45 | `get_unit_range` | query | FUNC | i unit_recno, i kind | i range (0 = unknown) |  | Const table by unit type class (type+2) x kind 1/2/3: c1 800/1600/2500 c2 600/1200/2500 c3 600/800/2500 c4 1000/2000/2500 c5 250/1200/1600 c6 250/1400/1800 c8 400/1600/1600(+kind4 3200) | 0x65b280 | C |
| 46 | `get_unit_hp_percentage` | query | FUNC | i unit_recno | i hp percent (0 if missing) |  | unit+0x135 (hp) *100 / max hp (FUN_00523320: +0x13d + equipment bonus) | 0x65b500 | C |
| 47 | `set_unit_mode` | unit-state | FUNC | i unit_recno, s mode | i always 0 |  | unit+0x167 = IDLE 3 / ATTACK 1 / ACTIVE 2 (strcmp); unknown string ignored; no alive check | 0x6515d0 | C |
| 48 | `set_unit_attack_behaviour` | unit-state | FUNC | i unit_recno, s behaviour | i always 0 |  | unit+0x16b = NEAREST 1/WEAKEST 2/IMPORTANT 3/NEAREST_ARCHER 4/NEAREST_HIGH_PRIORITY 5 | 0x651690 | C |
| 49 | `is_attacked` | query | FUNC | i unit_recno | i bool |  | unit action +0x215 in {0x23..0x2a,0x40,0x41,0x42} (hit/knockdown/dying states) | 0x6512d0 | C |
| 50 | `will_be_attacked` | query | FUNC | i unit_recno, i attacker_recno | i bool |  | attacker action +0x215 in {0x1c,0x1e,0x1f,0x20,0x21,0x53} (attack swings) and unit within (-90,90) deg of attacker's facing (FUN_00505960) | 0x651430 | C |
| 51 | `is_idle_mode` | query | FUNC | i unit_recno | i bool |  | unit+0x167 > 1 (mode IDLE 3 or ACTIVE 2); no alive check | 0x651550 | C |
| 52 | `is_attack_mode` | query | FUNC | i unit_recno | i bool |  | unit+0x167 == 1 (ATTACK) | 0x651590 | C |
| 53 | `search_for_unit` | query | FUNC | i unit_recno, s filter, i radius, i exclude, i arg5, i arg6 | i unit recno (0 = none) |  | FUN_0054b480(filter,radius(0->1600),exclude,arg5,arg6,0) on unit; filter letters T(target +0x2d7)/E[P](enemy)/L/P/S/F/O; arg5/arg6 limits; case pushes extra 0 | 0x6517c0 | Y |
| 54 | `is_enemy_exist_for_unit_team` | query | FUNC | i unit_recno, i radius | i bool |  | FUN_0054b480("E",radius) on unit; if unit in team (+0x37/+0x33) also on leader's members (+0x33 list) -> 1 if any enemy found | 0x651830 | C |
| 55 | `is_facing_enemy` | query | FUNC | i unit_recno, i target_recno | i bool |  | 1 if (int)bearing(deg) of target.pos-unit.pos == unit+0x1e (float degree field) exactly | 0x6526e0 | H |
| 56 | `is_facing_knock_down_enemy` | query | FUNC | i unit_recno | i bool |  | Any other unit in action 0x40-0x42 (knocked down) that is hostile (FUN_005094d0), within +-90 deg of facing and dist^2 <= 3600 | 0x652820 | C |
| 57 | `is_in_danger` | query | FUNC | i unit_recno | i bool |  | FUN_004fd980(0): hp/base_hp(+0x135/+0x13d) < DAT_00a3cd24 low-HP check; side effect: may auto-use healing item (class 0x15) from inventory; player unit DAT_00a5b9e8 -> 0 | 0x652a40 | Y |
| 58 | `can_attack_down` | query | FUNC | i unit_recno | i bool |  | FUN_00500d40(0x22): checks unit skill/combo list (+0x2f1) allows down-attack action 0x22 | 0x652aa0 | Y |
| 59 | `can_rush_attack` | query | FUNC | i unit_recno | i bool |  | 1 if player DAT_00a5b9e8 alive and hostile to unit; else skill list (+0x2f1) has skill 0x53 and (ready FUN_00416ed0 or inventory item class 0xc FUN_0050d320) | 0x652b00 | C |
| 60 | `can_energy_attack` | query | FUNC | i unit_recno | i bool |  | 1 if player hostile; else needs mp +0x145 >= cost, hp +0x135 > 3*x, skill 0x1e and (ready or FUN_0050d3d0(0xd,0)>0) | 0x652c10 | H |
| 61 | `get_rotation` | query | FUNC | i unit_recno, i target_recno | i degrees (0 if missing) |  | (int)(unit facing deg (+0x1a*180/pi) - relative angle to target (FUN_00505960)) = absolute bearing to target | 0x652ee0 | C |
| 62 | `get_angle_diff` | query | FUNC | i unit_recno, i target_recno | i degrees -180..180 |  | (int)FUN_00505960: angle from unit facing to target pos, wrapped to [-180,180] | 0x652f80 | C |
| 63 | `get_character_recno` | query | FUNC |  | i recno |  | Returns *(short*)executor_arg (FUNC executor [ebp+8], the script's owning object) - recno of character running script | 0x653130 | H |
| 64 | `is_died` | query | FUNC | i unit_recno | i bool |  | unit action +0x215 == 0x2a (dead); no alive check | 0x653c60 | C |
| 65 | `is_inside_facility` | query | FUNC | i unit_recno, i facility_recno | i bool |  | Facility array 0xa5d100: facility(+0x6d)->+0x1de (map) == unit+0x335 (map) and unit pos (+0xe) inside facility rect (FUN_0060d5f0) | 0x653ca0 | C |
| 66 | `is_unit_inside_facility` | query | FUNC | i facility_recno | i unit recno (0 = none) |  | Highest unit recno for which is_inside_facility(unit,facility) | 0x653d60 | C |
| 67 | `get_unit_dist` | query | FUNC | i unit_recno, i unit2_recno | i distance (0 if missing) |  | (int)\|unit2.pos - unit.pos\| (+0xe vec3) | 0x653150 | C |
| 68 | `set_unit_drop_item_id` | unit-state | FUNC | i unit_recno, i item_type_id | i 1 ok / 0 missing |  | unit+0x1c5 (short) = drop item type; if !=0 unit+0x1c7 = item type+0x36 (default level) | 0x653210 | C |
| 69 | `set_unit_drop_item_level` | unit-state | FUNC | i unit_recno, i level | i 1 ok / 0 missing |  | unit+0x1c7 = drop item level | 0x6532a0 | C |
| 70 | `set_unit_track` | unit-state | FUNC | i unit_recno, s track | i 1 ok / 0 missing |  | FUN_00427b50: unit+0x3c9 = track id by name in unit's map path data (+0x339 ->+0x5c table, stride 0x52, name +0x25) | 0x653300 | C |
| 71 | `unit_track_to_id` | query | FUNC | i unit_recno, s track | i track id (0 = none) |  | Same lookup as set_unit_track without storing | 0x653360 | C |
| 72 | `is_unit_track_exist` | query | FUNC | s track | i bool |  | Scan all loaded maps (array 0xa45f08, +1 flag) and their layers (+0x72 count,+0x7a list) for track name | 0x653400 | C |
| 73 | `set_unit_can_process_in_cut_scene` | unit-state | FUNC | i unit_recno (0 = all), i flag | i 1 (0 if missing) |  | unit+0x2e (byte) = flag; unit 0 -> all units | 0x653590 | C |
| 74 | `set_unit_team_can_process_in_cut_scene` | unit-state | FUNC | i unit_recno (0 = all), i flag | i always 1 |  | unit+0x2e = flag and every live member in unit+0x33 team list; unit 0 -> all units | 0x653640 | C |
| 75 | `get_unit_medal_number` | query | FUNC | i unit_recno | i medals (-1 missing) |  | (short)unit+0x1ae | 0x65c0d0 | C |
| 76 | `get_unit_killed_number` | query | FUNC | i unit_recno | i kills (-1 missing) |  | (short)unit+0x1b0 + (short)unit+0x1b2 | 0x65c130 | C |
| 77 | `get_unit_level` | query | FUNC | i unit_recno | i level (-1 missing) |  | (short)unit+0x124 | 0x65c1a0 | C |
| 78 | `set_unit_level` | unit-state | FUNC | i unit_recno, i level | i 1 ok / 0 missing |  | FUN_00523190: only raises (level > +0x124): adds xp from level table, levels up; then hp = max hp (FUN_004fe510) | 0x65c250 | C |
| 79 | `set_unit_id` | unit-state | FUNC | i unit_recno, i unit_type_id | i 1 ok / 0 missing |  | unit+0x3b = type id; if active (+0x2ed) move unit-type refcount (+0x3c5 slot) old->new | 0x65c2e0 | C |
| 80 | `get_unit_arrow_quantity` | query | FUNC | i unit_recno | i arrows (0 none) |  | equipment obj unit+0x1c1 -> arrow slot (+0xe) -> (short)+2 quantity | 0x65c3a0 | H |
| 81 | `get_unit_gold` | query | FUNC |  | i gold |  | Returns DAT_00a596c4 (party gold) | 0x65c200 | C |
| 82 | `add_gold` | unit-state | FUNC | i amount | i always 1 |  | DAT_00a596c4 += amount | 0x65c220 | C |
| 83 | `set_unit_hide_hit_point_bar_flag` | unit-state | FUNC | i unit_recno, i flag | i 1 ok / 0 no render obj |  | (*(unit+0x31d))+0x52 = flag (hide HP bar) | 0x652de0 | C |
| 84 | `activate_all_unit` | unit-state | FUNC |  | i always 1 |  | For every live unit FUN_00528300: release formation slot (+0x2aa/+0x380) and clear two order slots (FUN_004e4520 0s); despite name does not activate | 0x652e60 | H |
| 85 | `add_unit` | spawn | FUNC | s unit_code, s nation | i unit recno |  | Create unit of type (FUN_0051dfc0, UnitArray 0xa47c08); if nation found: set nation (FUN_00509250), unit+0x102 = 1 | 0x655150 | C |
| 86 | `load_unit` | spawn | FUNC | s unit_name, s nation | i unit recno (0 = fail) |  | Needs DAT_00a58f95 and current map: new unit (0x4b4) from scenario unit def FUN_004c91c0(name); set nation on unit+team, +0x102=1, hp=max, mp=max | 0x655200 | H |
| 87 | `del_unit` | spawn | FUNC | i unit_recno | i 1 ok / 0 missing |  | Cancel orders (FUN_00528300), delete ridden mount (+0x1e2), if rank +0x104 > 3 FUN_0050b390 (drop keys), delete unit (FUN_0051e1f0) | 0x655420 | C |
| 88 | `set_unit_pos` | unit-state | FUNC | i unit_recno, f x, f y, f z | i unit_recno (0 missing) |  | unit map +0x335 = DAT_00a45f3e (current map); pos +0xe/+0x12/+0x16 (or physics body +0x2f5) | 0x6554f0 | C |
| 89 | `set_unit_dir` | unit-state | FUNC | i unit_recno, i degrees | i 1 ok / 0 missing |  | unit facing +0x1a = deg*pi/180 (FUN_004ff870, also physics/mount) and +0x1e = (float)deg | 0x655590 | C |
| 90 | `set_unit_degree` | unit-state | FUNC | i unit_recno, i degrees | i 1 ok / 0 missing |  | unit+0x1e = (float)deg only (FUN_0050abd0) | 0x655620 | C |
| 91 | `get_unit_dir` | query | FUNC | i unit_recno | i degrees (0 missing) |  | (int)(unit+0x1a / 0.017453292) | 0x655680 | C |
| 92 | `set_unit_ranking` | unit-state | FUNC | i unit_recno, s rank | i unit_recno (0 missing) |  | Rank +0x104 = KING 5/GENERAL 4/CAPTAIN 3/CAVALRY 2/ARCHER 1/INFANTRY 0 (else 0) via FUN_005089e0 (updates team/nation lists) | 0x6556e0 | C |
| 93 | `set_unit_nation` | unit-state | FUNC | i unit_recno, s nation | i nation recno (0 = not found) |  | Nation name->recno; FUN_00509250 moves unit to nation (unit+2, nation counts); not found -> unit+2 = 0 | 0x655750 | C |
| 94 | `deactivate_unit` | unit-state | FUNC | i unit_recno | i unit_recno (0 missing) |  | FUN_0041fcc0: if active (+0x2ed): clear, +0x2ee=1, remove render/physics of unit and held items (+0x1bd/+0x1bf) | 0x655810 | C |
| 95 | `activate_unit` | unit-state | FUNC | i unit_recno | i unit_recno (0 missing) |  | FUN_00442820: if inactive: +0x2ed=1, +0x2ee=1, create render/physics objs for unit and held items | 0x655870 | C |
| 96 | `set_unit_target_unit_recno` | unit-state | FUNC | i unit_recno, i target_recno | i unit_recno (0 missing) |  | (short)unit+0x2d7 = target recno | 0x6558d0 | C |
| 97 | `set_unit_ride_horse` | unit-state | FUNC | i unit_recno, i horse_recno | i unit_recno (0 missing) |  | FUN_004fc220(horse): mount procedure | 0x655930 | H |
| 98 | `get_unit_riding_recno` | query | FUNC | i unit_recno | i recno (0) |  | unit+0x1e2 (riding/mount recno) | 0x655990 | C |
| 99 | `get_unit_horse_recno` | query | FUNC | i unit_recno | i recno (0) |  | unit+0x1e2 (identical to get_unit_riding_recno) | 0x655b20 | C |
| 100 | `set_unit_ride_cart` | unit-state | FUNC | i unit_recno, i cart_recno | i unit_recno (0 missing) |  | FUN_0051a250(cart): board cart | 0x6559e0 | H |
| 101 | `set_unit_ride_off_cart` | unit-state | FUNC | i unit_recno | i 1 ok / 0 missing |  | If unit+0x1f5 (cart) set: cart+0x1f9 = 0, unit+0x1f5 = 0 | 0x655a40 | C |
| 102 | `get_interact_unit_recno` | query | FUNC | i unit_recno | i recno (0) |  | (short)unit+0x2e3 (interact unit) | 0x655bd0 | C |
| 103 | `set_interact_unit_recno` | unit-state | FUNC | i unit_recno, i target_unit_recno | i 1 always |  | Sets unit+0x2e3 (short interact unit recno) if unit valid. | 0x655c20 | C |
| 104 | `get_interact_item_recno` | query | FUNC | i unit_recno | i item recno (0 = invalid unit) |  | Returns short unit+0x2e5 (interact item recno). | 0x655c80 | C |
| 105 | `set_interact_item_recno` | unit-state | FUNC | i unit_recno, i item_recno | i 1 always |  | Sets unit+0x2e5 (short interact item recno) if unit valid. | 0x655cd0 | C |
| 106 | `get_can_interact_flag` | query | FUNC | i unit_recno | i flag (0 = invalid unit) |  | Returns short unit+0x2e9 (can-interact flag). | 0x655de0 | C |
| 107 | `set_can_interact_flag` | unit-state | FUNC | i unit_recno, i flag | i 1 always |  | Sets unit+0x2e9 (short can-interact flag) if unit valid. | 0x655e30 | C |
| 108 | `set_can_bribe_flag` | unit-state | FUNC | i unit_recno, i flag | i 1 always |  | Sets unit+0x2eb (short bribe state: 0/1 can-bribe, 2 = already bribed) unless it is already 2. | 0x655e90 | C |
| 109 | `get_can_bribe_flag` | query | FUNC | i unit_recno | i flag (0 if invalid or bribed) |  | Returns short unit+0x2eb (can-bribe flag); returns 0 when value is 2 (bribed) or unit invalid. | 0x655f10 | C |
| 110 | `set_units_can_bribe_flag` | unit-state | FUNC | i unit_type_id, i flag | i 1 always |  | For every unit (1..UnitArray size) whose type id unit+0x3b == arg1 and unit+0x2eb != 2, sets unit+0x2eb = flag. | 0x655f80 | C |
| 111 | `set_units_in_place_can_bribe_flag` | unit-state | FUNC | i unit_type_id, i place_recno, i flag | i 1 ok, 0 invalid/inactive place |  | Place (FUN_00456c70) must be valid and place+0x1de active; walks units in place area cells, for type==arg1 inside area and +0x2eb!=2 sets unit+0x2eb=flag. | 0x656030 | C |
| 112 | `is_unit_bribed` | query | FUNC | i unit_recno | i 1 if bribed |  | Returns 1 when unit+0x2eb == 2 (bribed), else 0 (also 0 for invalid unit). | 0x6561b0 | C |
| 113 | `get_interact_stage` | query | FUNC | i unit_recno | i stage (0 = invalid unit) |  | Returns short unit+0x2e7 (interact/dialog stage). | 0x655d30 | C |
| 114 | `set_interact_stage` | unit-state | FUNC | i unit_recno, i stage | i 1 always |  | Sets unit+0x2e7 (short interact stage) if unit valid. | 0x655d80 | C |
| 115 | `set_unit_retreat` | unit-state | FUNC | i unit_recno, i retreat | i 1 always |  | Sets int unit+0x114 (retreat value/threshold; -1 used as special) if unit valid. | 0x656210 | C |
| 116 | `get_unit_retreat` | query | FUNC | i unit_recno | i retreat (0 = invalid unit) |  | Returns int unit+0x114. | 0x656260 | C |
| 117 | `set_unit_armor_id` | unit-state | FUNC | i unit_recno, i armor_id | i 1 always |  | Sets short unit+0x15f (armor id) if unit valid. | 0x6562b0 | C |
| 118 | `get_unit_armor_id` | query | FUNC | i unit_recno | i armor id (0 = invalid unit) |  | Returns short unit+0x15f. | 0x656300 | C |
| 119 | `set_unit_name` | unit-state | FUNC | i unit_recno, s name | i 0 always |  | strncpy(unit+0x3d, name, 64) if unit valid and name non-null. | 0x656350 | C |
| 120 | `get_unit_name` | query | FUNC | i unit_recno | s unit name ("" if invalid) |  | Executor allocates a 0x191 string object if needed, helper copies unit+0x3d name (or empty DAT_009cfb9c) into it (FUN_005a1440). | 0x6563b0 | C |
| 121 | `set_unit_dead` | unit-state | FUNC | i unit_recno | i 1 ok, 0 invalid |  | Sets fade-out params (+0x30=-0.1, +0x38=DAT_00a500ba, alpha) on model +0x2f5/+0x319 and held items +0x1bd/+0x1bf, frees effects, then FUN_004fe510(0) (die). | 0x656410 | H |
| 122 | `reset_unit_hp` | unit-state | FUNC | i unit_recno | i 1 always |  | Restores HP: unit+0x135 = unit+0x13d (max HP). | 0x6568d0 | C |
| 123 | `set_unit_hp` | unit-state | FUNC | i unit_recno, i hp | i 1 ok, 0 invalid |  | Sets int unit+0x135 (HP). | 0x656990 | C |
| 124 | `get_unit_hp` | query | FUNC | i unit_recno | i hp (0 = invalid) |  | Returns int unit+0x135 (HP). | 0x656940 | C |
| 125 | `get_leader_recno` | query | FUNC | i unit_recno | i leader recno (0 = none or unit itself is leader) |  | Gets unit team leader via FUN_004216d0; returns leader recno unless null or equal to unit. | 0x6569f0 | C |
| 126 | `contain_team_member` | query | FUNC | i team_id | i 1 if any member |  | Scans units from last to first; 1 if any unit with team id unit+0x31 == arg and +0x2eb != 2 (not bribed). | 0x656a60 | C |
| 127 | `attach_unit` | unit-state | FUNC | i rider_recno, i mount_recno, s slot_kind (M\|F), i radius | i 1 attached, 0 no slot in range, -1 invalid |  | Finds free attach slot on mount type (+0x1ad count,+0x1b1 pos,+0x1b9 mask 1=M 2=F) within radius; links mount+0x378[slot]=rider, rider+0x384=mount,+0x388=slot; sets pose +0x21d by type. | 0x657670 | H |
| 128 | `get_attaching_unit_recno` | query | FUNC | i unit_recno | i mount recno (-1 invalid) |  | Returns int unit+0x384 (unit this unit is attached to). | 0x656af0 | C |
| 129 | `detach_unit` | unit-state | FUNC | i mount_recno, i slot (<0 = all) | i 1 ok, 0 bad slot, -1 invalid |  | Detaches rider(s) from mount slots: clears mount+0x378/0x37c[slot], rider+0x384/0x388=0,+0x380=-1, pose 0, idles rider (FUN_00508b90), FUN_0065b9b0. | 0x657cc0 | C |
| 130 | `can_attach` | query | FUNC | i unit_recno | i attachable flag (-1 invalid) |  | Returns short unit+0x38c (unit accepts attachments). | 0x656b50 | C |
| 131 | `is_all_attached` | query | FUNC | i mount_recno, s slot_kind (M\|F) | i 1 all filled, 0 not, -1 invalid |  | For each slot matching mask, requires mount+0x378[slot] valid rider whose +0x384 == mount; clears stale links. 0 if +0x38c == 0. | 0x656bb0 | C |
| 132 | `get_nearest_attach_point` | query | FUNC | i unit_recno, i mount_recno, s slot_kind (M\|F) | i slot index (-1 none/invalid) |  | Picks nearest free slot (both +0x378 and +0x37c empty) on mount to unit and reserves it: mount+0x37c[slot]=unit (side effect). | 0x656dc0 | H |
| 133 | `get_nearest_attach_point_x` | query | FUNC | i unit_recno, i slot | i world x (-1 invalid) |  | Transforms unit type attach point (+0x1b1[slot]) by unit matrix; returns X as int (ftol). | 0x657110 | H |
| 134 | `get_nearest_attach_point_y` | query | FUNC | i unit_recno, i slot | i world y (-1 invalid) |  | Same as 133, returns Y component as int. | 0x657240 | H |
| 135 | `get_nearest_attach_point_z` | query | FUNC | i unit_recno, i slot | i world z (-1 invalid) |  | Same as 133, returns Z component as int. | 0x657370 | H |
| 136 | `set_control_flag` | unit-state | FUNC | i unit_recno, i flag | i 1 ok, 0 no team, -1 invalid |  | unit+0x2f = (flag==0); if unit has team (+0x33) and is its leader, applies to all members too. | 0x6574a0 | C |
| 137 | `get_control_flag` | query | FUNC | i unit_recno | i flag (-1 invalid) |  | Returns (unit+0x2f byte == 0). | 0x6575c0 | C |
| 138 | `get_follow_unit_distance` | query | FUNC | i unit_recno | i distance (0 invalid) |  | 200 if no team; else 200+100*k, k from member count when team+0x37==1 (<2:1,<5:2,<10:3,<17:4,<26:5,else 0). | 0x6592c0 | C |
| 139 | `get_two_unit_distance` | query | FUNC | i unit_a, i unit_b | i distance (-1 invalid) |  | Length of position difference between two units (FUN_004fedb0 positions), ftol. | 0x659400 | C |
| 140 | `set_idle` | unit-order | FUNC | i unit_recno | i 1 ok, 0 invalid |  | Sets unit idle: FUN_00508b90(0,1) on unit. | 0x657610 | C |
| 141 | `set_unit_team` | unit-state | FUNC | i unit_recno, i team_id | i 1 joined/leader, 0 invalid or set id only |  | Non-leader: join first leader unit with +0x31==team (FUN_00438f80) else just set +0x31. Leader: FUN_00447600, set +0x31, recruit all units with same team id. | 0x657fd0 | H |
| 142 | `get_unit_team_size` | query | FUNC | i unit_recno | i member count (0 = none) |  | If unit in team (FUN_004b62b0) returns team leader +0x33 -> team+2 (char member count). | 0x658160 | C |
| 143 | `get_unit_team_leader_by_team_id` | query | FUNC | i team_id | i unit recno (0 = not found) |  | Scans nations (FUN_0047b600) unit lists (+0x152d count,+0x1531 recnos) from last nation; returns first valid unit with +0x31 == team_id. | 0x6581f0 | C |
| 144 | `set_team_nation` | unit-state | FUNC | i team_id, s nation_code | i 1 ok, 0 fail |  | Leader = builtin 143; nation recno = FUN_004b67e0(FUN_004b91a0(code)); sets short +2 (nation) on leader and all team members. | 0x6582f0 | C |
| 145 | `set_team_nation2` | unit-state | FUNC | i team_id, s nation_code | i 1 ok, 0 bad nation |  | Sets short +2 (nation recno from code) on every valid unit with +0x31 == team_id. | 0x658420 | C |
| 146 | `recruit_team_member` | spawn | FUNC | i leader_unit_recno, s unit_type_name, i count | i 1 ok, 0 no team/invalid |  | Type via FUN_005270b0(name); up to count while team size < 20: create unit FUN_0051dfc0, set nation=leader +2, place at leader, join team (FUN_00438f80), +0x102=1. | 0x6584e0 | H |
| 147 | `deactivate_team_member` | unit-state | FUNC | i unit_recno | i 0 always |  | For each member of unit team (team+3.. shorts) calls FUN_0041fcc0 (deactivate/hide member). | 0x658650 | H |
| 148 | `activate_team_member` | unit-state | FUNC | i unit_recno | i 0 always |  | For each member of unit team calls FUN_00442820 (reactivate member). | 0x658730 | H |
| 149 | `set_formation_pos` | unit-order | FUNC | i unit_recno, f x, f y, f z, f dir_deg | i unit recno (0 = no team) |  | Team formation anchor: team+0x38/+0x3c/+0x40 = xyz (also +0x48..), +0x44/+0x54 = dir*pi/180; FUN_004fa0f0(1,0) re-form. | 0x658810 | H |
| 150 | `set_team_member_pos` | unit-state | FUNC | i unit_recno | i 0 always |  | Teleports each team member to leader pos + member formation offset (member+0x412) and leader facing (FUN_004fedf0/FUN_004ff870). | 0x658920 | H |
| 151 | `kill_team` | unit-state | FUNC | i team_id | i 1 always |  | Kills leader (via 143) and members, or all units with +0x31==team: if alive and type flag +0x11c&1: +0x120=0, FUN_004fe510(0), FUN_007c6240(DAT_00a5b9e8). | 0x658ad0 | H |
| 152 | `disable_team_drop_item` | unit-state | FUNC | i team_id | i 1 always |  | Sets short unit+0x1c5 = 0 (no drop item) for every unit with +0x31 == team_id. | 0x658d90 | C |
| 153 | `set_team_prisoner` | unit-state | FUNC | i team_id, i prisoner_flag, i bound | i 1 always |  | For valid non-prisoner units of team: FUN_00519800: idle, action 1 or 7(bound), unit+0x2e1=flag, +0x2e2=bound, unequip both hands. | 0x65dc20 | H |
| 154 | `free_team_prisoner` | unit-state | FUNC | i team_id | i 1 always |  | For each unit of team that is prisoner (FUN_00432b90) calls FUN_005199b0 (release, re-equip). | 0x65dcf0 | H |
| 155 | `set_team_control_flag` | unit-state | FUNC | i team_id, i flag | i 1 always |  | unit+0x2f = (flag==0) for every unit with +0x31 == team_id (see 136). | 0x658e10 | C |
| 156 | `set_unit_curr_item` | item | FUNC | i unit_recno, i item_id (0 = clear), s item_name, i hand | i 1 ok, 0 invalid unit/name |  | item_id==0: FUN_0050a480(0,0,hand,0) clears hand slot; else looks up name (FUN_00423220) and equips via FUN_0050a480(item_id, obj, hand, 0). | 0x658ea0 | Y |
| 157 | `set_main_character` | unit-state | FUNC | i unit_recno, i mode | i 1 always |  | mode 1: unit+0x102=0, DAT_00a47c36=unit, FUN_0042f9c0 (player unit), DAT_00a57fae=unit nation, camera 0 target=unit. mode 2: DAT_00a5b9ec=unit. | 0x658f70 | C |
| 158 | `get_main_character` | query | FUNC |  | i main character recno |  | Returns DAT_00a5b9e8 (player-controlled unit). | 0x659060 | C |
| 159 | `del_unit_team` | spawn | FUNC | i team_id | i 1 always |  | Deletes team: if leader found (143) deletes members+leader via FUN_00655420; else deletes all units with +0x31==team (FUN_0050b390 first when +0x104>3). | 0x659080 | C |
| 160 | `del_unit_team_member` | spawn | FUNC | i team_id | i 1 always |  | Deletes every non-leader unit (FUN_0044a660==0) with +0x31==team via FUN_00655420. | 0x659210 | C |
| 161 | `string_to_order_para` | math | FUNC | s text, i part (1..3) | i packed dword (0 if part invalid) |  | Packs up to 12 chars big-endian into 3 dwords (FUN_004bbac0); returns dword #part. | 0x6511f0 | C |
| 162 | `get_curr_task` | query | FUNC | i unit_recno | i task id (0 none) |  | Returns unit+0x2a5; if 0 uses team leader task (unless 0xc) and copies leader+0x3c9 to unit. | 0x6520d0 | C |
| 163 | `get_curr_task_para` | query | FUNC | i unit_recno, i para (1..3) | i task param |  | Returns unit (or leader if unit task 0) +0x2aa/+0x2ae/+0x2b2 for para 1/2/3; else 0. | 0x652190 | C |
| 164 | `get_curr_task_state` | query | FUNC | i unit_recno | i task state |  | Returns unit+0x2ba, or leader+0x2ba when unit task 0 (0 if no leader). | 0x652280 | C |
| 165 | `set_curr_task_state` | unit-state | FUNC | i unit_recno, i state | i 1 ok, 0 no task/leader |  | Writes unit+0x2ba = state when unit has task or a leader. | 0x6522f0 | C |
| 166 | `stop_curr_task` | unit-order | FUNC | i unit_recno | i 1 ok, 0 invalid |  | FUN_00528300: if task 0xf (attach) frees reserved mount slot; clears current task (FUN_004e4520 x2). | 0x652370 | C |
| 167 | `stop_all_team_task` | unit-order | FUNC | i team_id | i 1 always |  | FUN_00528300 (stop task) on every unit with +0x31 == team_id. | 0x6523c0 | C |
| 168 | `search_place_by_name` | query | FUNC | s place_name | i place recno (0 = not found) |  | FUN_004c54a0: linear search place array, strcmp name at place+5; 1-based recno. | 0x653eb0 | C |
| 169 | `search_for_unit_in_place` | query | FUNC | i place_recno, s mode (P\|E\|F) | i unit recno (0 none) |  | In active place area: P = main char DAT_00a5b9e8; E = alive combat unit (type+0x11c&0x2443) enemy of place nation; F = same nation or relation>3. | 0x653ef0 | H |
| 170 | `is_unit_inside_place` | query | FUNC | i place_recno, i unit_recno | i 1 inside, 0 not/invalid |  | Tests unit pos (+0xe) in place area FUN_0060d5f0. | 0x653f30 | C |
| 171 | `number_of_enemy_in_place` | query | FUNC | i place_recno, i unit_recno | i count |  | FUN_0049c470: counts alive units in place area whose nation is enemy (FUN_004b6860) of the given unit nation; also considers place owner nation. | 0x653fe0 | H |
| 172 | `number_of_fightable_enemy_in_place` | query | FUNC | i place_recno, i unit_recno | i count |  | As 171 but excludes non-combat types (type+2 in 0/8/9, type+0x118 in 2/3/7) (FUN_0049e500). | 0x654020 | H |
| 173 | `enemy_exist_in_place` | query | FUNC | i place_recno, i unit_recno | i 1 if any |  | Existence variant of 171 (FUN_004a65d0). | 0x654060 | H |
| 174 | `fightable_enemy_exist_in_place` | query | FUNC | i place_recno, i unit_recno | i 1 if any |  | Existence variant of 172 (FUN_004a8c20). | 0x6540a0 | H |
| 175 | `number_of_people_by_nation_in_place` | query | FUNC | i place_recno, s nation_code | i count |  | FUN_004ab020: counts alive units of nation (code) inside place area. | 0x6540e0 | H |
| 176 | `people_by_nation_exist_in_place` | query | FUNC | i place_recno, s nation_code | i count/1 if any |  | FUN_004b79a0: alive combat-type units of nation in place (excludes type class 0/8/9 and +0x118 2/3/7). | 0x654120 | H |
| 177 | `general_by_nation_exist_in_place` | query | FUNC | i place_recno, s nation_code | i count/1 if any |  | FUN_004c2b50: as 176 but only generals (unit+0x104 >= 4). | 0x654160 | H |
| 178 | `get_spawn_unit_count_in_place` | query | FUNC | i place_recno | i spawn count |  | Returns place+0x1ca (units queued/spawned for place). | 0x6541a0 | C |
| 179 | `change_all_item_in_place` | item | FUNC | i place_recno, i old_item_type, i new_item_type | i 1 always |  | FUN_004c4440: every item (+10 == old) inside place area is recreated as new type at same pos/rotation, owner (+8) = place nation +0x26. | 0x654240 | H |
| 180 | `get_place_captured` | query | FUNC | i place_recno | i captured flag |  | Returns place+0x1ce. | 0x654280 | C |
| 181 | `set_place_captured` | world | FUNC | i place_recno, i captured | i 1 always |  | place+0x1ce=flag; if place nation FRENCH/ENGLISH picks side (captured flips to other); sets nation of class-8 units and owner of class 1/4 items in area. | 0x6542b0 | H |
| 182 | `set_place_nation_by_code` | world | FUNC | i place_recno, s nation_code | i 1 always |  | Valid nation: FUN_0042acd0(code) sets place nation, then class-8 units in area get nation and class 1/4 items get owner (+8). | 0x654560 | H |
| 183 | `get_place_unit_view_angle` | query | FUNC | i place_recno | i angle deg |  | Returns place+0x1d2 (radians) / DAT_009cf884 (pi/180) as int. | 0x6547a0 | C |
| 184 | `set_place_unit_view_angle` | world | FUNC | i place_recno, i angle_deg | i 1 always |  | place+0x1d2 = deg*pi/180 (unit view angle). | 0x6547e0 | C |
| 185 | `set_parking_dir` | world | FUNC | i place_recno, i dir_deg | i 1 always |  | For each parking slot (count place+0xc2) sets float place+0x162[i] = deg*pi/180. | 0x654830 | C |
| 186 | `search_facility_by_name` | query | FUNC | s facility_name | i facility recno (0 = not found) |  | FUN_0046d200: reverse search facility array, strcmp name at +8. | 0x6548b0 | C |
| 187 | `facility_barrack_add_leader` | spawn | FUNC | i facility_recno, i leader_type, i group, i param, s name1, s name2 | i 1 always |  | Barrack (vtbl+0x18) FUN_00467dd0: if +0x7b empty set +0x7b,+0x7f,+0x7d, strncpy +0x81/+0xc1 (64), place+0x1ca++. | 0x6548f0 | H |
| 188 | `facility_barrack_add_member` | spawn | FUNC | i facility_recno, i member_type, i group, i count, i param | i 1 always |  | Barrack FUN_00467ea0: if entries +0x122 < 4 append type +0x124[n], count +0x12c[n], param +0x134[n], +0x7f=group; place+0x1ca += count. | 0x654960 | H |
| 189 | `get_unit_facility_distance` | query | FUNC | i unit_recno, i facility_recno | i distance (0xffff invalid) |  | Length from unit pos to facility pos (+0x29), ftol. | 0x6549c0 | C |
| 190 | `set_facility_follow_unit_recno` | world | FUNC | i facility_recno, i unit_recno | i 1 always |  | Barrack (vtbl+0x18) +0x13c = unit recno (spawned units follow this unit). | 0x654ae0 | H |
| 191 | `get_spawning_count` | query | FUNC | i facility_recno | i total members queued |  | Sums short barrack+0x12c[i] for i < +0x122. | 0x654b30 | C |
| 192 | `reset_spawn_count_in_facility` | spawn | FUNC | i facility_recno | i 1 always |  | place(facility+0x6d)+0x1ca = 0; barrack FUN_00467f90 memset(+0x79, 0, 0xc3) clears leader/member queue. | 0x6541d0 | C |
| 193 | `set_can_recruit_flag` | world | FUNC | i facility_recno, i flag | i 1 always |  | Recruit object (vtbl+0x24) byte +0x97 = flag (can recruit). | 0x654bc0 | C |
| 194 | `set_recruit_level` | world | FUNC | i facility_recno, i level | i 1 always |  | Recruit object (vtbl+0x24) byte +0x98 = level. | 0x654c20 | C |
| 195 | `set_barrack_captured` | world | FUNC | i facility_recno, i captured | i 1 always |  | captured==1: place+0x1ce=1, place nation=main char nation, refresh (FUN_004693b0/469330), clear linked barrack (+0x91) queue, FUN_00468c10. | 0x654c80 | H |
| 196 | `facility_barrack_add_leader_order` | spawn | FUNC | i facility_recno, s order_name, i a, i b, i p5, i p6, i p7, i p8, i p9 | i 1 always |  | Barrack: +0x101 = order id (FUN_004bbbc0: COMBO_ATTACK=1, DOWN_ATTACK=0x13, ENERGY_ATTACK=0x14, RUSH_ATTACK=0x15, SHOOT..), +0x105=a, +0x106..+0x112=p5..p8, +0x11e=p9; b unused. | 0x654d70 | H |
| 197 | `shop_set_sell_item_list` | item | FUNC | i facility_recno, i list_id | i 1 ok, 0 invalid facility |  | Shop object (vtbl+0x20) FUN_004697c0(list_id) rebuilds sell item list. | 0x654e30 | H |
| 198 | `set_can_regen_sell_item` | item | FUNC | i facility_recno, i flag | i 1 ok, 0 invalid |  | Shop object (vtbl+0x20) +0x81 = flag (restock sell items). | 0x654ea0 | C |
| 199 | `barrack_set_recruit_list` | world | FUNC | i facility_recno, i linked_facility, i list_a, i list_b | i 1 ok, 0 invalid |  | Recruit object (vtbl+0x24) FUN_00468790(list_a, list_b) sets recruit list; +0x91 = linked facility. | 0x654f10 | H |
| 200 | `workshop_set_recruit_list` | world | FUNC | i facility_recno, i list_id | i 1 ok, 0 invalid |  | Workshop (vtbl+0x1c) FUN_0046b730: if list valid sets +0xec=list, rebuilds products (+0xa2/+0x112=0). | 0x654fa0 | H |
| 201 | `set_nation_friendship` | world | FUNC | s nation_a, s nation_b, i friendship | i 1 ok, 0 bad code |  | Nation a record: int +0x5d + (b-1)*0xf4 = friendship (FUN_0042a1c0). | 0x653df0 | C |
| 202 | `set_relation_status` | world | FUNC | s nation_a, s nation_b, i status | i 1 ok, 0 bad code |  | Nation a FUN_00499770(b, status, 0): set relation status (with 5-day cooldown vs DAT_00a47bb0 for some cases), caps +0x12 to status*20, updates. | 0x655010 | H |
| 203 | `set_nation_military_strength` | world | FUNC | s nation_code, i strength | i 1 ok, 0 bad code |  | Validates nation then calls FUN_004b5580 which is an empty stub: no-op. | 0x6550d0 | C |
| 204 | `set_game_finish` **[VM]** | objective | FUNC | s result (WIN\|LOSE) | i 1 always |  | "WIN" -> DAT_00a59298 = 1, "LOSE" -> DAT_00a59298 = 2; other strings ignored. | 0x6594e0 | C |
| 205 | `set_game_start_date` | world | FUNC | i year, i month, i day, i minute, i min_start, i min_end | i 1 |  | Date obj 0xa47ba8 FUN_00495290(y,m,d): memset 0x50; DAT_00a47bb8=day, bbc=month, bc0=year, ba8=bb0=JDN(y,m,d), bf0=day-of-year, bac=year, bec=0, be4=time(0). Then DAT_00a47bc4=(float)min_start, DAT_00a47bc8=(float)min_end, DAT_00a47bb4 (minute-of-day float) = clamp((double)minute, min_start, min_end) (FUN_00590b00: <lo->lo, >hi->hi); FUN_004a5630 (ECX 0xa47e08) reselects time-of-day lighting entry with threshold<=DAT_00a47bb4. Clock: FUN_00476bb0 each tick (when DAT_00a500cc!=0): DAT_00a47bb4 += (uint)DAT_00a500be*24/60/1000 (0.4 game-min per game-sec); if >=1440 -> DAT_00a47bb4=DAT_00a47bc4 and FUN_004953d0 next day | 0x659550 | C |
| 206 | `is_game_finish` **[VM]** | objective | FUNC |  | i 1 if DAT_00a59298 != 0 |  | Game-finish state byte DAT_00a59298 (0 running, 1 won [FUN_00455c00/FUN_00471620], 2 lost [LOSE cond or time limit out], 3 set by FUN_00720ca0). Returns state!=0 | 0x6595e0 | C |
| 207 | `set_game_time_limit` **[VM]** | timer | FUNC | i limit_ms | i 1 |  | Single-stage: DAT_00a59161=limit, DAT_00a59165=remaining=limit; multi-stage: R+0x1a6 / R+0x1aa (scenario record R = DAT_00a58fa7==0 ? 0xa58fbb : DAT_00a58fb7+(DAT_00a58fab-1)*0x1b2). FUN_004cd5d0 each tick (state 0, in-game): if limit>0: remaining-=DAT_00a500be; remaining<1 -> DAT_00a59298=2 (lose). Init -1/-1 by FUN_0041dfe0 | 0x659610 | C |
| 208 | `get_game_time_limit` **[VM]** | timer | FUNC |  | i remaining ms |  | Returns remaining time DAT_00a59165 (multi-stage R+0x1aa), not the limit | 0x6596a0 | C |
| 209 | `calculate_date` | world | FUNC | i year, i month, i day | i Julian day number or -1 |  | FUN_0057f990: doy=FUN_0057fef0 (cumulative month table DAT_00a15198, Gregorian leap; -1 if year<0, month!=1..12, day<1 or >days-in-month); returns 1721425 + (y-1)*365+(y-1)/4-(y-1)/100+(y-1)/400 + doy; -1 if doy<1 | 0x6596f0 | C |
| 210 | `get_game_date` | world | FUNC |  | i current date JDN |  | Returns DAT_00a47bb0 (current JDN; +1 per day in FUN_004953d0) | 0x659720 | C |
| 211 | `get_game_hour` | world | FUNC |  | f hours (fractional) |  | Returns DAT_00a47bb4/60.0 (FUN_00659760 on 0xa47ba8 reads +0xc); minute-of-day float advanced by FUN_00476bb0 (see 205) | 0x659740 | C |
| 212 | `get_game_minute` | world | FUNC |  | i minute 0..59 (int bits) |  | __ftol(fmod((double)DAT_00a47bb4, 60.0)) (truncate); executor stores the int32 into node+0x10 with mov even though table type is F, so a float reader sees raw int bits | 0x659790 | C |
| 213 | `get_game_day` | world | FUNC |  | i day |  | Returns DAT_00a47bb8. Next-day FUN_004953d0: day++ capped at 30; JDN++; weekday DAT_00a47bec=JDN%7; if month(JDN)!=DAT_00a47bbc -> day=1, month=month(JDN); if year(JDN)!=DAT_00a47bc0 -> month=1, year=year(JDN); doy recomputed | 0x6597d0 | C |
| 214 | `get_game_month` | world | FUNC |  | i month 1..12 |  | Returns DAT_00a47bbc (see 213 for rollover) | 0x6597f0 | C |
| 215 | `get_game_year` | world | FUNC |  | i year |  | Returns DAT_00a47bc0 (see 213 for rollover) | 0x659810 | C |
| 216 | `change_scenario_step` | flags | FUNC | i step | i 1 |  | DAT_00a58faf=step. Next scenario tick FUN_00471620: if multi-stage (DAT_00a58fa7!=0) and current stage DAT_00a58fab != DAT_00a58faf -> FUN_0041cc80(DAT_00a58fa7, step, 0), FUN_0041c810, FUN_004d7e70(9) (stage switch) | 0x659830 | C |
| 217 | `set_ride_mode` | unit-state | FUNC | i mode | i 1 |  | FUN_0051c4b0 on player-control obj 0xa5b9e8: byte DAT_00a5baac (+0xc4)=(char)mode | 0x659860 | C |
| 218 | `set_fog_distance` | world | FUNC | i distance | i 1 |  | _DAT_00a48cdc=(float)distance (fog distance) | 0x659890 | C |
| 219 | `set_objective_count_time` **[VM]** | timer | FUNC | i seconds | i 1 |  | DAT_00a591cc=seconds (objective count time, not ticked). Objective UI (FUN_00704810/FUN_00734a30) for end cond para1==1 shows minutes left = (DAT_00a591cc - DAT_00a59171/1000)/60 | 0x6598c0 | C |
| 220 | `get_objective_count_time` **[VM]** | timer | FUNC |  | i seconds |  | Returns DAT_00a591cc | 0x6598f0 | C |
| 221 | `set_scenario_count_time` **[VM]** | timer | FUNC |  | i 1 |  | Resets DAT_00a59171=0 (scenario count time 1, ms; 0xa58fa7+0x1ca). Incremented in FUN_004cd5d0 (ECX 0xa58fa7, via scenario tick FUN_00471620 <- Game_process_tick) by DAT_00a500be (game-ms delta = ftol(real ms DAT_00a500b6 * speed DAT_00a500cc)) when DAT_00a59298==0 && DAT_00a51294>9 && DAT_00a500d2 (clock running) && !DAT_00a596d4 (cut-scene flag); also zeroed by FUN_0065f6a0 | 0x659910 | C |
| 222 | `get_scenario_count_time` **[VM]** | timer | FUNC |  | i elapsed ms |  | Returns DAT_00a59171 (ms since reset; see 221) | 0x659940 | C |
| 223 | `set_scenario_count_time_2` **[VM]** | timer | FUNC |  | i 1 |  | Resets DAT_00a59175=0 (count time 2, ms; 0xa58fa7+0x1ce); ticks exactly like 221 | 0x659960 | C |
| 224 | `get_scenario_count_time_2` **[VM]** | timer | FUNC |  | i elapsed ms |  | Returns DAT_00a59175 | 0x659990 | C |
| 225 | `set_scenario_count_time_3` **[VM]** | timer | FUNC |  | i 1 |  | Resets DAT_00a59179=0 (count time 3, ms; 0xa58fa7+0x1d2); ticks exactly like 221 | 0x6599b0 | C |
| 226 | `get_scenario_count_time_3` **[VM]** | timer | FUNC |  | i elapsed ms |  | Returns DAT_00a59179 | 0x6599e0 | C |
| 227 | `set_scenario_rescue_count` | objective | FUNC | i count | i 1 |  | DAT_00a591d0=count (rescue count; shown with DAT_00a591d4 in objective UI FUN_00704810 for para1==2) | 0x659a00 | C |
| 228 | `set_scenario_rescue_limit` | objective | FUNC | i limit | i 1 |  | DAT_00a591d4=limit (rescue limit) | 0x659a30 | C |
| 229 | `add_reputation` | objective | FUNC | i delta | i 1 |  | DAT_00a591d8 += delta (reputation, no clamp) | 0x659a60 | C |
| 230 | `set_reputation` | objective | FUNC | i value | i 1 |  | DAT_00a591d8=value | 0x659a90 | C |
| 231 | `get_reputation` | objective | FUNC |  | i reputation |  | Returns DAT_00a591d8 | 0x659ac0 | C |
| 232 | `set_citizen_killed_count` | objective | FUNC |  | i 1 |  | Resets DAT_00a5918d=0 (no arg). ++ in FUN_0064f320 when a unit with short +2==0 (citizen) is removed and FUN_005094d0(DAT_00a5b9e8&0xffff)==0 | 0x659ae0 | C |
| 233 | `get_citizen_killed_count` | objective | FUNC |  | i count |  | Returns DAT_00a5918d | 0x659e80 | C |
| 234 | `set_fcitizen_killed_count` | objective | FUNC |  | i 1 |  | Resets DAT_00a59199=0. Incremented with 232 when victim byte +0x117=='F' (female citizen) | 0x659ea0 | C |
| 235 | `get_fcitizen_killed_count` | objective | FUNC |  | i count |  | Returns DAT_00a59199 | 0x659ed0 | C |
| 236 | `set_enemy_bribed_count` | objective | FUNC |  | i 1 |  | Resets DAT_00a591a9=0 (bribed-enemy count; FUN_004e8500 does ++count and DAT_00a591ad+=cost). Cost total not reset | 0x659b30 | C |
| 237 | `get_enemy_bribed_count` | objective | FUNC |  | i count |  | Returns DAT_00a591a9 | 0x659b10 | C |
| 238 | `get_enemy_bribed_cost` | objective | FUNC |  | i total gold |  | Returns DAT_00a591ad (sum of bribe costs) | 0x659e40 | C |
| 239 | `get_gold_crates_get` | objective | FUNC |  | i count |  | Returns DAT_00a591b6 (gold crates picked; ++ in FUN_0049da30) | 0x659b60 | C |
| 240 | `change_mission_gold_to_real_gold` | item | FUNC |  | i 1 |  | Loops items 1..n: if not deleted, type +0x4e==10 and name(+0xc) stricmp 's7_goldbag'==0: model(+0xc2 of vtbl+0x50 obj)=id('S7_LGOLDBAG'), name='S7_Barrels' | 0x659b80 | C |
| 241 | `get_food_crates_get` | objective | FUNC |  | i count |  | Returns DAT_00a591be (food crates got; ++ in FUN_0048a1f0) | 0x659ca0 | C |
| 242 | `get_food_picked_up` | objective | FUNC |  | i count |  | Returns DAT_00a591c2 (food picked up; ++ in FUN_0049da30) | 0x659cc0 | C |
| 243 | `set_food_can_be_picked` | item | FUNC | i enable | i 1 |  | Byte DAT_00a591c6=(char)enable; gates food pickup in FUN_0049da30 | 0x659ce0 | C |
| 244 | `set_total_food` | objective | FUNC | i total | i 1 |  | DAT_00a591ba=total | 0x659d10 | C |
| 245 | `get_total_food` | objective | FUNC |  | i total |  | Returns DAT_00a591ba | 0x659d40 | C |
| 246 | `set_citizen_saved` | objective | FUNC |  | i 1 |  | Resets DAT_00a591c8=0 (citizens saved; ++ in FUN_0043bef0) | 0x659d60 | C |
| 247 | `get_citizen_saved` | objective | FUNC |  | i count |  | Returns DAT_00a591c8 | 0x659d90 | C |
| 248 | `increase_citizen_saved` | objective | FUNC |  | i old count |  | DAT_00a591c8++ ; returns value before increment | 0x659db0 | C |
| 249 | `add_scenario_general` | objective | FUNC | i unit_id | i 1 |  | FUN_004cdd40 (ECX 0xa58fa7): if id>0 && count<31: short list DAT_00a591dc[count]=id, count (short DAT_00a5921c)++ | 0x659de0 | C |
| 250 | `remove_scenario_general` | objective | FUNC | i unit_id | i 1 |  | FUN_004cdc40: if id>0: shift entries after first match down by one, zero last, count-- (even when id not found) | 0x659e10 | C |
| 251 | `get_bribed_enemy_kill_count` | objective | FUNC |  | i count |  | Returns DAT_00a591b1 (no direct writer found) | 0x659e60 | H |
| 252 | `set_house_burnt_count` | objective | FUNC |  | i 1 |  | Resets DAT_00a591a5=0 (house burnt count) | 0x659ef0 | C |
| 253 | `get_house_burnt_count` | objective | FUNC |  | i count |  | Returns DAT_00a591a5 | 0x659f20 | C |
| 254 | `set_challenge_restart_duration` **[VM]** | timer | FUNC | s stat, i slot, i ms | i 1 |  | int[4][5] DAT_00a59244+cat*0x14+(slot-1)*4 = ms; cat = stricmp(stat): STRENGTH 0, DEXTERITY 1, LEADERSHIP 2, DEFENSE 3, else 0; slot 1-based, unchecked. FUN_00471620 each scenario tick when mode (FUN_004cd510) != 2 (training): each of 20 entries -= DAT_00a500be, clamped at 0 | 0x659f40 | C |
| 255 | `get_challenge_restart_duration` **[VM]** | timer | FUNC | s stat, i slot | i remaining ms |  | Returns int at DAT_00a59244+cat*0x14+(slot-1)*4 (see 254) | 0x65a010 | C |
| 256 | `set_challenge_flag` **[VM]** | flags | FUNC | s stat, i slot, i value | i 1 |  | byte[4][5] DAT_00a59230+cat*5+(slot-1)=(char)value; cat = stricmp(stat): STRENGTH 0, DEXTERITY 1, LEADERSHIP 2, DEFENSE 3, else 0; slot 1-based, unchecked | 0x65a0e0 | C |
| 257 | `get_challenge_flag` **[VM]** | flags | FUNC | s stat, i slot | i value (signed char) |  | Returns (int)(char) DAT_00a59230+cat*5+(slot-1) | 0x65a1b0 | C |
| 258 | `set_challenge_duration` **[VM]** | timer | FUNC | i ms | i 1 |  | DAT_00a59294=ms. FUN_00471620: when scenario mode==2 (SCENARIO_TRAINING_MODE) it -= DAT_00a500be per tick, clamped at 0 | 0x65a2a0 | C |
| 259 | `get_challenge_duration` **[VM]** | timer | FUNC |  | i remaining ms |  | Returns DAT_00a59294 | 0x65a280 | C |
| 260 | `battle_is_in_game` | query | FUNC |  | i bool |  | FUN_0041c7e0 on 0xa51294: DAT_00a51294 > 9 (game state in-game) | 0x65a2d0 | C |
| 261 | `switch_controllable_unit` | unit-state | FUNC | i hero_index | i 1 |  | FUN_00432300 on 0xa5b9e8: if controlled unit is a hero from 4-name table s_JOAN_OF_ARC_009f8b10 (stride 0x104): 0 = cycle to next eligible living hero, 1..4 = switch to that hero | 0x65a2f0 | H |
| 262 | `quit_rts_mode` | unit-state | FUNC |  | i 1 |  | FUN_0042e040(DAT_00a5b9fc,0) on 0xa5b9e8: control mode DAT_00a5b9f8 := previous mode DAT_00a5b9fc (if not locked) | 0x65a320 | H |
| 263 | `add_end_condition` | objective | FUNC |  | i new 1-based index (0 if 40 full) |  | FUN_00471e20 (ECX 0xa58f90): if DAT_00a59299<40: memset E[count] 0x1a, enabled(+0xf)=1, count++, objective UI add FUN_007078c0(idx-1) | 0x65a350 | C |
| 264 | `del_end_condition` | objective | FUNC |  | i new count (0 if none) |  | FUN_00471ef0: if count>0: UI remove FUN_007072c0(count-1), free des/link strings FUN_00467260, count-- | 0x65a370 | C |
| 265 | `set_end_condition_type` | objective | FUNC | i index, i type | i 1 |  | byte E+0 = type (end cond E[i]=0xa5929b+(i-1)*0x1a (i 1-based, no bounds check; count short DAT_00a59299, max 40)). Types: 0 required, 1 fail (finish==2 -> DAT_00a59298=2), 2 optional (unfinished -> 2 at win), 3 marked finished at win | 0x65a390 | C |
| 266 | `set_end_condition_des` | objective | FUNC | i index, s text | i 1 |  | text: if starts '[' and number matches script string-table id -> table text (len capped 400) else literal; FUN_0065a3c0 -> FUN_00467300 realloc+copy into E+1 (char* des) | 0x65a3c0 | C |
| 267 | `set_end_condition_des_2` | objective | FUNC | i index, s text | i 1 |  | As 266 (localized lookup, E+1 des) plus sound 'OBJECTIVE_UPDATE' (FUN_004ceef0) and UI update FUN_007076c0(index-1) | 0x65a400 | C |
| 268 | `set_end_condition_territory_link` | objective | FUNC | i index, s link | i 1 |  | FUN_00467390 on E: realloc+copy string into E+5 (char*, territory link) | 0x65a460 | C |
| 269 | `set_end_condition_finish` | objective | FUNC | i index, s state | i 1 |  | stricmp state 'WIN'/'LOSE'/'NORMAL' -> E+9 short finish 1/2/0; only if changed; WIN plays 'OBJECTIVE_FINISH'+UI FUN_00707960, LOSE UI FUN_00707b20, NORMAL UI FUN_00707400 | 0x65a4a0 | C |
| 270 | `get_end_condition_finish` | objective | FUNC | i index | i finish 0/1/2 |  | Returns (short) E+9 | 0x65a670 | C |
| 271 | `set_end_condition_para1` | objective | FUNC | i index, i para1 | i 1 |  | short E+0xb=para1 (1 = show objective count time, 2 = show rescue count/limit in UI) | 0x65a6a0 | C |
| 272 | `set_end_condition_para2` | objective | FUNC | i index, i para2 | i 1 |  | short E+0xd=para2 | 0x65a6e0 | C |
| 273 | `set_end_condition_enabled` | objective | FUNC | i index, i enabled | i 1 |  | short E+0xf: if was 0 and new!=0 play 'OBJECTIVE_ADD'; if changed store, new==0 -> UI remove FUN_007072c0 else UI insert FUN_00707400 | 0x65a720 | C |
| 274 | `get_end_condition_enabled` | objective | FUNC | i index | i enabled |  | Returns (short) E+0xf | 0x65a7e0 | C |
| 275 | `set_end_condition_gold_award` | objective | FUNC | i index, i gold | i gold |  | int E+0x11=gold; returns gold. Awarded once (E+0x19 flag) to DAT_00a596c4 when type 0/2 and finish==1 (FUN_00471620) | 0x65a810 | C |
| 276 | `set_end_condition_experience_award` | objective | FUNC | i index, i exp | i exp |  | int E+0x15=exp; returns exp. Awarded once via FUN_005229a0(exp,1,0) to qualifying player units | 0x65a840 | C |
| 277 | `set_scenario_bit_flag` **[VM]** | flags | FUNC | i bit, i value | i 1 |  | FUN_00472000 (ECX 0xa58f90): word R+0x18a (single: DAT_00a59145). bit n uses shift s=(n-1)&31; set: w=(w&~(1<<s))\|((v&0xff)<<s) (v>1 leaks into higher bits); get: (w>>s)&1; word in scenario record R = DAT_00a58fa7==0 ? 0xa58fbb : DAT_00a58fb7+(DAT_00a58fab-1)*0x1b2 | 0x65a870 | C |
| 278 | `get_scenario_bit_flag` **[VM]** | flags | FUNC | i bit | i 0/1 |  | FUN_004720d0: bit of R+0x18a (DAT_00a59145), see 277 | 0x65a8a0 | C |
| 279 | `set_scenario_event_bit_flag` **[VM]** | flags | FUNC | i bit, i value | i 1 |  | FUN_00472150: word R+0x18e (single DAT_00a59149), same bit rules as 277 | 0x65a8d0 | C |
| 280 | `get_scenario_event_bit_flag` **[VM]** | flags | FUNC | i bit | i 0/1 |  | FUN_00472220: bit of R+0x18e (DAT_00a59149) | 0x65a900 | C |
| 281 | `reset_scenario_event_bit_flag` **[VM]** | flags | FUNC |  | i 1 |  | FUN_00472540: R+0x18e (DAT_00a59149) = 0; only event word 1, not _2/_3 or bit_flag | 0x65a930 | C |
| 282 | `set_scenario_event_bit_flag_2` **[VM]** | flags | FUNC | i bit, i value | i 1 |  | FUN_004722a0: word R+0x192 (single DAT_00a5914d), rules as 277 | 0x65a960 | C |
| 283 | `get_scenario_event_bit_flag_2` **[VM]** | flags | FUNC | i bit | i 0/1 |  | FUN_00472370: bit of R+0x192 (DAT_00a5914d) | 0x65a990 | C |
| 284 | `set_scenario_event_bit_flag_3` **[VM]** | flags | FUNC | i bit, i value | i 1 |  | FUN_004723f0: word R+0x196 (single DAT_00a59151), rules as 277 | 0x65a9c0 | C |
| 285 | `get_scenario_event_bit_flag_3` **[VM]** | flags | FUNC | i bit | i 0/1 |  | FUN_004724c0: bit of R+0x196 (DAT_00a59151) | 0x65a9f0 | C |
| 286 | `set_scenario_stage` **[VM]** | flags | FUNC | i stage | i 1 |  | int R+0x19a (single DAT_00a59155)=stage | 0x65aa20 | C |
| 287 | `get_scenario_stage` **[VM]** | flags | FUNC |  | i stage |  | Returns R+0x19a (single DAT_00a59155) | 0x65aa80 | C |
| 288 | `set_scenario_mode` **[VM]** | flags | FUNC | s mode | i 1 |  | FUN_004cd790 stricmp SCENARIO_NORMAL_MODE 0, _INTERACT_MODE 1, _TRAINING_MODE 2, _BATTLE_MODE/other 3 -> FUN_004cd570: R+0x1ae (single DAT_00a59169) | 0x65aad0 | C |
| 289 | `set_scenario_name` | flags | FUNC | s name | i 1 |  | Localized lookup as 266; FUN_004cde30 on R (FUN_004cd840; null if stage out of range): free+strdup into char* R+0x19e (single DAT_00a59159) | 0x65ab10 | C |
| 290 | `set_scenario_description` | flags | FUNC | s text | i 1 |  | As 289 into char* R+0x1a2 (single DAT_00a5915d) via FUN_004cdf00 | 0x65ab50 | C |
| 291 | `backup_bit_flag` **[VM]** | flags | FUNC |  | i 1 |  | Copies 4 flag words R+0x18a/18e/192/196 (single DAT_00a59145/49/4d/51) to backup DAT_00a58f97/9b/9f/a3 (one slot, overwritten each call) | 0x65ab90 | C |
| 292 | `restore_bit_flag` **[VM]** | flags | FUNC |  | i 1 |  | Copies backup DAT_00a58f97/9b/9f/a3 back into the 4 flag words of current R (all four incl. bit_flag) | 0x65aca0 | C |
| 293 | `set_music_side_environment_enable` | sound | FUNC | i enable | i 1 |  | Byte DAT_00a501da = (enable!=0) (side/environment music enable) | 0x65adb0 | C |
| 294 | `set_cut_scene_flag` | camera | FUNC | i on | i bool started/stopped |  | on==1: FUN_006cb9e0(DAT_00a500ba,128.0,1,0,...) start cut-scene letterbox on ctrl 0xa5d380; else FUN_006cc6a0(DAT_00a500ba,-128.0,0,0) end | 0x65ade0 | H |
| 295 | `get_cut_scene_flag` | camera | FUNC |  | i flag |  | FUN_004301c0 (ECX 0xa58f90): byte DAT_00a596d4 (cut-scene active; also halts count times/win check) | 0x65b130 | C |
| 296 | `start_cut_scene` | camera | FUNC | i a1, i a2, i a3, i a4, i a5, i a6, i a7, i a8 | i bool | W | argc 6: FUN_0065aef0(a1,a2,a3,(f)a4,(f)a5,(f)a6,0) -> FUN_006cb9e0(DAT_00a500ba,128.0,...); argc 8: FUN_0065ae60(a1..a5,(f)a6..a8,0) -> FUN_006cad50. WAIT: FUN_0065af50 | 0x65aef0 | H |
| 297 | `start_cut_scene_2` | camera | FUNC | i a1, i a2, i a3, i a4, i a5, i a6, i a7, i a8, i a9 | i bool | W | As 296 with last arg passed as byte flag: argc 7 -> FUN_0065aef0(a1,a2,a3,f4,f5,f6,a7); argc 9 -> FUN_0065ae60(a1..a5,f6,f7,f8,a9). WAIT FUN_0065af50 | 0x65aef0 | H |
| 298 | `end_cut_scene` | camera | FUNC | i a1 | i bool |  | FUN_0065aff0(a1,0) -> FUN_006cc6a0(DAT_00a500ba,-128.0,a1,0) on 0xa5d380: end cut scene, restore camera/state (a1 = restore camera flag) | 0x65aff0 | H |
| 299 | `end_cut_scene_2` | camera | FUNC | i a1, i a2 | i bool |  | FUN_0065aff0(a1,(byte)a2) -> FUN_006cc6a0(DAT_00a500ba,-128.0,a1,a2) | 0x65aff0 | H |
| 300 | `get_cut_scene_status` | camera | FUNC |  | i status |  | Returns int status at 0xa5d380+0xf (cut-scene controller) | 0x65afd0 | C |
| 301 | `start_cut_scene_camera` | camera | FUNC | i unit, s script, i delay_ms, f a4, i a5, i a6 | i result | W | FUN_006537a0: if unit valid and has camera data (+0x339 ->+0x60): FUN_00654a80(a5), FUN_00428a70(a4), FUN_00654ab0(a6), FUN_00428850(script, DAT_00a500ba+delay). WAIT FUN_00653a90 | 0x6537a0 | H |
| 302 | `change_camera_in_cut_scene` | camera | FUNC | i a1, i a2, i a3, i a4, i a5, i a6, i a7 | i bool | W | argc 5: FUN_0065b030((byte)a1,a2,(f)a3,(f)a4,(f)a5) -> FUN_006cd720 camera on unit a2 (+50 z) ; argc 7: FUN_0065b070((byte)a1,a2,a3,a4,(f)a5..a7) -> FUN_006ccf00 camera at x,y,z; only if status 2/7. WAIT FUN_0065af50 | 0x65b030 | H |
| 303 | `is_cut_scene_status_black_screen` | camera | FUNC |  | i bool | W | FUN_0065b0e0: true if cut scene not active (0xa5d380+4==0) or status (+0xf)==7 | 0x65b0e0 | C |
| 304 | `start_buy_interface` | ui | FUNC | i unit, i seller | i 1 ok / 0 | W | FUN_00653890: if units valid: FUN_006c8d00(1), set buyer FUN_00655b70(a1), seller FUN_00655ba0(a2), FUN_00592c30 (open buy UI). WAIT FUN_00653c20 | 0x653890 | H |
| 305 | `close_inventory_interface` | ui | FUNC |  | i 1 |  | FUN_00653930: if inventory UI open (FUN_00430340 on 0xa56398) close it FUN_006f7e90 | 0x653930 | C |
| 306 | `show_objective` | ui | FUNC |  | i 1 |  | FUN_00707ce0 on 0xa4d618: show objectives panel | 0x653970 | H |
| 307 | `start_scenario_part_interface` | ui | FUNC | i part | i 1 |  | DAT_00a5027e=part (byte), DAT_00a5027f=2, interface object +0x3c=0xc (FUN_004d7e70): opens the scenario-part (chapter) intro interface | 0x6539a0 | C |
| 308 | `minimap_set_way_point` | ui | FUNC | i slot, f x, f y, f z, i type, i flag | i 1 |  | minimap waypoint slot (1-based) FUN_00718010: type 0 clears; else stores type at +0x46[slot], pos at +0xdc[slot], blink start=game_ms (DAT_00a500ba), flag at +0x244[slot] | 0x6539e0 | C |
| 309 | `minimap_set_way_point_follow_unit` | ui | FUNC | i slot, i unit_recno, i flag | i 1 |  | minimap waypoint slot (1-based) follows unit: minimap+100[slot]=unit_recno (0 = none), +0x244[slot]=flag (FUN_007180f0) | 0x653a50 | C |
| 310 | `get_distance` | query | FUNC | i unit_recno, i x, i y, i z | i distance (-1 = bad unit) |  | ints converted to float point; 3D length(point - unit position) truncated (__ftol) | 0x65b800 | C |
| 311 | `set_getting_weapon_flag` | unit-state | FUNC | i unit_recno, i flag | i 1 ok / 0 bad unit |  | unit+0x374=flag; unit+0x21d (special anim state)=4 if flag else 0 | 0x65b8c0 | C |
| 312 | `is_getting_weapon` | query | FUNC | i unit_recno | i flag (0 = bad unit) |  | returns unit+0x374 (getting-weapon flag set by 311) | 0x65b950 | C |
| 313 | `get_default_weapon` | unit-state | FUNC | i unit_recno | i 1 ok / 0 bad unit |  | despite name: re-equips the unit's default weapon(s): for hand slots unit+0x1bd/+0x1bf, if item gone creates default item (FUN_004974f0/FUN_0049ea90) and equips via FUN_0050a480 | 0x65b9b0 | H |
| 314 | `drop_item_on_floor` | item | FUNC | i unit_recno, i item_recno, i flag | i 1 ok / 0 bad unit or item |  | creates a copy of item's type (FUN_0049ea90), copies item+0x86 data, places it at item world position/orientation (unit's place +0x335), FUN_0050a480(0,0,flag,0) | 0x65bce0 | Y |
| 315 | `save_game` | world | FUNC | s file_name, s description | i 1 |  | save game: path = file_name+'.SAV'; reuses matching save slot (FUN_00477640) else new slot 'SAVE\\'+path, desc at +0x11a; +0x19a=DAT_00a58fa7, +0x19e=0; GameFile_save() | 0x65bed0 | C |
| 316 | `add_dialog` | dialog | DIALOG | i speaker_recno, s text, i x1, i y1, i x2, i y2, i unused (args 3-6 optional, 7 optional) | i dialog id (0 = bad speaker) | W | deletes dialogs; text localised min(len,400) (+8/+0xe anim if entry+2), voice by tag; FUN_0063cdc0 box '_'+name(+0x3d)+':_ '+text, rect x1,y1,x2,y2 else default; WAIT until done | 0x63cdc0 | C |
| 317 | `del_dialog` | dialog | DIALOG | i dialog_id | i (unchanged) |  | FUN_0063d120 -> FUN_0063e1d0(dialog_id): deletes dialog box | 0x63e1d0 | C |
| 318 | `del_all_dialog` | dialog | DIALOG |  | i (unchanged) |  | FUN_0063d150: deletes all dialog boxes not finished-guarded, clears list (FUN_00585d20) | 0x63d150 | C |
| 319 | `add_choice_dialog` | dialog | DIALOG | i speaker_recno, s question, s choice1, i enabled1, ... (pairs) | i dialog id | W | deletes all dialogs; question localised (min(len,400)) + voice; FUN_0063cdc0 creates box; each (choice,enabled) pair localised -> FUN_0063d250(dlg,text,enabled!=0); WAIT FUN_0063d860 | 0x63d250 | C |
| 320 | `get_choice` | dialog | DIALOG | i dialog_id | i chosen index |  | returns dialog object (FUN_0063e3c0)+0x353 (selected choice) | 0x63d770 | C |
| 321 | `dialog_text` | dialog | DIALOG | s text_tag | s localised text (401-byte buffer) |  | Result: node-owned 0x191-byte string. FUN_006380b0(text_tag): only if tag[0]=='[', id=atoi(tag+1) (e.g. '[0063N]...' -> 63) searched in script text table (root node+0x64; count short +1, 16-byte entries at *(+3): +0 u16 id, +4 char* text, +0xc i16 len); copies min(len,400) chars (DIALOG path: no -2) then strncpy 400. No fallback: unknown tag indexes entry 0 (garbage). Underscores/%N placeholders untouched (feed to format_string). | 0x65fd70 | C |
| 322 | `set_dialog_voice` | dialog | DIALOG | i dialog_id, s text_tag | i 1 |  | voice id = FUN_00639380(script stem, atoi(text_tag+1)); dialog object+0x340 = voice id | 0x65fd70 | C |
| 323 | `give_order` | unit-order | ACTION | i unit_recno, s order_name, i param1, i team_flag, i p3, i p4, i p5, i p6, i time_limit_ms | i result of FUN_004bac60 (0 = bad unit) | W | order name pre-resolved to node+0x22 id; creates unit+0x2a1 order obj; FUN_004bac60(unit,id,p1,p3..p6,game_ms+time_limit); team_flag also orders team (id 0xf: skip if member has it) | 0x661340 | H |
| 324 | `stop_all_order` | unit-order | ACTION | i unit_recno | i 1 ok / 0 bad unit |  | cancels current order (FUN_004bb660 if active), frees unit+0x2a1 order object, FUN_00422f10 | 0x6618f0 | C |
| 325 | `get_curr_order` | query | FUNC | i unit_recno | i order id (0 = none/bad unit) |  | returns (unit+0x2a1 order object)+0x1d = current script order id | 0x651260 | C |
| 326 | `give_task` | unit-order | ACTION | i unit_recno, s task_name, i p1, i p2, i p3, i p4, i time_ms | i 1 ok / 0 bad unit |  | task name pre-resolved to node+0x22 id; FUN_004e4520(id,p1(byte),p2,p3,p4, time_ms?game_ms+time_ms:0) then FUN_00527fb0; task 0xb with p3==1 also for team | 0x6619d0 | H |
| 327 | `give_team_task` | unit-order | ACTION | i unit_recno, s task_name, i p1, i p2, i p3, i p4, i time_ms | i 1 ok / 0 bad unit |  | as 326 but always also applies FUN_00527fb0 to every live member of the unit's team (FUN_004216d0 leader team list) | 0x661b40 | H |
| 328 | `change_animation_special` | unit-state | FUNC | i unit_recno, s anim_name | i 1 ok / 0 bad unit |  | unit+0x21d = FUN_00416aa0(name): DEFAULT 0,TIRED 1,HAPPY 2,POISON 3,CAMP 4,CANNON 5,FAUCONN 6,BOMBARD 7,CROSSBOW 8,SIEGET 9,TREB_L 10,TREB_R,TREB_F,BATTER,BATTER_L/R... | 0x65c050 | C |
| 329 | `display_title_screen` | ui | FUNC | s title, i duration_ms, i fade_in_ms, i fade_out_ms | i 1 | W | title-screen object FUN_00761680: active=1, start=game_ms (DAT_00a500ba), the 3 ints stored at +5/+9/+0xd, title strncpy 30 chars (+0x11). WAIT predicate FUN_0065c470 | 0x65c420 | C |
| 330 | `pause_script` **[VM]** | timer | FUNC | i ms | i 1 | W | DAT_00a6a254 = game_ms(DAT_00a500ba)+ms (wake time); WAIT completes when wake < game_ms | 0x65c4a0 | C |
| 331 | `set_stop_unit_process_flag` | flags | FUNC | i flag | i 1 |  | DAT_00a596ab = flag (byte): global stop-unit-processing switch | 0x65c500 | C |
| 332 | `add_gate_trigger` | world | FUNC | i gate_recno, i delay_ms | i 1 ok / 0 bad gate object |  | gate is in item/object array; queues event FUN_004eef00(type 0x20, 2, gate+6 id, ..., player DAT_00a5b9e8, ..., fire at game_ms+delay_ms) | 0x65c530 | H |
| 333 | `give_item_to_unit` | item | FUNC | s item_name, i param, i quantity, i unit_recno | i result of add (0 = fail) |  | item class by name (FUN_004a36c0); FUN_0050fd90(class,quantity,param,0) adds to inventory; if unit is player (DAT_00a5b9e8): pickup message + item-hint popup once | 0x65c5f0 | H |
| 334 | `give_item_to_unit_troop` | item | FUNC | s item_name, i param, i quantity, i unit_recno | i result of add (0 = fail) |  | as 333 but FUN_0050fd90(...,1) (give to unit's troop/team); player pickup message only | 0x65c770 | H |
| 335 | `set_unit_training_mode` | unit-state | FUNC | i unit_recno, i mode | i 1 ok / 0 bad unit |  | FUN_00510030: unit+0x10c=mode; entering mode saves +0x120 to +0x110 and sets +0x120=1, leaving restores; recomputes +0x135/+0x145 (hp/stats) | 0x65c870 | H |
| 336 | `is_unit_dead` | query | FUNC | i unit_recno | i 1 dead / 0 alive or bad recno |  | FUN_0042d0e0: dead when unit+0x135 (hp) < 1 and unit+0x120 == 0 | 0x65c8d0 | C |
| 337 | `is_unit_dead_by_name` | query | FUNC | i unit_recno, s unit_name | i 1 dead/replaced / 0 alive |  | 1 if recno invalid or unit+0x3d name != unit_name (recno reused), else dead test FUN_0042d0e0 | 0x65c930 | C |
| 338 | `get_scenario_passed_real_time` **[VM]** | timer | FUNC |  | i ms |  | DAT_00a500ba (game_ms) - DAT_00a5916d (scenario start time) | 0x65c9b0 | C |
| 339 | `add_unit_experience` | unit-state | FUNC | i unit_recno, i exp | i 1 ok / 0 bad unit |  | FUN_005229a0(exp,0,0): adds experience (skipped for unit class type 8 / mode 2) | 0x65c9e0 | H |
| 340 | `get_item_class_quantity_in_unit_inventory` | query | FUNC | s item_class_name, i unit_recno | i quantity (0 = none) |  | item class index by name (FUN_0049fab0, table stride 0x2c); counts items of that class in unit inventory (+0x1c1) via FUN_004985f0 | 0x65ca40 | H |
| 341 | `get_item_quantity_in_unit_inventory` | query | FUNC | i item_type_id, i unit_recno | i quantity |  | FUN_004987d0: sums quantity (+2) of inventory entries whose id == item_type_id; 0 if no inventory (+0x1c1) | 0x65cac0 | C |
| 342 | `number_of_enemy_in_world` | query | FUNC | i unit_recno | i count |  | counts live units in same place (+0x335) whose class kind (+0x118) is 0/1 with nation (+2)!=0 and hostile to unit's nation (FUN_004b6860) | 0x65cb80 | C |
| 343 | `number_of_friend_in_world` | query | FUNC | i unit_recno | i count |  | counts live non-hidden (+0x2f==0) units in same place (+0x335), class kind 0/1, nation same as unit (+2) and nation not 0/8/9 | 0x65cd00 | C |
| 344 | `attention_call_message` | ui | FUNC | s text, i tutorial_flag (optional) | i message recno (0 = duplicate) |  | text localised ([NNNNN] tag -> table text min(len-2,400), else raw 400); FUN_004ad710(text, flag?'TMESS':'M0000' icon, voice 0); refused if same text already live | 0x65cea0 | C |
| 345 | `attention_call_message_2` | ui | FUNC | s text, ? speaker | i message recno (0 = duplicate) |  | text localised as 344; voice=FUN_00639380(script stem, atoi(text+1)); speaker string -> FUN_004ad710 portrait by name; int unit recno -> FUN_004ad9a0 | 0x65cf00 | C |
| 346 | `set_attention_call_message_voice` | sound | FUNC | i message_recno, s text_tag | i msg ptr (0 = no message) |  | voice id = FUN_00639380(script stem, atoi(text_tag+1)) if tag found in text table else 0; attention message+0x19 = voice id | 0x65cf80 | C |
| 347 | `switch_unit_model` | unit-state | FUNC | i unit_recno, i model | i 1 ok / 0 bad unit |  | FUN_0050c8a0: model 0 = default mesh (+0x309/+0x311), 1 = alternate (+0x2f9/+0x301) swapped into +0x2f5; model 1 also FUN_0050cbc0(1,1) | 0x65cfd0 | H |
| 348 | `enable_unit_physic` | unit-state | FUNC | i unit_recno, i enable | i 1 ok / 0 bad unit |  | FUN_005102a0: enable=0 sets unit+0x3b8=1, detaches physics body and saves pos to +0x3cd..; enable!=0 clears +0x3b8 and re-creates physics | 0x65d070 | H |
| 349 | `enable_unit_bounding_volume` | unit-state | FUNC | i unit_recno, i enable | i 1 ok / 0 bad unit |  | mesh(+0x2f5)+0xb8 bounding-volume type: 0 disables; else copies type/size from unit class (+0x1c3.. shape 0..3) | 0x65d0d0 | C |
| 350 | `enable_unit_team_bounding_volume` | unit-state | FUNC | i unit_recno, i enable | i 1 ok / 0 bad unit |  | applies 349 to the unit and every member of its team (unit+0x33 team list) | 0x65d350 | C |
| 351 | `mount_siege_tower_to_wall` | unit-state | FUNC | i tower_recno, i wall_recno, i mount | i 1 ok / 0 bad unit |  | siege tower: FUN_004633e0 links wall object; state unit+0x3b4: 0->1 when mount, 1->0 unmount, 2->3 unmount, 3->2 mount | 0x65d450 | H |
| 352 | `is_unit_parked` | query | FUNC | i unit_recno | i 1 parked / 0 |  | unit on track (+4 track set, +6 point): 1 if within 10.0 of track point position and heading diff to point angle < 0.1 | 0x65d560 | C |
| 353 | `unit_dist_to_track` | query | FUNC | i unit_recno, i track_recno, i use_end | f 2D distance (0.0 on failure) |  | unit must have a path/mesh (+0x339)+0x5c; distance XY from unit to track's first point (use_end=0) or last point | 0x65d6f0 | C |
| 354 | `get_unit_cur_track_recno` | query | FUNC | i unit_recno | f track recno (int converted, 0 bad unit) |  | returns unit+0x3c9 (current track recno) as float (fild) | 0x65d850 | C |
| 355 | `get_team_size` | query | FUNC | i team_id | i members (0 = none) |  | scans places/towns; finds unit with +0x31 == team_id having team (+0x33); returns team member count (team+2) | 0x65d8a0 | C |
| 356 | `display_string` | ui | FUNC | s text, i x, i y | i 1 |  | FUN_006255d0 queues on-screen text at screen x,y, colour white (FUN_0042a260(255,255,255,255)), scale 1.0, one frame | 0x65d9c0 | H |
| 357 | `display_number` | ui | FUNC | i value, i x, i y | i 1 |  | formats value with FUN_0058fca0(value,4) (plain integer, negative in parentheses) then draws like 356 | 0x65da50 | C |
| 358 | `set_prisoner` | unit-state | FUNC | i unit_recno, i prisoner_type (optional, default 1) | i 1 ok / 0 bad unit |  | no-op if already prisoner with anim 7; else FUN_00519800(type,1): stops unit, plays anim 7 (prisoner), unit+0x2e1=type | 0x65daf0 | C |
| 359 | `free_prisoner` | unit-state | FUNC | i unit_recno | i 1 ok / 0 bad unit |  | if unit+0x2e1 prisoner: FUN_005199b0 releases, re-equips default weapon and clears prisoner state | 0x65dba0 | H |
| 360 | `dist_to_attaching_point` | query | FUNC | i unit_recno | f 2D distance (-1.0 none, 0.0 bad unit) |  | attached unit (+0x388 index, +0x384 vehicle): XY distance from unit to the vehicle's attach point index (class +0x1b1 table, vehicle transform) | 0x65ddb0 | C |
| 361 | `get_attaching_index` | query | FUNC | i unit_recno | i attach point index (0 bad unit) |  | returns unit+0x388 | 0x65dfa0 | C |
| 362 | `set_item_path_item_owner` | item | FUNC | i item_recno, i owner_recno | i 1 ok / 0 bad item |  | item vtable+100(2, owner); FUN_00491c40(2, owner): sets owner of path item | 0x65dff0 | H |
| 363 | `format_string` **[VM]** | ui | FUNC | s template, ? arg1..arg7 | s formatted text (401-byte buffer) |  | Result: node-owned 0x191-byte string (alloc once). Template=arg1 localised: if script text table exists and arg1[0]=='[' and atoi(arg1+1) == entry id, entry text min(len-2,400) chars, else arg1 raw (400). argc 2..8 pass args 2..argc raw 32-bit (argc 2 only if arg2 int/string, else no update). Formatter FUN_005a31e0: %%->%; '%'+non-digit->literal '%'; %N[,W]c N=1..9 arg index (args consumed in N order, 4 bytes each, f=8), c: d int with ',' thousands (W 2 '$' prefix, 3 '%' suffix, 4 no commas; negative '(n)'); z = d but empty if 0; f double (2 decimals if \|x\|<1000); r roman numeral (empty if <1); s string; ?{nonzero\|zero}; p{1-or-0\|other}; q{0\|1\|other}; j{a\|b\|...} 1-based pick. Underscores e.g. _%1s_ are literal (UI highlight). Final strncpy 400. | 0x65e060 | C |
| 364 | `get_key_string` | ui | FUNC | s action_name | s key name text (401-byte buffer) |  | Result: node-owned 0x191-byte string. Name->binding index: A_ATTACK 13,B_ATTACK 14,DEFEND 12,ARROW_SHOOTING 18,INTERACT/RIDE 17,CHARACTER_INTERFACE 35,DOWN_ATTACK 12,USE_HEAL_ITEM 19,COMBO_INTERFACE 36,SWITCH_CHARACTER 24,FORWARD 4,BACKWARD 5,TURN_LEFT 8,TURN_RIGHT 9,SHIFT_LEFT 6,SHIFT_RIGHT 7,SHIFT 3,SHIFT_CAMERA 2,ZOOM_IN 48,ZOOM_OUT 49,ENERGY_ATTACK 15,SWITCH_JOAN 25,SWITCH_JEAN 26,SWITCH_LA_HIRE 27,GAME_OBJECTIVE_KEY 40,RTS_MODE 23. Text=FUN_004cb5b0(DAT_00a5b9f8 control mode, idx): binding entry (+8 table, (idx-1)*0x30) key name for mode 0 (+0x24), 1 (+0x22/+0x28), 3 (+0x2c/+0x23), other modes NULL; strncpy 400. Unknown name: buffer unchanged. | 0x65e0a0 | C |
| 365 | `set_kill_citizen_limit` | objective | FUNC | i limit | i 1 if limit>0 else 0 |  | _DAT_00a50522 = limit (only if >0): citizen-kill limit for kill interface 1 | 0x65e7d0 | C |
| 366 | `start_kill_interface` | ui | FUNC | i mode (optional, default 0) | i 1 |  | if DAT_00a50520==0: kill-counter interface FUN_006dfe10(mode): active=1, +0x12=mode; modes 0/1/4 count existing units | 0x65e810 | H |
| 367 | `end_kill_interface` | ui | FUNC |  | i 1 |  | closes kill-counter interface 1 (FUN_006dff10) | 0x65e850 | H |
| 368 | `set_kill_citizen_limit_2` | objective | FUNC | i limit | i 1 if limit>0 else 0 |  | _DAT_00a50526 = limit (only if >0): kill limit for kill interface 2 | 0x65e880 | C |
| 369 | `start_kill_interface_2` | ui | FUNC | i mode (optional, default 0) | i 1 |  | if DAT_00a50521==0: kill-counter interface 2 FUN_006e00b0(mode) | 0x65e8c0 | H |
| 370 | `end_kill_interface_2` | ui | FUNC |  | i 1 |  | closes kill-counter interface 2 (FUN_006e01d0) | 0x65e900 | H |
| 371 | `remove_item_from_unit_inventory` | item | FUNC | i item_type_id, i unit_recno, i quantity (optional, default 0) | i removed (0 = fail) |  | FUN_00498970(item_id, unit, quantity): removes matching inventory item(s) from unit (+0x1c1) | 0x65cb20 | H |
| 372 | `set_item_can_interact_flag` | item | FUNC | i item_recno, i flag | i 1 ok / 0 bad item |  | item+0x7e = flag ? 0x200 : 0 (interactable bit) | 0x65e930 | C |
| 373 | `set_unit_can_rided_flag` | unit-state | FUNC | i unit_recno, i flag | i 1 ok / 0 bad unit |  | unit+0x3c0 = flag (byte): horse/mount can be ridden | 0x65e9a0 | C |
| 374 | `is_item_on_floor` | query | FUNC | i item_recno | i nonzero if on floor (0 = carried/bad) |  | returns item+0xa2 (floor/world object pointer) | 0x65ea00 | C |
| 375 | `get_item_pos_x` | query | FUNC | i item_recno | f x (0.0 bad item) |  | if item+0xa2 world object: obj+0x12 else item+0x56 | 0x65ea50 | C |
| 376 | `get_item_pos_y` | query | FUNC | i item_recno | f y (0.0 bad item) |  | if item+0xa2 world object: obj+0x16 else item+0x5a | 0x65ead0 | C |
| 377 | `get_item_pos_z` | query | FUNC | i item_recno | f z (0.0 bad item) |  | if item+0xa2 world object: obj+0x1a else item+0x5e | 0x65eb50 | C |
| 378 | `set_minimap_icon_display_enable` | ui | FUNC | s icon_name, i enable | i 1 found / 0 |  | if minimap ready (FUN_0044cf30/FUN_0044cf10): icon table (stride 0xae) entry named icon_name +0x9f = enable | 0x65ec90 | C |
| 379 | `get_minimap_icon_display_enable` | ui | FUNC | s icon_name | i enable (0 = not found) |  | returns icon entry +0x9f (see 378) | 0x65ecf0 | C |
| 380 | `get_charmove_mode` | query | FUNC |  | i control mode |  | returns DAT_00a5b9f8 (control/char-move mode; 4 = RTS) | 0x65ed50 | C |
| 381 | `is_rts_mode` | query | FUNC |  | i 1 if RTS |  | DAT_00a5b9f8 == 4 | 0x65ed70 | C |
| 382 | `set_minimap_rotation` | ui | FUNC | i rotation | i 1 |  | DAT_00a4f3ac = rotation (short) | 0x65eda0 | C |
| 383 | `set_minimap_display_gold_enable` | ui | FUNC | i enable | i 1 |  | minimap object +0x11 = enable (short) via FUN_0065ee00: show gold on minimap | 0x65edd0 | C |
| 384 | `set_critical_attack_enabled` | flags | FUNC | i enable | i 1 |  | DAT_00a4f858 = enable (byte): critical attacks allowed | 0x65ee30 | C |
| 385 | `show_scenario_brief` | ui | FUNC |  | i 1 | W | DAT_00a52934=6; FUN_006f5cc0 switches interface to scenario brief; WAIT predicate FUN_0065eea0 | 0x65ee60 | C |
| 386 | `turn_camera` | camera | FUNC |  | i 1 |  | camera FUN_00436190(1) then (0): starts turn to player heading; loops FUN_004384e0 while DAT_00a48cef==0 && DAT_00a5ba5c (synchronous) | 0x65eee0 | H |
| 387 | `share_team_member` | unit-state | FUNC | i receiver_recno, i donor_recno, i count | i 1 ok / 0 bad unit or no team |  | moves up to count live members of donor's team into receiver's team while receiver team size < FUN_00448080 (leadership cap) | 0x65ef40 | H |
| 388 | `get_item_angle_diff` | query | FUNC | i unit_recno, i item_recno | i angle diff degrees (0 = bad) |  | FUN_00505a00(item pos +0x56): heading difference from unit to item, truncated | 0x65f0f0 | H |
| 389 | `move_and_attach_unit` | unit-state | FUNC | i rider_recno, i vehicle_recno, s slot_kind | i 1 attached / 0 no free slot / -1 bad |  | slot_kind 'M'=mask 1,'F'=mask 2; first free vehicle attach point: teleports rider, links +0x384/+0x388, sets heading and anim state by vehicle class | 0x65f160 | C |
| 390 | `is_cut_scene_finished` | camera | FUNC | i allow_skip (optional, default 0) | i 1 finished / 0 running | W | 0 while cut scene active (FUN_004301c0 or FUN_0042ced0) unless allow_skip and DAT_00a5d3c0 set | 0x65f600 | C |
| 391 | `set_charmove_detect_block_flag` | flags | FUNC | i flag | i 1 |  | DAT_00a5ba64 = flag (byte): char-move block detection | 0x65f670 | C |
| 392 | `set_victory_display_flag` | objective | FUNC | i flag (ignored) | i 1 |  | arg unused: if DAT_00a500d2: clear, DAT_00a500d0=ftol(DAT_00a500cc), FUN_0042f930(0); if no attention msg (FUN_004af410): FUN_006e8c80, clear DAT_00a59171/75/79 | 0x65f6a0 | H |
| 393 | `set_minimap_rectangular_vision_area_enable` | ui | FUNC | i area, i x1, i y1, i x2, i y2 | i 1 |  | minimap vision area[area] (0..9) enabled +0x2f7, shape 0 rect (+0x301), coords at +0x308/+0x330/+0x358/+0x380 | 0x65f740 | C |
| 394 | `set_minimap_circular_vision_area_enable` | ui | FUNC | i area, i x, i y, i radius | i 1 |  | vision area[area] enabled, shape 1 circle: x +0x308, y +0x330, radius +0x448 | 0x65f780 | C |
| 395 | `set_minimap_quadrilateral_vision_area_enable` | ui | FUNC | i area, i x1, i y1, i x2, i y2, i x3, i y3, i x4, i y4 | i 1 |  | vision area[area] enabled, shape 2 quadrilateral: 8 coords at +0x308..+0x420 | 0x65f7c0 | C |
| 396 | `disable_minimap_vision_area` | ui | FUNC | i area | i 1 |  | vision area[area] +0x2f7 = 0 | 0x65f820 | C |
| 397 | `set_territory_nation` | world | FUNC | s territory_name, s nation_name | i 1 |  | territory by name (FUN_004e4b20); +0x38 = nation recno (FUN_004b91a0/FUN_004b67e0), 'Neutral' -> -1; +0x46 = 0 | 0x65f850 | C |
| 398 | `show_letter_interface` | ui | FUNC | i letter_id | i 1 | W | letter interface: FUN_006e0b70 loads letter text (i_letter.cpp), FUN_006e0900 opens; WAIT predicate FUN_0065f950 | 0x65f910 | C |
| 399 | `set_unit_vision_range` | unit-state | FUNC | i unit_recno, i range | i 1 ok / 0 bad unit |  | unit+0x3c1 = range | 0x65f980 | C |
| 400 | `set_unit_team_vision_range` | unit-state | FUNC | i unit_recno, i range | i 1 ok / 0 bad unit |  | unit+0x3c1=range for unit and its team members (+0x33); no team: all units with same +0x31 team id | 0x65f9e0 | C |
| 401 | `set_unit_hide_in_command_interface` | unit-state | FUNC | i unit_recno, i flag | i 1 ok / 0 bad unit |  | unit+0x30 = flag (byte): hide in command interface | 0x65fb50 | C |
| 402 | `set_delay_before_win` | objective | FUNC | i ms | i 1 |  | _DAT_00a596c8 = ms (delay before win screen) | 0x65fba0 | C |
| 403 | `is_objective_menu_opened_in_scenario` | query | FUNC |  | i flag |  | returns (int)DAT_00a591c7 (objective menu opened this scenario) | 0x65fbd0 | C |
| 404 | `is_attention_call_message_shown` | ui | FUNC | s text | i 0 while shown / 1 otherwise |  | text localised as 344; scans attention messages (FUN_004af2d0): returns 0 if a live one has text +0x21 == text, else 1 (inverse of name) | 0x65fbf0 | C |
| 405 | `set_end_cut_scene_type` | camera | FUNC | i type | i 1 |  | DAT_00a5d386 = type (byte): end cut-scene type | 0x65fc90 | C |
| 406 | `show_ending` | objective | FUNC | i ending | i 1 |  | ending 0 or 1 only: FUN_006d19e0 sets +0x234=1,+0x233=ending, starts ending sequence (0 uses 'good1' camera) | 0x65fcc0 | H |
| 407 | `set_prisoner_can_be_attacked_flag` | unit-state | FUNC | i unit_recno, i flag | i 1 ok / 0 bad unit |  | unit+0x358 = flag (byte): prisoner can be attacked | 0x65fd10 | C |

## 9. Verification

- `game.script_vm`: precedence, int/float division and raw-bit assignment, `^`, overflow,
  WAIT + pause_script tick timing, host-blocking WAIT, IF resume without re-test, else-if,
  init once, exit, nested/shared SCRIPT with own variables, argument and RPN evaluation order,
  `&&` short-circuit, keyword/executor mismatch, RNG sequence (seed 0, n = 100: 0, 1, 0, 33),
  bit flags, format_string, compile errors.
- `game.script_corpus`: all 35 `.sce` as main scripts and the 18 unitAI bodies for 3000 ticks
  each, and the 8 campaign scenarios as loaded by the game (main + `_des`) twice — no compile or
  runtime errors, identical trace hashes (6.6 M builtin calls for the 35 files alone).

## 10. Gaps

- Builtin semantics beyond one helper level, and the world side of every non-VM builtin (the
  table is the spec for the world layer). HYPOTHESIS rows: 5, 39, 53, 57, 58, 156, 314.
- Exact frame timing: the original advances the clock by real elapsed ms × game speed per
  frame; the VM uses a fixed 33 ms (identical only in the original's fixed-step mode).
- format_string letters f r ? p q j; GOTO/ID labels, CHOICE, vectors (unused).
- Optional/variable argument handling per builtin (316, 319, 344, 358, 366, 369, 371, 390, cut
  scene ids choose a helper by argument count) is left to the host.
- The parser accepts a few forms the original rejects (unparenthesised binary arguments,
  parenthesised single DIALOG arguments); shipped scripts use neither.
