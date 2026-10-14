#!/usr/bin/env python3
"""Game-file open order from a WINEDEBUG=+timestamp,+file log.

    python3 tools/reference/file_order.py work/reference/logs/s06_fileorder.log > out.tsv

Uses ntdll's get_nt_and_unix_names traces (every path resolution, incl. relative paths the game opens
from its cwd, which Wine resolves to Z:\\...\\joa\\). Emits first time (s, relative to the first hit),
hit count, existed (ret 0) flag and path relative to the game dir, in first-hit order.
"""
import re, sys

PAT = re.compile(r'^(\d+\.\d+):[0-9a-f]+:trace:file:get_nt_and_unix_names L"\\\\\?\?\\\\[^"]*?\\\\joa\\\\([^"]+)" -> ret (\S+)', re.I)


def order(lines):
    first, count, ok = {}, {}, {}
    for line in lines:
        m = PAT.match(line)
        if not m:
            continue
        ts, path = float(m.group(1)), m.group(2).replace("\\\\", "/")
        first.setdefault(path, ts)
        count[path] = count.get(path, 0) + 1
        ok[path] = ok.get(path, False) or m.group(3) == "0"
    t0 = min(first.values(), default=0.0)
    return [(round(ts - t0, 3), count[p], int(ok[p]), p) for p, ts in sorted(first.items(), key=lambda kv: kv[1])]


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        z = 'L"\\\\??\\\\Z:\\\\mnt\\\\x\\\\joa\\\\'
        demo = [f'10.5:00f0:trace:file:get_nt_and_unix_names {z}RESOURCE\\\\A.RES" -> ret 0 nt',
                f'11.0:00f0:trace:file:get_nt_and_unix_names {z}CONFIG.DAT" -> ret c0000034 nt',
                f'12.0:00f0:trace:file:get_nt_and_unix_names {z}RESOURCE\\\\A.RES" -> ret 0 nt',
                '12.5:00f0:trace:file:get_nt_and_unix_names L"\\\\??\\\\C:\\\\windows\\\\x.dll" -> ret 0 nt']
        assert order(demo) == [(0.0, 2, 1, "RESOURCE/A.RES"), (0.5, 1, 0, "CONFIG.DAT")], order(demo)
        print("ok")
        sys.exit()
    print("t_rel_s\thits\texists\tpath")
    with open(sys.argv[1], errors="replace") as f:
        for row in order(f):
            print(*row, sep="\t")
