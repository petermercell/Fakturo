# SPDX-License-Identifier: GPL-3.0-or-later
# Fakturo — invoicing for Slovak and Czech sole traders
# Copyright (C) 2026 Peter Mercell
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

from PIL import Image, ImageDraw, ImageFont
import struct, os

FONT = "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf"
if not os.path.exists(FONT):
    FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

INK = (26, 26, 26)        # #1a1a1a — the same ink the invoice is set in
PAPER = (255, 255, 255)

def draw(size):
    # Work at 4x and downsample: PIL has no antialiased shape drawing, and the
    # corner of a squircle is where that shows.
    s = size * 4
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    # macOS Big Sur grid: the shape occupies 824 of a 1024 canvas, with a
    # corner radius of about 22,4 % of the shape.
    pad = round(s * 100 / 1024)
    box = [pad, pad, s - pad - 1, s - pad - 1]
    radius = round((box[2] - box[0]) * 0.2237)
    d.rounded_rectangle(box, radius=radius, fill=INK)

    # "FA", set as tightly as the letters allow and optically centred: the
    # bounding box of caps sits high, so centring on the box rather than on the
    # line is what puts it in the middle of the square.
    text = "FA"
    inner = box[2] - box[0]
    fs = round(inner * 0.46)
    font = ImageFont.truetype(FONT, fs)
    l, t, r, b = d.textbbox((0, 0), text, font=font)
    x = box[0] + (inner - (r - l)) / 2 - l
    y = box[1] + (inner - (b - t)) / 2 - t
    d.text((x, y), text, font=font, fill=PAPER)

    return im.resize((size, size), Image.LANCZOS)

# The type codes an .icns needs for a modern macOS app, and the pixel size each
# one carries. Written by hand because iconutil is macOS-only.
ENTRIES = [("ic07", 128), ("ic08", 256), ("ic09", 512), ("ic10", 1024),
           ("ic11", 32), ("ic12", 64), ("ic13", 256), ("ic14", 512),
           ("ic04", 16), ("ic05", 32)]

chunks = b""
for code, px in ENTRIES:
    p = f"png_{code}_{px}.png"
    draw(px).save(p, "PNG")
    data = open(p, "rb").read()
    chunks += code.encode("ascii") + struct.pack(">I", len(data) + 8) + data

icns = b"icns" + struct.pack(">I", len(chunks) + 8) + chunks
open("Fakturo.icns", "wb").write(icns)

# Windows wants the same drawing in a .ico. Pillow writes that container
# itself, so there is nothing to assemble by hand — but the sizes have to be
# listed, because the Explorer view, the taskbar and the Alt-Tab card each
# reach for a different one and a missing size is silently upscaled from
# whatever is nearest.
draw(256).save("Fakturo.ico", "ICO",
               sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64),
                      (128, 128), (256, 256)])

draw(512).save("preview.png", "PNG")
print("icns bytes:", len(icns), "entries:", len(ENTRIES))
print("ico written")
