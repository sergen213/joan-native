#!/usr/bin/env python3
"""Options pages + remap / mission-brief pages of the ORIGINAL game (Wine, private Xvfb, sandbox).

    python3 tools/reference/capture_ui.py          # options pages
    python3 tools/reference/capture_ui.py brief    # mission 1 brief: topic menu + every topic page

The s04 session desynced (Escape sent before the movies accepted it); here every step waits until the screen
matches a known image (main menu = shots/s06_main_menu.png) or changes, instead of fixed sleeps.
Writes work/reference/shots/ui_options_*.png / ui_brief_*.png and logs/ui_*.steps.tsv. Coordinates: 1024x768.
"""
import subprocess, sys, time

from PIL import Image, ImageChops, ImageStat

from joa_ref import *  # noqa: F403

MENU_OPTIONS = (258, 408)
OPT = dict(video=(508, 341), audio=(508, 421), input=(508, 501), game=(508, 581))
GRAB = REF / "shots" / "_grab.png"


def grab():
    subprocess.run(["import", "-window", "root", str(GRAB)], env=ENV, check=True)
    return Image.open(GRAB).convert("L").crop((0, 0, 1024, 768))


def diff(a, b):
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]


def wait_like(ref, timeout, note, tol=12.0):
    """Poll until the screen matches `ref` (mean abs luma diff < tol); Escape skips movies meanwhile."""
    target = Image.open(ref).convert("L").crop((0, 0, 1024, 768))
    end = time.time() + timeout
    while time.time() < end:
        d = diff(grab(), target)
        if d < tol:
            step(f"{note}: matched (diff {d:.1f})")
            return True
        if d > 40:  # clearly still a movie (the menu's own slide-in stays below this)
            key("Escape", note=f"{note}: diff {d:.1f}, Escape")
        time.sleep(5)
    step(f"{note}: TIMEOUT")
    return False


def settle(note, quiet=2.0, timeout=20):
    """Wait until two grabs `quiet` seconds apart are identical (slide-in animations finished)."""
    prev, end = grab(), time.time() + timeout
    while time.time() < end:
        time.sleep(quiet)
        cur = grab()
        if diff(cur, prev) < 0.5:
            return
        prev = cur
    step(f"{note}: still animating")


def page(name, pos, back_x):
    gclick(*pos, wait=1, note=name); settle(name); shot(f"ui_options_{name}", f"{name} page")
    gclick(back_x, 727, wait=1, note="BACK"); settle("options root")


def main():
    setup()
    reset_state()
    start("joan.exe", "ui_options")
    wait_window("Wars & Warriors: Joan of Arc")
    if not wait_like(REF / "shots/s06_main_menu.png", 300, "main menu"):
        stop()
        raise SystemExit("main menu never appeared")
    settle("main menu")
    gclick(*MENU_OPTIONS, wait=1, note="Options"); settle("options root"); shot("ui_options_root", "options root")
    page("video", OPT["video"], 587)
    page("audio", OPT["audio"], 587)
    page("game", OPT["game"], 512)
    gclick(*OPT["input"], wait=1, note="input"); settle("input"); shot("ui_options_input", "input page")
    gclick(287, 727, wait=1, note="REMAP KEYS"); settle("remap"); shot("ui_options_remap_0", "remap Movement")
    for i in range(1, 7):
        gclick(524, 261, wait=1, note="next category"); settle("remap"); shot(f"ui_options_remap_{i}", f"remap category {i}")
    gclick(737, 727, wait=1, note="CANCEL"); settle("input")
    stop()
    GRAB.unlink(missing_ok=True)


def brief():
    """New campaign (Normal) -> skip the mission 1 cutscene -> F12 -> each topic (cs_* rows at y 210+63i) -> BACK."""
    setup()
    reset_state()
    start("joan.exe", "ui_brief")
    wait_window("Wars & Warriors: Joan of Arc")
    if not wait_like(REF / "shots/s06_main_menu.png", 300, "main menu"):
        stop()
        raise SystemExit("main menu never appeared")
    settle("main menu")
    gclick(258, 256, wait=3, note="New Campaign"); settle("difficulty")
    gclick(258, 297, wait=30, note="NORMAL")
    key("Escape", note="Escape: skip cutscene"); time.sleep(6)
    key("ctrl", note="Ctrl: close attention message"); time.sleep(1)
    key("F12", note="F12: mission brief"); settle("topic menu"); shot("ui_brief_topics", "topic menu")
    for i, name in enumerate(("background", "objectives", "townships", "commanders", "battleground")):
        gclick(512, 234 + 63 * i, wait=1, note=name); settle(name); shot(f"ui_brief_{name}", f"{name} page")
        if name in ("townships", "commanders"):  # second list row (rows at (0x17, 0xdc+0x28 i), objmenu init)
            gclick(200, 0xdc + 0x28 + 20, wait=1, note=f"{name} row 2"); settle(name); shot(f"ui_brief_{name}_2", f"{name} row 2")
        gclick(512, 679, wait=1, note="BACK"); settle("topic menu")
    stop()
    GRAB.unlink(missing_ok=True)


if __name__ == "__main__":
    brief() if sys.argv[1:] == ["brief"] else main()
