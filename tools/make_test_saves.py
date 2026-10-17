#!/usr/bin/env python3
"""Generate Load-menu test saves: one per campaign mission at its start plus one per key place.

Phase 1, per mission: `joan-native --headless --scenario <MAP> --seed 1` with an autoplay script that saves right
after the opening (`cmd Save 0 <file> <description>`) as Test_<MAP>_00_start.jsav. S9L1 only exists as the
continuation of S5L1's world, so its start comes from S5L1 forced won (`win`) + ContinueCampaign.
Phase 2, per place: `--load` that start save, let the opening scripts run, `warp <place>` (hero to the place centre /
facility point), settle, save as Test_<MAP>_<NN>_<slug>.jsav, description "TEST <MAP> - <place>".
Every save is checked with `--headless --load <file> --frames 60`: right mission, hero alive where she was saved.
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

# Default start: --scenario <MAP>, saved at frame 30 (headless: one tick per frame); place saves warp 120 frames
# after loading it (the opening scripts have run, their dialogs auto-advance headless) and save 30 frames later.
START = (None, "", 30, 120)
# S9L1 (scenario 6) continues S5L1's world: S5L1 won at once (`win`, its objectives brief closed with F12),
# ContinueCampaign, saved once S9L1 plays (loading screen done) and before its first script brief (~130 ticks later),
# which the place saves must also beat: they warp 10 frames after loading.
# ponytail: frame numbers, fine while headless runs are deterministic (fixed dt, --seed 1).
CONTINUE = {"S9L1": ("S5L1", "30 win\n450 tap F12\n600 cmd ContinueCampaign\n", 1200, 10)}
SETTLE = 30
PLAYER = re.compile(r"player \d+ \(\S+\) at (-?\d+),(-?\d+) yaw -?\d+ hp (-?\d+)")
MISSION = re.compile(r"^autoplay \d+: state (\S+).* mission (\d+) ", re.M)


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


def last_player(log):
    m = PLAYER.findall(log)
    return tuple(map(int, m[-1])) if m else None


def names(stem, nn, place):
    label = "start" if place is None else pretty(stem, place)
    slug = re.sub(r"[^A-Za-z0-9]+", "_", label).strip("_")
    return f"Test_{stem}_{nn:02d}_{slug}.jsav", f"TEST {stem} - {label}"[:63]  # save_description limit


def make(a, stem, nn, place):
    name, desc = names(stem, nn, place)
    src, prelude, start_frame, warp_frame = CONTINUE.get(stem, START)
    base = [a.binary, "--headless", "--game-dir", a.game_dir]
    if place is None:
        cmd, script, save_at = base + ["--scenario", src or stem, "--seed", "1"], prelude, start_frame
    else:
        start = Path(a.out, names(stem, 0, None)[0])
        if not start.exists():
            return name, desc, "FAIL generate: no start save"
        cmd, save_at = base + ["--load", str(start)], warp_frame + SETTLE
        script = f"{warp_frame} warp {place}\n{warp_frame + 1} log warped\n"
    script += f"{save_at - 1} log saving\n{save_at} cmd Save 0 {name} {desc}\n{save_at + 2} quit\n"
    with tempfile.TemporaryDirectory(prefix="joa-testsave-") as xdg:
        rc, log = run(cmd, dict(os.environ, XDG_DATA_HOME=xdg), script)  # native_save_dir() = $XDG_DATA_HOME/joan-native/saves
        made = Path(xdg, "joan-native", "saves", name)
        bad = [l for l in log.splitlines() if l.startswith("joan-native: autoplay:") or l.startswith("joan-native: save:")]
        if rc != 0 or bad or not made.exists():
            return name, desc, f"FAIL generate: {bad[0] if bad else f'exit {rc}, no save'}"
        saved = last_player(log)
        for f in made.parent.glob(made.stem + ".*"):  # .jsav (+ .bmp thumbnail if any)
            if f.suffix in (".jsav", ".bmp"):
                shutil.move(str(f), Path(a.out, f.name))
    rc, log = run(base + ["--load", str(Path(a.out, name)), "--frames", "60"], dict(os.environ), "2 log verify\n")
    m = MISSION.search(log)
    p = last_player(log)
    if rc != 0 or not m or int(m.group(2)) != MISSIONS[stem][0] or m.group(1) != "playing":
        return name, desc, f"FAIL load: exit {rc}, {m.group(0).strip() if m else 'no mission log'}"
    if not p or p[2] <= 0:
        return name, desc, "FAIL load: no living hero"
    if saved and math.dist(saved[:2], p[:2]) > 400:
        return name, desc, f"FAIL load: hero at {p[:2]}, saved at {saved[:2]}"
    return name, desc, f"ok, hero at {p[0]},{p[1]} hp {p[2]}"


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
