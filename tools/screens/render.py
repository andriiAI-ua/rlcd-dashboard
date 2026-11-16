#!/usr/bin/env python3
# Make the pictures of the screens for the README: docs/screens/<language>/<screen>.png (800 x 600, the screen
# 400 x 300 at scale 2) and <screen>-small.png (200 x 150, grey). The screens come from the code of the
# firmware (components/ui, gfx, texts) built for the computer with gcc, with the sample data of render.c.
#
# Run it on Linux or in WSL (gcc and Python 3, no other packages):
#   wsl python3 /mnt/d/work/rlcd-dashboard/tools/screens/render.py
import glob
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'docs', 'screens')
W, H = 400, 300


def build(tmp):
    gen = os.path.join(tmp, 'gen')
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'lang.py'), '--out', gen, '--default', 'uk']
                          + sorted(glob.glob(os.path.join(ROOT, 'lang', '*.json'))))
    qr = os.path.join(ROOT, 'managed_components', 'espressif__qrcode')
    sources = ([os.path.join(ROOT, 'tools', 'screens', 'render.c'),
                os.path.join(ROOT, 'components', 'ui', 'ui.c'),
                os.path.join(ROOT, 'components', 'gfx', 'gfx.c'),
                os.path.join(ROOT, 'components', 'texts', 'texts.c'),
                os.path.join(gen, 'texts_data.c'),
                os.path.join(ROOT, 'components', 'util', 'time_util.c'),
                os.path.join(qr, 'esp_qrcode_main.c'),
                os.path.join(qr, 'esp_qrcode_wrapper.c'),
                os.path.join(qr, 'qrcodegen.c')]
               + sorted(glob.glob(os.path.join(ROOT, 'components', 'gfx', 'fonts', '*.c'))))
    includes = ['tools/screens/stub', 'components/ui/include', 'components/gfx/include', 'components/texts/include',
                'components/texts/private', 'components/util/include', 'components/hadata/include',
                'components/settings/include']
    exe = os.path.join(tmp, 'render')
    # esp_qrcode_main.c uses calloc() without <stdlib.h> (the ESP-IDF headers include it).
    cmd = (['gcc', '-std=gnu17', '-O1', '-D_GNU_SOURCE', '-include', 'stdlib.h', '-Wall', '-Wno-unused-function',
            '-o', exe]
           + ['-I' + os.path.join(ROOT, i) for i in includes] + ['-I' + gen, '-I' + os.path.join(qr, 'include'), '-I' + qr]
           + sources + ['-lm'])
    subprocess.check_call(cmd)
    return exe


def read_pbm(path):
    with open(path, 'rb') as f:
        data = f.read()
    header = data.split(b'\n', 2)
    assert header[0] == b'P4' and header[1] == b'%d %d' % (W, H), path
    return header[2]


def pixel(bits, x, y):
    return (bits[y * (W // 8) + x // 8] >> (7 - x % 8)) & 1     # 1 = black


def png(path, width, height, rows, grey):
    """rows: bytes of each row; grey: 8-bit grey, else 1-bit (0 = black)."""
    raw = b''.join(b'\0' + r for r in rows)
    ihdr = struct.pack('>IIBBBBB', width, height, 8 if grey else 1, 0, 0, 0, 0)

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def full(bits):
    """Scale 2, 1 bit: PNG has 1 = white, the canvas has 1 = black."""
    rows = []
    for y in range(H):
        row = bytearray(W * 2 // 8)
        for x in range(W):
            if not pixel(bits, x, y):
                for dx in (0, 1):
                    xx = x * 2 + dx
                    row[xx // 8] |= 0x80 >> (xx % 8)
        rows += [bytes(row), bytes(row)]
    return rows


def small(bits):
    """Half size, grey: the mean of each 2 x 2 block."""
    rows = []
    for y in range(0, H, 2):
        row = bytearray(W // 2)
        for x in range(0, W, 2):
            black = pixel(bits, x, y) + pixel(bits, x + 1, y) + pixel(bits, x, y + 1) + pixel(bits, x + 1, y + 1)
            row[x // 2] = 255 - black * 255 // 4
        rows.append(bytes(row))
    return rows


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        pbm = os.path.join(tmp, 'pbm')
        subprocess.check_call([exe, pbm])
        for path in sorted(glob.glob(os.path.join(pbm, '*', '*.pbm'))):
            lang = os.path.basename(os.path.dirname(path))
            name = os.path.splitext(os.path.basename(path))[0]
            bits = read_pbm(path)
            out = os.path.join(OUT, lang)
            os.makedirs(out, exist_ok=True)
            png(os.path.join(out, name + '.png'), W * 2, H * 2, full(bits), False)
            png(os.path.join(out, name + '-small.png'), W // 2, H // 2, small(bits), True)
            print(os.path.join('docs', 'screens', lang, name + '.png'))


if __name__ == '__main__':
    main()
