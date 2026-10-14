#!/usr/bin/env python3
"""CTest tools.hd_upscale: decoders, dry run (writes nothing, exit 0, also without the upscaler) and a full
run through a fake realesrgan (nearest-neighbour) on a synthetic game dir. No GPU, no game data."""
import importlib.machinery
import importlib.util
import os
import stat
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "hd-upscale")
loader = importlib.machinery.SourceFileLoader("hd_upscale", SCRIPT)
spec = importlib.util.spec_from_loader("hd_upscale", loader)
hu = importlib.util.module_from_spec(spec)
loader.exec_module(hu)


def dds(w, h, body, fourcc=None, argb=False):
    hdr = [0] * 31
    hdr[0], hdr[1], hdr[2], hdr[3], hdr[6], hdr[18] = 124, 0x1007, h, w, 1, 32
    if fourcc:
        hdr[19], hdr[20] = 0x4, struct.unpack("<I", fourcc)[0]
    else:
        hdr[19], hdr[21], hdr[22], hdr[23], hdr[24], hdr[25] = 0x41, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000
    return b"DDS " + struct.pack("<31I", *hdr) + body


RED_DXT1 = struct.pack("<HHI", 0xF800, 0x001F, 0)  # all texels colour0 = red
DXT5 = bytes([255, 0]) + (0x249249249249).to_bytes(6, "little") + struct.pack("<HHI", 0x07E0, 0, 0)  # alpha idx 1 = 0, green


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main():
    w, h, px = hu.decode_dds(dds(8, 4, RED_DXT1 * 2, b"DXT1"))
    assert (w, h) == (8, 4) and px[:4] == bytes((255, 0, 0, 255)) and px[-4:] == bytes((255, 0, 0, 255))
    _, _, px = hu.decode_dds(dds(4, 4, DXT5, b"DXT5"))
    assert px[:4] == bytes((0, 255, 0, 0)), px[:4]
    _, _, px = hu.decode_dds(dds(1, 1, bytes((10, 20, 30, 40))))  # BGRA in memory
    assert px == bytes((30, 20, 10, 40))
    assert hu.pow2(96) == 128 and hu.pow2(80) == 64 and hu.pow2(256) == 256 and hu.pow2(1) == 1

    with tempfile.TemporaryDirectory() as tmp:
        # PNG round trip through the pure-Python reader.
        pil, hu.Image = hu.Image, None
        rgba = bytearray(range(3 * 2 * 4))
        hu.write_png(os.path.join(tmp, "t.png"), 3, 2, rgba)
        assert hu.read_png(os.path.join(tmp, "t.png")) == (3, 2, rgba)
        assert hu.resize(2, 1, bytearray([0, 0, 0, 255, 200, 200, 200, 255]), 4, 1)[12:16] == bytearray([200, 200, 200, 255])
        hu.Image = pil

        game, out = os.path.join(tmp, "game"), os.path.join(tmp, "out")
        write(os.path.join(game, "TEXTURE/Item/Sword.DDS"), dds(8, 8, RED_DXT1 * 4, b"DXT1"))
        write(os.path.join(game, "texture/unit/joan.dds"), dds(4, 4, bytes((0, 0, 255, 128)) * 16))  # red, alpha 128
        write(os.path.join(game, "texture/terrain/S1L1_scene_sector00.dds"), dds(4, 4, RED_DXT1, b"DXT1"))
        write(os.path.join(game, "map/other.dds"), dds(4, 4, RED_DXT1, b"DXT1"))
        base = [sys.executable, SCRIPT, "--game-dir", game, "--out", out]

        r = subprocess.run(base + ["--dry-run", "--scale", "2"], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        lines = r.stdout.splitlines()
        assert len(lines) == 2, r.stdout
        assert "TEXTURE/Item/Sword.DDS 8x8 -> 16x16 " + os.path.join(out, "TEXTURE/Item/Sword.png") in lines, r.stdout
        assert not os.path.exists(out)

        r = subprocess.run(base + ["--tool", os.path.join(tmp, "missing")], capture_output=True, text=True)
        assert r.returncode == 0 and "dry run" in r.stderr and not os.path.exists(out), r.stderr

        # Fake upscaler: nearest-neighbour by -s, RGB PNG out (like realesrgan for RGB input).
        fake = os.path.join(tmp, "fake-esrgan")
        write(fake, ("#!%s\nimport sys, importlib.machinery as m, importlib.util as u\n"
                     "s_ = u.spec_from_loader('hu', m.SourceFileLoader('hu', %r)); hu = u.module_from_spec(s_); s_.loader.exec_module(hu)\n"
                     "a = dict(zip(sys.argv[1::2], sys.argv[2::2])); s = int(a['-s'])\n"
                     "w, h, p = hu.read_png(a['-i'])\n"
                     "o = bytearray(w * s * h * s * 3)\n"
                     "for y in range(h * s):\n"
                     "    for x in range(w * s):\n"
                     "        i = ((y // s) * w + x // s) * 4; o[(y * w * s + x) * 3:(y * w * s + x) * 3 + 3] = p[i:i + 3]\n"
                     "hu.write_png(a['-o'], w * s, h * s, o, 3)\n" % (sys.executable, SCRIPT)).encode())
        os.chmod(fake, os.stat(fake).st_mode | stat.S_IEXEC)
        r = subprocess.run(base + ["--tool", fake, "--scale", "4", "--model", "realesr-animevideov3"], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        w, h, px = hu.read_png(os.path.join(out, "TEXTURE/Item/Sword.png"))
        assert (w, h) == (32, 32) and px[:4] == bytes((255, 0, 0, 255))
        w, h, px = hu.read_png(os.path.join(out, "texture/unit/joan.png"))
        assert (w, h) == (16, 16) and all(px[i:i + 4] == bytes((255, 0, 0, 128)) for i in range(0, len(px), 4))
        assert not os.path.exists(os.path.join(out, "texture/terrain")) and not os.path.exists(os.path.join(out, "map"))
    print("hd_upscale: ok")


if __name__ == "__main__":
    main()
