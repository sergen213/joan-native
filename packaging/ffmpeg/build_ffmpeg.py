#!/usr/bin/env python3
"""Build the authenticated, LGPL-only FFmpeg subset joan-native needs, in a fresh rootless prefix.

Decoders: wmav1, wmav2 (music/*.wma, movie soundtracks), wmv1, wmv2, wmv3 (movie/*.wmv);
demuxer: asf; protocol: file; libraries: avcodec, avformat, avutil, swscale, swresample.
Shared libraries only, so end users can replace/relink them (LGPL-2.1 §6).

Run: python3 packaging/ffmpeg/build_ffmpeg.py --prefix ~/.cache/joan-native/ffmpeg-prefix --tools-dir work/tools
(the prefix path must not contain whitespace: FFmpeg writes broken *.pc files otherwise)
Then configure joan-native with PKG_CONFIG_PATH=<prefix>/lib/pkgconfig (src/engine/CMakeLists.txt
uses pkg-config) and ship <prefix>/lib/*.so.* plus <prefix>/share/licenses/FFmpeg next to the binary.
acquire_source() keeps the byte-exact official archive, its detached signature and the release key:
redistribute those with this recipe and ffmpeg.lock.json as the corresponding source. Never
configure with --enable-gpl/--enable-nonfree/--enable-version3 (check_configuration() enforces it).
Requires Linux x86_64, Python 3.11+, gcc, make, nasm, binutils and gpg. No sudo or installer.
Adapted from the CitroenC4Robot packaging/build_ffmpeg.py recipe (same lock/proof layout).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import signal
import subprocess
import tarfile
import tempfile
import urllib.request

LOCK = Path(__file__).with_name("ffmpeg.lock.json")
METADATA = Path("share/joan-native/ffmpeg-source.json")
NOTICE_ROOT = Path("share/licenses/FFmpeg")
NOTICE_DIRS = {"libavcodec", "libavformat", "libavutil", "libswscale", "libswresample", "compat", "ffbuild"}
NOTICE_SUFFIXES = (".c", ".h", ".asm", ".inc", ".S", ".sh", ".mak")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _download(cache: Path, name: str, url: str, sha256: str) -> Path:
    """Fetch once into the cache; any digest mismatch (cached or fresh) is fatal."""
    target = cache / name
    if not target.exists():
        with tempfile.NamedTemporaryFile(dir=cache, prefix=".download-", delete=False) as tmp:
            try:
                with urllib.request.urlopen(url, timeout=120) as response:
                    shutil.copyfileobj(response, tmp)
                tmp.close()
                os.replace(tmp.name, target)
            except BaseException:
                os.unlink(tmp.name)
                raise
    if _sha256(target) != sha256:
        raise ValueError(f"SHA-256 mismatch for {target}")
    return target


def load_lock() -> dict:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    if lock.get("format") != 1 or lock["license"] != "LGPL-2.1-or-later":
        raise ValueError("Unsupported FFmpeg source/license lock")
    return lock


def local_directory(path: Path) -> Path:
    """Reject system installation/cache targets, including symlinked parent escapes."""
    path = path.absolute()
    if path.is_symlink():
        raise ValueError(f"Directory must not be a symlink: {path}")
    path = path.resolve()
    forbidden = ("/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/boot", "/dev", "/proc", "/sys", "/var", "/run", "/opt")
    if path == Path("/") or any(path == Path(p) or Path(p) in path.parents for p in forbidden):
        raise ValueError(f"Refusing a system directory: {path}")
    return path


def command(args: list[str], cwd: Path, env: dict | None = None, capture: bool = False) -> bytes:
    """Own/reap the whole build group before temporary staging can be removed."""
    child = subprocess.Popen(args, cwd=cwd, env=env, start_new_session=True,
                             stdout=subprocess.PIPE if capture else None)
    try:
        stdout, _ = child.communicate()
        if child.returncode:
            raise subprocess.CalledProcessError(child.returncode, args)
        return stdout or b""
    finally:
        if child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGTERM)
                child.wait(timeout=10)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                pass
        try:  # descendants may outlive a terminated shell
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait()


def acquire_source(tools_dir: Path) -> Path:
    """Authenticate the preserved official source; no configure or compilation."""
    lock = load_lock()
    cache = local_directory(Path(tools_dir)) / "ffmpeg-source"
    cache.mkdir(parents=True, exist_ok=True)
    source = _download(cache, lock["archive"], lock["url"], lock["source_sha256"])
    if source.stat().st_size != lock["source_size"]:
        raise ValueError("FFmpeg source archive size mismatch")
    proof = lock["provenance"]
    signature = _download(cache, lock["archive"] + ".asc", proof["signature_url"], proof["signature_sha256"])
    key = _download(cache, "ffmpeg-devel.asc", proof["key_url"], proof["key_sha256"])
    gpg = shutil.which("gpg")
    if not gpg:
        raise RuntimeError("gpg is required to authenticate the official FFmpeg release")
    with tempfile.TemporaryDirectory(dir=cache, prefix=".signature-") as temporary:
        home = Path(temporary)
        home.chmod(0o700)
        base = [gpg, "--homedir", str(home), "--batch", "--no-options", "--no-auto-key-retrieve"]
        command(base + ["--import", str(key)], cache)
        status = command(base + ["--status-fd=1", "--verify", str(signature), str(source)], cache, capture=True)
        valid = [line.split()[2] for line in status.decode("utf-8").splitlines() if line.startswith("[GNUPG:] VALIDSIG ")]
        if valid != [proof["key_fingerprint"]]:
            raise ValueError("FFmpeg release signature fingerprint mismatch")
    return source


def extract_source(archive: Path, destination: Path, top_directory: str) -> Path:
    """Extract only bounded regular files/directories; never links, devices or escapes."""
    with tarfile.open(archive, "r:xz") as source:
        members = source.getmembers()
        if len(members) > 20000 or sum(m.size for m in members) > 256 * 1024 * 1024:
            raise ValueError("FFmpeg source exceeds extraction bounds")
        seen = set()
        for member in members:
            relative = PurePosixPath(member.name)
            if (relative.is_absolute() or ".." in relative.parts or not relative.parts
                    or relative.parts[0] != top_directory
                    or not (member.isdir() or member.isfile()) or relative in seen):
                raise ValueError(f"Unsafe FFmpeg source member: {member.name}")
            seen.add(relative)
        for member in members:
            target = destination.joinpath(*PurePosixPath(member.name).parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as input_stream, target.open("xb") as output:
                    shutil.copyfileobj(input_stream, output)
                target.chmod(0o755 if member.mode & 0o111 else 0o644)
    return destination / top_directory


def source_notices(source: Path) -> bytes:
    parts = [b"FFmpeg source copyright notices (including disabled source components).\n"
             b"The accompanying byte-exact source archive preserves all original notices.\n"]
    for path in sorted(source.rglob("*"), key=lambda p: p.relative_to(source).as_posix()):
        name = path.relative_to(source).as_posix()
        if name.split("/")[0] not in NOTICE_DIRS or not name.endswith(NOTICE_SUFFIXES) or not path.is_file():
            continue
        text = path.read_bytes().decode("utf-8", errors="replace")
        match = re.match(r"\s*(/\*.*?\*/)", text, re.S)
        if match:
            header = match.group(1)
        else:
            lines = []
            for line in text.splitlines():
                if line.startswith((";", "#")) or (lines and not line.strip()):
                    lines.append(line)
                else:
                    break
            header = "\n".join(lines).strip()
        if header:
            parts.append(("\n=== " + name + " ===\n" + header + "\n").encode("utf-8"))
    return b"".join(parts)


def license_contents(source: Path, lock: dict, verify: bool = True) -> dict[str, bytes]:
    files = {name: (source / name).read_bytes() for name in ("COPYING.LGPLv2.1", "LICENSE.md", "CREDITS")}
    files["SOURCE-NOTICES.txt"] = source_notices(source)
    aggregate = (f"FFmpeg {lock['version']} - LGPL-2.1-or-later minimal WMA/WMV (ASF) decoder build for joan-native.\n"
                 "No GPL, nonfree, external codecs or network support are enabled.\n"
                 "Unmodified corresponding source and release authentication are supplied separately.\n\n").encode("utf-8")
    for name, data in files.items():
        aggregate += ("=== " + name + " ===\n").encode("utf-8") + data + b"\n"
    files["LICENSE.txt"] = aggregate
    if verify:
        for record in lock["licenses"]:
            data = files[Path(record["path"]).name]
            if hashlib.sha256(data).hexdigest() != record["sha256"]:
                raise ValueError(f"Authenticated FFmpeg notice digest mismatch: {record['path']}")
    return files


def check_configuration(source: Path) -> None:
    config = (source / "config.h").read_text(encoding="utf-8")
    for name, enabled in {"GPL": 0, "NONFREE": 0, "VERSION3": 0, "NETWORK": 0,
                          "AVCODEC": 1, "AVFORMAT": 1, "AVUTIL": 1, "SWSCALE": 1, "SWRESAMPLE": 1,
                          "AVDEVICE": 0, "AVFILTER": 0}.items():
        if not re.search(rf"^#define CONFIG_{name} {enabled}$", config, re.M):
            raise ValueError(f"Unexpected FFmpeg configuration: {name}")
    components = (source / "config_components.h").read_text(encoding="utf-8")
    for name in ("WMAV1_DECODER", "WMAV2_DECODER", "WMV1_DECODER", "WMV2_DECODER", "WMV3_DECODER", "ASF_DEMUXER"):
        if not re.search(rf"^#define CONFIG_{name} 1$", components, re.M):
            raise ValueError(f"Required FFmpeg component disabled: {name}")


def check_install(root: Path, lock: dict) -> list[dict]:
    allowed = set()
    records = []
    for library in lock["libraries"]:
        base = "lib" + library["name"]
        full = base + ".so." + library["version"]
        allowed.update((base + ".so", library["soname"], full, "pkgconfig/" + base + ".pc"))
        binary = root / "lib" / full
        if binary.is_symlink() or not binary.is_file():
            raise ValueError(f"Missing regular FFmpeg library: {binary}")
        for alias in (base + ".so", library["soname"]):
            link = root / "lib" / alias
            if not link.is_symlink() or link.resolve() != binary.resolve():
                raise ValueError(f"Invalid FFmpeg shared-library alias: {link}")
        pc = (root / "lib/pkgconfig" / (base + ".pc")).read_text(encoding="utf-8")
        if f"Version: {library['version']}" not in pc.splitlines():
            raise ValueError(f"FFmpeg pkg-config version mismatch: {base}")
        records.append({**library, "path": "lib/" + full, "sha256": _sha256(binary)})
    actual = {p.relative_to(root / "lib").as_posix() for p in (root / "lib").rglob("*") if not p.is_dir()}
    if actual != allowed:
        raise ValueError(f"Unexpected FFmpeg library/metadata installation: {sorted(actual ^ allowed)}")
    if {p.name for p in root.iterdir()} != {"lib", "include"}:
        raise ValueError("FFmpeg installed unexpected programs, data or documentation")
    return records


def build(prefix: Path, tools_dir: Path) -> Path:
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64"):
        raise RuntimeError("FFmpeg release recipe requires Linux x86_64")
    prefix = local_directory(prefix)
    cache = local_directory(tools_dir)
    if prefix.exists() or prefix.is_symlink():
        raise FileExistsError(f"FFmpeg requires a fresh dedicated prefix: {prefix}")
    if any(c.isspace() for c in str(prefix)):  # ffbuild/pkgconfig_generate.sh word-splits: empty prefix= in *.pc
        raise ValueError(f"FFmpeg prefix must not contain whitespace: {prefix}")
    source_cache = cache / "ffmpeg-source"
    if prefix == cache or prefix in cache.parents or prefix == source_cache or source_cache in prefix.parents:
        raise ValueError("FFmpeg prefix must not overlap the source cache or contain the tool cache")
    tools = {}
    for name in ("gcc", "make", "nasm", "ar", "ranlib", "strip", "nm"):
        tools[name] = shutil.which(name)
        if not tools[name]:
            raise RuntimeError(f"Required FFmpeg build tool is missing: {name}")
    lock = load_lock()
    archive = acquire_source(cache)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=prefix.parent, prefix=".ffmpeg-build-") as temporary:
        work = Path(temporary)
        source = extract_source(archive, work, lock["top_directory"])
        notices = license_contents(source, lock)  # verify before executing configure
        args = [str(source / "configure"), "--prefix=" + str(prefix),
                "--libdir=" + str(prefix / "lib"), "--shlibdir=" + str(prefix / "lib"),
                "--pkgconfigdir=" + str(prefix / "lib/pkgconfig"), *lock["configure_args"],
                "--cc=" + tools["gcc"], "--ld=" + tools["gcc"], "--ar=" + tools["ar"],
                "--ranlib=" + tools["ranlib"], "--strip=" + tools["strip"], "--nm=" + tools["nm"],
                "--x86asmexe=" + tools["nasm"], "--pkg-config=false"]
        # Do not inherit host CFLAGS/CPPFLAGS/LDFLAGS, native CPU flags or package discovery.
        env = {"PATH": os.environ.get("PATH", os.defpath), "HOME": str(work), "LC_ALL": "C", "TZ": "UTC",
               "TMPDIR": str(work), "SOURCE_DATE_EPOCH": str(lock["provenance"]["signature_unix_time"])}
        command(args, source, env)
        check_configuration(source)
        command([tools["make"], "-j" + str(os.cpu_count() or 1)], source, env)
        stage = work / "install"
        command([tools["make"], "install-libs", "install-headers", "DESTDIR=" + str(stage)], source, env)
        installed = stage / prefix.relative_to("/")
        libraries = check_install(installed, lock)
        for name, data in notices.items():
            output = installed / NOTICE_ROOT / name
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(data)
        proof = installed / NOTICE_ROOT / "source-proof"
        proof.mkdir()
        for name in (lock["archive"] + ".asc", "ffmpeg-devel.asc"):
            shutil.copyfile(archive.parent / name, proof / name)
        shutil.copyfile(LOCK, proof / LOCK.name)
        configuration = []
        for name in ("config.h", "config_components.h", "ffbuild/config.mak", "ffbuild/config.sh"):
            destination = proof / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, destination)
            configuration.append({"path": destination.relative_to(installed).as_posix(), "sha256": _sha256(destination)})
        metadata = {"format": 1, "component": "FFmpeg", "version": lock["version"], "url": lock["url"],
                    "source_sha256": lock["source_sha256"], "license": lock["license"], "licenses": lock["licenses"],
                    "libraries": lock["libraries"], "built_libraries": libraries, "source_cache_path": str(archive),
                    "prefix": str(prefix), "configure_args": args[1:], "lock_sha256": _sha256(LOCK),
                    "proof": lock["provenance"], "source_terms": lock["source_terms"], "configuration": configuration,
                    "tool_paths": {k: v for k, v in tools.items() if k != "make"}}
        output = installed / METADATA
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if prefix.exists() or prefix.is_symlink():
            raise FileExistsError(prefix)
        installed.rename(prefix)
    return prefix


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path)
    parser.add_argument("--tools-dir", required=True, type=Path)
    parser.add_argument("--print-notice-digests", action="store_true",
                        help="authenticate + extract the source and print the lock's license digests (lock maintenance)")
    arguments = parser.parse_args()
    if arguments.print_notice_digests:
        lock = load_lock()
        with tempfile.TemporaryDirectory() as temporary:
            source = extract_source(acquire_source(arguments.tools_dir), Path(temporary), lock["top_directory"])
            for name, data in license_contents(source, lock, verify=False).items():
                print(f"{hashlib.sha256(data).hexdigest()}  {NOTICE_ROOT / name}")
        return
    if not arguments.prefix:
        parser.error("--prefix is required")

    def terminate(*_: object) -> None:
        raise KeyboardInterrupt
    previous = signal.signal(signal.SIGTERM, terminate)
    try:
        print("JOA_FFMPEG_PREFIX=" + str(build(arguments.prefix, arguments.tools_dir)))
    finally:
        signal.signal(signal.SIGTERM, previous)


if __name__ == "__main__":
    main()
