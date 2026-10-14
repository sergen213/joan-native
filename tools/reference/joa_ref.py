"""Reference-capture driver for the ORIGINAL Joan of Arc under Wine on a private Xvfb.

Runs the game from work/wine/drive_c/joa (reflink copy; the original install is never written),
isolated prefix work/wine/prefix, private X server JOA_DISPLAY (default :91). Never touches the
user's desktop. Scenarios live in capture.py; this module is the plumbing.
"""
import math, os, pathlib, shutil, struct, subprocess, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
COPY = ROOT / "work/wine/drive_c/joa"
PREFIX = ROOT / "work/wine/prefix"
REF = ROOT / "work/reference"
DISPLAY = os.environ.get("JOA_DISPLAY", ":91")
ENV = dict(os.environ, WINEPREFIX=str(PREFIX), DISPLAY=DISPLAY,
           # no mono/gecko prompts; no winemenubuilder (it writes into ~/.local/share otherwise)
           WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
# Untracked original files copied into the sandbox (everything except repo/tooling paths).
NOT_GAME = {"work", "src", "docs", "tools", "tests", "launcher", "updater", "cmake", "packaging", "shaders",
            "third_party", "CMakeLists.txt", "README.md", "goal.txt", ".git", ".github", ".gitignore"}
# RandR modes added to Xvfb: joan.exe exits right after EnumAdapterModes if the CONFIG.DAT mode is absent.
MODES = [(640, 480), (800, 600), (1024, 768), (1280, 720), (1280, 1024)]
REG_KEY = r"HKLM\Software\Enlight Software\Joan of Arc"  # read by VSetting.exe (Path, Version must be "1.00")

_proc = None
_steps = None
T0 = time.time()


def sh(*a, **kw):
    return subprocess.run(a, env=ENV, capture_output=True, text=True, **kw).stdout


# ---------------------------------------------------------------- environment
def xvfb():
    if subprocess.run(["xdpyinfo"], env=ENV, capture_output=True).returncode != 0:
        subprocess.Popen(["Xvfb", DISPLAY, "-screen", "0", "1920x1080x24", "+extension", "GLX",
                          "+extension", "RANDR", "-nolisten", "tcp", "-noreset"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        time.sleep(2)
    have = sh("xrandr")
    for w, h in MODES:
        name = f"{w}x{h}_60.00"
        if name in have:
            continue
        line = sh("cvt", str(w), str(h), "60").split("Modeline", 1)[1].split()
        sh("xrandr", "--newmode", name, *line[1:])
        sh("xrandr", "--addmode", "screen", name)
    screen_reset()


def screen_reset():
    """Back to 1920x1080 with a nonzero physical size: Xvfb reports 0 mm, and Wine's next mode switch
    then died with X error BadValue on RRSetScreenSize (seen in the first s05_640x480 run)."""
    sh("xrandr", "-s", "1920x1080")
    sh("xrandr", "--fbmm", "508x286")


def setup():
    """Idempotent: X server, sandbox copy, prefix, C:\\joa link, registry keys."""
    xvfb()
    COPY.mkdir(parents=True, exist_ok=True)
    for f in ROOT.iterdir():
        if f.name not in NOT_GAME and not f.name.startswith("build") and not (COPY / f.name).exists():
            subprocess.run(["cp", "-a", "--reflink=auto", str(f), str(COPY)], check=True)
    if not (PREFIX / "system.reg").exists():
        subprocess.run(["wineboot", "-i"], env=dict(ENV, WINEDEBUG="-all"), capture_output=True)
        subprocess.run(["wineserver", "-w"], env=ENV)
    link = PREFIX / "drive_c/joa"
    if not link.exists():
        link.symlink_to("../../drive_c/joa")
    for v, d in (("Path", r"C:\joa"), ("Version", "1.00")):
        subprocess.run(["wine", "reg", "add", REG_KEY, "/v", v, "/d", d, "/f", "/reg:32"],
                       env=dict(ENV, WINEDEBUG="-all"), capture_output=True)
    for d in ("logs", "shots", "video"):
        (REF / d).mkdir(parents=True, exist_ok=True)


def reset_state():
    """Restore CONFIG.DAT and Save/ in the sandbox from the (read-only) original install."""
    shutil.copy2(ROOT / "CONFIG.DAT", COPY / "CONFIG.DAT")
    shutil.rmtree(COPY / "Save")
    shutil.copytree(ROOT / "Save", COPY / "Save")


def set_video(w, h, bpp=32, hz=60):
    """CONFIG.DAT video mode: u32 LE width@0x82 height@0x86 bpp@0x8a refresh@0x8e (offsets from VSetting.exe)."""
    p = COPY / "CONFIG.DAT"
    d = bytearray(p.read_bytes())
    struct.pack_into("<4I", d, 0x82, w, h, bpp, hz)
    p.write_bytes(d)


# ---------------------------------------------------------------- process + capture
def start(exe, scenario, debug="-all"):
    """Start exe in C:\\joa; WINEDEBUG output -> logs/<scenario>.log, step log -> logs/<scenario>.steps.tsv."""
    global _proc, _steps, T0
    T0 = time.time()
    _steps = open(REF / "logs" / f"{scenario}.steps.tsv", "w")
    _steps.write("t_sec\taction\tartifact\n")
    _proc = subprocess.Popen(["wine", f"C:\\joa\\{exe}"], cwd=PREFIX / "drive_c/joa",
                             env=dict(ENV, WINEDEBUG=debug),
                             stdout=open(REF / "logs" / f"{scenario}.log", "w"), stderr=subprocess.STDOUT)
    step(f"start {exe} WINEDEBUG={debug}")


def t():
    return round(time.time() - T0, 1)


def step(action, artifact=""):
    print(f"[{t():6.1f}s] {action} {artifact}")
    if _steps:
        _steps.write(f"{t()}\t{action}\t{artifact}\n"); _steps.flush()


def shot(name, action="screenshot"):
    p = REF / "shots" / f"{name}.png"
    subprocess.run(["import", "-window", "root", str(p)], env=ENV, check=True)
    step(action, f"shots/{p.name}")
    return p


def record(name, secs, fps=15):
    size = sh("xdpyinfo").split("dimensions:")[1].split()[0]
    step(f"record {secs}s @{fps}fps", f"video/{name}.mp4")
    return subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab", "-framerate", str(fps),
                             "-video_size", size, "-i", DISPLAY, "-t", str(secs), "-c:v", "libx264",
                             "-preset", "veryfast", "-crf", "26", "-pix_fmt", "yuv420p",
                             str(REF / "video" / f"{name}.mp4")], env=ENV)


def geometry(win):
    g = dict(l.split("=") for l in sh("xdotool", "getwindowgeometry", "--shell", win).split())
    return int(g["X"]), int(g["Y"]), int(g["WIDTH"]), int(g["HEIGHT"])


def wait_window(title, timeout=180):
    """Wait for a top-level window and give it X input focus: without a WM nothing focuses it, and
    DirectInput (foreground cooperative level) then ignores the mouse; the keyboard still works."""
    end = time.time() + timeout
    while time.time() < end:
        ids = sh("xdotool", "search", "--name", title).split()
        if ids:
            sh("xdotool", "windowfocus", ids[-1])
            step(f"window '{title}' appeared; focused")
            return ids[-1]
        time.sleep(1)
    raise TimeoutError(title)


# ---------------------------------------------------------------- input
def key(*keys, note=None):
    """Each key held 0.25 s: the game polls DirectInput state per frame (~10-20 fps on llvmpipe),
    so xdotool's default ~12 ms taps are sometimes missed (observed for F1/F2/F12)."""
    for k in keys:
        sh("xdotool", "keydown", k); time.sleep(0.25); sh("xdotool", "keyup", k); time.sleep(0.15)
    step(note or "key " + " ".join(keys))


def hold(k, secs, note=None):
    sh("xdotool", "keydown", k); time.sleep(secs); sh("xdotool", "keyup", k)
    step(note or f"hold {k} {secs}s")


def rel(dx, dy, step_px=40):
    """Relative pointer motion in small steps (the game reads DirectInput relative deltas)."""
    n = max(1, math.ceil(max(abs(dx), abs(dy)) / step_px))
    for _ in range(n):
        sh("xdotool", "mousemove_relative", "--", str(round(dx / n)), str(round(dy / n)))
        time.sleep(0.03)


def button(b=1, secs=0.1):
    sh("xdotool", "mousedown", str(b)); time.sleep(secs); sh("xdotool", "mouseup", str(b))


def gclick(x, y, wait=2.0, note=None):
    """Click at game-screen pixel (x, y): DirectInput cursor is relative-only, so slam it into the
    top-left corner first (it clamps at 0,0), then move by (x, y); measured 1:1 at default sensitivity."""
    rel(-4000, -4000, step_px=200); time.sleep(0.2); rel(x, y); time.sleep(0.2)
    button(1)
    step(note or f"game click {x},{y}")
    time.sleep(wait)


def click(x, y, note=None):
    """Absolute click for plain Win32 dialogs (VSetting)."""
    sh("xdotool", "mousemove", str(x), str(y)); time.sleep(0.2); button(1)
    step(note or f"click {x},{y}")


def stop():
    global _steps
    subprocess.run(["wineserver", "-k"], env=ENV)
    time.sleep(1)
    screen_reset()
    if _steps:
        step("stop (wineserver -k)"); _steps.close(); _steps = None
