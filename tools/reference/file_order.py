#!/usr/bin/env python3
"""Game-file open order from a WINEDEBUG=+timestamp,+file log.

    python3 tools/reference/file_order.py work/reference/logs/s06_fileorder.log > out.tsv

Emits: first-open time (s, relative to first game-file open), open count, path relative to the
game dir, for every NtCreateFile under C:\\joa (the sandbox copy), in first-open order.
"""
import re, sys

PAT = re.compile(r'^(\d+\.\d+):[0-9a-f]+:trace:file:NtCreateFile .*?name=L"\\\\\?\?\\\\C:\\\\joa\\\\([^"]*)"', re.I)


def order(lines):
    first, count = {}, {}
    for line in lines:
        m = PAT.match(line)
        if not m:
            continue
        ts, path = float(m.group(1)), m.group(2).replace("\\\\", "/")
        if path not in first:
            first[path] = ts
        count[path] = count.get(path, 0) + 1
    t0 = min(first.values(), default=0.0)
    return [(round(ts - t0, 3), count[p], p) for p, ts in sorted(first.items(), key=lambda kv: kv[1])]


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        demo = ['10.500:00ec:trace:file:NtCreateFile handle=0x1 access=1 name=L"\\\\??\\\\C:\\\\joa\\\\Resource\\\\A.RES" x',
                '11.000:00ec:trace:file:NtCreateFile handle=0x1 access=1 name=L"\\\\??\\\\C:\\\\joa\\\\CONFIG.DAT" x',
                '12.000:00ec:trace:file:NtCreateFile handle=0x1 access=1 name=L"\\\\??\\\\C:\\\\joa\\\\Resource\\\\A.RES" x',
                '12.500:00ec:trace:file:NtCreateFile handle=0x1 access=1 name=L"\\\\??\\\\C:\\\\windows\\\\x.dll" x']
        assert order(demo) == [(0.0, 2, "Resource/A.RES"), (0.5, 1, "CONFIG.DAT")], order(demo)
        print("ok")
        sys.exit()
    print("t_rel_s\topens\tpath")
    with open(sys.argv[1], errors="replace") as f:
        for row in order(f):
            print(*row, sep="\t")
