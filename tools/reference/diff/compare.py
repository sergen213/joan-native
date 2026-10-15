#!/usr/bin/env python3
"""Metrics from work/diff/{orig,port}/<scenario>.json (diffrun.py) and the original-vs-port table.
  python3 tools/reference/diff/compare.py [--self-test]   -> work/diff/report.md + stdout
"""
import json, math, pathlib, sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from diffrun import OUT, JOAN, JEAN, CI01, ARCHER, aname, wrap180  # noqa: E402

ATTACKS = {28, 29, 30, 31, 32, 33, 34}  # ATTACK JATTACKB ENGATK.. DOWNATK
IDLE = {0, 1, 2, 3, 4, 5, 6, 7}
MOVES = {10, 11, 14, 15, 16, 17, 18, 19, 20}  # WALK RUN MOVE_L/R/B/FL/FR/BL/BR (a 100 ms RUN burst counts as a reaction)


def load(backend, sc):
    p = OUT / backend / f"{sc}.json"
    if not p.exists():
        return None
    r = json.loads(p.read_text())
    r["samples"] = [(t, {u["rec"]: u for u in us}) for t, us in r["samples"]]
    r["m"] = {}
    for t, label in r["marks"]:
        r["m"].setdefault(label, t)
    return r


def at(r, t):
    """Last sample at or before t (first sample if none)."""
    best = r["samples"][0]
    for s in r["samples"]:
        if s[0] > t:
            break
        best = s
    return best


def window(r, a, b):
    return [s for s in r["samples"] if a <= s[0] <= b]


MELEE = {28, 29, 30, 31, 32, 33, 34, 73}  # ATTACK JATTACKB ENGATK1-4 DOWNATK H_ATTACK


def hp_drops(r, a, b, recs=None):
    """(t - a, rec, damage, Joan's action, attacker) for every HP decrease between consecutive samples. attacker =
    the opposing units within 250 that were in a melee attack state in this or the previous sample: one name,
    "+"-joined names, or "ranged/none" (the original keeps no attacker log; same heuristic on both sides)."""
    out, prev = [], None
    for t, us in window(r, a, b):
        if prev:
            for rec, u in us.items():
                if (recs is None or rec in recs) and rec in prev and u["hp"] < prev[rec]["hp"]:
                    j = us.get(r["player"])
                    att = sorted({o["name"].split()[0] for s in (prev, us) for o in s.values()
                                  if o["nat"] != u["nat"] and o["nat"] and o["act"] in MELEE
                                  and math.hypot(o["x"] - u["x"], o["y"] - u["y"]) < 250})
                    out.append((t - a, rec, prev[rec]["hp"] - u["hp"], aname(j["act"]) if j else "?", "+".join(att) or "ranged/none"))
        prev = us
    return out


def speed(r, a, b, rec=JOAN):
    """Mean horizontal speed (units/s) of rec between game times a and b."""
    (ta, ua), (tb, ub) = at(r, a), at(r, b)
    if rec not in ua or rec not in ub or tb <= ta:
        return None
    return math.hypot(ub[rec]["x"] - ua[rec]["x"], ub[rec]["y"] - ua[rec]["y"]) / ((tb - ta) / 1000)


def m_start(r):
    us = r["extra"]["units"]
    out = {}
    for rec in (str(JOAN), str(JEAN)):
        u = us[rec]
        for k in ("hp", "mhp", "ep", "mep", "str", "dex", "def", "lead", "lvl", "xp"):
            out[f"{u['name']} {k}"] = u[k]
        out[f"{u['name']} pos"] = f"({u['x']:.0f}, {u['y']:.0f})"
        out[f"{u['name']} yaw"] = round(u["yaw"])
    return out


def m_move(r):
    out = {}
    for lab in ("run", "back", "strafe", "strafe_r", "walk"):
        a, b = r["m"][lab + "_start"], r["m"][lab + "_end"]
        out[f"{lab} speed u/s (0.7..1.7 s)"] = speed(r, a + 700, a + 1700)
        (ta, ua), (tb, ub) = at(r, a), at(r, b)
        out[f"{lab} distance u ({(b - a) / 1000:.0f} s)"] = math.hypot(ub[JOAN]["x"] - ua[JOAN]["x"], ub[JOAN]["y"] - ua[JOAN]["y"])
        out[f"{lab} start latency ms"] = moved_after(r, a, b, JOAN)
        out[f"{lab} actions"] = "/".join(sorted({aname(us[JOAN]["act"]) for t, us in window(r, a + 500, b)}))
    a, b = r["m"]["run_start"], r["m"]["run_end"]
    ds = [math.hypot(us[JEAN]["x"] - us[JOAN]["x"], us[JEAN]["y"] - us[JOAN]["y"]) for t, us in window(r, a, b + 1500) if JEAN in us]
    out["Jean dist to Joan at run start"] = ds[0] if ds else None
    out["Jean reaction latency ms"] = moved_after(r, a, b + 1500, JEAN)
    out["Jean max dist during run"] = max(ds) if ds else None
    out["Jean dist 1.5 s after run"] = ds[-1] if ds else None
    out["Jean peak 1 s speed u/s"] = max((speed(r, t, t + 1000, JEAN) or 0) for t, _ in window(r, a, b + 500))
    out["Jean actions"] = "/".join(sorted({aname(us[JEAN]["act"]) for t, us in window(r, a, b + 1500) if JEAN in us}))
    return out


def moved_after(r, a, b, rec):
    p0 = at(r, a)[1][rec]
    return next((t - a for t, us in window(r, a, b) if math.hypot(us[rec]["x"] - p0["x"], us[rec]["y"] - p0["y"]) > 5), None)


def m_boundary(r):
    a, b = r["m"]["edge_start"], r["m"]["edge_end"]
    w = window(r, a, b)
    hit = next((t for t, us in w if us[JOAN]["y"] > 14000), None)
    j0, j1 = w[0][1][JOAN], w[-1][1][JOAN]
    return {"max y reached": max(us[JOAN]["y"] for t, us in w), "ms to y > 14000": hit - a if hit else None,
            "Joan yaw at start / end": f"{j0['yaw']:.0f} / {j1['yaw']:.0f}",
            "camera yaw at start / end": f"{j0.get('cam', 0):.0f} / {j1.get('cam', 0):.0f}",
            "end position": f"({j1['x']:.0f}, {j1['y']:.0f})",
            "distance moved after reaching the edge": speed(r, hit, b) * (b - hit) / 1000 if hit else None,
            "actions after the edge": "/".join(sorted({aname(us[JOAN]["act"]) for t, us in w if hit and t > hit}))}


def m_turn(r):
    out = {}
    for lab, mick in (("turn_stand", 500), ("turn_run", -500)):
        ya, yb = at(r, r["m"][lab])[1][JOAN]["yaw"], at(r, r["m"][lab + "_end"])[1][JOAN]["yaw"]
        d = wrap180(yb - ya)
        out[f"{lab} yaw change deg ({mick:+d} mickeys)"] = d
        out[f"{lab} deg per mickey"] = d / mick if mick else None
    return out


def by_attacker(drops, who):
    """Damage values of the hits attributed to `who` alone, grouped by sample time (one swing can hit several units
    at once): "4 7x2 13" = swings of 4, 7 (two victims), 13."""
    sw = {}
    for t, rec, dmg, act, att in drops:
        if att == who:
            sw.setdefault((t, dmg), 0)
            sw[(t, dmg)] += 1
    return " ".join(f"{d}x{n}" if n > 1 else str(d) for (t, d), n in sorted(sw.items()))


def m_combo(r):
    a, b = r["m"]["combo_start"], r["m"]["combo_end"] + 1500
    hits = [h for h in hp_drops(r, a, b) if h[1] not in (JOAN, JEAN)]
    taken = hp_drops(r, a, b, {JOAN})
    return {"Joan swings (damage x victims)": by_attacker(hits, "Joan"),
            "Jean swings (damage x victims)": by_attacker(hits, "Jean"),
            "ambiguous hits (Joan+Jean / none)": sum(1 for h in hits if h[4] not in ("Joan", "Jean")),
            "damage taken by Joan per hit": " ".join(str(h[2]) for h in taken),
            "Joan attack actions": " ".join(sorted({aname(us[JOAN]["act"]) for t, us in window(r, a, b) if us[JOAN]["act"] in ATTACKS | {27}}))}


def enemy_swings(r, a, b, near=200):
    n, prev = 0, {}
    for t, us in window(r, a, b):
        j = us[JOAN]
        for rec, u in us.items():
            if u["nat"] != j["nat"] and u["act"] == 28 and prev.get(rec) != 28 and math.hypot(u["x"] - j["x"], u["y"] - j["y"]) < near:
                n += 1
            prev[rec] = u["act"]
    return n


def m_block(r):
    out = {}
    for lab in ("block", "noblock"):
        a, b = r["m"][lab + "_start"], r["m"][lab + "_end"]
        d = hp_drops(r, a, b, {JOAN})
        out[f"{lab}: enemy swings within 200"] = enemy_swings(r, a, b)
        out[f"{lab}: Joan HP lost / hits"] = f"{sum(h[2] for h in d)} / {len(d)}"
        acts = [us[JOAN]["act"] for t, us in window(r, a, b)]
        out[f"{lab}: blocked hits (GUARDR entries)"] = sum(1 for x, y in zip(acts, acts[1:]) if y == 45 and x != 45)
        out[f"{lab}: Joan actions"] = " ".join(sorted({aname(x) for x in acts}))
    return out


def m_archer(r):
    a, b = r["m"]["archer_start"], r["m"]["archer_end"]
    d = hp_drops(r, a, b, {JOAN})
    ranged = [(t + a, rec) for t, rec, dmg, _, att in hp_drops(r, a - 15000, b + 1000) if att == "ranged/none"]
    shots, hits, tgt, prev = [], 0, None, {}
    for t, us in window(r, a - 15000, b):  # the archer's shots: SHTLOAD entry -> nearest live opponent = target
        u = us.get(ARCHER)
        if u and u["act"] == 48 and prev.get("act") != 48:
            opp = [o for o in us.values() if o["nat"] and o["nat"] != u["nat"] and o["hp"] > 0]
            if opp:
                o = min(opp, key=lambda o: math.hypot(o["x"] - u["x"], o["y"] - u["y"]))
                tgt = o["rec"]
                shots.append(math.hypot(o["x"] - u["x"], o["y"] - u["y"]))
        if u and u["act"] in (51, 56) and prev.get("act") not in (51, 56) and tgt:  # SHTOUT/SHTOUTH: hit within 1 s
            hits += any(rec == tgt and t < th <= t + 1000 for th, rec in ranged)
        prev = u or {}
    return {"archer shot distance (mean of SHTLOAD entries)": sum(shots) / len(shots) if shots else None,
            "archer shot distances": " ".join(str(round(s)) for s in shots) or "none",
            "arrow hits / shots": f"{hits} / {len(shots)}",
            "melee hits on Joan in 12 s": len([h for h in d if h[4] != "ranged/none"]),
            "archer actions": " ".join(sorted({aname(us[ARCHER]["act"]) for t, us in window(r, a, b) if ARCHER in us}))}


def m_potion(r):
    a, b = r["m"]["potion_before"], r["m"]["potion_after"]
    ha, hb = at(r, a)[1][JOAN]["hp"], at(r, b)[1][JOAN]["hp"]
    return {"HP before F3": ha, "HP after F3": hb, "healed": hb - ha}


def kill_xp(r, rec, a):
    """XP gains of rec after game time a, each with the unit(s) that died within 300 ms: 'CI01 L2 +100'."""
    out, prev = [], None
    for t, us in r["samples"]:
        if t >= a and prev and rec in us and rec in prev and us[rec]["xp"] > prev[rec]["xp"]:
            dead = [u for t2, s in r["samples"] if abs(t2 - t) <= 300 for u in s.values()
                    if u["hp"] <= 0 and (p := at(r, t2 - 300)[1].get(u["rec"])) and p["hp"] > 0]
            names = sorted({f"{u['name']} L{u['lvl']}" for u in dead})
            out.append(f"{'/'.join(names) or '?'} +{us[rec]['xp'] - prev[rec]['xp']}")
        prev = us
    return ", ".join(out) or "none"


def m_levelup(r):
    a = r["m"]["fight_start"]
    out = {"Joan kill XP": kill_xp(r, JOAN, a), "Jean kill XP": kill_xp(r, JEAN, a)}
    prev = None
    for t, us in r["samples"]:
        j = us[JOAN]
        if prev and j["lvl"] > prev["lvl"]:
            return out | {f"{k} before/after": f"{prev[k]} / {j[k]}" for k in ("lvl", "xp", "mhp", "mep", "str", "dex", "def", "lead")} | \
                   {"game ms into fight": t - a}
        prev = j
    j = r["samples"][-1][1][JOAN]
    return out | {"level-up": f"not reached (lvl {j['lvl']}, xp {j['xp']})"}


def m_aggro(r):
    first = r["samples"][0][1]
    out = {}
    for rec, u0 in first.items():
        if u0["nat"] == first[JOAN]["nat"] or not u0["nat"]:
            continue
        for t, us in r["samples"]:
            u, j = us.get(rec), us[JOAN]
            if not u:
                break
            if math.hypot(u["x"] - u0["x"], u["y"] - u0["y"]) > 30 or u["act"] in ATTACKS | MOVES:
                out[f"{rec} {u0['name']}: Joan dist at reaction"] = round(math.hypot(u["x"] - j["x"], u["y"] - j["y"]))
                break
    return dict(sorted(out.items(), key=lambda kv: int(kv[0].split()[0])))


def m_mstart(r):
    """Mission start inventory: units per nation (per-recno stat agreement: stat_diff), structures, gold,
    unit-type costs (Gold +0x161 / Resource +0x165 of the type table vs the port's UnitClass)."""
    us, out = r["extra"]["units"], {}
    for nat in sorted({u["nat"] for u in us.values()}):
        out[f"units of nation {nat}"] = sum(u["nat"] == nat and u["hp"] > 0 for u in us.values())
    out["units total (alive)"] = sum(u["hp"] > 0 for u in us.values())
    for i in sorted(r["extra"]["items"].values(), key=lambda i: i["rec"]):
        if i["cls"] in (1, 4):
            out[f"structure {i['rec']} {i['name'].replace('_', ' ') or '-'} hp/max/state"] = f"{i['hp']}/{i['mhp']}/{i['state']}"
    out["gold"] = r["extra"]["gold"]
    if "types" in r["extra"]:
        ty = r["extra"]["types"]
        for k in sorted(ty, key=int):
            if ty[k]["code"].upper() in COST_CLASSES:
                out[f"cost {ty[k]['code'].upper()} gold / resource"] = f"{ty[k]['gold']} / {ty[k]['res']}"
    return out


COST_CLASSES = {"FRENCH_LIGHT_INFANTRY", "FRENCH_HEAVY_INFANTRY", "FRENCH_ARCHER", "FRENCH_CROSSBOW_MAN", "FRENCH_POLEMAN",
                "CANNON", "BOMBARD", "TREBUCHET", "FAUCONNEAU", "BATTERRAM", "SIEGE_TOWER", "CROSSBOW",
                "MERCENARY_LIGHT_INFANTRY", "MER_EX_ARCHER"}


def stat_diff(ro, rp):
    """Units whose class / nation / level / stats differ between the sides (same recno), grouped by class."""
    uo, up = ro["extra"]["units"], rp["extra"]["units"]
    bad = {}
    for k in uo.keys() & up.keys():
        a, b = uo[k], up[k]
        f = [f for f in ("cls", "nat", "lvl", "mhp", "mep", "str", "dex", "def", "lead") if a[f] != b[f]]
        if f:
            bad.setdefault(a["cls"], []).append(f"{k} {'/'.join(f)} {'/'.join(str(a[x]) for x in f)} vs {'/'.join(str(b[x]) for x in f)}")
    only = sorted(uo.keys() - up.keys(), key=int), sorted(up.keys() - uo.keys(), key=int)
    return {"units compared": len(uo.keys() & up.keys()), "stat mismatches": sum(map(len, bad.values())),
            "mismatching classes": "; ".join(f"cls {c}: {len(v)}x e.g. {v[0]}" for c, v in sorted(bad.items())) or "none",
            "recnos only in orig / port": f"{len(only[0])} / {len(only[1])}"}


SHOTS = {51, 52, 53, 56}  # SHTOUT SHTOUT2 SHTOUTC SHTOUTH


def drops(trace, col):
    """HP decrements of a structure trace [(t, hp...)] in column col: [(t, amount)]."""
    return [(b[0], a[col] - b[col]) for a, b in zip(trace, trace[1:]) if b[col] < a[col]]


def m_cannon(r):
    from diffrun import M4_CANNONS, M4_CREW
    a, b = r["m"]["trigger"], r["m"]["end"]
    tr = r["extra"]["structures"]
    at_t = at(r, a)[1]
    out = {"crew attached at trigger (veh set)": sum(at_t.get(c, {}).get("veh", 0) != 0 for c in M4_CREW),
           "crew slots": " ".join(f"{c}:{at_t[c]['veh']}/{at_t[c]['slot']}" for c in M4_CREW if c in at_t)}
    for name, col in (("abtower", 1), ("Tourelles gate", 2)):
        d = drops(tr, col)
        out[f"{name} first HP drop ms after trigger"] = d[0][0] - a if d else None
        out[f"{name} HP drop sizes"] = " ".join(sorted({str(x) for _, x in d}, key=int)) or "none"
        out[f"{name} HP at end"] = tr[-1][col]
        z = next((t for t, *h in tr if h[col - 1] <= 0), None)
        out[f"{name} ms to 0 HP"] = z - a if z else None
    w = window(r, a, b)
    shots = 0
    for c in M4_CANNONS:
        prev = None
        for t, us in w:
            act = us[c]["act"] if c in us else None
            shots += act in SHOTS and prev not in SHOTS
            prev = act
    out["cannon shots (SHTOUT* entries, 4 cannons)"] = shots
    out["cannon actions"] = " ".join(sorted({aname(us[c]["act"]) for t, us in w for c in M4_CANNONS if c in us}))
    out["crew actions"] = " ".join(sorted({aname(us[c]["act"]) for t, us in w for c in M4_CREW if c in us}))
    out["cannon yaw at end"] = " ".join(str(round(w[-1][1][c]["yaw"])) for c in M4_CANNONS if c in w[-1][1])
    return out


def m_horse(r):
    from diffrun import M4_HORSE, M4_FOOT
    m, J, H = r["m"], r["player"], M4_HORSE
    out = {}
    w = window(r, m["mount"], m["ride"])
    out["mount actions"] = " ".join(dict.fromkeys(aname(us[J]["act"]) for t, us in w if J in us))
    seat = next((t for t, us in w if J in us and us[J]["act"] == 67 and us[J]["mnt"]), None)
    out["mount: ms to H_IDLE"] = seat - m["mount"] if seat else None
    out["mounted horse"] = w[-1][1][J]["mnt"] if J in w[-1][1] else None
    a = m["ride"]
    run = next((t for t, us in window(r, a, m["ride_end"]) if H in us and us[H]["act"] == 11), None)
    out["ride: ms to run gear (horse RUN)"] = run - a if run else None
    out["ride: horse speed u/s 0.2..0.9 s"] = speed(r, a + 200, a + 900, H)
    out["ride: horse speed u/s 1.5..3.5 s"] = speed(r, a + 1500, a + 3500, H)
    out["ride: distance 4 s"] = speed(r, a, m["ride_end"], H) and speed(r, a, m["ride_end"], H) * (m["ride_end"] - a) / 1000
    out["ride: rider actions"] = " ".join(dict.fromkeys(aname(us[J]["act"]) for t, us in window(r, a, m["ride_end"] + 1500) if J in us))
    (t0, u0), (t1, u1) = at(r, m["hturn_m"]), at(r, m["hturn_end"])
    if H in u0 and H in u1:
        out["turn: horse yaw change deg (-300 mickeys while moving)"] = round((u1[H]["yaw"] - u0[H]["yaw"] + 180) % 360 - 180, 1)
    hits = [d for d in hp_drops(r, m["hatk"], m["hatk_end"] + 1000, {M4_FOOT})]
    out["attack: H_ATTACK entries"] = sum(1 for (ta, ua), (tb, ub) in zip(window(r, m["hatk"], m["hatk_end"]), window(r, m["hatk"], m["hatk_end"])[1:])
                                          if J in ub and ub[J]["act"] == 73 and ua.get(J, {}).get("act") != 73)
    out["attack: damage to the infantryman per hit"] = " ".join(str(h[2]) for h in hits) or "none"
    return out


def m_m4aggro(r):
    a, b = r["m"]["walk"], r["m"]["end"]
    J = r["player"]
    first = at(r, a)[1]
    out = {}
    for rec, u0 in sorted(first.items()):
        if u0["nat"] == first[J]["nat"] or not u0["nat"] or u0["hp"] <= 0:
            continue
        for t, us in window(r, a, b):
            u, j = us.get(rec), us[J]
            if not u:
                break
            if math.hypot(u["x"] - u0["x"], u["y"] - u0["y"]) > 30 or u["act"] in ATTACKS | MOVES | {48}:
                out[f"{rec} {u0['name']}: Joan dist at reaction"] = round(math.hypot(u["x"] - j["x"], u["y"] - j["y"]))
                break
    hits = hp_drops(r, a, b, {J})
    out["hits on Joan"] = len(hits)
    out["damage per hit on Joan (sorted)"] = " ".join(str(h[2]) for h in sorted(hits, key=lambda h: h[2]))
    out["reacting English units"] = sum(1 for k in out if k.endswith("reaction"))
    return out


METRICS = {"start": m_start, "move": m_move, "boundary": m_boundary, "turn": m_turn, "combo": m_combo, "block": m_block,
           "archer": m_archer, "potion": m_potion, "levelup": m_levelup, "aggro": m_aggro, "m4start": m_mstart,
           "cannon": m_cannon, "horse": m_horse, "m4aggro": m_m4aggro, "m5start": m_mstart}
PAIRED = {"m4start": stat_diff, "m5start": stat_diff}  # metrics over both traces at once (one column)


def fmt(v):
    return f"{v:.2f}" if isinstance(v, float) else "—" if v is None else str(v)


def verdict(o, p, key=""):
    """Numbers: within 10 % (min 1); latencies ("ms" in the metric): within 70 ms, i.e. one or two original
    frames + the 10 ms sampling period of the live-memory reader; positions "(x, y)": within 50 units."""
    if isinstance(o, (int, float)) and isinstance(p, (int, float)):
        tol = 70.0 if " ms" in key else max(abs(o) * 0.10, 1.0)
        return "match" if abs(o - p) <= tol else "MISMATCH"
    xy = [tuple(map(float, s[1:-1].split(","))) for s in (o, p) if isinstance(s, str) and s.startswith("(") and s.count(",") == 1]
    if len(xy) == 2:
        return "match" if math.dist(*xy) <= 50 else "MISMATCH"
    return "match" if o == p else "differs"


def report():
    lines = ["| scenario | metric | original | port | verdict |", "|---|---|---|---|---|"]
    for sc, f in METRICS.items():
        ro, rp = load("orig", sc), load("port", sc)
        if not ro and not rp:
            continue
        mo, mp = (f(ro) if ro else {}), (f(rp) if rp else {})
        for k in list(dict.fromkeys(list(mo) + list(mp))):
            o, p = mo.get(k), mp.get(k)
            lines.append(f"| {sc} | {k} | {fmt(o)} | {fmt(p)} | {verdict(o, p, k) if ro and rp else 'n/a'} |")
    for sc, f in PAIRED.items():
        ro, rp = load("orig", sc), load("port", sc)
        if ro and rp:
            for k, v in f(ro, rp).items():
                lines.append(f"| {sc} | {k} | {v} | (paired) | — |")
    text = "\n".join(lines) + "\n"
    (OUT / "report.md").write_text(text)
    print(text)


def self_test():
    j = dict(name="Joan of Arc", nat=2, act=28)
    r = {"player": JOAN, "samples": [(0, {JOAN: dict(j, hp=10, x=0, y=0), 5: dict(name="CI01", nat=1, act=1, hp=70, x=50, y=0)}),
                                     (100, {JOAN: dict(j, hp=7, x=30, y=40), 5: dict(name="CI01", nat=1, act=1, hp=66, x=50, y=0)})]}
    assert speed(r, 0, 100) == 500.0
    d = hp_drops(r, 0, 100)
    assert [(h[1], h[2], h[4]) for h in d] == [(JOAN, 3, "ranged/none"), (5, 4, "Joan")]
    assert by_attacker(d, "Joan") == "4"
    assert verdict(100, 109) == "match" and verdict(100, 120) == "MISMATCH" and verdict("a", "a") == "match"
    assert verdict("(13528, 13021)", "(13527, 13022)") == "match" and verdict("(0, 0)", "(60, 0)") == "MISMATCH"
    print("ok")


if __name__ == "__main__":
    self_test() if "--self-test" in sys.argv else report()
