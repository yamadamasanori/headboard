#!/usr/bin/env python3
"""OLED 表示（XIAO ESP32S3 用）のフォントを、BDF から C の配列にする。

使うフォントは X11 の misc-fixed（パブリックドメイン）:
    https://gitlab.freedesktop.org/xorg/font/misc-misc/-/raw/master/5x8.bdf
    https://gitlab.freedesktop.org/xorg/font/misc-misc/-/raw/master/8x13B.bdf

使い方（プロジェクトのルートから）:
    python tools/gen_font.py 5x8.bdf 8x13B.bdf > main/oled_font.c

ASCII（0x20〜0x7E）だけを、1 行 1 バイト（最上位ビットが左端）で書き出す。幅は 8 ドットまで
"""
import pathlib
import sys

FIRST, LAST = 0x20, 0x7E


def load(path):
    lines = pathlib.Path(path).read_text(encoding="latin-1").splitlines()
    fbb = None
    glyphs = {}
    i = 0
    while i < len(lines):
        parts = lines[i].split()
        if not parts:
            i += 1
            continue
        if parts[0] == "FONTBOUNDINGBOX":
            fbb = tuple(int(v) for v in parts[1:5])
        elif parts[0] == "STARTCHAR":
            enc = bbx = None
            bitmap = []
            i += 1
            while not lines[i].startswith("ENDCHAR"):
                p = lines[i].split()
                if p[0] == "ENCODING":
                    enc = int(p[1])
                elif p[0] == "BBX":
                    bbx = tuple(int(v) for v in p[1:5])
                elif p[0] == "BITMAP":
                    i += 1
                    while not lines[i].startswith("ENDCHAR"):
                        bitmap.append(int(lines[i][:2], 16))  # 幅 8 ドットまでなので先頭の 1 バイトだけ
                        i += 1
                    break
                i += 1
            if enc is not None and FIRST <= enc <= LAST:
                glyphs[enc] = (bbx, bitmap)
        i += 1
    w, h, xoff, yoff = fbb
    if w > 8:
        sys.exit(f"{path}: 幅 {w} は 8 ドットを超えています")
    ascent = h + yoff
    rows = []
    for c in range(FIRST, LAST + 1):
        cell = [0] * h
        if c in glyphs:
            (gw, gh, gx, gy), bitmap = glyphs[c]
            top = ascent - (gy + gh)
            for r, bits in enumerate(bitmap):
                y = top + r
                if 0 <= y < h:
                    cell[y] = (bits >> (gx - xoff)) & 0xFF
        rows.append((c, cell))
    return w, h, rows


def emit(name, path, out):
    w, h, rows = load(path)
    out.append(f"// {pathlib.Path(path).name}（X11 misc-fixed、パブリックドメイン）")
    out.append(f"static const uint8_t {name}_data[] = {{")
    for c, cell in rows:
        ch = chr(c)
        label = "\\\\" if ch == "\\" else ch
        out.append("    " + ", ".join(f"0x{b:02x}" for b in cell) + f",  // '{label}'")
    out.append("};")
    out.append(f"const oled_font_t {name} = {{.w = {w}, .h = {h}, .data = {name}_data}};")
    out.append("")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out = [
        "// tools/gen_font.py で作ったファイル。直接編集しない",
        '#include "oled_font.h"',
        "",
    ]
    emit("oled_font_small", sys.argv[1], out)
    emit("oled_font_large", sys.argv[2], out)
    sys.stdout.write("\n".join(out))


main()
