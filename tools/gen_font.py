#!/usr/bin/env python3
"""Generate pre-rasterized 8-bit coverage font bitmaps for ARC OS M1.

Outputs kernel/font_inter.h with OFL-licensed Inter glyphs at 12,14,18,24 px.
Run with: py -3 tools/gen_font.py
"""
from __future__ import annotations
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = os.path.join(ROOT, 'tools', 'fonts', 'Inter-Regular.ttf')
OUT_PATH = os.path.join(ROOT, 'kernel', 'font_inter.h')
SIZES = [12, 14, 18, 24]
FIRST = 32
LAST = 126


def render_glyph(font, ch: str):
    # Use getbbox for tight bitmap, getlength for advance
    bbox = font.getbbox(ch, anchor='ls')
    if bbox is None:
        return 0, 0, 0, 0, int(font.getlength(ch)), b''
    x0, y0, x1, y1 = bbox
    w = max(0, x1 - x0)
    h = max(0, y1 - y0)
    adv = int(round(font.getlength(ch)))
    if w == 0 or h == 0:
        return 0, 0, 0, 0, adv, b''
    # Render on L image with padding
    img = Image.new('L', (w, h), 0)
    draw = ImageDraw.Draw(img)
    # anchor ls: baseline at (0, -y0) relative to image top-left
    draw.text((-x0, -y0), ch, fill=255, font=font, anchor='ls')
    return w, h, x0, y0, adv, img.tobytes()


def main():
    if not os.path.exists(FONT_PATH):
        raise SystemExit(f'missing font: {FONT_PATH}')
    with open(OUT_PATH, 'w', newline='\n') as out:
        out.write('/* Generated from Inter-Regular.ttf (OFL) by tools/gen_font.py. */\n')
        out.write('#ifndef ARC_FONT_INTER_H\n')
        out.write('#define ARC_FONT_INTER_H\n\n')
        out.write('#include <stdint.h>\n')
        out.write('#include "gfx.h"\n\n')
        font_names = []
        for size in SIZES:
            font = ImageFont.truetype(FONT_PATH, size)
            ascent, descent = font.getmetrics()
            line_h = ascent + descent
            glyphs = []
            data = bytearray()
            for code in range(FIRST, LAST + 1):
                w, h, xoff, yoff, adv, pix = render_glyph(font, chr(code))
                off = len(data)
                data.extend(pix)
                glyphs.append((w, h, xoff, yoff, adv, off))
            name = f'font_inter_{size}'
            font_names.append((size, name, FIRST, LAST, ascent, line_h))
            out.write(f'static const uint8_t {name}_data[] = {{\n')
            for i in range(0, len(data), 16):
                chunk = ','.join(str(b) for b in data[i:i+16])
                out.write('    ' + chunk + ',\n')
            out.write('};\n\n')
            out.write(f'static const struct gfx_glyph {name}_glyphs[] = {{\n')
            for i, (w, h, xoff, yoff, adv, off) in enumerate(glyphs):
                code = FIRST + i
                ch = chr(code)
                label = ch if 33 <= code <= 126 and ch != '\\' and ch != "'" else f'0x{code:02x}'
                if 33 <= code <= 126 and ch not in "\\'":
                    comment = f" /* '{ch}' */"
                else:
                    comment = f' /* 0x{code:02x} */'
                out.write(f'    {{{w},{h},{xoff},{yoff},{adv},{off}}},{comment}\n')
            out.write('};\n\n')
            out.write(f'static const struct gfx_font {name} = {{\n')
            out.write(f'    {size}, {FIRST}, {LAST}, {ascent}, {line_h}, {name}_glyphs, {name}_data\n')
            out.write('};\n\n')
        out.write('static const struct gfx_font* const gfx_inter_fonts[] = {\n')
        for _, name, *_ in font_names:
            out.write(f'    &{name},\n')
        out.write('};\n')
        out.write(f'#define GFX_INTER_FONT_COUNT {len(font_names)}\n\n')
        out.write('#endif\n')
    print(f'wrote {OUT_PATH}')


if __name__ == '__main__':
    main()
