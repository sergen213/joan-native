#!/usr/bin/env python3
"""Build per-component release tarballs and manifest.json (format 1) from an install prefix.

  make_release.py --prefix STAGE --version 0.2.0 --channel stable --output-dir dist

STAGE is a `cmake --install build --prefix STAGE` tree containing only bin/ lib/ share/.
Files are split into components (first matching rule wins, see COMPONENTS); each non-empty
component becomes joan-native-<version>-linux-x86_64-<component>.tar.gz. Archives are
deterministic (sorted, root-owned, mtime 0) and never contain original game data.
Sign afterwards with sign_manifest.py dist/manifest.json.
"""
from __future__ import annotations

import argparse
import fnmatch
import gzip
import hashlib
import io
import json
import os
import platform
import re
import stat
import sys
import tarfile
from pathlib import Path, PurePosixPath

TOP = ("bin", "lib", "share")
COMPONENTS = (  # (glob over the relative path, component)
    ("bin/joa-updater", "updater"),
    ("bin/joa-launcher", "launcher"), ("bin/joa-install", "launcher"),
    ("share/applications/*", "launcher"), ("share/icons/*", "launcher"), ("share/metainfo/*", "launcher"),
    ("share/joan-native/shaders/*", "shaders"), ("*.spv", "shaders"),
    ("bin/joa-*", "tools"),
    ("*", "engine"),
)
REQUIRED = "bin/joa-updater"
VERSION = re.compile(r"(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?")
# Defense in depth: original Joan of Arc data must never enter an update payload.
GAME = re.compile(r".*\.(?:res|set|pro|am1|am2|am3|smo|lgt|sca|scr|dds|fin|wma|wmv|sav|dat|nvo|pso|exe|dll|sys|bmp|hashdb)", re.I)
MAX_TOTAL = 2 * 1024 * 1024 * 1024


def fail(message: str) -> None:
    sys.exit("make_release: " + message)


def component(name: str) -> str:
    return next(c for pattern, c in COMPONENTS if fnmatch.fnmatchcase(name, pattern))


def tarinfo(name: str, path: Path) -> tuple[tarfile.TarInfo, bytes | None]:
    info = path.lstat()
    entry = tarfile.TarInfo(name)
    entry.mtime, entry.uid, entry.gid, entry.uname, entry.gname = 0, 0, 0, "", ""
    if info.st_mode & (stat.S_ISUID | stat.S_ISGID | stat.S_ISVTX):
        fail(f"setuid/setgid/sticky bit: {name}")
    if any(ord(char) < 32 or ord(char) == 127 for char in name) or "\\" in name:
        fail(f"unsafe name: {name!r}")
    if stat.S_ISDIR(info.st_mode):
        entry.type, entry.mode = tarfile.DIRTYPE, 0o755
        return entry, None
    if stat.S_ISLNK(info.st_mode):
        target = os.readlink(path)
        resolved = PurePosixPath(os.path.normpath(PurePosixPath(name).parent / target))
        # Same rule as the updater (update.cpp): relative, no "." / ".." / empty components (a ".." can cancel a
        # component that is itself a payload symlink, so lexical containment is not enough).
        plain = bool(target) and all(part not in ("", ".", "..") for part in target.split("/"))
        if target.startswith("/") or not plain or resolved.parts[:1] == ("..",) or resolved.parts[0] not in TOP:
            fail(f"symlink escapes payload: {name} -> {target}")
        entry.type, entry.linkname, entry.mode = tarfile.SYMTYPE, target, 0o777
        return entry, None
    if not stat.S_ISREG(info.st_mode):
        fail(f"not a regular file/directory/symlink: {name}")
    if GAME.fullmatch(name):
        fail(f"refusing game-data-like file in update payload: {name}")
    data = path.read_bytes()
    entry.type, entry.size = tarfile.REGTYPE, len(data)
    entry.mode = 0o755 if info.st_mode & 0o111 else 0o644
    return entry, data


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--channel", choices=("stable", "experimental"), required=True)
    parser.add_argument("--min-glibc", default=".".join(platform.libc_ver()[1].split(".")[:2]),
                        help="default: glibc of the build host")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if not VERSION.fullmatch(args.version) or len(args.version) > 64:
        fail(f"invalid semantic version: {args.version}")
    if not re.fullmatch(r"[0-9]+\.[0-9]+", args.min_glibc):
        fail("invalid --min-glibc")
    extra = sorted(path.name for path in args.prefix.iterdir() if path.name not in TOP)
    if extra:
        fail(f"prefix may contain only bin/ lib/ share/, found: {extra}")
    groups: dict[str, list[tuple[str, Path]]] = {}
    for top in TOP:
        base = args.prefix / top
        if not base.exists():
            continue
        if not base.is_dir() or base.is_symlink():
            fail(f"{top}/ is not a real directory")
        for path in sorted(base.rglob("*")):
            name = top + "/" + path.relative_to(base).as_posix()
            if not path.is_dir() or path.is_symlink():
                groups.setdefault(component(name), []).append((name, path))
    required = args.prefix / REQUIRED
    if not required.is_file() or required.is_symlink() or not required.stat().st_mode & 0o111:
        fail(f"prefix lacks executable {REQUIRED}")
    if sum(len(files) for files in groups.values()) > 16384:
        fail("too many payload entries")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    artifacts, total = [], 0
    for name in sorted(groups):
        files = groups[name]
        dirs = sorted({str(parent) for entry, _ in files for parent in PurePosixPath(entry).parents if str(parent) != "."})
        buffer = io.BytesIO()
        with gzip.GzipFile(fileobj=buffer, mode="wb", mtime=0, compresslevel=9) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for entry, path in sorted([(d, args.prefix / d) for d in dirs] + files):
                    info, data = tarinfo(entry, path)
                    total += info.size
                    if total > MAX_TOTAL:
                        fail("payload exceeds 2 GiB")
                    archive.addfile(info, io.BytesIO(data) if data is not None else None)
        payload = buffer.getvalue()
        filename = f"joan-native-{args.version}-linux-x86_64-{name}.tar.gz"
        (args.output_dir / filename).write_bytes(payload)
        artifacts.append({"name": filename, "component": name,
                          "sha256": hashlib.sha256(payload).hexdigest(), "size": len(payload)})
        print(args.output_dir / filename)
    manifest = {"format": 1, "product": "joan-native", "version": args.version, "channel": args.channel,
                "min_glibc": args.min_glibc, "artifacts": artifacts}
    (args.output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(args.output_dir / "manifest.json")


if __name__ == "__main__":
    main()
