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
           "THROWN STANDUP DIZZY STOP STOP_B KNOCKDN KNOCKDN2 KNOCKDN3 H_IDLE H_WALK H_BACK H_RUN H_STOP_B H_STOP "
           "H_ATTACK H_HITED H_BHITED H_LHITED H_RHITED H_DROP H_MNT_L H_MNT_R H_DMNT_L H_DMNT_R RUSH PUSH PUSH2 PUSH_L "
           "PUSH_R BATTER_C").split()
# mnt +0x1e2 ridden horse, rid +0x1e6 rider (horse), veh +0x384 crewed siege weapon, slot +0x388, task +0x2a5, tp1 +0x2aa
FIELDS = "rec name nat cls hp mhp ep mep str dex def lead lvl xp act x y z yaw mor arm ldr mnt rid veh slot task tp1".split()
# key name -> (xdotool key / mouse button, port InputFrame action id; DEFAULTK.csv rows)
KEYS = {"w": ("w", 4), "s": ("s", 5), "a": ("a", 6), "d": ("d", 7), "shift": ("shift", 10), "q": ("q", 12),
        "lmb": (1, 13), "rmb": (3, 14), "c": ("c", 15), "space": ("space", 16), "r": ("r", 17), "F3": ("F3", 19),
        "F4": ("F4", 20)}
MOUSE_DEG = 0.18  # deg per mickey at default sensitivity: ((0+1)*0.7+0.2)*0.2 (0x431790)


def aname(a):
    return ACTIONS[a] if 0 <= a < len(ACTIONS) else str(a)


def wrap180(a):
    return (a + 180.0) % 360.0 - 180.0


class Backend:
    """Common API. Times are game ms; units() -> {rec: dict(FIELDS)} for units within 4000 of the player and the
    watched recnos (watch()). mission = campaign index (scenario_0N.scr)."""
    name = "?"

    def __init__(self, mission=1):
        self.samples, self.marks, self.held = [], [], set()
        self.mission = mission

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
        wps = nav_path((p["x"], p["y"]), (t["x"], t["y"]), self.mission)[1:-1]
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

    def __init__(self, seed=1, mission=1):
        super().__init__(mission)
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
        v = [int(x) for x in tok[1:2]] + [tok[2].replace("_", " ")] + [int(x) for x in tok[3:16]] + [float(x) for x in tok[16:20]] + [int(x) for x in tok[20:29]]
        return dict(zip(FIELDS, v))

    def reset(self):
        r = self.cmd(f"start {self.mission} 1 {self.seed}")
        self.player_rec = int(r[0][1]); self.t = int(r[0][3])
        self.cur = {u["rec"]: u for u in map(self.parse, self.cmd("units"))}
        self.samples, self.marks, self.held = [(self.t, self.cur)], [], set()
        self.pressed, self.chunks = set(), []

    def watch(self, recs):
        self.cmd("watch " + (",".join(map(str, recs)) or "-"))

    def items(self, recs=None):
        """Map items: {rec: dict(rec name cls type res x y z hp mhp state)} (gates, walls, boxes, ...)."""
        out = {}
        for t in self.cmd("objects all"):
            if recs is None or int(t[1]) in recs:
                out[int(t[1])] = dict(rec=int(t[1]), name=t[2].replace("_", " "), cls=int(t[3]), type=int(t[4]), res=int(t[9]),
                                      x=float(t[6]), y=float(t[7]), z=float(t[8]), hp=int(t[12]), mhp=int(t[13]), state=int(t[14]))
        return out

    def types(self):
        return {int(t[1]): dict(code=t[2], gold=int(t[9]), res=int(t[10])) for t in self.cmd("classes")}

    def set_yaw(self, rec, deg):
        self.cmd(f"set {rec} yaw {deg}")

    def set_item_hp(self, rec, hp):
        self.cmd(f"setitem {rec} hp {hp}")

    def set_flag(self, word, bit, v):
        self.cmd(f"flag {word} {bit} {v}")

    def gold(self, v=None):
        return int(self.cmd("gold" + ("" if v is None else f" {v}"))[0][1])

    def teleport(self, rec, x, y, z=None):
        self.cmd(f"set {rec} x {x}"); self.cmd(f"set {rec} y {y}")
        if z is not None:
            self.cmd(f"set {rec} z {z}")

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
        # as joa_ref.rel: ceil(|dx|/40) chunks, one per frame (0x431790 truncates ftol(dx*0.18) per frame). Blocks until
        # the whole move is sent, as Orig.mouse (rel() is synchronous): queued chunks must not pile up across the
        # closed-loop re-aims (face() every 150 ms), or the port over-turns and orbits waypoints.
        n = max(1, math.ceil(abs(dx) / 40))
        self.chunks += [round(dx / n)] * n
        self.sleep(n * 1000 / 30)

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


_nav = {}


def nav_path(frm, to, mission=1):
    """Waypoints [(x, y, z)] of the port's navmesh path (original .PFD data) of the mission from frm to to."""
    if mission not in _nav:
        n = Port(mission=mission); n.reset(); _nav[mission] = n
    n = _nav[mission]
    n.teleport(n.player_rec, *frm)
    return [tuple(map(float, t[1:4])) for t in n.cmd(f"path {to[0]} {to[1]}")] or [(*frm, 0.0), (*to, 0.0)]


# ------------------------------------------------------------------------------------------- original
class Orig(Backend):
    """joan.exe 2004-01-19 VAs: UnitArray 0xa47c08 (DynArrayB: +0x0c count, +0x10 elem size, +0x19 body of unit
    pointers, 1-based), player recno 0xa5b9e8, game clock 0xa500ba, RNG seed 0xa46022. Unit offsets:
    GAMEPLAY_CHARACTER_COMBAT.md Part A §1."""
    name = "orig"
    UA, PLAYER, CLOCK, SEED = 0xa47c08, 0xa5b9e8, 0xa500ba, 0xa46022

    def __init__(self, seed=1, mission=1):
        super().__init__(mission)
        self.watched = set()
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
            b = self.rd(p, 0x3a0)
            g = lambda f, o: struct.unpack_from(f, b, o)[0]
            u = dict(rec=g("<h", 0), name=b[0x3d:0x65].split(b"\0")[0].decode("latin1"), nat=g("<h", 2), cls=g("<h", 0x3b) - 1,
                     hp=g("<i", 0x135), mhp=g("<i", 0x13d), ep=g("<i", 0x145), mep=g("<i", 0x149),
                     str=g("<h", 0x155), dex=g("<h", 0x157), lead=g("<h", 0x15b), lvl=g("<h", 0x124), xp=g("<i", 0x126),
                     act=g("<h", 0x215), x=g("<f", 0xe), y=g("<f", 0x12), z=g("<f", 0x16),
                     yaw=math.degrees(g("<f", 0x1a)) % 360.0, mor=g("<h", 0x163), arm=g("<h", 0x15d), ldr=g("<h", 0x37),
                     mnt=g("<h", 0x1e2), rid=g("<h", 0x1e6), veh=g("<i", 0x384), slot=g("<i", 0x388), task=g("<i", 0x2a5),
                     tp1=g("<i", 0x2aa))
            u["def"] = g("<h", 0x159)
            out[u["rec"]] = u
        return prec, out

    def unit_ptr(self, rec):
        return self.u32(self.u32(self.UA + 0x19) + (rec - 1) * 4)

    def wr(self, a, fmt, *v):
        with self.lock:
            self.mem.seek(a); self.mem.write(struct.pack(fmt, *v))

    IA = 0xa460a4  # item array 0xa460a0: vtable, then a DynArrayB as UnitArray

    def items(self, recs=None):
        """Map items {rec: dict(rec name cls type res x y z hp mhp state)} (all, or only recs). HP (get_gate_hp 0x650b90
        / get_wall_hp 0x650d50, the vf4c/vf58 objects are the item itself): gate/obstacle/house +0xc2/+0xc6, wall
        +0xc6/+0xca; gate state +0xd1. Recnos match the port's (checked for mission 4: 134/160/161/165/191)."""
        cnt = struct.unpack("<i", self.rd(self.IA + 0xc, 4))[0]
        ptrs = struct.unpack(f"<{cnt}I", self.rd(self.u32(self.IA + 0x19), cnt * 4)) if cnt > 0 else ()
        out = {}
        for i, p in enumerate(ptrs):
            if not p or (recs is not None and i + 1 not in recs):
                continue
            b = self.rd(p, 0xf0)
            g = lambda f, o: struct.unpack_from(f, b, o)[0]
            cls = g("<h", 0x4e)
            hp, mhp = (g("<i", 0xc6), g("<i", 0xca)) if cls == 4 else (g("<i", 0xc2), g("<i", 0xc6)) if cls in (1, 2, 3) else (0, 0)
            out[i + 1] = dict(rec=i + 1, name=b[0xc:0x4c].split(b"\0")[0].decode("latin1"), cls=cls, type=g("<h", 0x50),
                              res=g("<h", 0xa), x=g("<f", 0x56), y=g("<f", 0x5a), z=g("<f", 0x5e), hp=hp, mhp=mhp,
                              state=b[0xd1] if cls == 1 else 0)
        return out

    def types(self):
        """Unit-type table *0xa48d88 (stride 0x203, +0 rec, +4 code): {cls: dict(code, gold +0x161, res +0x165)}."""
        base, out = self.u32(0xa48d88), {}
        for k in range(512):
            b = self.rd(base + k * 0x203, 0x169)
            code = b[4:0x24].split(b"\0")[0].decode("latin1")
            if not code:
                break
            out[k] = dict(code=code, gold=struct.unpack_from("<i", b, 0x161)[0], res=struct.unpack_from("<i", b, 0x165)[0])
        return out

    def set_yaw(self, rec, deg):  # +0x1a radians, +0x1e degrees
        p = self.unit_ptr(rec)
        self.wr(p + 0x1a, "<2f", math.radians(deg), deg)

    def set_item_hp(self, rec, hp):
        p = self.u32(self.u32(self.IA + 0x19) + (rec - 1) * 4)
        self.wr(p + (0xc6 if struct.unpack("<h", self.rd(p + 0x4e, 2))[0] == 4 else 0xc2), "<i", hp)

    def set_flag(self, word, bit, v):
        """Scenario bit flags (FUN_00472000): word w of the step record R+0x18a, R = *0xa58fb7 + (cur-1)*0x1b2."""
        a = self.u32(0xa58fb7) + (self.u32(0xa58fab) - 1) * 0x1b2 + 0x18a + 4 * word
        s = (bit - 1) & 31
        self.wr(a, "<I", (self.u32(a) & ~(1 << s) & 0xffffffff) | ((v & 0xff) << s))

    def gold(self, v=None):
        if v is not None:
            self.wr(0xa596c4, "<i", v)
        return struct.unpack("<i", self.rd(0xa596c4, 4))[0]

    def teleport(self, rec, x, y, z=None):
        """Test hook: move a unit. Writing +0x0e alone does not stick (the physics body wins on the next move), and
        the model / body copies alone leave the body stuck; what works is rewriting every heap copy of the exact
        (x, y) float pair (unit, model +0x490/+0x4e8/+0x1008, physics body at z+50, AI targets ...) at once, and
        shifting the float after each copy (its z, possibly offset) by the same dz."""
        pat = self.rd(self.unit_ptr(rec) + 0xe, 12)
        uz = struct.unpack("<f", pat[8:])[0]
        pat, new, dz = pat[:8], struct.pack("<2f", x, y), 0.0 if z is None else z - uz
        for line in open(f"/proc/{self.pid}/maps"):
            a, perm = line.split()[:2]
            lo, hi = (int(v, 16) for v in a.split("-"))
            if "rw" not in perm or hi > 0x80000000 or hi - lo > 512 << 20 or (lo < 0x1000000 and not lo <= 0xa5ba38 < hi):
                continue
            try:
                data = self.rd(lo, hi - lo)
            except OSError:
                continue
            i = data.find(pat)
            while i >= 0:
                if lo + i >= 0x1000000 or lo + i == 0xa5ba38:
                    self.wr(lo + i, "8s", new)
                    f = struct.unpack_from("<f", data, i + 8)[0] if i + 12 <= len(data) else 1e9
                    if dz and abs(f - uz) < 200:
                        self.wr(lo + i + 8, "<f", f + dz)
                i = data.find(pat, i + 1)

    def watch(self, recs):
        self.watched = set(recs)

    def snapshot(self):
        prec, us = self.read_units()
        p = us.get(prec)
        if p:
            us = {r: u for r, u in us.items() if r in self.watched or math.hypot(u["x"] - p["x"], u["y"] - p["y"]) < 4000}
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
        pid = self.pid = int(J.sh("pgrep", "-f", "C:.joa.joan.exe").split()[0])
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
        if self.mission > 1:
            self.jump()
        J.key("F5", note="quick save = scenario checkpoint"); time.sleep(4)
        self.sampling = True
        threading.Thread(target=self._sampler, daemon=True).start()
        self.wait_player()
        self.checkpoint = dict(self.player())

    def jump(self):
        """Mission N without playing 1..N-1: Scenario.no (0xa58fa7) = N-1, then Sys requested screen mode
        (+0x3c) = 0xd: ModeNextScenario_process 0x755d80 runs Battle_start_scenario(no+1, 1, 1) and the loading
        screen (0xc), as CONTINUE does after a win (fresh profile: mission-1 start heroes). Then Escape skips the
        opening cut scene (cut-scene flag Game+0x744 = 0xa596d4) and closes the mission brief (clock stopped)."""
        J = self.J
        self.wr(0xa58fa7, "<i", self.mission - 1)
        self.wr(0xa50088 + 0x3c, "B", 0xd)
        self.skip_intro()

    def skip_intro(self):
        J = self.J
        seen = set()
        for _ in range(600):
            seen.add(self.screen())
            if self.screen() == 9 and 0xc in seen:
                break
            time.sleep(1)
        time.sleep(6)
        for _ in range(8):
            t0 = self.now(); time.sleep(3)
            if self.u32(0xa596d4) == 0 and self.now() - t0 > 2000:
                break
            J.key("Escape", note="skip cut scene / close brief"); time.sleep(3)
        J.key("ctrl"); time.sleep(1)

    def wait_player(self):
        for _ in range(500):
            if self.player_rec in self.cur:
                return
            time.sleep(0.02)
        raise RuntimeError("no player unit in the sampled state")

    def reset(self):
        J = self.J
        if self.mem is None:
            self.player_rec = JOAN  # the sampler then follows 0xa5b9e8
            self.launch()
        else:
            for k in list(self.held):
                self.up(k)
            if self.player()["hp"] <= 0:  # game-over box (scripted LOSE): F6 is dead there -> Restart Mission
                J.gclick(512, 577, wait=5, note="Restart Mission")
                self.skip_intro()
                J.key("F5", note="quick save = scenario checkpoint"); time.sleep(4)
                self.checkpoint = dict(self.player())
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
        self.wr(self.unit_ptr(rec) + 0x135, "<i", hp)

    def set_xp(self, rec, xp):  # +0x126 EXPERIENCE; LEVELUP 0x522c40 runs on the next XP gain (EXPUP 0x5229a0)
        self.wr(self.unit_ptr(rec) + 0x126, "<i", xp)

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
    """Navmesh route toward the archer "Unit 63" (14), HP pinned; stand 700 units away facing it for 12 s (HP kept
    >= 30). The archer follows its leader Anger Greetham (11) into melee range first on both sides, so the metrics
    split arrow hits from melee hits and report the distance of every shot."""
    d.goto(ARCHER, 700, timeout_ms=60000, pin=True)
    d.set_hp(JOAN, 60)
    d.mark("archer_start"); d.face(d.bearing(ARCHER))
    t0 = d.now()
    while d.now() - t0 < 12000:
        d.sleep(150)
        if d.player()["hp"] < 30:
            d.set_hp(JOAN, 60)
    d.mark("archer_end")
    return {}


# Map item 1 (class 10 box, res 283, content 36 SMALL_BREAD: ItemClass POTION, "Red Potion"), on the hill north of the
# first camp, away from every enemy. (Map item 2 by the start holds content 92 = ARROW, not an HP item.)
BOX = (12527, 14822)


def sc_potion(d):
    """Break the bread box (LMB from <= 90 units), walk over the drop at full HP (it goes to the bag), then HP = 20
    (test hook) and F3 (HP recovery item 0x43b490)."""
    d.goto(BOX, 90, timeout_ms=40000)
    for _ in range(4):
        d.face(math.degrees(math.atan2(BOX[1] - d.player()["y"], BOX[0] - d.player()["x"])), tol=10)
        d.tap("lmb"); d.sleep(500)
    d.goto(BOX, 10, timeout_ms=4000)
    d.sleep(1000); d.set_hp(JOAN, 20); d.sleep(500)
    d.mark("potion_before"); d.tap("F3"); d.sleep(1500); d.mark("potion_after")
    return {}


def sc_levelup(d, limit_ms=120000):
    """XP test hook: Joan's XP = 990 (level 2 at 1000); fight the first camp with HP pinned until the next XP gain
    levels her up. The target is kept until it dies, and it is set to 1 HP (test hook) right before each of Joan's
    swings, so that Joan, not Jean, lands the kill that credits XP (Unit_die 0x7c6240 -> EXPUP 0x5229a0)."""
    d.set_xp(JOAN, 990)
    d.goto(CI01, 90, timeout_ms=30000, pin=True)
    d.mark("fight_start")
    t0, tgt = d.now(), CI01   # first CI01 (the camp member the route ends at), then the nearest live foe
    while d.player()["lvl"] < 2 and d.now() - t0 < limit_ms:
        if tgt is None or (d.target(tgt) or {"hp": 0})["hp"] <= 0:
            foes = [u for u in d.units().values() if u["nat"] != d.player()["nat"] and u["hp"] > 0 and u["nat"]]
            if not foes:
                break
            tgt = min(foes, key=lambda u: d.dist(u["rec"]))["rec"]
        if d.dist(tgt) > 100:
            d.face(d.bearing(tgt), tol=10); d.hold("w", 300)
        else:
            d.face(d.bearing(tgt), tol=20); d.set_hp(tgt, 1); d.tap("lmb"); d.sleep(250)
        if d.player()["hp"] < 40:
            d.set_hp(JOAN, 60)
    d.mark("fight_end"); d.sleep(1000)
    return {}


def sc_aggro(d):
    """Walk (Shift+W) along the navmesh route toward CI01; per enemy: Joan's distance when it first reacts."""
    d.goto(CI01, 60, timeout_ms=60000, walk=True)
    d.sleep(1000)
    return {}


# ------------------------------------------------------------------------------------------- later missions (round 4)
# Mission 4 (S4L1) recnos, identical on both sides: Cannon 1..4 and their LARGE_WEAPON_PUSH crews (S4L1_tactic.sce
# l.1043), map items 165 abtower (wall, 1000 HP) and 160 Tourelles gate (1500 HP), horse 89 ("Unit 14" enghorseA,
# French, in a pen of the Alexander de la Pole camp: moved out by the test hook), 4 "Unit 241" English light
# infantry L13, 1 "Unit 244" English crossbowman L13 (west group).
M4_CANNONS, M4_CREW, ABTOWER, TOURELLES_GATE = (344, 341, 444, 443), (343, 475, 342, 472, 446, 474, 445, 473), 165, 160
M4_HORSE, M4_FOOT, M4_XBOW = 89, 4, 1


def sc_m4start(d):
    """Mission 4 start (after the opening cut scene): every unit, structure HP, gold, unit-type costs."""
    return {"units": d.all_units(), "items": {r: i for r, i in d.items().items() if i["cls"] in (1, 2, 3, 4)},
            "gold": d.gold(), "types": d.types()}


def sc_cannon(d, window_ms=100000):
    """Siege: Joan next to the four French cannons (their crews attach once the player is near), then the test hooks
    of the S4L1 trigger (tactic l.4605): scenario bit 27 (Jean freed) and abtower HP = max - 1 -> bit 31, the cannons
    turn on and SHOOT the abtower (l.4637), then the Tourelles gate once it falls. Structure HP every 250 ms."""
    c = d.all_units()[343]
    d.teleport(d.player_rec, c["x"], c["y"] + 160, c["z"])
    d.watch(M4_CANNONS + M4_CREW)
    d.sleep(8000)
    its = d.items((ABTOWER, TOURELLES_GATE))
    d.mark("trigger"); d.set_flag(0, 27, 1); d.set_item_hp(ABTOWER, its[ABTOWER]["mhp"] - 1)
    trace, t0 = [], d.now()
    while d.now() - t0 < window_ms:
        d.sleep(250)
        its = d.items((ABTOWER, TOURELLES_GATE))
        trace.append((d.now(), its[ABTOWER]["hp"], its[TOURELLES_GATE]["hp"]))
        if d.player()["hp"] < 40:
            d.set_hp(d.player_rec, 60)
    d.mark("end")
    return {"structures": trace}


def sc_horse(d):
    """Riding: horse 89 and Joan moved (test hook) onto the open road north of the start, horse facing north (90 deg);
    R mounts; W 4 s (walk gear, then run gear); W again with -300 mickeys while moving (turn); then an English light
    infantryman (4) is put 90 units in front of the horse and Joan swings three times (LMB = H_ATTACK 'A')."""
    d.teleport(M4_HORSE, 18690, 1500, -18); d.set_yaw(M4_HORSE, 90)
    d.teleport(d.player_rec, 18640, 1500, -18)
    d.watch([M4_HORSE, M4_FOOT])
    d.sleep(1500)
    d.face(0); d.sleep(300)
    d.mark("mount"); d.tap("r"); d.sleep(2500)
    d.mark("ride"); d.down("w"); d.sleep(4000); d.up("w"); d.mark("ride_end"); d.sleep(1500)
    d.mark("hturn"); d.down("w"); d.sleep(800); d.mark("hturn_m"); d.mouse(-300); d.sleep(1200); d.up("w")
    d.mark("hturn_end"); d.sleep(1500)
    h = d.all_units()[M4_HORSE]
    a = math.radians(h["yaw"])
    d.teleport(M4_FOOT, h["x"] + 90 * math.cos(a), h["y"] + 90 * math.sin(a), h["z"])
    d.sleep(300)
    d.mark("hatk")
    for _ in range(3):
        if d.player()["hp"] < 40:
            d.set_hp(d.player_rec, 60)
        d.tap("lmb"); d.sleep(900)
    d.mark("hatk_end"); d.sleep(1000)
    return {}


def sc_m4aggro(d):
    """AI reaction in mission 4: Joan moved (test hook) to the navmesh waypoint ~2500 units from the west crossbow
    group (1), then walks (Shift+W) to it and stands 15 s (HP pinned >= 40). Per English unit: Joan's distance at its
    first reaction; per hit on Joan: damage."""
    p, t = d.player(), d.all_units()[M4_XBOW]
    wps = nav_path((p["x"], p["y"]), (t["x"], t["y"]), d.mission)
    w = next((w for w in wps if math.hypot(w[0] - t["x"], w[1] - t["y"]) < 2500), wps[-1])
    d.teleport(d.player_rec, w[0], w[1], w[2])
    d.sleep(1500)
    d.mark("walk"); d.goto(M4_XBOW, 120, timeout_ms=60000, walk=True, pin=True); d.mark("arrive")
    t0 = d.now()
    while d.now() - t0 < 15000:
        d.face(d.bearing(M4_XBOW), tol=20); d.sleep(300)
        if d.player()["hp"] < 40:
            d.set_hp(d.player_rec, 60)
    d.mark("end")
    return {}


def sc_m5start(d):
    """Mission 5 start (after the cut scene and the brief): every unit, structure HP, gold."""
    return {"units": d.all_units(), "items": {r: i for r, i in d.items().items() if i["cls"] in (1, 2, 3, 4)},
            "gold": d.gold()}


SCENARIOS = {f.__name__[3:]: f for f in (sc_start, sc_move, sc_boundary, sc_turn, sc_combo, sc_block, sc_archer,
                                         sc_potion, sc_levelup, sc_aggro, sc_m4start, sc_cannon, sc_horse, sc_m4aggro,
                                         sc_m5start)}
MISSION = {"m4start": 4, "cannon": 4, "horse": 4, "m4aggro": 4, "m5start": 5}


def run_on(d, names):
    """Scenarios of one mission on an open backend; traces -> work/diff/<backend>/<scenario>.json."""
    (OUT / d.name).mkdir(parents=True, exist_ok=True)
    for n in names:
        d.reset()
        d.watch([])
        d.sleep(1000)  # settle: the port's body yaw still eases toward the goal right after the cut-scene skip
        d.samples, d.marks = d.samples[-1:], []
        print(f"[{d.name}] {n}", flush=True)
        extra = SCENARIOS[n](d)
        rec = dict(scenario=n, backend=d.name, mission=d.mission, player=d.player_rec, marks=d.marks, extra=extra,
                   samples=[(t, list(us.values())) for t, us in d.samples])
        (OUT / d.name / f"{n}.json").write_text(json.dumps(rec))


def run(backend, names):
    for m in sorted({MISSION.get(n, 1) for n in names}):
        d = Orig(mission=m) if backend == "orig" else Port(mission=m)
        try:
            run_on(d, [n for n in names if MISSION.get(n, 1) == m])
        finally:
            d.close()


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2:] or list(SCENARIOS))
