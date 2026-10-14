#!/usr/bin/env python3
"""Reproducible reference scenarios for the ORIGINAL game (Wine, private Xvfb, sandbox copy).

    python3 tools/reference/capture.py setup|launcher|vsetting|devicelog|session|resolutions ...

Artifacts: work/reference/{shots,video,logs}; every scenario writes logs/<scenario>.steps.tsv.
Coordinates are 1024x768 game-screen pixels (default CONFIG.DAT mode).
"""
import sys, time
from joa_ref import *  # noqa: F403

MENU = dict(new=(258, 256), load=(258, 332), options=(258, 408), credits=(258, 483), quit=(258, 559))
OPT = dict(video=(508, 341), audio=(508, 421), input=(508, 501), game=(508, 581))


def intro_to_menu(prefix):
    """Each Escape skips the movie currently playing (enlight.wmv, then intro.wmv)."""
    wait_window("Wars & Warriors: Joan of Arc")
    time.sleep(4); shot(f"{prefix}_enlight_logo", "enlight.wmv playing")
    key("Escape", note="Escape: skip enlight.wmv"); time.sleep(6)
    shot(f"{prefix}_intro_movie", "intro.wmv playing")
    key("Escape", note="Escape: skip intro.wmv"); time.sleep(8)
    shot(f"{prefix}_main_menu", "main menu")


def remap_pages(prefix):
    gclick(287, 727, note="REMAP KEYS")
    shot(f"{prefix}_remap_0", "remap page (initial)")
    for i in range(1, 7):
        gclick(518, 259, wait=1.5, note="next remap category")
        shot(f"{prefix}_remap_{i}", f"remap page {i}")
    gclick(737, 727, note="CANCEL")


def session():
    """Menus -> new campaign (Normal) -> mission 1 -> controls, HUD panels, save/load."""
    reset_state()
    start("joan.exe", "s04_session")
    intro_to_menu("s04_01")
    p = "s04_02"
    gclick(*MENU["options"]); shot(f"{p}_options")
    gclick(*OPT["video"]); shot(f"{p}_video_options"); gclick(586, 727, note="BACK")
    gclick(*OPT["audio"]); shot(f"{p}_audio_options"); gclick(558, 727, note="BACK")
    gclick(*OPT["input"]); shot(f"{p}_input_options"); remap_pages(p); gclick(737, 727, note="BACK")
    gclick(*OPT["game"]); shot(f"{p}_game_options"); gclick(512, 727, note="BACK")
    gclick(512, 727, note="BACK to main menu"); shot(f"{p}_main_menu_again")
    p = "s04_03"
    gclick(*MENU["credits"], wait=3); shot(f"{p}_credits_3s"); time.sleep(7); shot(f"{p}_credits_10s")
    key("Escape"); time.sleep(3); shot(f"{p}_after_credits_escape")
    gclick(*MENU["load"], wait=3); shot(f"{p}_load_campaign")
    key("Escape"); time.sleep(3); shot(f"{p}_after_load_escape")
    p = "s04_04"
    gclick(*MENU["new"], wait=3); shot(f"{p}_difficulty")
    gclick(258, 297, wait=1, note="NORMAL"); shot(f"{p}_loading_1s")
    time.sleep(6); shot(f"{p}_loading_7s")
    time.sleep(15); shot(f"{p}_mission1_cutscene")
    time.sleep(8); shot(f"{p}_mission1_cutscene_b")
    key("Escape", note="Escape: skip cutscene"); time.sleep(5); shot(f"{p}_mission1_hud")
    time.sleep(5); shot(f"{p}_mission1_tutorial_msg")
    key("ctrl", note="Ctrl: close attention message"); time.sleep(1)
    p = "s04_05"
    rec = record(f"{p}_joan_move_attack", 30, fps=15)
    hold("w", 3, note="W: run forward 3s"); rel(300, 0); step("mouse +300px x: turn right")
    hold("w", 2); button(1); time.sleep(0.6); button(1); time.sleep(0.6); button(1); step("LMB x3: light attacks")
    time.sleep(1); button(3); step("RMB: heavy attack"); time.sleep(1.5)
    hold("q", 2, note="Q: defend 2s"); key("space", note="Space: jump"); time.sleep(1.5)
    key("c", note="C: energy attack"); time.sleep(2)
    hold("shift+w", 3, note="Shift+W: walk"); hold("a", 1.5, note="A: strafe left"); hold("d", 1.5, note="D: strafe right")
    hold("s", 2, note="S: back"); rec.wait()
    shot(f"{p}_after_moves")
    p = "s04_06"
    for k, name in (("F1", "character_details"), ("m", "message_log"), ("F12", "mission_brief")):
        key(k); time.sleep(2.5); shot(f"{p}_{name}"); key("Escape"); time.sleep(2); shot(f"{p}_{name}_closed")
    key("o"); time.sleep(1.5); shot(f"{p}_objective"); key("o")
    key("Tab"); time.sleep(1.5); shot(f"{p}_minimap_zoom"); key("Tab")
    key("F11"); time.sleep(1.5); shot(f"{p}_hitpoint_bars"); key("F11")
    for _ in range(5):
        button(4); time.sleep(0.2)
    step("wheel up x5: zoom in"); time.sleep(1); shot(f"{p}_zoom_in")
    for _ in range(10):
        button(5); time.sleep(0.2)
    step("wheel down x10: zoom out"); time.sleep(1); shot(f"{p}_zoom_out")
    key("F2", note="F2: RTS mode"); time.sleep(3); shot(f"{p}_rts_mode")
    key("2", note="2: select Jean de Metz"); time.sleep(1.5); shot(f"{p}_rts_select_jean")
    rel(-4000, -4000, step_px=200); rel(700, 500); button(3); step("cursor to 700,500; RMB: order move")
    time.sleep(4); shot(f"{p}_rts_move_order")
    key("F2", note="F2: back to action mode"); time.sleep(3); shot(f"{p}_action_mode")
    p = "s04_07"
    key("F5", note="F5: quick save"); time.sleep(3); shot(f"{p}_quicksave")
    key("F7"); time.sleep(2.5); shot(f"{p}_save_menu"); key("Escape"); time.sleep(2)
    key("F8"); time.sleep(2.5); shot(f"{p}_load_menu"); key("Escape"); time.sleep(2)
    key("F9"); time.sleep(2.5); shot(f"{p}_options_ingame"); key("Escape"); time.sleep(2)
    key("Escape", note="Esc: in-game menu"); time.sleep(2.5); shot(f"{p}_ingame_menu"); key("Escape"); time.sleep(2)
    hold("w", 3); key("F6", note="F6: quick load"); time.sleep(3); shot(f"{p}_quickload_prompt")
    gclick(415, 399, wait=15, note="OK (load last Quick Save)"); shot(f"{p}_quickload_done")
    p = "s04_08"
    rec = record(f"{p}_advance_and_fight", 45, fps=15)
    hold("w", 8, note="W: advance 8s toward 'enemy just ahead'")
    for _ in range(4):
        for _ in range(3):
            button(1); time.sleep(0.5)
        button(3); time.sleep(1.2)
    step("4x combo AAAB (LMB x3, RMB)")
    hold("q", 2, note="Q: defend"); key("c", note="C: energy attack"); time.sleep(3)
    rec.wait(); shot(f"{p}_after_fight")
    stop()
    step_listing = sorted(f"{f.name} {f.stat().st_size}" for f in (COPY / "Save").iterdir())
    (REF / "logs/s04_session.save_dir.txt").write_text("\n".join(step_listing) + "\n")


def launcher():
    """JoanofArc.exe -> WinExec(JOAN.EXE); movies left to play 40 s (recorded), then skipped."""
    reset_state()
    start("JoanofArc.exe", "s01_launcher", debug="+timestamp,+process")
    wait_window("Wars & Warriors: Joan of Arc")
    rec = record("s01_startup_movies", 40, fps=10)
    time.sleep(4); shot("s01_enlight_logo", "enlight.wmv playing")
    time.sleep(16); shot("s01_intro_movie_20s", "movie at ~20 s")
    rec.wait(); shot("s01_intro_movie_42s", "movie at ~42 s")
    key("Escape", note="Escape: skip current movie"); time.sleep(8); shot("s01_after_escape")
    stop()


def vsetting():
    reset_state()
    start("VSetting.exe", "s02_vsetting", debug="+timestamp,+reg")
    win = wait_window("Wars & Warriors: Joan of Arc"); time.sleep(3)
    x, y, w, h = geometry(win)
    shot("s02_vsetting_dialog", f"VSetting dialog at {x},{y} {w}x{h}")
    for name, cy in (("device", 77), ("video_mode", 119), ("reflection", 163), ("bump", 206)):
        click(x + 362, y + cy, note=f"open {name} combo"); time.sleep(1.5)
        shot(f"s02_vsetting_{name}_list", f"{name} dropdown open")
        key("Escape"); time.sleep(0.5)
    click(x + 342, y + 243, note="Cancel"); time.sleep(2)
    stop()


def devicelog():
    """+d3d8,+d3d for device creation only (trace is ~400 MB/25 s; kept: first 5000 lines)."""
    reset_state()
    start("joan.exe", "s03_devicelog", debug="+timestamp,+d3d8,+d3d")
    wait_window("Wars & Warriors: Joan of Arc"); time.sleep(6)
    stop()
    log = REF / "logs/s03_devicelog.log"
    with open(log) as f:
        head = [next(f, "") for _ in range(5000)]
    log.write_text("".join(head))


def resolutions(modes=((640, 480), (800, 600), (1280, 720), (1280, 1024), (1920, 1080))):
    """Patch the sandbox CONFIG.DAT mode (all modes exist in the X mode list), start joan.exe, record the
    X screen size Wine switched to + d3d8 mode selection. Observed: always 1024x768 (see ORIGINAL_BEHAVIOR.md)."""
    for w, h in modes:
        reset_state(); set_video(w, h)
        s = f"s05_cfg{w}x{h}"
        start("joan.exe", s, debug="+timestamp,+d3d8")
        intro_to_menu(s)
        step("X screen " + sh("xdpyinfo").split("dimensions:")[1].split()[0])
        stop()
        _truncate(REF / f"logs/{s}.log", 4000)


def _truncate(log, n):
    """d3d8 traces every call each frame; keep the startup part (mode enumeration + CreateDevice)."""
    with open(log, errors="replace") as f:
        head = [next(f, "") for _ in range(n)]
    log.write_text("".join(head))


def combat():
    """Mission 1 from a fresh campaign: run straight ahead (initial facing) toward the tutorial's
    'enemy is just ahead', then fight with AAAB combos; recorded."""
    reset_state()
    start("joan.exe", "s08_combat")
    intro_to_menu("s08")
    gclick(*MENU["new"], wait=3); gclick(258, 297, wait=30, note="NORMAL")
    key("Escape", note="Escape: skip cutscene"); time.sleep(5); key("ctrl"); time.sleep(1)
    rec = record("s08_combat", 75, fps=15)
    for i in range(3):
        hold("w", 5, note="W: run straight 5s"); shot(f"s08_run_{i}")
    for i in range(8):
        for _ in range(3):
            button(1); time.sleep(0.45)
        button(3); time.sleep(1.0)
        hold("w", 0.6)
        if i % 2:
            shot(f"s08_fight_{i}")
    step("8x (AAAB combo + short advance)")
    rec.wait(); shot("s08_end")
    stop()


def fileorder():
    """+file trace: startup -> menu -> New Campaign/Normal -> mission 1 loaded. Slow under tracing."""
    reset_state()
    start("joan.exe", "s06_fileorder", debug="+timestamp,+file")
    wait_window("Wars & Warriors: Joan of Arc")
    time.sleep(20); key("Escape", note="Escape: skip enlight.wmv")
    time.sleep(20); key("Escape", note="Escape: skip intro.wmv")
    time.sleep(40); shot("s06_main_menu")
    gclick(*MENU["new"], wait=5); shot("s06_difficulty")
    gclick(258, 297, wait=90, note="NORMAL"); shot("s06_mission1")
    stop()


def nomode():
    """1024x768 removed from the X mode list -> does joan.exe start? Then 1024x768x16 (VSetting's other choice)."""
    reset_state()
    sh("xrandr", "--delmode", "screen", "1024x768_60.00")
    start("joan.exe", "s07_no1024_mode", debug="+timestamp,+d3d8,+process")
    time.sleep(40); step("joan.exe alive=" + str(_alive())); shot("s07_no1024_mode_40s")
    stop()
    sh("xrandr", "--addmode", "screen", "1024x768_60.00")
    reset_state(); set_video(1024, 768, bpp=16)
    start("joan.exe", "s07_1024x768x16", debug="+timestamp,+d3d8")
    intro_to_menu("s07_1024x768x16")
    stop()
    _truncate(REF / "logs/s07_1024x768x16.log", 4000)


def _alive():
    return "joan.exe" in sh("ps", "-eo", "args")


if __name__ == "__main__":
    setup()
    for name in sys.argv[1:]:
        globals()[name]()
