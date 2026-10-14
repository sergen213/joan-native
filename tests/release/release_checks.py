#!/usr/bin/env python3
"""Release pipeline checks (CTest release.*, registered by tools/release-checks/CMakeLists.txt).

  release_checks.py selftest
  release_checks.py dist          --out DIR --ffmpeg-prefix P            build DIR/dist (dev-key signed)
  release_checks.py reproducible  --out DIR --ffmpeg-prefix P            rebuild elsewhere, byte-compare
  release_checks.py appimage      --out DIR                              run the AppImage: CLI, offscreen, X11, Wayland
  release_checks.py product-update --out DIR --testhttp BIN              0.1.0 -> 0.1.1 -> tampered -> rollback
  release_checks.py install-flow  --out DIR --game-dir G                 import -> verify -> desktop entry -> Play
  release_checks.py license-audit --out DIR [--game-dir G]               inventory/licenses/no game data

DIR/dist comes from `dist` (CTest orders it first). A check exits 77 (skipped) when its inputs are
absent: no FFmpeg prefix (packaging/ffmpeg/build_ffmpeg.py), no dist, no game data. Displays are
always private (Xvfb, kwin_wayland --virtual); the user's desktop session is never used.
"""
from __future__ import annotations

import argparse
import functools
import hashlib
import http.server
import io
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if not (Path("/dev/fuse").exists() and shutil.which("fusermount3")):  # containers/CI: no FUSE mount
    os.environ["APPIMAGE_EXTRACT_AND_RUN"] = "1"
RELEASE = ROOT / "packaging/release"
SKIP = 77
sys.path.insert(0, str(RELEASE))
import make_release  # noqa: E402  (GAME pattern, deterministic tarball writer)


def run(*args, env=None, check=True, timeout=600, cwd=None) -> subprocess.CompletedProcess:
    result = subprocess.run([str(a) for a in args], env=env, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    if check and result.returncode:
        raise AssertionError(f"{args} failed {result.returncode}:\n{result.stdout[-4000:]}\n{result.stderr[-4000:]}")
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def project_version() -> str:
    return re.search(r"project\(joan_native VERSION (\S+)", (ROOT / "CMakeLists.txt").read_text()).group(1)


def epoch() -> str:
    if os.environ.get("SOURCE_DATE_EPOCH"):
        return os.environ["SOURCE_DATE_EPOCH"]
    git = subprocess.run(["git", "-C", str(ROOT), "log", "-1", "--format=%ct"], capture_output=True, text=True)
    return git.stdout.strip() or "1700000000"


def dev_key(out: Path) -> Path:
    """Throwaway signing key shared by both reproducibility builds (Ed25519 signatures are deterministic)."""
    key = out / "dev-ed25519.key"
    if not key.exists():
        out.mkdir(parents=True, exist_ok=True)
        run(sys.executable, RELEASE / "sign_manifest.py", "--generate-key", key)
    return key


def build_dist(out: Path, ffmpeg: Path, work: Path, dist: Path, umask: int = 0o022) -> None:
    env = {**os.environ, "SOURCE_DATE_EPOCH": epoch()}
    env.pop("JOA_UPDATE_SIGNING_KEY", None)
    previous = os.umask(umask)
    try:
        run(sys.executable, RELEASE / "build_release.py", "--version", project_version(), "--channel", "stable",
            "--ffmpeg-prefix", ffmpeg, "--work-dir", work, "--output-dir", dist, "--sign-key", dev_key(out),
            env=env, timeout=7200)
    finally:
        os.umask(previous)


def require_dist(out: Path) -> Path:
    dist = out / "dist"
    if not (dist / "SHA256SUMS").exists():
        print(f"SKIP: no release dist at {dist} (release.dist did not run)")
        sys.exit(SKIP)
    return dist


def appimage_of(dist: Path) -> Path:
    return next(dist.glob("JoanNative-*-x86_64.AppImage"))


def extract_appimage(image: Path, into: Path) -> Path:
    run(image, "--appimage-extract", cwd=into)
    return into / "squashfs-root"


def tar_members(path: Path) -> dict[str, bytes]:
    with tarfile.open(path) as archive:
        return {m.name: archive.extractfile(m).read() for m in archive.getmembers() if m.isfile()}


def free_display() -> int:
    for number in range(140, 400):
        if not Path(f"/tmp/.X11-unix/X{number}").exists() and not Path(f"/tmp/.X{number}-lock").exists():
            return number
    raise AssertionError("no free X display number")


class Xvfb:
    """Private Xvfb server (llvmpipe through GLX); never the user's display."""
    def __enter__(self):
        self.display = f":{free_display()}"
        self.process = subprocess.Popen(["Xvfb", self.display, "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if Path(f"/tmp/.X11-unix/X{self.display[1:]}").exists():
                return self
            time.sleep(0.1)
        raise AssertionError("Xvfb did not start")

    def __exit__(self, *_):
        self.process.terminate()
        self.process.wait(timeout=10)


class KWinVirtual:
    """Private headless kwin_wayland compositor in its own D-Bus session and runtime dir."""
    def __init__(self, scratch: Path):
        self.runtime = scratch / "runtime"
        self.runtime.mkdir(mode=0o700, parents=True)
        self.socket = f"joa-release-{os.getpid()}"

    def __enter__(self):
        env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS")}
        env.update(XDG_RUNTIME_DIR=str(self.runtime), QT_QPA_PLATFORM="offscreen", KWIN_COMPOSE="Q")
        self.process = subprocess.Popen(["dbus-run-session", "--", "kwin_wayland", "--virtual", "--no-lockscreen",
                                         "--socket", self.socket, "--width", "1280", "--height", "800"],
                                        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        for _ in range(150):
            if (self.runtime / self.socket).exists():
                return self
            time.sleep(0.1)
        raise AssertionError("kwin_wayland --virtual did not create its socket")

    def __exit__(self, *_):
        os.killpg(self.process.pid, signal.SIGTERM)
        self.process.wait(timeout=20)


def isolated_env(home: Path, **extra) -> dict:
    """XDG root under home; no user session variables leak in."""
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "APPIMAGE",
                        "LD_LIBRARY_PATH", "LD_PRELOAD") and not k.startswith(("QT_", "SDL_", "GTK_"))}
    for name in ("data", "config", "state", "cache"):
        (home / name).mkdir(parents=True, exist_ok=True)
    env.update(HOME=str(home), XDG_DATA_HOME=str(home / "data"), XDG_CONFIG_HOME=str(home / "config"),
               XDG_STATE_HOME=str(home / "state"), XDG_CACHE_HOME=str(home / "cache"), **extra)
    return env


def snapshot(root: Path) -> dict[str, str]:
    result = {}
    for path in sorted(root.rglob("*")):
        rel = str(path.relative_to(root))
        result[rel] = "->" + os.readlink(path) if path.is_symlink() else sha256(path) if path.is_file() else "dir"
    return result


# ---------------------------------------------------------------- selftest (cheap, always runs)
def selftest(_args) -> None:
    with tempfile.TemporaryDirectory(prefix="joa-release-selftest-") as scratch:
        work = Path(scratch)
        prefix = work / "prefix"
        (prefix / "bin").mkdir(parents=True)
        (prefix / "lib").mkdir()
        for name in ("joa-updater", "joan-native", "joa-launcher"):
            (prefix / "bin" / name).write_text("#!/bin/sh\n")
            (prefix / "bin" / name).chmod(0o755)
        (prefix / "lib/libx.so.1").write_bytes(b"x")
        (prefix / "lib/libx.so").symlink_to("libx.so.1")
        outputs = []
        for index, mask in enumerate((0o022, 0o077)):
            os.utime(prefix / "lib/libx.so.1", (index * 1000, index * 1000))
            previous = os.umask(mask)
            try:
                run(sys.executable, RELEASE / "make_release.py", "--prefix", prefix, "--version", "1.2.3",
                    "--channel", "stable", "--min-glibc", "2.17", "--output-dir", work / f"out{index}")
            finally:
                os.umask(previous)
            outputs.append({p.name: p.read_bytes() for p in (work / f"out{index}").iterdir()})
        assert outputs[0] == outputs[1], "make_release.py output depends on mtimes/umask"
        for planted in ("share/joan-native/JOAN.RES", "share/movie/intro.WMV", "lib/x.am3"):
            assert make_release.GAME.fullmatch(planted), planted
        assert not make_release.GAME.fullmatch("lib/libQt6Core.so.6")
        key = work / "k"
        run(sys.executable, RELEASE / "sign_manifest.py", "--generate-key", key)
        manifest = work / "out0/manifest.json"
        run(sys.executable, RELEASE / "sign_manifest.py", manifest, "--key", key)
        public = run(sys.executable, RELEASE / "sign_manifest.py", "--public-key", "--key", key).stdout.strip()
        run(sys.executable, RELEASE / "sign_manifest.py", "--verify", manifest, "--public", public)
        manifest.write_text(manifest.read_text() + " ")
        assert run(sys.executable, RELEASE / "sign_manifest.py", "--verify", manifest, "--public", public, check=False).returncode
        import yaml
        for workflow in ("ci.yml", "release.yml"):
            jobs = yaml.safe_load((ROOT / ".github/workflows" / workflow).read_text())["jobs"]
            assert all(step.get("run") or step.get("uses") for job in jobs.values() for step in job["steps"]), workflow
        known = game_hashes()
        assert len(known) > 1000, "known_versions.json lost its file hashes"
    print("release selftest: deterministic tarballs, game-data refusal, signature verify/tamper, workflow YAML: OK")


# ---------------------------------------------------------------- dist + reproducibility
def dist(args) -> None:
    if not args.ffmpeg_prefix or not (Path(args.ffmpeg_prefix) / "share/joan-native/ffmpeg-source.json").exists():
        print("SKIP: JOA_FFMPEG_PREFIX is not an LGPL FFmpeg prefix (packaging/ffmpeg/build_ffmpeg.py)")
        sys.exit(SKIP)
    out = Path(args.out)
    for name in ("w", "dist"):
        shutil.rmtree(out / name, ignore_errors=True)
    build_dist(out, Path(args.ffmpeg_prefix), out / "w", out / "dist")
    for line in (out / "dist/SHA256SUMS").read_text().splitlines():
        digest, name = line.split("  ")
        print(f"{name}  {(out / 'dist' / name).stat().st_size}  {digest}")


def reproducible(args) -> None:
    out = Path(args.out)
    first = require_dist(out)
    # Different directory name, depth (with a space), umask and TMPDIR than release.dist.
    other = out / "repro" / "second build dir"
    shutil.rmtree(out / "repro", ignore_errors=True)
    (other / "tmp").mkdir(parents=True)
    os.environ["TMPDIR"] = str(other / "tmp")
    build_dist(out, Path(args.ffmpeg_prefix), other / "work", other / "dist", umask=0o077)
    second = other / "dist"
    names = sorted(p.name for p in first.iterdir())
    assert names == sorted(p.name for p in second.iterdir()), (names, sorted(p.name for p in second.iterdir()))
    differing = [n for n in names if sha256(first / n) != sha256(second / n)]
    for name in differing:
        if name.endswith(".tar.gz") or name.endswith(".tar"):
            a, b = tar_members(first / name), tar_members(second / name)
            print(f"{name}: members differing: {sorted(k for k in a.keys() | b.keys() if a.get(k) != b.get(k))[:20]}")
    assert not differing, f"not reproducible: {differing}"
    for name in names:
        print(f"identical  {sha256(first / name)}  {name}")
    shutil.rmtree(out / "repro")
    print(f"release.reproducible: {len(names)} artifacts byte-identical across build directories/umask/TMPDIR")


# ---------------------------------------------------------------- AppImage
HOST_ONLY = re.compile(r"(libGL|libGLX|libEGL|libOpenGL|libGLdispatch|libvulkan|libdrm|libgbm|libc|libm|libstdc\+\+"
                       r"|libgcc_s|libwayland-client|libX11|libxcb)\.so(\.\d+)*|.*_dri\.so|.*nvidia.*|.*icd\.json")


def appimage(args) -> None:
    out = Path(args.out)
    dist_dir = require_dist(out)
    image = appimage_of(dist_dir)
    version = project_version()
    with tempfile.TemporaryDirectory(prefix="joa-appimage-") as scratch:
        work = Path(scratch)
        root = extract_appimage(image, work)
        usr = root / "usr"
        # payload: our 4 programs, Qt wayland+xcb+offscreen platform plugins, LGPL FFmpeg, SDL3, notices
        for name in ("joan-native", "joa-launcher", "joa-install", "joa-updater"):
            assert (usr / "bin" / name).is_file(), name
        for plugin in ("libqwayland.so", "libqxcb.so", "libqoffscreen.so"):
            assert (usr / "lib/qt6/plugins/platforms" / plugin).is_file(), plugin
        assert (usr / "lib/qt6/plugins/wayland-shell-integration/libxdg-shell.so").is_file()
        assert (usr / "lib/libSDL3.so.0").is_file()
        ffmeta = json.loads((usr / "share/joan-native/ffmpeg-source.json").read_text())
        for library in ffmeta["built_libraries"]:
            assert sha256(usr / "lib" / Path(library["path"]).name) == library["sha256"], library
            assert (usr / "lib" / library["soname"]).exists()
        for notice in ("share/doc/joan-native/NOTICE.txt", "share/doc/joan-native/THIRD_PARTY_INVENTORY.json",
                       "share/licenses/FFmpeg/LICENSE.txt", "share/licenses/qt6-base/LGPL-3.0-only.txt",
                       "share/licenses/sdl3/LICENSE", "share/licenses/appimage-type2-runtime/libfuse-LICENSE.txt"):
            assert (usr / notice).is_file(), notice
        leaks = [str(p.relative_to(root)) for p in root.rglob("*") if p.is_file() and not p.is_symlink()
                 and any(marker in p.read_bytes() for marker in (str(ROOT).encode(), str(out).encode(), b"/joa-ffmpeg-build-"))]
        assert not leaks, f"build/source paths embedded in: {leaks}"
        shipped = [p for p in root.rglob("*") if p.is_file() or p.is_symlink()]
        host = [str(p.relative_to(root)) for p in shipped if HOST_ONLY.fullmatch(p.name)]
        assert not host, f"host GPU/driver/C runtime libraries bundled: {host}"
        # every executable/plugin resolves all its libraries inside the bundle or on the host, none missing
        clean = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
        elves = [usr / "bin" / n for n in ("joan-native", "joa-launcher", "joa-install", "joa-updater")]
        elves += sorted((usr / "lib/qt6/plugins").rglob("*.so"))
        for elf in elves:
            ldd = run("ldd", elf, env=clean).stdout
            assert "not found" not in ldd, f"{elf}: {ldd}"
            if elf.name == "joan-native":
                for soname in ("libavcodec.so.62", "libavformat.so.62", "libavutil.so.60", "libswscale.so.9",
                               "libswresample.so.6", "libSDL3.so.0"):
                    line = next(l for l in ldd.splitlines() if l.strip().startswith(soname))
                    assert str(usr / "lib") in line.replace("/bin/../lib", "/lib"), line
        print("ldd joan-native (bundled LGPL FFmpeg):")
        print("\n".join(l for l in run("ldd", usr / "bin/joan-native", env=clean).stdout.splitlines()
                        if re.search(r"libav|libsw|SDL3", l)))

        home = work / "home"
        env = isolated_env(home, QT_QPA_PLATFORM="offscreen")
        entry = lambda name, *a, e=None, check=True: run(image, *a, env={**(e or env), "JOA_APPIMAGE_ENTRY": name},
                                                         check=check, timeout=120)
        assert entry("joan-native", "--version").stdout.strip() == f"joan-native {version}"
        assert "--renderer" in entry("joan-native", "--help").stdout
        assert entry("joa-updater", "--version").stdout.startswith(f"joa-updater {version}")
        assert "import <path>" in entry("joa-install", "--help").stderr
        assert entry("../bin/sh", check=False).returncode == 2  # AppRun refuses path entries
        bundled = {p.name for p in (usr / "lib").rglob("*.so*")}

        def launcher_shot(name: str, platform: str, **extra) -> None:
            """Launcher screenshot; LD_DEBUG proves the platform plugin and every bundled library came from the AppImage."""
            shot = work / f"{name}.png"
            result = run(image, "--screenshot", shot, env={**env, "QT_QPA_PLATFORM": platform, "LD_DEBUG": "files", **extra},
                         timeout=120)
            assert shot.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", name
            loaded = re.findall(r"calling init: (\S+)", result.stderr)
            mount = [p for p in loaded if "/usr/lib/qt6/plugins/platforms/" in p]
            assert any(p.endswith(f"/libq{platform.replace('xcb', 'xcb')}.so") for p in mount), (platform, mount)
            assert all(not p.startswith("/usr/") for p in mount), f"host Qt platform plugin loaded: {mount}"
            leaked = sorted({p for p in loaded if p.startswith("/usr/") and Path(p).name in bundled})
            assert not leaked, f"bundled libraries loaded from the host instead: {leaked}"
            print(f"  {name}: libq{platform}.so + {len(loaded)} libraries, bundled ones all from the AppImage; "
                  f"screenshot {shot.stat().st_size} B")
        launcher_shot("offscreen", "offscreen")
        print("AppImage: joan-native/joa-updater/joa-install CLI + offscreen launcher OK")

        with Xvfb() as x:
            launcher_shot("x11", "xcb", DISPLAY=x.display)
            print(f"AppImage launcher on private Xvfb {x.display}: OK")
        if shutil.which("kwin_wayland") and shutil.which("dbus-run-session"):
            with KWinVirtual(work) as kwin:
                launcher_shot("wayland", "wayland", XDG_RUNTIME_DIR=str(kwin.runtime), WAYLAND_DISPLAY=kwin.socket)
                print(f"AppImage launcher on private kwin_wayland --virtual ({kwin.socket}): OK")
        else:
            print("NOTE: kwin_wayland not installed; Wayland launcher smoke skipped")


# ---------------------------------------------------------------- updater with the real product
def serve(directory: Path, repo: str):
    handler = functools.partial(type("Quiet", (http.server.SimpleHTTPRequestHandler,),
                                     {"log_message": lambda *a: None}), directory=str(directory))
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    port = httpd.server_address[1]
    listing = directory / "repos" / repo / "releases"
    listing.parent.mkdir(parents=True, exist_ok=True)

    def publish(*versions: str) -> None:
        listing.write_text(json.dumps([{"tag_name": "v" + v, "draft": False, "prerelease": "-" in v, "body": f"Notes {v}",
                                        "assets": [{"name": p.name, "size": p.stat().st_size,
                                                    "browser_download_url": f"http://127.0.0.1:{port}/{v}/{p.name}"}
                                                   for p in sorted((directory / v).iterdir())]} for v in versions]))
    return httpd, port, publish


def product_update(args) -> None:
    out = Path(args.out)
    dist_dir = require_dist(out)
    testhttp = Path(args.testhttp)
    key = dev_key(out)
    public = run(sys.executable, RELEASE / "sign_manifest.py", "--public-key", "--key", key).stdout.strip()
    version = project_version()
    with tempfile.TemporaryDirectory(prefix="joa-product-update-") as scratch:
        work = Path(scratch)
        server = work / "server"
        shutil.copytree(dist_dir, server / version, ignore=shutil.ignore_patterns("*.AppImage", "*source.tar", "SHA256SUMS"))
        manifest = json.loads((server / version / "manifest.json").read_text())
        assert sorted(a["component"] for a in manifest["artifacts"]) == ["engine", "launcher", "updater"], manifest
        run(sys.executable, RELEASE / "sign_manifest.py", "--verify", server / version / "manifest.json", "--public", public)

        def respin(new: str, tamper: bool = False) -> None:
            """Same product as `version`, relabelled (marker file) -> signed release `new`."""
            stage = work / f"stage-{new}"
            for artifact in sorted((server / version).glob("*.tar.gz")):
                with tarfile.open(artifact) as archive:
                    archive.extractall(stage, filter="tar")
            (stage / "share/joan-native/RELEASE").write_text(new + "\n")
            run(sys.executable, RELEASE / "make_release.py", "--prefix", stage, "--version", new, "--channel", "stable",
                "--min-glibc", manifest["min_glibc"], "--output-dir", server / new)
            run(sys.executable, RELEASE / "sign_manifest.py", server / new / "manifest.json", "--key", key)
            if tamper:
                victim = server / new / f"joan-native-{new}-linux-x86_64-engine.tar.gz"
                data = bytearray(victim.read_bytes())
                data[len(data) // 2] ^= 0x40
                victim.write_bytes(bytes(data))
        next1 = re.sub(r"\.(\d+)$", lambda m: f".{int(m.group(1)) + 1}", version)
        next2 = re.sub(r"\.(\d+)$", lambda m: f".{int(m.group(1)) + 2}", version)
        respin(next1)
        respin(next2, tamper=True)

        home = work / "home"
        env = isolated_env(home, QT_QPA_PLATFORM="offscreen")
        root = home / "data/joan-native"
        (root / "game/gog-test").mkdir(parents=True)
        (root / "game/gog-test/JOAN.RES").write_bytes(os.urandom(4096))
        (root / "game/current").symlink_to("gog-test")
        (root / "game/gog-test/Save").mkdir()
        (root / "game/gog-test/Save/slot1.sav").write_bytes(b"save")
        (root / "saves").mkdir()
        (root / "saves/native-1.json").write_text("{}")
        (root / "hd/hd-update/texture").mkdir(parents=True)
        (root / "hd/hd-update/texture/a.dds").write_bytes(b"DDS hd")
        (home / "config/joan-native").mkdir(parents=True)
        (home / "config/joan-native/settings.ini").write_text("[update]\nchannel=stable\n[renderer]\nbackend=vulkan\n")
        protected = lambda: {k: v for k, v in snapshot(home).items() if not k.startswith("data/joan-native/app")}
        untouched = protected()

        httpd, port, publish = serve(server, args.repo)
        try:
            uenv = {**env, "JOA_UPDATER_TEST_API": f"http://127.0.0.1:{port}", "JOA_UPDATER_TEST_PUBKEY": public}
            updater = lambda *a, check=True: run(testhttp, *a, "--json", env=uenv, check=check, timeout=600)
            app = root / "app"
            publish(version)
            applied = json.loads(updater("apply").stdout)
            assert applied["applied"] and applied["to"] == version, applied
            assert os.readlink(app / "current") == version
            installed = app / "current/bin"
            # The installed tarball payload is a self-contained product (DT_RPATH, bundled Qt/FFmpeg).
            assert run(installed / "joan-native", "--version", env=env).stdout.strip() == f"joan-native {version}"
            shot = work / "installed-launcher.png"
            run(installed / "joa-launcher", "--screenshot", shot, env=env, timeout=120)
            assert shot.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
            status = json.loads(run(installed / "joa-updater", "status", "--json", env=env).stdout)
            assert status["current"] == version and status["installed"] == [version], status
            assert "not found" not in run("ldd", installed / "joan-native", env=env).stdout

            publish(version, next1)
            check = json.loads(updater("check").stdout)
            assert check == {"current": version, "latest": next1, "available": True, "notes": f"Notes {next1}"}, check
            applied = json.loads(updater("apply").stdout)
            assert applied == {"applied": True, "from": version, "to": next1}, applied
            assert os.readlink(app / "current") == next1
            assert (app / "current/share/joan-native/RELEASE").read_text().strip() == next1
            assert protected() == untouched, "update touched game/, saves, settings or HD packs"

            before = snapshot(home)
            publish(version, next1, next2)
            rejected = updater("apply", check=False)
            assert rejected.returncode == 1 and "SHA-256" in rejected.stdout, rejected
            assert snapshot(home) == before, "tampered artifact changed the installation"

            rolled = json.loads(updater("rollback").stdout)
            assert rolled == {"from": next1, "to": version}, rolled
            assert os.readlink(app / "current") == version
            assert not (app / "current/share/joan-native/RELEASE").exists()
            assert protected() == untouched, "rollback touched game/, saves, settings or HD packs"
            # the AppImage's own (shipped, network-locked) updater sees the same installation
            image = appimage_of(dist_dir)
            status = json.loads(run(image, "status", "--json", env={**env, "JOA_APPIMAGE_ENTRY": "joa-updater"}).stdout)
            assert status["current"] == version and status["previous"] == next1, status
        finally:
            httpd.shutdown()
    print(f"release.product_update: apply {version} (tarballs) -> {next1} (current switch) -> tampered {next2} rejected "
          f"-> rollback {version}; game/, saves, settings, HD packs untouched")


# ---------------------------------------------------------------- full install flow
def install_flow(args) -> None:
    out = Path(args.out)
    dist_dir = require_dist(out)
    game = Path(args.game_dir or "")
    if not (game / "joan.exe").is_file():
        print(f"SKIP: no original game at {game}")
        sys.exit(SKIP)
    image = appimage_of(dist_dir)
    version = project_version()
    # Disk-backed scratch (~1 GiB import), never inside the game dir (joa-install refuses that).
    scratch = Path(tempfile.mkdtemp(prefix="joa-install-flow-", dir="/var/tmp" if Path("/var/tmp").is_dir() else None))
    home = scratch / "home"
    env = isolated_env(home, APPIMAGE=str(image))
    before = sha256(game / "joan.exe")
    imported = json.loads(run(image, "--json", "import", game, env={**env, "JOA_APPIMAGE_ENTRY": "joa-install"},
                              timeout=3600).stdout)
    assert imported["ok"] and imported["version"], imported
    verified = json.loads(run(image, "--json", "verify", env={**env, "JOA_APPIMAGE_ENTRY": "joa-install"}, timeout=3600).stdout)
    assert verified["ok"] and verified["checked"] > 9000 and not verified["missing"] and not verified["mismatched"], verified
    desktop = home / "data/applications/joan-native.desktop"
    text = desktop.read_text()
    assert f'Exec="{image}"' in text, text
    print(f"joa-install import/verify from AppImage: {verified['checked']} files, desktop entry Exec -> AppImage")

    settings = home / "config/joan-native/settings.ini"
    settings.parent.mkdir(parents=True, exist_ok=True)
    settings.write_text("[renderer]\nbackend=opengl\npreset=original\n[display]\nwidth=640\nheight=480\nmode=windowed\n")
    with Xvfb() as x:
        penv = {**env, "DISPLAY": x.display, "QT_QPA_PLATFORM": "offscreen", "SDL_VIDEO_DRIVER": "x11"}
        started = time.monotonic()
        run(image, "--play", "--", "--no-movies", "--frames", str(args.frames), env=penv, timeout=900)
        elapsed = time.monotonic() - started
    logs = sorted((home / "state/joan-native/logs").glob("game-*.log"))
    assert len(logs) == 1 and logs[0].stat().st_size > 0, logs
    log = logs[0].read_text(errors="replace")
    assert "error" not in log.lower() or "0 error" in log.lower(), log[-2000:]
    saved = settings.read_text()
    assert "backend=opengl" in saved and "width=640" in saved, saved
    assert sha256(game / "joan.exe") == before
    print(f"launcher --play (AppImage, private Xvfb llvmpipe): joan-native ran {args.frames} frames in {elapsed:.1f}s "
          f"with the launcher's settings; log {logs[0].name} ({logs[0].stat().st_size} B):")
    print("\n".join(log.splitlines()[-8:]))
    uninstall = json.loads(run(image, "--json", "uninstall", env={**env, "JOA_APPIMAGE_ENTRY": "joa-install"}).stdout)
    assert uninstall["ok"] and not desktop.exists()
    shutil.rmtree(scratch)
    print(f"release.install_flow {version}: import -> verify -> desktop entry -> Play -> log -> uninstall OK")


# ---------------------------------------------------------------- license / content audit
def game_hashes() -> set[str]:
    known = json.loads((ROOT / "src/install/known_versions.json").read_text())
    hashes = set()
    for version in known["versions"]:
        hashes.add(version["exe_sha256"])
        hashes.update(sha for _path, _size, sha in version["files"])
    return hashes


PERMISSIVE = re.compile(r"LGPL.*|MIT.*|BSD.*|0BSD|Apache-2\.0|Zlib|ISC|curl|X11.*|CC0-1\.0|libpng-2\.0|LicenseRef-.*"
                        r"|HPND.*|custom.*|Unicode-.*|NAIST-2003|OpenSSL|BSL-1\.0|MPL-2\.0|FTL|IJG|Libpng|blessing")


def license_audit(args) -> None:
    out = Path(args.out)
    dist_dir = require_dist(out)
    known = game_hashes()
    game_names = set()
    if args.game_dir and Path(args.game_dir, "joan.exe").is_file():
        game_names = {path.lower() for v in json.loads((ROOT / "src/install/known_versions.json").read_text())["versions"]
                      for path, _size, _sha in v["files"]}
    with tempfile.TemporaryDirectory(prefix="joa-license-audit-") as scratch:
        root = extract_appimage(appimage_of(dist_dir), Path(scratch))
        files: dict[str, str] = {}  # "<artifact>:<member>" -> sha256
        for path in root.rglob("*"):
            if path.is_file() and not path.is_symlink():
                files[f"AppImage:{path.relative_to(root)}"] = sha256(path)
        for artifact in sorted(dist_dir.glob("*.tar*")):
            for name, data in tar_members(artifact).items():
                files[f"{artifact.name}:{name}"] = hashlib.sha256(data).hexdigest()
        hits = [k for k, v in files.items() if v in known]
        assert not hits, f"original game files inside release artifacts: {hits[:10]}"
        named = [k for k in files if make_release.GAME.fullmatch(k.split(":", 1)[1])
                 or k.split(":", 1)[1].split("/", 1)[-1].lower() in game_names]
        assert not named, f"game-data-like names inside release artifacts: {named[:10]}"
        print(f"no original game data: {len(files)} shipped files checked against {len(known)} known_versions.json "
              f"hashes and the game-data name patterns")

        usr = root / "usr"
        inventory = json.loads((usr / "share/doc/joan-native/THIRD_PARTY_INVENTORY.json").read_text())
        covered = {f["path"]: f["sha256"] for p in inventory["bundled_packages"] for f in p["files"]}
        covered.update({f["path"]: f["sha256"] for f in inventory["ffmpeg"]["files"]})
        ours = {"bin/joan-native", "bin/joa-launcher", "bin/joa-install", "bin/joa-updater"}
        for path in usr.rglob("*"):
            rel = path.relative_to(usr).as_posix()
            if path.is_file() and not path.is_symlink() and path.read_bytes()[:4] == b"\x7fELF" and rel not in ours:
                assert covered.get(rel) == sha256(path), f"ELF not covered by the inventory: {rel}"
        texts = [t for p in inventory["bundled_packages"] + inventory["compiled_in"] for t in p["license_texts"]]
        texts += inventory["ffmpeg"]["license_texts"]
        missing = [t for t in texts if not (usr / "share" / t).is_file()]
        assert not missing, f"license texts missing: {missing}"
        for package in inventory["bundled_packages"]:
            ids = [i for i in re.findall(r"[A-Za-z0-9][A-Za-z0-9.+:-]*", package["license"]) if i not in ("AND", "OR", "WITH")]
            assert any(PERMISSIVE.fullmatch(i) for i in ids) or "GCC-exception-3.1" in ids, \
                f"bundled library without an LGPL/permissive option: {package['package']} {package['license']}"
        ff = inventory["ffmpeg"]
        flags = " ".join(ff["configure_args"])
        assert "--disable-gpl" in flags and "--enable-gpl" not in flags and "--enable-nonfree" not in flags \
            and "--enable-version3" not in flags and ff["license"] == "LGPL-2.1-or-later", ff
        for notice in ("libfuse-LICENSE.txt", "LICENSE.txt", "squashfuse-LICENSE.txt", "musl-LICENSE.txt"):
            assert (usr / "share/licenses/appimage-type2-runtime" / notice).is_file(), notice
        source = tar_members(next(dist_dir.glob("*-third-party-source.tar")))
        assert f"ffmpeg/ffmpeg-{ff['version']}.tar.xz" in source and "appimage-runtime/fuse-3.15.0.tar.xz" in source
        assert hashlib.sha256(source[f"ffmpeg/ffmpeg-{ff['version']}.tar.xz"]).hexdigest() == ff["source_sha256"]

        by_license: dict[str, list[str]] = {}
        for package in inventory["bundled_packages"]:
            by_license.setdefault(package["license"], []).append(f"{package['package']} {package['version']}")
        print("bundled libraries by license:")
        for license_id, packages in sorted(by_license.items()):
            print(f"  {license_id}: {', '.join(packages)}")
        print(f"  {ff['license']}: FFmpeg {ff['version']} (own minimal build, {len(ff['files'])} libraries)")
        print("compiled in: " + ", ".join(f"{c['name']} ({c['license']})" for c in inventory["compiled_in"]))
        print("host provided (not shipped): " + " ".join(inventory["host_provided"]))
    print("release.license_audit: inventory covers every shipped ELF; all license texts present; FFmpeg LGPL-only")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("check", choices=("selftest", "dist", "reproducible", "appimage", "product-update",
                                          "install-flow", "license-audit"))
    parser.add_argument("--out", default=str(ROOT / "work/release/checks"))
    parser.add_argument("--ffmpeg-prefix", default="")
    parser.add_argument("--testhttp", default="")
    parser.add_argument("--repo", default="sergen213/joan-native")
    parser.add_argument("--game-dir", default="")
    parser.add_argument("--frames", type=int, default=120)
    args = parser.parse_args()
    args.out = str(Path(args.out).resolve())
    {"selftest": selftest, "dist": dist, "reproducible": reproducible, "appimage": appimage,
     "product-update": product_update, "install-flow": install_flow, "license-audit": license_audit}[args.check](args)


if __name__ == "__main__":
    main()
