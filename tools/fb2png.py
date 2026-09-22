#!/usr/bin/env python3
"""Convert fbshot raw dumps (FB W H bpp roff goff boff header + raw pixels) to PNG."""
import sys, struct
from PIL import Image

def convert(path, out):
    with open(path, 'rb') as f:
        header = f.readline().decode('ascii').strip().split()
        assert header[0] == 'FB', header
        W, H, bpp, ro, go, bo = map(int, header[1:7])
        data = f.read(W * H * (bpp // 8))
    img = Image.new('RGB', (W, H))
    px = img.load()
    for y in range(H):
        base = y * W * (bpp // 8)
        for x in range(W):
            off = base + x * (bpp // 8)
            v = struct.unpack_from('<I', data, off)[0]
            r = (v >> ro) & 0xFF
            g = (v >> go) & 0xFF
            b = (v >> bo) & 0xFF
            px[x, y] = (r, g, b)
    img.save(out)
    return img

for p in sys.argv[1:]:
    out = p.rsplit('.', 1)[0] + '.png'
    img = convert(p, out)
    # sample some pixels for quick verification
    print(f"{p} -> {out} size={img.size} px(400,240)={img.getpixel((400,240))} px(10,10)={img.getpixel((10,10))}")
