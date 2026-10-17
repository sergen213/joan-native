#!/usr/bin/env python3
"""Generate Load-menu test saves: one per campaign mission at its start plus one per key place.

Phase 1, per mission: `joan-native --headless --scenario <MAP> --seed 1`, saved (`cmd Save 0 <file> <description>`)
as Test_<MAP>_00_start.jsav once the opening cut scene / brief is over and the player has control. S9L1 only exists
as the continuation of S5L1's world, so its start comes from S5L1 forced won (`win`) + ContinueCampaign.
Phase 2, per place: `--load` that start save, `spawn <place>` (hero and party to the place centre / facility point,
or the nearest point on the way back toward the start beyond every hostile's search range: then described "near"),
settle until controllable, save as Test_<MAP>_<NN>_<slug>.jsav, description "TEST <MAP> - [near ]<place>".
Every save is checked with `--headless --load <file>`: right mission, controllable, hero within RADIUS of the spawn
point (start: of where she was saved) and still alive after IDLE idle frames.
Saves are written under a private $XDG_DATA_HOME and moved to --out (existing Test_<MAP>_* there are replaced).

    tools/make_test_saves.py [--binary B] [--game-dir G] [--out DIR] [--missions S1L1,S3L1] [--jobs N]
"""
import argparse
import concurrent.futures
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Map stem -> (campaign mission number, places). Names as in map/<scenario>/*.pla (PLACE_NAME, or a facility name).
MISSIONS = {
    "S1L1": (1, ["StartPointPlace", "Healing Place", "Strength Challenge Place 1", "BreakBoxPlace", "AttackKeyPlace",
                 "Messenger Detect", "Tactic Event 2 Ambush Zone", "Tactic Event 3 Bridge", "Acher Fire Zone",
                 "Tactic Event 6 Ambush Zone", "Camp 1 Place", "Castle Bridge Place", "Castle Gate Place"]),
    "S3L1": (2, ["Introduce Orlean Place", "The Building of Orlean", "Barrack", "Shop", "Defend Gate Place",
                 "Castle Wall Place L", "Left Battle Place", "Middle Battle Place", "Right Battle Place 1",
                 "Reinforcement Place", "John Retreat Place", "S3L1_2_Town_1_Place", "S3L1_2_Town_2_Place",
                 "S3L1_2_Block_Place"]),
    "S2L1": (3, ["HorseTutorial", "Dock", "English Camp", "Camp Main Defend Place", "C Defend Place",
                 "H Defend Place", "La Hire Spwan", "PrisonDetect", "Training Camp", "Large Camp Center Place",
                 "Large English Camp", "Trap Place", "Backdoor Place", "I Defend Place"]),
    "S4L1": (4, ["Loches", "Loches1", "Loches2", "Loches3", "Tours", "Small_English_Troops",
                 "Large_English_Camp_E1_R1", "Large_English_Camp_E3", "Fierbois", "Church", "Augustin", "Siege",
                 "Metz Prison", "Tourelles", "Tourelles Inside", "Boulevard"]),
    "S5L1": (5, ["arriveOrlean", "OrleanBarrack", "Shops", "meetOrlean", "Town_Checy", "Town_Chateauneut8",
                 "meetSuffolk", "reachLord", "crossBridge", "havePoleman", "reachJargeauGate", "JargeauBarrack",
                 "Castle_Jargeau", "castleJargeau2"]),
    "S9L1": (6, ["Town_Blois", "Settlement_Bourges", "Town_Loches", "Settle_Montargis", "Town_Chateaudun",
                 "Town_St_Aignar", "Town_Saint", "Town_Rouvray", "Settle_Fountainbleu", "Town_Patay", "Town_Javille",
                 "Town_Tours", "Castle_Meung", "Castle_Beaugency", "getReinforcement", "finalCamp"]),
    "S7L1": (7, ["eng camp", "eng camp 2", "eng camp 3", "food msg", "priest cut scene", "StPierre Outside", "St Pierre",
                 "StPierre barrack", "french garrison", "eng camp 4", "eng camp 5", "eng camp 6", "workshop1 place",
                 "la charite outside", "invasion center", "St La Charite"]),
    "S6L1": (8, ["S6L1_1_Bridge_Place", "S6L1_1_Find_Enemy_Place1", "S6L1_2_See_Auxerres_Place",
                 "S6L1_2_Open_Gate_Place", "S6L1_2_Iron_Workshop_Place", "S6L1_3_Castle_Gate_Place",
                 "S6L1_3_Barrack_Place", "S6L1_3_After_Troyes_Place", "S6L1_4_Approach_Camp_Place", "S6L1_4_Camp_Place",
                 "S6L1_5_Town_Place", "S6L1_6_Camp_Place", "S6L1_7_Camp_Place", "S6L1_7_Barrack_Place",
                 "S6L1_8_Approach_Reims_Place", "S6L1_8_Gate_Place"]),
}

# Every save is taken in a controllable window (settle): playing, no "(paused)" / "(loading screen)", no brief, no
# cut scene / input lock, hero alive for STABLE frames from the window start, at MISSIONS' mission. Headless runs are
# deterministic (fixed dt, --seed 1), so blockers that never clear are answered and the run repeated: a script brief
# (show_scenario_brief waits for it) is closed with F12, a cut scene skipped with Escape.
# Default start: --scenario <MAP>, settled from frame 5; place saves `spawn` 10 frames after loading the start save.
# S9L1 (scenario 6) continues S5L1's world: S5L1 won at once (`win`, its objectives brief closed with F12), then
# ContinueCampaign; settled once S9L1 plays.
START = (None, "", 5)
CONTINUE = {"S9L1": ("S5L1", "30 win\n450 tap F12\n600 cmd ContinueCampaign\n", 650)}
SPAWN_FRAME = 10
STABLE, LEAD, HORIZON, STEP = 150, 30, 3000, 5   # controllable frames needed / saved this far into them / probe frames
MARGINS = (500, 1000, 1500)  # spawn: beyond hostile engage range + margin, widened while the hero is attacked
RADIUS = 200      # loaded hero within this of the spawn point
IDLE = 600        # verify: hero still alive after this many idle frames from load
LINE = re.compile(r"^autoplay (\d+): state (\S+)((?: \([a-z ]+\))*) mission (\d+) tick \d+ menu \d+ brief (\d+) .*\n"
                  r"  player \d+ \(\S+\) at (-?\d+),(-?\d+) yaw -?\d+ hp (-?\d+) .*? lock (\d); nearest hostile (?:.*? dist (\d+))?", re.M)
SPAWN = re.compile(r"^autoplay: spawn .* at (-?\d+),(-?\d+) \(target (-?\d+),(-?\d+), party (\d+)\)", re.M)


def pretty(stem, place):  # "S6L1_3_Castle_Gate_Place" -> "3 Castle Gate"
    p = re.sub(rf"^{stem}_(\d+)_", r"\1 ", place, flags=re.I)
    p = re.sub(r"[_ ]Place$", "", p, flags=re.I) or place
    return p.replace("_", " ").strip()


def run(cmd, env, script):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(script)
    try:
        r = subprocess.run(cmd + ["--autoplay-script", f.name], env=env, capture_output=True, text=True, timeout=600)
        return r.returncode, r.stdout + r.stderr
    finally:
        os.unlink(f.name)


def records(log):  # (frame, controllable, mission, (x, y), hp, nearest hostile dist, brief open)
    return [(int(m[1]), m[2] == "playing" and not m[3] and m[5] == "0" and m[9] == "0", int(m[4]),
             (int(m[6]), int(m[7])), int(m[8]), int(m[10]) if m[10] else None, m[5] != "0") for m in LINE.finditer(log)]


def settle(cmd, env, script, start, mission):
    """-> (script with blocker answers, save frame, log) or (None, reason, log)."""
    for _ in range(12):
        probe = script + "".join(f"{f} log probe\n" for f in range(start, start + HORIZON, STEP))
        rc, log = run(cmd, env, probe)
        rs = [r for r in records(log) if r[0] >= start]
        if rc != 0 or not rs:
            return None, f"exit {rc}, no probe log", log
        good = [r[1] and r[2] == mission and r[4] > 0 for r in rs]
        need = STABLE // STEP
        for i in range(len(rs) - need):
            if all(good[i:i + need + 1]):
                return script, rs[i][0] + LEAD, log
        if rs[-1][4] <= 0:
            return None, f"hero died at {rs[-1][3]}", log
        i = len(rs) - 1  # answer the blocker that lasts to the horizon, from where it began
        while i > 0 and not good[i - 1] and rs[i - 1][6] == rs[i][6]:
            i -= 1
        script += f"{rs[i][0] + 1} tap {'F12' if rs[i][6] else 'Escape'}\n"
    return None, "never controllable", log


def names(stem, nn, place):
    label = "start" if place is None else pretty(stem, place)
    slug = re.sub(r"[^A-Za-z0-9]+", "_", label).strip("_")
    return f"Test_{stem}_{nn:02d}_{slug}.jsav", label


def make_one(a, stem, nn, place, margin):
    name, label = names(stem, nn, place)
    desc = f"TEST {stem} - {label}"[:63]  # save_description limit
    mission = MISSIONS[stem][0]
    src, prelude, settle_from = CONTINUE.get(stem, START)
    base = [a.binary, "--headless", "--game-dir", a.game_dir]
    spawn = None
    with tempfile.TemporaryDirectory(prefix="joa-testsave-") as xdg:
        env = dict(os.environ, XDG_DATA_HOME=xdg)  # native_save_dir() = $XDG_DATA_HOME/joan-native/saves
        if place is None:
            cmd, script = base + ["--scenario", src or stem, "--seed", "1"], prelude
        else:
            start = Path(a.out, names(stem, 0, None)[0])
            if not start.exists():
                return name, desc, "FAIL generate: no start save"
            cmd, script, settle_from = base + ["--load", str(start)], f"{SPAWN_FRAME} spawn {place}@{margin}\n", SPAWN_FRAME + 1
        script, save_at, log = settle(cmd, env, script, settle_from, mission)
        if script is None:
            return name, desc, f"FAIL settle: {save_at}"
        if place is not None:
            s = SPAWN.search(log)
            if not s:
                return name, desc, "FAIL generate: no spawn log"
            spawn, target = (int(s[1]), int(s[2])), (int(s[3]), int(s[4]))
            if math.dist(spawn, target) > RADIUS:  # moved out of hostile reach: say so
                desc = f"TEST {stem} - near {label}"[:63]
        script += f"{save_at - 1} log saving\n{save_at} cmd Save 0 {name} {desc}\n{save_at + 2} quit\n"
        rc, log = run(cmd, env, script)
        made = Path(xdg, "joan-native", "saves", name)
        bad = [l for l in log.splitlines() if l.startswith("joan-native: autoplay:") or l.startswith("joan-native: save:")]
        if rc != 0 or bad or not made.exists():
            return name, desc, f"FAIL generate: {bad[0] if bad else f'exit {rc}, no save'}"
        saved = [r for r in records(log) if r[0] == save_at - 1]
        for f in made.parent.glob(made.stem + ".*"):  # .jsav (+ .bmp thumbnail if any)
            if f.suffix in (".jsav", ".bmp"):
                shutil.move(str(f), Path(a.out, f.name))
    # Verify: right mission, controllable at once, hero where saved (and at the spawn point), alive after IDLE frames.
    rc, log = run(base + ["--load", str(Path(a.out, name)), "--frames", str(IDLE + 2)], dict(os.environ),
                  f"2 log verify\n{IDLE} log idle\n")
    rs = records(log)
    if rc != 0 or len(rs) != 2 or rs[0][2] != mission:
        return name, desc, f"FAIL load: exit {rc}, {rs[0] if rs else 'no log'}"
    (_, ok, _, at, hp, foe, _), hp_idle = rs[0], rs[1][4]
    want = spawn or (saved[0][3] if saved else None)
    if not ok or hp <= 0:
        return name, desc, f"FAIL load: not controllable / dead (hp {hp})"
    if want is None or math.dist(at, want) > RADIUS:
        return name, desc, f"FAIL load: hero at {at}, spawn point {want}"
    if hp_idle * 2 < hp:  # attacked at once: not out of hostile reach
        return name, desc, f"FAIL load: hero hp {hp}->{hp_idle} after {IDLE} idle frames at {at}"
    where = f"{math.dist(at, target):.0f} from target {target[0]},{target[1]}, margin {margin}, party {s[5]}" if spawn else "start"
    return name, desc, f"ok, hero at {at[0]},{at[1]} ({where}), hp {hp}->{hp_idle}, foe {foe}"


def make(a, stem, nn, place):  # a spawn attacked at once is retried further out of reach
    for margin in MARGINS:
        r = make_one(a, stem, nn, place, margin)
        if place is None or "idle frames" not in r[2]:
            break
    return r


def main():
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=str(REPO / "build/src/engine/joan-native"))
    ap.add_argument("--game-dir", default=str(data / "joan-native/game/current"))
    ap.add_argument("--out", default=str(data / "joan-native/saves"), help="default: the native save dir")
    ap.add_argument("--missions", help="comma-separated map stems (default: all of " + ",".join(MISSIONS) + ")")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    a = ap.parse_args()
    a.out = str(Path(a.out).resolve())
    stems = [s.strip().upper() for s in a.missions.split(",")] if a.missions else list(MISSIONS)
    if bad := [s for s in stems if s not in MISSIONS]:
        sys.exit(f"unknown mission(s) {bad}; known: {list(MISSIONS)}")
    Path(a.out).mkdir(parents=True, exist_ok=True)
    for s in stems:  # regenerate cleanly (a renamed place must not leave its old save behind)
        for f in Path(a.out).glob(f"Test_{s}_*"):
            f.unlink()
    failed = total = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:  # each job runs joan-native processes
        for jobs in ([(s, 0, None) for s in stems],
                     [(s, nn, p) for s in stems for nn, p in enumerate(MISSIONS[s][1], 1)]):
            for name, desc, status in pool.map(lambda j: make(a, *j), jobs):
                failed += status.startswith("FAIL")
                print(f"{name:50} {desc:42} {status}", flush=True)
            total += len(jobs)
    print(f"{total - failed}/{total} saves in {a.out}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
