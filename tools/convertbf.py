#!/usr/bin/env python3
"""boot_frames.h (png2frames.py --format alpha4)  ->  boot_frames.bin for the Android app.
Usage: python convert_boot_frames.py boot_frames.h assets/boot_frames.bin
Copy the result to app/src/main/assets/boot_frames.bin"""
import re, struct, sys

src, dst = sys.argv[1], sys.argv[2]
t = open(src, encoding="utf-8", errors="replace").read()

def define(name, default=None):
    m = re.search(r"#define\s+%s\s+(\d+)" % name, t)
    if not m:
        if default is None: sys.exit("missing #define " + name)
        return default
    return int(m.group(1))

def array(name):
    m = re.search(name + r"[^=]*=\s*\{(.*?)\};", t, re.S)
    if not m: sys.exit("missing array " + name)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)]

assert define("BOOT_FRAME_FORMAT") == 1, "only alpha4 (format 1) is supported"
w, h = define("BOOT_FRAME_W"), define("BOOT_FRAME_H")
n, fps = define("BOOT_FRAME_COUNT"), define("BOOT_FRAME_FPS")
dsize, dscale = define("BOOT_DITHER_SIZE", 4), define("BOOT_DITHER_SCALE", 2)
offs, data = array("BOOT_ALPHA_OFFSETS"), bytes(array("BOOT_ALPHA_DATA"))
assert len(offs) == n + 1 and offs[-1] == len(data), (len(offs), offs[-1], len(data))

# the firmware picks the largest integer scale leaving a 16 px margin on a 320 px high screen
scale = (320 - 32) // h
with open(dst, "wb") as f:
    f.write(struct.pack("<HHBBBBH", w, h, scale, fps, dsize, dscale, n))
    for i in range(n):
        chunk = data[offs[i]:offs[i + 1]]
        f.write(struct.pack("<I", len(chunk))); f.write(chunk)
print(f"{n} frames {w}x{h} @ {fps} fps, scale x{scale}, dither {dsize}/{dscale}, {len(data)} B RLE")