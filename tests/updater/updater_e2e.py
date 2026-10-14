#!/usr/bin/env python3
"""End-to-end joa-updater check against a local HTTP fixture (CTest updater.e2e, no network).

  updater_e2e.py TESTHTTP_BINARY SHIPPED_BINARY OWNER/REPO

Builds releases with packaging/release/make_release.py, signs them with a throwaway key
(or $JOA_UPDATER_E2E_KEY, e.g. work/updater/dev-ed25519.key), serves them on 127.0.0.1 and
checks: apply is atomic, bad signature / hash mismatch / unsafe payloads / downgrades are
rejected without changing anything, rollback restores the previous version, interrupted
updates are recovered, and game/, saves, settings and asset packs are never touched.
Adapted from the Kabus22 updater end-to-end test.
"""
from __future__ import annotations

import fcntl
import functools
import hashlib
import http.server
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import threading
from pathlib import Path

RELEASE = Path(__file__).resolve().parents[2] / "packaging/release"


def run(*args, check=True, env=None) -> subprocess.CompletedProcess:
    result = subprocess.run([str(arg) for arg in args], env=env, capture_output=True, text=True, timeout=120)
    if check and result.returncode:
        raise AssertionError(f"{args} failed {result.returncode}:\n{result.stdout}\n{result.stderr}")
    return result


def full(root: Path, skip_app: bool = False) -> dict:
    """Snapshot of root; skip_app omits the updater-owned */joan-native/app subtree."""
    result = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if skip_app and "app" in relative.parts and relative.parts[relative.parts.index("app") - 1] == "joan-native":
            continue
        if path.is_symlink():
            result[str(relative)] = "->" + os.readlink(path)
        elif path.is_file():
            result[str(relative)] = path.read_bytes()
        else:
            result[str(relative)] = "dir"
    return result


def prefix(base: Path, marker: str) -> Path:
    (base / "bin").mkdir(parents=True)
    for name in ("joan-native", "joa-launcher", "joa-install", "joa-updater", "joa-am3-dump"):
        (base / "bin" / name).write_text(f"#!/bin/sh\necho {name} {marker}\n")
        (base / "bin" / name).chmod(0o755)
    (base / "lib").mkdir()
    (base / "lib/libjoatest.so.1").write_text(marker)
    (base / "lib/libjoatest.so").symlink_to("libjoatest.so.1")
    (base / "share/joan-native/shaders").mkdir(parents=True)
    (base / "share/joan-native/shaders/terrain.frag.spv").write_text("spv " + marker)
    return base


def release(tag: str, prerelease: bool, port: int, directory: Path, server: Path) -> dict:
    return {"tag_name": "v" + tag, "draft": False, "prerelease": prerelease, "body": f"Notes for {tag}",
            "assets": [{"name": path.name, "size": path.stat().st_size,
                        "browser_download_url": f"http://127.0.0.1:{port}/{path.relative_to(server)}"}
                       for path in sorted(directory.iterdir())]}


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args) -> None:
        pass


def main() -> None:
    testhttp, shipped, repo = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    with tempfile.TemporaryDirectory(prefix="joa-updater-e2e-") as scratch:
        work = Path(scratch)
        wrong = work / "wrong-key"
        run(sys.executable, RELEASE / "sign_manifest.py", "--generate-key", wrong)
        if os.environ.get("JOA_UPDATER_E2E_KEY"):
            key = Path(os.environ["JOA_UPDATER_E2E_KEY"]).resolve()
        else:
            key = work / "key"
            run(sys.executable, RELEASE / "sign_manifest.py", "--generate-key", key)
            assert oct(key.stat().st_mode & 0o777) == "0o600"
        public = run(sys.executable, RELEASE / "sign_manifest.py", "--public-key", "--key", key).stdout.strip()
        assert public not in key.read_text()
        server = work / "server"

        def build(version: str, channel: str, signer: Path = key) -> Path:
            out = server / version
            run(sys.executable, RELEASE / "make_release.py", "--prefix", prefix(work / ("prefix-" + version), version),
                "--version", version, "--channel", channel, "--min-glibc", "2.17", "--output-dir", out)
            run(sys.executable, RELEASE / "sign_manifest.py", out / "manifest.json", "--key", signer)
            return out

        def handmade(version: str, members: list[tuple[str, bytes]], manifest_version: str | None = None) -> None:
            out = server / version
            out.mkdir(parents=True)
            buffer = io.BytesIO()
            with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
                for name, data in members:
                    info = tarfile.TarInfo(name)
                    info.size, info.mode = len(data), 0o755
                    archive.addfile(info, io.BytesIO(data))
            data = buffer.getvalue()
            name = f"joan-native-{manifest_version or version}-linux-x86_64-engine.tar.gz"
            (out / name).write_bytes(data)
            manifest = {"format": 1, "product": "joan-native", "version": manifest_version or version, "channel": "stable",
                        "min_glibc": "2.17", "artifacts": [{"name": name, "component": "engine",
                                                            "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}]}
            (out / "manifest.json").write_text(json.dumps(manifest))
            run(sys.executable, RELEASE / "sign_manifest.py", out / "manifest.json", "--key", key)

        # Release tooling refuses non-payload content and game data.
        bad = prefix(work / "bad-prefix", "x")
        (bad / "game").mkdir()
        assert run(sys.executable, RELEASE / "make_release.py", "--prefix", bad, "--version", "9.9.9",
                   "--channel", "stable", "--output-dir", work / "refused", check=False).returncode
        (bad / "game").rmdir()
        (bad / "share/joan-native/JOAN.RES").write_text("asset")
        assert run(sys.executable, RELEASE / "make_release.py", "--prefix", bad, "--version", "9.9.9",
                   "--channel", "stable", "--output-dir", work / "refused", check=False).returncode

        good = build("0.2.0", "stable")
        components = sorted(item["component"] for item in json.loads((good / "manifest.json").read_text())["artifacts"])
        assert components == ["engine", "launcher", "shaders", "tools", "updater"], components
        build("0.3.0-beta.1", "experimental")
        tampered = build("0.2.1", "stable")
        artifact = tampered / "joan-native-0.2.1-linux-x86_64-engine.tar.gz"
        artifact.write_bytes(artifact.read_bytes()[:-1] + bytes([artifact.read_bytes()[-1] ^ 1]))
        build("0.2.2", "stable", signer=wrong)
        edited = build("0.2.3", "stable")
        (edited / "manifest.json").write_text((edited / "manifest.json").read_text().replace("2.17", "2.16"))
        build("0.2.4", "experimental")
        handmade("0.2.5", [("bin/joa-updater", b"x"), ("../escaped", b"x")])
        handmade("0.2.6", [("bin/joa-updater", b"x")], manifest_version="0.2.0")
        handmade("0.2.7", [("bin/joan-native", b"x")])
        build("0.1.5", "stable")

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=str(server)))
        port = httpd.server_address[1]
        listing = server / "repos" / repo / "releases"
        listing.parent.mkdir(parents=True)

        def serve(*versions: str) -> None:
            listing.write_text(json.dumps([release(v, "-" in v, port, server / v, server) for v in versions]))

        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            home = work / "home"  # HOME, data and config; the fixture server lives outside it
            data = home / "data"
            root = data / "joan-native"
            (root / "game/current").mkdir(parents=True)
            (root / "game/current/JOAN.RES").write_text("imported original data")
            (root / "Save").mkdir()
            (root / "Save/Mission1.SAV").write_text("save slot")
            (root / "hd/pack1").mkdir(parents=True)
            (root / "hd/pack1/tex.dds").write_text("hd asset")
            (home / "config/joan-native").mkdir(parents=True)
            (home / "config/joan-native/settings.ini").write_text("[update]\nchannel=stable\n")
            untouched = full(home, skip_app=True)
            env = {**os.environ, "HOME": str(home), "XDG_DATA_HOME": str(data), "XDG_CONFIG_HOME": str(home / "config"),
                   "JOA_UPDATER_TEST_API": f"http://127.0.0.1:{port}", "JOA_UPDATER_TEST_PUBKEY": public}
            updater = lambda *args, check=True: run(testhttp, *args, "--json", check=check, env=env)
            app = root / "app"

            built = run(shipped, "--version").stdout.split()[1]
            for binary in (testhttp, shipped):
                assert run(binary, "--version").stdout.startswith("joa-updater " + built)
                assert "rollback" in run(binary, "--help").stdout
                assert run(binary, "bogus", check=False).returncode == 2
                assert run(binary, "check", "--channel", "nightly", check=False).returncode == 2
            assert b"JOA_UPDATER_TEST_API" not in shipped.read_bytes() and b"JOA_UPDATER_TEST_API" in testhttp.read_bytes()
            assert json.loads(run(shipped, "status", "--json", env=env).stdout)["current"] is None

            serve("0.2.0", "0.3.0-beta.1")
            assert json.loads(updater("check").stdout) == {"current": built, "latest": "0.2.0", "available": True,
                                                            "notes": "Notes for 0.2.0"}
            assert json.loads(updater("check", "--channel", "experimental").stdout)["latest"] == "0.3.0-beta.1"

            applied = updater("apply")
            assert json.loads(applied.stdout) == {"applied": True, "from": built, "to": "0.2.0"}, applied
            lines = [line for line in applied.stderr.splitlines() if line.startswith("progress ")]
            assert lines[0].startswith("progress 0 ") and lines[-1].startswith("progress 100 "), applied.stderr
            assert os.readlink(app / "current") == "0.2.0"
            assert "joan-native 0.2.0" in run(app / "current/bin/joan-native").stdout
            assert (app / "current/share/joan-native/shaders/terrain.frag.spv").read_text() == "spv 0.2.0"
            assert os.readlink(app / "current/lib/libjoatest.so") == "libjoatest.so.1"
            inventory = json.loads((app / "0.2.0/.joan-native-payload.json").read_text())
            assert any(item["path"] == "bin/joa-updater" for item in inventory["files"])
            assert json.loads(updater("status").stdout)["installed"] == ["0.2.0"]
            assert json.loads(updater("apply").stdout) == {"applied": False, "from": "0.2.0", "to": "0.2.0"}
            assert full(home, skip_app=True) == untouched

            before = full(home)
            for version, reason in (("0.2.1", "SHA-256 mismatch"), ("0.2.2", "signature is invalid"),
                                    ("0.2.3", "signature is invalid"), ("0.2.4", "not a stable release"),
                                    ("0.2.5", "Unsafe payload path"), ("0.2.6", "does not match release"),
                                    ("0.2.7", "lacks executable bin/joa-updater"), ("0.1.5", "Refusing downgrade")):
                serve(version)
                result = updater("apply", check=False)
                assert result.returncode == 1 and reason in result.stderr, (version, result.stdout, result.stderr)
                assert reason in json.loads(result.stdout)["error"]
                assert full(home) == before, f"{version} changed the installation"
            assert not (work / "escaped").exists() and not (app / "escaped").exists()
            serve("0.1.5")
            assert json.loads(updater("check").stdout)["available"] is False

            # Concurrent updates are serialized by app/.lock.
            serve("0.2.0", "0.3.0-beta.1")
            with open(app / ".lock") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                assert "in progress" in updater("apply", "--channel", "experimental", check=False).stderr
            updater("apply", "--channel", "experimental")
            status = json.loads(updater("status").stdout)
            assert status == {"current": "0.3.0-beta.1", "previous": "0.2.0", "installed": ["0.2.0", "0.3.0-beta.1"],
                              "app_dir": str(app)}, status

            assert json.loads(updater("rollback").stdout) == {"from": "0.3.0-beta.1", "to": "0.2.0"}
            assert "joan-native 0.2.0" in run(app / "current/bin/joan-native").stdout
            assert json.loads(run(shipped, "status", "--json", env=env).stdout)["previous"] == "0.3.0-beta.1"

            # Interrupted update: leftover stage, download and an unactivated version dir.
            (app / ".stage-0.4.0-abcdef/bin").mkdir(parents=True)
            (app / ".stage-0.4.0-abcdef-engine.tar.gz").write_text("partial")
            (app / "0.4.0/bin").mkdir(parents=True)
            updater("apply", "--channel", "experimental")
            leftovers = sorted(p.name for p in app.iterdir())
            assert leftovers == [".lock", ".state", "0.2.0", "0.3.0-beta.1", "current"], leftovers
            # Crash between the current swap and the .state write: rollback target still derivable.
            os.symlink("0.2.0", app / ".tmp"); os.replace(app / ".tmp", app / "current")
            assert json.loads(updater("status").stdout)["previous"] == "0.3.0-beta.1"

            assert full(home, skip_app=True) == untouched
        finally:
            httpd.shutdown()
    print("updater end-to-end check/apply/reject/rollback/recovery passed")


if __name__ == "__main__":
    main()
