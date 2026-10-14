#!/usr/bin/env python3
"""Differential gameplay harness: the same closed-loop scenarios on the ORIGINAL joan.exe (Wine, live memory
reads of /proc/<pid>/mem) and on the port (joa-port-trace lockstep). docs/reverse-engineering/DIFFERENTIAL_VERIFICATION.md

  python3 tools/reference/diff/diffrun.py orig|port [scenario ...]     traces -> work/diff/<backend>/<scenario>.json
  python3 tools/reference/diff/compare.py                              table  -> work/diff/report.md

Original side: own Wine prefix + game copy under work/diff/wine (reflinks), private Xvfb :92, mission 1 Normal from
a fresh campaign; every scenario starts from the F5 quick save taken right after the opening cut scene. The harness
process is made a child subreaper so the double-forked joan.exe becomes its child (Yama ptrace_scope 1 then allows
reading /proc/<pid>/mem).
"""
import ctypes, json, math, os, pathlib, struct, subprocess, sys, threading, time

ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT = ROOT / "work/diff"
ACTIONS = ("DEFAULT IDLE IDLE2 IDLE3 IDLE_BAT IDLEUP IDLEDN IDLE_T JATTBI TIPTOE WALK RUN T_LEFT T_RIGHT MOVE_L MOVE_R "
           "MOVE_B MOVE_FL MOVE_FR MOVE_BL MOVE_BR JUMP_B JUMP_R JUMP_L DOWN JATTBD UP CHAIN ATTACK JATTACKB ENGATK "
           "ENGATK2 ENGATK3 ENGATK4 DOWNATK ATTACKED ATTKED2 ATTKEDR BATTKED LATTKED RATTKED DNATKED DIE GUARD1 GUARD2 "
           "GUARDR BACK CLIMB SHTLOAD SHTLOADC SHTLOAD2 SHTOUT SHTOUT2 SHTOUTC SHTIDLE SHTIDLEC SHTOUTH SHTIDLEH THROW "
           "THROWN STANDUP DIZZY STOP STOP_B KNOCKDN KNOCKDN2 KNOCKDN3").split()
FIELDS = "rec name nat cls hp mhp ep mep str dex def lead lvl xp act x y z yaw mor arm ldr".split()
# key name -> (xdotool key / mouse button, port InputFrame action id; DEFAULTK.csv rows)
KEYS = {"w": ("w", 4), "s": ("s", 5), "a": ("a", 6), "d": ("d", 7), "shift": ("shift", 10), "q": ("q", 12),
        "lmb": (1, 13), "rmb": (3, 14), "c": ("c", 15), "space": ("space", 16), "F3": ("F3", 19), "F4": ("F4", 20)}
MOUSE_DEG = 0.18  # deg per mickey at default sensitivity: ((0+1)*0.7+0.2)*0.2 (0x431790)


def aname(a):
    return ACTIONS[a] if 0 <= a < len(ACTIONS) else str(a)


def wrap180(a):
    return (a + 180.0) % 360.0 - 180.0


class Backend:
    """Common API. Times are game ms; units() -> {rec: dict(FIELDS)} for units within 4000 of the player."""
    name = "?"

    def __init__(self):
        self.samples, self.marks, self.held = [], [], set()

    def mark(self, label):
        self.marks.append((self.now(), label))

    def player(self):
        return self.units()[self.player_rec]

    def target(self, rec):
        return self.units().get(rec) or self.all_units().get(rec)

    # closed-loop helpers (identical on both sides)
    def bearing(self, rec):
        p, t = self.player(), self.target(rec)
        return math.degrees(math.atan2(t["y"] - p["y"], t["x"] - p["x"]))

    def dist(self, rec):
        p, t = self.player(), self.target(rec)
        return math.hypot(t["x"] - p["x"], t["y"] - p["y"]) if t else 1e9

    def face(self, deg, tol=6.0):
        d = wrap180(deg - self.player()["yaw"])
        if abs(d) > tol:
            self.mouse(-d / MOUSE_DEG)

    def goto(self, rec, stop, timeout_ms=30000, walk=False, pin=False):
        """Follow the port navmesh path (nav_path; the same waypoints on both sides) toward rec (a unit recno or an
        (x, y) point), then close in directly. pin: test hook keeping Joan's HP >= 40 (combat scenarios that do not
        measure damage taken)."""
        if isinstance(rec, tuple):
            pt = rec
            self.target = lambda r: {"x": pt[0], "y": pt[1]} if r == pt else Backend.target(self, r)
        t0 = self.now()
        p, t = self.player(), self.target(rec)
        wps = nav_path((p["x"], p["y"]), (t["x"], t["y"]))[1:-1]
        if walk:
            self.down("shift")
        self.down("w")
        while self.dist(rec) > stop and self.now() - t0 < timeout_ms:
            p = self.player()
            while wps and math.hypot(wps[0][0] - p["x"], wps[0][1] - p["y"]) < 80:
                wps.pop(0)
            if wps:
                self.face(math.degrees(math.atan2(wps[0][1] - p["y"], wps[0][0] - p["x"])), tol=8)
            else:
                self.face(self.bearing(rec), tol=8)
            if pin and p["hp"] < 40:
                self.set_hp(self.player_rec, 60)
            self.sleep(150)
        self.up("w")
        if walk:
            self.up("shift")

    def hold(self, k, ms):
        self.down(k); self.sleep(ms); self.up(k)

    def tap(self, k):
        self.down(k); self.sleep(100); self.up(k)


# ------------------------------------------------------------------------------------------- port
class Port(Backend):
    name = "port"
    exe = ROOT / os.environ.get("JOA_PORT_BUILD", "build-DiffVerify") / "tools/reference/diff/joa-port-trace"

    def __init__(self, seed=1):
        super().__init__()
        self.seed = seed
        self.p = subprocess.Popen([str(self.exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  env=dict(os.environ, JOA_GAME_DIR=str(ROOT)))
        self.t, self.cur, self.pressed, self.chunks = 0, {}, set(), []

    def cmd(self, line):
        self.p.stdin.write(line + "\n"); self.p.stdin.flush()
        out = []
        for l in self.p.stdout:
            if l.strip() == "end":
                return out
            out.append(l.split())
        raise RuntimeError("joa-port-trace exited")

    @staticmethod
    def parse(tok):
        v = [int(x) for x in tok[1:2]] + [tok[2].replace("_", " ")] + [int(x) for x in tok[3:16]] + [float(x) for x in tok[16:20]] + [int(x) for x in tok[20:23]]
        return dict(zip(FIELDS, v))

    def reset(self):
        r = self.cmd(f"start 1 1 {self.seed}")
        self.player_rec = int(r[0][1]); self.t = int(r[0][3])
        self.cur = {u["rec"]: u for u in map(self.parse, self.cmd("units"))}
        self.samples, self.marks, self.held = [(self.t, self.cur)], [], set()
        self.pressed, self.chunks = set(), []

    def now(self):
        return self.t

    def units(self):
        return self.cur

    def all_units(self):
        return {u["rec"]: u for u in map(self.parse, self.cmd("units"))}

    def down(self, k):
        self.held.add(KEYS[k][1]); self.pressed.add(KEYS[k][1])

    def up(self, k):
        self.held.discard(KEYS[k][1])

    def mouse(self, dx):
        # as joa_ref.rel: ceil(|dx|/40) chunks, one per frame (0x431790 truncates ftol(dx*0.18) per frame)
        n = max(1, math.ceil(abs(dx) / 40))
        self.chunks += [round(dx / n)] * n

    def sleep(self, ms):
        n = max(1, round(ms / (1000 / 30)))
        lst = lambda s: ",".join(map(str, sorted(s))) or "-"
        while n > 0:
            k = 1 if self.chunks else n
            mdx = self.chunks.pop(0) if self.chunks else 0
            snap, cam = None, 0.0

            def flush():
                if snap is not None:
                    if self.player_rec in snap:
                        snap[self.player_rec]["cam"] = cam % 360.0
                    self.samples.append((self.t, snap))
            for tok in self.cmd(f"step {k} {lst(self.held)} {lst(self.pressed)} {mdx}"):
                if tok[0] == "T":
                    flush()
                    self.t, snap, cam = int(tok[1]), {}, float(tok[2])
                else:
                    u = self.parse(tok); snap[u["rec"]] = u
            flush(); self.cur = snap
            self.pressed, n = set(), n - k

    def set_hp(self, rec, hp):
        self.cmd(f"set {rec} hp {hp}")

    def set_xp(self, rec, xp):
        self.cmd(f"set {rec} xp {xp}")

    def close(self):
        self.p.stdin.close(); self.p.wait()


_nav = None


def nav_path(frm, to):
    """Waypoints [(x, y, z)] of the port's mission-1 navmesh path (original .PFD data) from frm to to."""
    global _nav
    if _nav is None:
        _nav = Port(); _nav.cmd("start 1 1 1")
    _nav.cmd(f"set {JOAN} x {frm[0]}"); _nav.cmd(f"set {JOAN} y {frm[1]}")
    return [tuple(map(float, t[1:4])) for t in _nav.cmd(f"path {to[0]} {to[1]}")] or [(*frm, 0.0), (*to, 0.0)]


# ------------------------------------------------------------------------------------------- original
class Orig(Backend):
    """joan.exe 2004-01-19 VAs: UnitArray 0xa47c08 (DynArrayB: +0x0c count, +0x10 elem size, +0x19 body of unit
    pointers, 1-based), player recno 0xa5b9e8, game clock 0xa500ba, RNG seed 0xa46022. Unit offsets:
    GAMEPLAY_CHARACTER_COMBAT.md Part A §1."""
    name = "orig"
    UA, PLAYER, CLOCK, SEED = 0xa47c08, 0xa5b9e8, 0xa500ba, 0xa46022

    def __init__(self, seed=1):
        super().__init__()
        os.environ.setdefault("JOA_DISPLAY", ":92")
        sys.path.insert(0, str(ROOT / "tools/reference"))
        import joa_ref as J
        self.J = J
        J.COPY, J.PREFIX, J.REF = OUT / "wine/drive_c/joa", OUT / "wine/prefix", OUT / "orig"
        J.ENV["WINEPREFIX"] = str(J.PREFIX)
        for d in ("logs", "shots", "video"):
            (J.REF / d).mkdir(parents=True, exist_ok=True)
        self.seed = seed
        ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER
        self.lock = threading.Lock()
        self.cur, self.mem, self.sampling = {}, None, False

    # memory
    def rd(self, a, n):
        with self.lock:
            self.mem.seek(a); return self.mem.read(n)

    def u32(self, a):
        return struct.unpack("<I", self.rd(a, 4))[0]

    def read_units(self):
        cnt = struct.unpack("<i", self.rd(self.UA + 0xc, 4))[0]  # elem size (+0x10) is 4: pointer body
        ptrs = struct.unpack(f"<{cnt}I", self.rd(self.u32(self.UA + 0x19), cnt * 4)) if cnt > 0 else ()
        prec = struct.unpack("<i", self.rd(self.PLAYER, 4))[0]
        out = {}
        for p in ptrs:
            if not p:
                continue
            b = self.rd(p, 0x220)
            g = lambda f, o: struct.unpack_from(f, b, o)[0]
            u = dict(rec=g("<h", 0), name=b[0x3d:0x65].split(b"\0")[0].decode("latin1"), nat=g("<h", 2), cls=g("<h", 0x3b) - 1,
                     hp=g("<i", 0x135), mhp=g("<i", 0x13d), ep=g("<i", 0x145), mep=g("<i", 0x149),
                     str=g("<h", 0x155), dex=g("<h", 0x157), lead=g("<h", 0x15b), lvl=g("<h", 0x124), xp=g("<i", 0x126),
                     act=g("<h", 0x215), x=g("<f", 0xe), y=g("<f", 0x12), z=g("<f", 0x16),
                     yaw=math.degrees(g("<f", 0x1a)) % 360.0, mor=g("<h", 0x163), arm=g("<h", 0x15d), ldr=g("<h", 0x37))
            u["def"] = g("<h", 0x159)
            out[u["rec"]] = u
        return prec, out

    def snapshot(self):
        prec, us = self.read_units()
        p = us.get(prec)
        if p:
            us = {r: u for r, u in us.items() if math.hypot(u["x"] - p["x"], u["y"] - p["y"]) < 4000}
            # camera yaw: View3D* 0xa48ccc, current CameraState +0xa0, yaw +0x10 (radians)
            p["cam"] = math.degrees(struct.unpack("<f", self.rd(self.u32(0xa48ccc) + 0xb0, 4))[0]) % 360.0
        return self.u32(self.CLOCK), prec, us

    def _sampler(self):
        while self.sampling:
            try:
                t, prec, us = self.snapshot()
                self.cur, self.player_rec = us, prec
                if not self.samples or self.samples[-1][0] != t:
                    self.samples.append((t, us))
            except (OSError, struct.error):
                pass
            time.sleep(0.01)

    # lifecycle
    def screen(self):
        """Sys+0x3a screen mode, observed: 0 enlight.wmv, 14 intro.wmv, 3 main menu (and its sub-screens),
        12 loading, 9 in-game world."""
        return self.rd(0xa50088 + 0x3a, 1)[0]

    def launch(self):
        J = self.J
        J.xvfb(); J.reset_state()
        J.start("joan.exe", "diff_session")
        J.wait_window("Wars & Warriors: Joan of Arc", timeout=600)  # 90 s under heavy host load
        pid = int(J.sh("pgrep", "-f", "C:.joa.joan.exe").split()[0])
        self.mem = open(f"/proc/{pid}/mem", "r+b", buffering=0)
        for _ in range(60):  # Escape skips the movie playing; stop at the main menu
            if self.screen() == 3:
                break
            if self.screen() in (0, 14):
                J.key("Escape", note=f"Escape (screen {self.screen()})")
            time.sleep(3)
        time.sleep(3)
        for _ in range(3):
            J.gclick(258, 256, wait=3); J.gclick(258, 297, wait=4, note="NORMAL")
            if self.screen() != 3:
                break
        for _ in range(600):
            if self.screen() == 9:
                break
            time.sleep(1)
        time.sleep(3); J.key("Escape", note="skip cut scene"); time.sleep(5); J.key("ctrl"); time.sleep(1)
        J.key("F5", note="quick save = scenario checkpoint"); time.sleep(4)
        self.sampling = True
        threading.Thread(target=self._sampler, daemon=True).start()
        self.wait_player()
        self.checkpoint = dict(self.player())

    def wait_player(self):
        for _ in range(500):
            if self.player_rec in self.cur:
                return
            time.sleep(0.02)
        raise RuntimeError("no player unit in the sampled state")

    def reset(self):
        J = self.J
        if self.mem is None:
            self.player_rec = JOAN
            self.launch()
        else:
            for k in list(self.held):
                self.up(k)
            for _ in range(3):  # F6 -> "Load the last Quick Save game now?" -> OK; verified by Joan's position
                J.key("F6", note="quick load"); time.sleep(3)
                J.gclick(415, 399, wait=12, note="OK (load last Quick Save)")
                J.key("ctrl"); time.sleep(1)
                p, c = self.player(), self.checkpoint
                if math.hypot(p["x"] - c["x"], p["y"] - c["y"]) < 30 and p["hp"] == c["hp"]:
                    break
            else:
                raise RuntimeError("quick load did not restore the checkpoint")
        with self.lock:  # fixed RNG seed: makes original runs repeatable up to frame timing
            self.mem.seek(self.SEED); self.mem.write(struct.pack("<I", self.seed))
        time.sleep(0.3)
        self.samples, self.marks, self.held = [], [], set()

    def now(self):
        return self.u32(self.CLOCK)

    def units(self):
        return self.cur

    def all_units(self):
        return self.read_units()[1]

    def down(self, k):
        x = KEYS[k][0]
        self.J.sh("xdotool", "mousedown" if isinstance(x, int) else "keydown", str(x)); self.held.add(k)

    def up(self, k):
        x = KEYS[k][0]
        self.J.sh("xdotool", "mouseup" if isinstance(x, int) else "keyup", str(x)); self.held.discard(k)

    def mouse(self, dx):
        self.J.rel(round(dx), 0)

    def sleep(self, ms):
        # game clock, not wall clock: frame hitches clamp real_dt to 1000 ms; wall guard for paused screens
        t0, w0 = self.now(), time.time()
        while self.now() - t0 < ms and time.time() - w0 < ms / 1000 * 4 + 5:
            time.sleep(0.01)

    def set_hp(self, rec, hp):
        p = self.u32(self.u32(self.UA + 0x19) + (rec - 1) * 4)
        with self.lock:
            self.mem.seek(p + 0x135); self.mem.write(struct.pack("<i", hp))

    def set_xp(self, rec, xp):
        p = self.u32(self.u32(self.UA + 0x19) + (rec - 1) * 4)
        with self.lock:  # +0x126 EXPERIENCE; LEVELUP 0x522c40 runs on the next XP gain (EXPUP 0x5229a0)
            self.mem.seek(p + 0x126); self.mem.write(struct.pack("<i", xp))

    def close(self):
        self.sampling = False
        self.J.stop()


# ------------------------------------------------------------------------------------------- scenarios
# Unit recnos are identical on both sides (scenario .uni order): 72 Joan, 73 Jean de Metz, 42/43 CI01/CI02
# (English light infantry at the first camp, ~2400 units west of the start), 14 "Unit 63" (archer west of the camp).
JOAN, JEAN, CI01, CI02, ARCHER = 72, 73, 42, 43, 14


def sc_start(d):
    """Mission 1 start: Joan and Jean stats, position, facing."""
    return {"units": {r: d.all_units()[r] for r in (JOAN, JEAN, CI01, ARCHER)}}


def sc_move(d):
    """From the start (open ground along the initial facing): run (W) 4 s, back (S) 3 s, strafe left (A) 2 s,
    strafe right (D) 2 s, walk (Shift+W) 3 s, 1.5 s rest between; Jean follows."""
    for k, label, ms in (("w", "run", 4000), ("s", "back", 3000), ("a", "strafe", 2000), ("d", "strafe_r", 2000),
                         ("shift+w", "walk", 3000)):
        d.mark(label + "_start")
        ks = k.split("+")
        for x in ks: d.down(x)
        d.sleep(ms)
        for x in reversed(ks): d.up(x)
        d.mark(label + "_end")
        d.sleep(1500)
    return {}


def sc_boundary(d):
    """Run (W) 8 s along the initial facing into the battlefield edge (y ~ 14093), then 3 s more."""
    d.mark("edge_start"); d.down("w"); d.sleep(11000); d.up("w"); d.mark("edge_end"); d.sleep(1000)
    return {}


def sc_turn(d):
    """Standing: +500 mickeys; then -500 mickeys while running."""
    d.mark("turn_stand"); d.mouse(500); d.sleep(1500); d.mark("turn_stand_end")
    d.down("w"); d.sleep(500); d.mark("turn_run"); d.mouse(-500); d.sleep(1500); d.mark("turn_run_end"); d.up("w")
    d.sleep(800)
    return {}


def sc_combo(d, presses=12, gap=280):
    """Navmesh route to CI01 (HP pinned on the way), then light attacks: LMB every `gap` ms, re-facing CI01."""
    d.goto(CI01, 90, timeout_ms=30000, pin=True)
    d.mark("combo_start")
    for _ in range(presses):
        if d.player()["hp"] < 25:
            d.set_hp(JOAN, 60)
        d.face(d.bearing(CI01), tol=20)
        d.tap("lmb"); d.sleep(gap)
    d.mark("combo_end"); d.sleep(1500)
    return {}


def sc_block(d):
    """Route to CI01 (pinned); guard (Q held, facing CI01) 6 s; then 6 s standing, facing CI01, no guard."""
    d.goto(CI01, 120, timeout_ms=30000, pin=True)
    d.set_hp(JOAN, 60)
    d.mark("block_start"); d.face(d.bearing(CI01), tol=5); d.down("q")
    for _ in range(12):
        d.sleep(500)
    d.up("q"); d.mark("block_end")
    d.set_hp(JOAN, 60)
    d.mark("noblock_start")
    for _ in range(12):
        d.face(d.bearing(CI01), tol=20); d.sleep(500)
    d.mark("noblock_end")
    return {}


def sc_archer(d):
    """Navmesh route toward the archer "Unit 63" (14), HP pinned; stand 700 units away facing it for 12 s."""
    d.goto(ARCHER, 700, timeout_ms=60000, pin=True)
    d.set_hp(JOAN, 60)
    d.mark("archer_start"); d.face(d.bearing(ARCHER)); d.sleep(12000); d.mark("archer_end")
    return {}


BOX = (13509, 12699)  # map item 2 (class 10 box, content item 92), the only box within 3000 of the start


def sc_potion(d):
    """Break the box near the start (LMB x8), walk over the drop at full HP (it goes to the bag), then HP = 20
    (test hook) and F3 (HP recovery item 0x43b490)."""
    d.goto(BOX, 70, timeout_ms=30000)
    for _ in range(8):
        d.face(math.degrees(math.atan2(BOX[1] - d.player()["y"], BOX[0] - d.player()["x"])), tol=10)
        d.tap("lmb"); d.sleep(500)
    d.goto(BOX, 10, timeout_ms=4000)
    d.sleep(1000); d.set_hp(JOAN, 20); d.sleep(500)
    d.mark("potion_before"); d.tap("F3"); d.sleep(1500); d.mark("potion_after")
    return {}


def sc_levelup(d, limit_ms=120000):
    """XP test hook: Joan's XP = 990 (level 2 at 1000); fight the first camp with HP pinned until the next XP gain
    levels her up."""
    d.set_xp(JOAN, 990)
    d.goto(CI01, 90, timeout_ms=30000, pin=True)
    d.mark("fight_start")
    t0 = d.now()
    while d.player()["lvl"] < 2 and d.now() - t0 < limit_ms:
        foes = [u for u in d.units().values() if u["nat"] != d.player()["nat"] and u["hp"] > 0 and u["nat"]]
        if not foes:
            break
        f = min(foes, key=lambda u: d.dist(u["rec"]))
        if d.dist(f["rec"]) > 100:
            d.face(d.bearing(f["rec"]), tol=10); d.hold("w", 300)
        else:
            d.face(d.bearing(f["rec"]), tol=20); d.tap("lmb"); d.sleep(250)
        if d.player()["hp"] < 40:
            d.set_hp(JOAN, 60)
    d.mark("fight_end"); d.sleep(1000)
    return {}


def sc_aggro(d):
    """Walk (Shift+W) along the navmesh route toward CI01; per enemy: Joan's distance when it first reacts."""
    d.goto(CI01, 60, timeout_ms=60000, walk=True)
    d.sleep(1000)
    return {}


SCENARIOS = {f.__name__[3:]: f for f in (sc_start, sc_move, sc_boundary, sc_turn, sc_combo, sc_block, sc_archer,
                                         sc_potion, sc_levelup, sc_aggro)}


def run(backend, names):
    d = Orig() if backend == "orig" else Port()
    (OUT / backend).mkdir(parents=True, exist_ok=True)
    try:
        for n in names:
            d.reset()
            d.sleep(1000)  # settle: the port's body yaw still eases toward the goal right after the cut-scene skip
            d.samples, d.marks = d.samples[-1:], []
            print(f"[{backend}] {n}", flush=True)
            extra = SCENARIOS[n](d)
            rec = dict(scenario=n, backend=backend, player=d.player_rec, marks=d.marks, extra=extra,
                       samples=[(t, list(us.values())) for t, us in d.samples])
            (OUT / backend / f"{n}.json").write_text(json.dumps(rec))
    finally:
        d.close()


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2:] or list(SCENARIOS))
