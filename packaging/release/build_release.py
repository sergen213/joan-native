#!/usr/bin/env python3
"""Build every joan-native release artifact, reproducibly (release workflow and local releases).

  build_release.py --version 0.1.0 --channel stable --ffmpeg-prefix work/tools/ffmpeg-prefix \
      --work-dir WORK --output-dir DIST [--repo OWNER/NAME] [--sign-key KEY | env JOA_UPDATE_SIGNING_KEY]

1. Release CMake build of joan-native, joa-launcher, joa-install, joa-updater (JOA_REPRODUCIBLE,
   DT_RPATH $ORIGIN/../lib, no tests) against the LGPL FFmpeg prefix (packaging/ffmpeg/), installed
   into WORK/stage = the update payload and the AppImage's usr/.
2. Bundles: the FFmpeg shared libraries + notices, Qt platform plugins (wayland, xcb, offscreen) under
   lib/qt6/plugins (Arch's own relative layout, so their $ORIGIN RUNPATHs keep working) and the
   DT_NEEDED closure of everything shipped, except HOST (glibc, libstdc++, GPU drivers/loaders,
   X11/wayland/font client libraries: always the host's own).
3. License inventory (share/doc/joan-native/THIRD_PARTY_INVENTORY.json, generated from pacman's owner,
   version and SPDX license of every bundled file) + license texts under share/licenses/.
4. DIST: per-component update tarballs + manifest.json (make_release.py), JoanNative-V-x86_64.AppImage
   (pinned type2 runtime + reproducible squashfs), joan-native-V-third-party-source.tar (FFmpeg and
   AppImage-runtime corresponding source), manifest.json.sig when a key is given, SHA256SUMS.
Same sources + SOURCE_DATE_EPOCH (default: HEAD commit time) => byte-identical DIST for any WORK.
Requires Arch Linux (pacman metadata), cmake, ninja, readelf, mksquashfs >= 4.6.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGING = ROOT / "packaging"
TARGETS = ("joan-native", "joa-launcher", "joa-install", "joa-updater")
QT_PLUGINS = {  # plugin dir -> files (None = all)
    "platforms": ("libqwayland.so", "libqxcb.so", "libqoffscreen.so", "libqminimal.so"),
    "wayland-shell-integration": ("libxdg-shell.so", "libqt-shell.so"),
    "wayland-decoration-client": ("libbradient.so",),
    "wayland-graphics-integration-client": ("libqt-plugin-wayland-egl.so", "libdmabuf-server.so", "libshm-emulation-server.so"),
    "xcbglintegrations": ("libqxcb-glx-integration.so", "libqxcb-egl-integration.so"),
    "imageformats": ("libqsvg.so", "libqico.so"),
    "iconengines": ("libqsvgicon.so",),
    "platforminputcontexts": ("libcomposeplatforminputcontextplugin.so", "libibusplatforminputcontextplugin.so"),
    "platformthemes": ("libqxdgdesktopportal.so",),
}
# Always the host's: C runtime, compiler runtime, GPU drivers/loaders and the display/font client
# libraries those drivers link themselves (bundling them breaks Mesa/NVIDIA on the user's system).
HOST = re.compile(r"(ld-linux-x86-64|libc|libm|libdl|libpthread|librt|libresolv|libutil|libanl|libmvec|libnss_.*"
                  r"|libstdc\+\+|libgcc_s|libatomic|libGL|libGLX|libGLdispatch|libOpenGL|libEGL|libGLESv2|libvulkan"
                  r"|libdrm|libgbm|libX11|libX11-xcb|libxcb|libxcb-(dri2|dri3|glx|present|sync|xfixes|shm|randr|render|xkb)"
                  r"|libXau|libXdmcp|libXext|libXrender|libXi|libXfixes|libwayland-(client|cursor|egl|server)"
                  r"|libfontconfig|libfreetype|libharfbuzz|libexpat|libz|libudev|libasound|libsystemd|libdbus-1)\.so(\.\d+)*")
STATIC_COMPONENTS = (  # compiled into our binaries (header-only / embedded); license texts come from the package
    {"name": "GLM", "package": "glm", "license": "MIT", "use": "math (headers, joan-native)"},
    {"name": "nlohmann/json", "package": "nlohmann-json", "license": "MIT", "use": "JSON (headers, joa-install, joa-updater)"},
    {"name": "Vulkan-Headers", "package": "vulkan-headers", "license": "Apache-2.0 OR MIT", "use": "Vulkan API headers (joan-native)"},
    {"name": "AMD FidelityFX FSR 1", "package": None, "license": "MIT", "use": "EASU/RCAS shader algorithms (shaders/common/post_easu.glsl, post_rcas.glsl)",
     "text": "packaging/licenses/FidelityFX-FSR1-LICENSE.txt"},
    {"name": "AMD FidelityFX SDK v1.1.4 (FSR 3.1.4)", "package": None, "license": "MIT",
     "use": "FSR 3 upscaler, frame interpolation, optical flow + Vulkan backend (third_party/fidelityfx-sdk, upscale=fsr3)",
     "text": "packaging/licenses/FidelityFX-SDK-LICENSE.txt"},
    {"name": "SMAA (Jimenez et al.)", "package": None, "license": "MIT",
     "use": "SMAA 1x shader library + area/search lookup textures (third_party/smaa, aa=smaa)", "text": "packaging/licenses/SMAA-LICENSE.txt"},
    {"name": "RenderDoc in-application API", "package": None, "license": "MIT",
     "use": "third_party/renderdoc/renderdoc_app.h (joan-native --renderdoc)", "text": "packaging/licenses/RenderDoc-app-API-LICENSE.txt"},
)


def fail(message: str) -> None:
    sys.exit("build_release: " + message)


def run(*args, env=None, cwd=None, capture=False) -> str:
    result = subprocess.run([str(a) for a in args], env=env, cwd=cwd, check=False, text=True,
                            stdout=subprocess.PIPE if capture else None)
    if result.returncode:
        fail(f"command failed ({result.returncode}): {' '.join(str(a) for a in args)}")
    return result.stdout or ""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fetch(cache: Path, name: str, url: str, digest: str) -> Path:
    """Cached download; any digest mismatch is fatal (asset URLs need an octet-stream Accept)."""
    target = cache / name
    if not target.exists():
        cache.mkdir(parents=True, exist_ok=True)
        request = urllib.request.Request(url, headers={"Accept": "application/octet-stream"})
        with urllib.request.urlopen(request, timeout=300) as response:
            data = response.read()
        if sha256(data) != digest:
            fail(f"SHA-256 mismatch downloading {url}")
        target.write_bytes(data)
    if sha256(target.read_bytes()) != digest:
        fail(f"SHA-256 mismatch: {target}")
    return target


def needed(path: Path) -> tuple[list[str], list[str]]:
    """(DT_NEEDED sonames, RUNPATH/RPATH entries) via readelf."""
    out = run("readelf", "-dW", path, capture=True)
    return re.findall(r"\(NEEDED\).*\[(.+?)\]", out), re.findall(r"\((?:RUNPATH|RPATH)\).*\[(.+?)\]", out)


def is_elf(path: Path) -> bool:
    with path.open("rb") as f:
        return f.read(4) == b"\x7fELF"


def system_libraries() -> dict[str, Path]:
    libs = {}
    for line in run("ldconfig", "-p", capture=True).splitlines():
        match = re.match(r"\s+(\S+) \((libc6,x86-64[^)]*)\) => (\S+)", line)
        if match:
            libs.setdefault(match.group(1), Path(match.group(3)))
    return libs


def bundle_closure(stage: Path) -> tuple[dict[str, Path], set[str]]:
    """Copy the DT_NEEDED closure of every shipped ELF into lib/. Returns (soname -> source, host sonames)."""
    system, lib = system_libraries(), stage / "lib"
    bundled, host = {}, set()
    queue = sorted(p for p in stage.rglob("*") if p.is_file() and not p.is_symlink() and is_elf(p))
    while queue:
        sonames, _ = needed(queue.pop(0))
        for soname in sonames:
            if HOST.fullmatch(soname):
                host.add(soname)
            elif soname not in bundled and not (lib / soname).exists():
                source = system.get(soname)
                if not source:
                    fail(f"unresolved shared library {soname}")
                bundled[soname] = source.resolve()
                shutil.copyfile(source, lib / soname)
                (lib / soname).chmod(0o755)
                queue.append(lib / soname)
    return bundled, host


def owner(path: Path) -> dict:
    package = run("pacman", "-Qqo", path, capture=True).strip()
    info = run("pacman", "-Qi", package, capture=True)
    field = lambda key: re.search(rf"^{key}\s*: (.*)$", info, re.M).group(1).strip()
    return {"package": package, "version": field("Version"), "license": " ".join(field("Licenses").split()),
            "url": field("URL")}


def copy_license_texts(package: str | None, expression: str, licenses: Path, extra: list[str] = ()) -> list[str]:
    """Package license dir + every SPDX id's text + extra repo texts; returns shipped paths (relative to share/)."""
    copied = []
    for text in extra:
        target = licenses / (package or "misc") / Path(text).name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / text, target)
        copied.append(target.relative_to(licenses.parent).as_posix())
    if package and Path("/usr/share/licenses", package).is_dir():
        for source in sorted(Path("/usr/share/licenses", package).rglob("*")):
            if source.is_file():
                target = licenses / package / source.relative_to(Path("/usr/share/licenses", package))
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source.resolve(), target)
                copied.append(target.relative_to(licenses.parent).as_posix())
    for spdx in re.findall(r"[A-Za-z0-9][A-Za-z0-9.+-]*", expression):
        if spdx in ("AND", "OR", "WITH") or spdx.startswith("LicenseRef-"):
            continue
        for source in (Path("/usr/share/licenses/spdx", spdx + ".txt"), Path("/usr/share/licenses/spdx/exceptions", spdx + ".txt")):
            if source.exists():
                target = licenses / "spdx" / source.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
                copied.append(target.relative_to(licenses.parent).as_posix())
    if not copied:
        fail(f"no license text for {package or expression}")
    return sorted(set(copied))


def deterministic_tar(path: Path, members: list[tuple[str, bytes]], epoch: int) -> None:
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        for name, data in sorted(members):
            info = tarfile.TarInfo(name)
            info.size, info.mtime, info.mode = len(data), epoch, 0o644
            archive.addfile(info, io.BytesIO(data))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", required=True)
    parser.add_argument("--channel", choices=("stable", "experimental"), required=True)
    parser.add_argument("--ffmpeg-prefix", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--repo", default="sergen213/joan-native", help="GitHub OWNER/NAME the updater follows")
    parser.add_argument("--cache-dir", type=Path, default=ROOT / "work/tools", help="runtime + source download cache")
    parser.add_argument("--sign-key", type=Path, help="Ed25519 seed file (else env JOA_UPDATE_SIGNING_KEY, else unsigned)")
    args = parser.parse_args()

    project = re.search(r"project\(joan_native VERSION (\S+)", (ROOT / "CMakeLists.txt").read_text()).group(1)
    if re.split(r"[-+]", args.version)[0] != project:
        fail(f"version {args.version} does not match project version {project}")
    epoch = os.environ.get("SOURCE_DATE_EPOCH") or run("git", "-C", ROOT, "log", "-1", "--format=%ct", capture=True).strip()
    work, dist = args.work_dir.resolve(), args.output_dir.resolve()
    ffmpeg = args.ffmpeg_prefix.resolve()
    for path in (work, dist):
        if path.exists() and any(path.iterdir()):
            fail(f"{path} must be empty or absent")
        path.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, "SOURCE_DATE_EPOCH": epoch, "LC_ALL": "C", "TZ": "UTC",
           "PKG_CONFIG_PATH": str(ffmpeg / "lib/pkgconfig") + os.pathsep + os.environ.get("PKG_CONFIG_PATH", "")}
    ffmeta = json.loads((ffmpeg / "share/joan-native/ffmpeg-source.json").read_text())
    for library in ffmeta["built_libraries"]:
        if sha256((ffmpeg / library["path"]).read_bytes()) != library["sha256"]:
            fail(f"FFmpeg prefix modified: {library['path']}")

    # 1. build + install
    build, stage = work / "build", work / "stage"
    run("cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
        "-DJOA_REPRODUCIBLE=ON", f"-DJOA_UPDATE_REPO={args.repo}", f"-DJOA_VERSION_STRING={args.version}", "-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib",
        "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON", "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--disable-new-dtags", env=env)
    run("cmake", "--build", build, "--target", *TARGETS, env=env)
    run("cmake", "--install", build, "--prefix", stage, "--strip", env=env)
    lib, share = stage / "lib", stage / "share"
    lib.mkdir(exist_ok=True)

    # 2. FFmpeg (LGPL, replaceable shared libraries), Qt plugins, closure
    for library in ffmeta["built_libraries"]:
        shutil.copyfile(ffmpeg / library["path"], lib / Path(library["path"]).name)
        (lib / library["soname"]).symlink_to(Path(library["path"]).name)
    shutil.copytree(ffmpeg / "share/licenses/FFmpeg", share / "licenses/FFmpeg")
    (share / "joan-native").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ffmpeg / "share/joan-native/ffmpeg-source.json", share / "joan-native/ffmpeg-source.json")
    qt_plugins = Path(run("qmake6", "-query", "QT_INSTALL_PLUGINS", capture=True).strip())
    for directory, names in QT_PLUGINS.items():
        for name in names:
            source = qt_plugins / directory / name
            if not source.exists():
                fail(f"missing Qt plugin {source}")
            target = lib / "qt6/plugins" / directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    (stage / "bin/qt.conf").write_text("[Paths]\nPrefix = ..\nPlugins = lib/qt6/plugins\n")
    bundled, host = bundle_closure(stage)
    for soname in sorted(bundled):
        absolute = [p for p in needed(lib / soname)[1] if not p.startswith("$ORIGIN")]
        if absolute:
            fail(f"{soname} has an absolute RUNPATH {absolute}")
    for plugin in sorted((lib / "qt6/plugins").rglob("*.so")):
        if needed(plugin)[1] != ["$ORIGIN/../../../"]:
            fail(f"unexpected RUNPATH in Qt plugin {plugin.name}")
    for exe in TARGETS:
        if needed(stage / "bin" / exe)[1] != ["$ORIGIN/../lib"]:
            fail(f"{exe} lacks DT_RPATH $ORIGIN/../lib")

    # 3. desktop integration, notices, license inventory
    for source, target in ((PACKAGING / "desktop/joan-native.desktop", share / "applications/joan-native.desktop"),
                           (PACKAGING / "desktop/joan-native.svg", share / "icons/hicolor/scalable/apps/joan-native.svg"),
                           (PACKAGING / "NOTICE.txt", share / "doc/joan-native/NOTICE.txt")):
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    packages: dict[str, dict] = {}
    for soname, source in sorted(bundled.items()):
        meta = owner(source)
        entry = packages.setdefault(meta["package"], {**meta, "files": []})
        entry["files"].append({"path": "lib/" + soname, "sha256": sha256((lib / soname).read_bytes())})
    for plugin in sorted((lib / "qt6/plugins").rglob("*.so")):
        meta = owner(qt_plugins / plugin.relative_to(lib / "qt6/plugins"))
        packages.setdefault(meta["package"], {**meta, "files": []})["files"].append(
            {"path": plugin.relative_to(stage).as_posix(), "sha256": sha256(plugin.read_bytes())})
    overrides = json.loads((PACKAGING / "licenses/overrides.json").read_text())
    for entry in packages.values():
        override = overrides.get(entry["package"])
        if override:
            entry.update(package_license=entry["license"], license=override["license"], license_note=override["note"])
        entry["license_texts"] = copy_license_texts(entry["package"], entry["license"], share / "licenses",
                                                    override["texts"] if override else [])
        entry["source"] = f"https://gitlab.archlinux.org/archlinux/packaging/packages/{entry['package']}"
    static = []
    for component in STATIC_COMPONENTS:
        entry = dict(component)
        if component["package"]:
            entry.update(version=owner(next(Path("/usr/share/licenses", component["package"]).iterdir()))["version"],
                         license_texts=copy_license_texts(component["package"], component["license"], share / "licenses"))
        else:  # packaging/licenses/<name>-LICENSE.txt -> share/licenses/<name>/LICENSE.txt
            name = Path(component["text"]).name.removesuffix("-LICENSE.txt")
            (share / "licenses" / name).mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / component["text"], share / "licenses" / name / "LICENSE.txt")
            entry["license_texts"] = [f"licenses/{name}/LICENSE.txt"]
        static.append(entry)
    inventory = {
        "format": 1, "product": "joan-native", "version": args.version,
        "ffmpeg": {"version": ffmeta["version"], "license": ffmeta["license"], "url": ffmeta["url"],
                   "source_sha256": ffmeta["source_sha256"], "configure_args": ffmeta["configure_args"],
                   "files": [{"path": "lib/" + Path(l["path"]).name, "sha256": l["sha256"]} for l in ffmeta["built_libraries"]],
                   "license_texts": sorted("licenses/FFmpeg/" + p.name for p in (share / "licenses/FFmpeg").iterdir() if p.is_file())},
        "bundled_packages": sorted(packages.values(), key=lambda e: e["package"]),
        "compiled_in": static,
        "host_provided": sorted(host),
        "appimage_runtime": json.loads((PACKAGING / "appimage/runtime.lock.json").read_text())["runtime"],
        "original_game_data": "none: users import their own copy (joa-install)",
    }
    (share / "doc/joan-native/THIRD_PARTY_INVENTORY.json").write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")

    # 4. update tarballs + manifest
    run(sys.executable, PACKAGING / "release/make_release.py", "--prefix", stage, "--version", args.version,
        "--channel", args.channel, "--output-dir", dist, env=env)

    # AppImage: pinned runtime + squashfs of AppDir (usr/ = stage)
    runtime_lock = json.loads((PACKAGING / "appimage/runtime.lock.json").read_text())
    rt = runtime_lock["runtime"]
    runtime = fetch(args.cache_dir / "runtime", rt["name"], rt["asset_url"], rt["sha256"]).read_bytes()
    appdir = work / "AppDir"
    shutil.copytree(stage, appdir / "usr", symlinks=True)
    for source in sorted((PACKAGING / "appimage/runtime-licenses").iterdir()):
        target = appdir / "usr/share/licenses/appimage-type2-runtime" / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    shutil.copyfile(PACKAGING / "appimage/AppRun", appdir / "AppRun")
    (appdir / "AppRun").chmod(0o755)
    shutil.copyfile(PACKAGING / "desktop/joan-native.desktop", appdir / "joan-native.desktop")
    shutil.copyfile(PACKAGING / "desktop/joan-native.svg", appdir / "joan-native.svg")
    (appdir / ".DirIcon").symlink_to("joan-native.svg")
    for path in [appdir, *appdir.rglob("*")]:  # independent of the caller's umask
        if not path.is_symlink():
            path.chmod(0o755 if path.is_dir() or path.stat().st_mode & 0o111 else 0o644)
    squashfs = work / "AppImage.squashfs"
    run("mksquashfs", appdir, squashfs, "-noappend", "-all-root", "-no-xattrs", "-comp", "zstd",  # times: SOURCE_DATE_EPOCH
        "-Xcompression-level", "19", "-b", "1M", "-quiet", "-no-progress", env=env)
    image = dist / f"JoanNative-{args.version}-x86_64.AppImage"
    image.write_bytes(runtime + squashfs.read_bytes())
    image.chmod(0o755)

    # corresponding source for the bundled FFmpeg build and the AppImage runtime (static libfuse)
    members = [(f"ffmpeg/{name}", (ffmpeg / "share/licenses/FFmpeg/source-proof" / name).read_bytes())
               for name in (f"ffmpeg-{ffmeta['version']}.tar.xz.asc", "ffmpeg-devel.asc")]
    archive = fetch(args.cache_dir / "ffmpeg-source", Path(ffmeta["url"]).name, ffmeta["url"], ffmeta["source_sha256"])
    members += [(f"ffmpeg/{archive.name}", archive.read_bytes()),
                ("ffmpeg/ffmpeg.lock.json", (PACKAGING / "ffmpeg/ffmpeg.lock.json").read_bytes()),
                ("ffmpeg/build_ffmpeg.py", (PACKAGING / "ffmpeg/build_ffmpeg.py").read_bytes()),
                ("appimage-runtime/runtime.lock.json", (PACKAGING / "appimage/runtime.lock.json").read_bytes()),
                ("NOTICE.txt", (PACKAGING / "NOTICE.txt").read_bytes())]
    for source in runtime_lock["sources"]:
        path = fetch(args.cache_dir / "runtime-sources", source["name"], source["url"], source["sha256"])
        members.append((f"appimage-runtime/{source['name']}", path.read_bytes()))
    deterministic_tar(dist / f"joan-native-{args.version}-third-party-source.tar", members, int(epoch))

    key = args.sign_key or (None if not os.environ.get("JOA_UPDATE_SIGNING_KEY") else "env")
    if key:
        run(sys.executable, PACKAGING / "release/sign_manifest.py", dist / "manifest.json",
            *(["--key", key] if key != "env" else []))
    sums = "".join(f"{sha256(p.read_bytes())}  {p.name}\n" for p in sorted(dist.iterdir()) if p.name != "SHA256SUMS")
    (dist / "SHA256SUMS").write_text(sums)
    print(sums, end="")


if __name__ == "__main__":
    main()
