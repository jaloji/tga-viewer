"""Generates TGA test files covering every image type, depth, orientation and alpha rule,
plus the PNG each one should decode to.

    python tools/make_testdata.py testdata              # write testdata/*.tga and testdata/expected/*.png
    python tools/make_testdata.py testdata --check out  # compare out/*.png (tga2png output) to expected

Standard library only.
"""
import os
import struct
import sys
import zlib

W, H = 37, 23  # odd sizes catch row / stride mistakes


def reference():
    """BGRA pixels, top-down, with gradients and a varying alpha."""
    px = []
    for y in range(H):
        for x in range(W):
            b = (x * 255) // (W - 1)
            g = (y * 255) // (H - 1)
            r = ((x + y) * 7) % 256
            a = 255 if (x // 6 + y // 6) % 2 == 0 else (x * 11 + y * 5) % 256
            px.append((b, g, r, a))
    return px


# ---- PNG I/O (RGBA8) -------------------------------------------------------------------

def write_png(path, px):
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            b, g, r, a = px[y * W + x]
            raw += bytes((r, g, b, a))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def read_png(path):
    d = open(path, "rb").read()
    i, idat = 8, b""
    while i < len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]
        t, body = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype == 6
        elif t == b"IDAT":
            idat += body
        i += 12 + n
    raw, stride, out, prev, p = zlib.decompress(idat), w * 4, [], bytearray(w * 4), 0
    for _ in range(h):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b, c = prev[x], (prev[x - 4] if x >= 4 else 0)
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(bytes(line))
        prev = line
    data = b"".join(out)
    return w, h, [(data[i + 2], data[i + 1], data[i], data[i + 3]) for i in range(0, len(data), 4)]  # BGRA


# ---- TGA writing -------------------------------------------------------------------------

def q5(v):
    return v >> 3


def e5(v):
    return (v << 3) | (v >> 2)


def rle(values):
    """RLE-encodes stored pixel bytes; packets deliberately cross scanlines."""
    out, i, n = bytearray(), 0, len(values)
    while i < n:
        run = 1
        while i + run < n and run < 128 and values[i + run] == values[i]:
            run += 1
        if run > 1:
            out.append(0x80 | (run - 1))
            out += values[i]
            i += run
            continue
        start = i
        while i < n and i - start < 128 and (i + 1 >= n or values[i + 1] != values[i]):
            i += 1
        out.append(i - start - 1)
        for v in values[start:i]:
            out += v
    return bytes(out)


def tga(image_type, depth, stored, descriptor=0, cmap=b"", cmap_bits=0, cmap_len=0, cmap_first=0,
        image_id=b"", footer_attr=None):
    """stored: list of per-pixel bytes in file order."""
    rle_on = image_type >= 9
    data = rle(stored) if rle_on else b"".join(stored)
    header = struct.pack("<BBBHHBHHHHBB", len(image_id), 1 if cmap_bits else 0, image_type,
                         cmap_first, cmap_len, cmap_bits, 0, 0, W, H, depth, descriptor)
    body = header + image_id + cmap + data
    if footer_attr is not None:
        ext_offset = len(body)
        ext = bytearray(495)
        struct.pack_into("<H", ext, 0, 495)
        ext[494] = footer_attr
        body += bytes(ext) + struct.pack("<II", ext_offset, 0) + b"TRUEVISION-XFILE.\0"
    return body


def file_order(px, top_down, right_to_left):
    rows = range(H) if top_down else range(H - 1, -1, -1)
    cols = range(W - 1, -1, -1) if right_to_left else range(W)
    return [px[y * W + x] for y in rows for x in cols]


def cases():
    ref = reference()
    gray = [((p[0] * 29 + p[1] * 150 + p[2] * 77) // 256) for p in ref]
    out = {}

    # True-color, every depth, raw and RLE.
    for rle_on in (False, True):
        t = 10 if rle_on else 2
        sfx = "_rle" if rle_on else ""
        order = file_order(ref, False, False)
        out[f"tc24{sfx}"] = (tga(t, 24, [bytes(p[:3]) for p in order]), [p[:3] + (255,) for p in ref])
        out[f"tc32{sfx}"] = (tga(t, 32, [bytes(p) for p in order], descriptor=8), ref)
        s16 = [struct.pack("<H", q5(p[0]) | q5(p[1]) << 5 | q5(p[2]) << 10 | (0x8000 if p[3] >= 128 else 0)) for p in order]
        exp16 = [(e5(q5(p[0])), e5(q5(p[1])), e5(q5(p[2])), 255 if p[3] >= 128 else 0) for p in ref]
        out[f"tc16_alpha{sfx}"] = (tga(t, 16, s16, descriptor=1), exp16)
        out[f"tc16_noalpha{sfx}"] = (tga(t, 16, s16, descriptor=0), [e[:3] + (255,) for e in exp16])
        out[f"tc15{sfx}"] = (tga(t, 15, s16, descriptor=0), [e[:3] + (255,) for e in exp16])

    # Grayscale 8 and 16 (with alpha), raw and RLE.
    for rle_on in (False, True):
        t = 11 if rle_on else 3
        sfx = "_rle" if rle_on else ""
        order_idx = file_order(list(range(W * H)), False, False)
        out[f"gray8{sfx}"] = (tga(t, 8, [bytes((gray[i],)) for i in order_idx]), [(g, g, g, 255) for g in gray])
        out[f"gray16{sfx}"] = (tga(t, 16, [bytes((gray[i], ref[i][3])) for i in order_idx], descriptor=8),
                               [(g, g, g, ref[i][3]) for i, g in enumerate(gray)])

    # Color-mapped: 8- and 16-bit indices, 15/16/24/32-bit entries, raw and RLE.
    palette = [((i * 37) % 256, (i * 91) % 256, (i * 53) % 256, (i * 17) % 256) for i in range(256)]
    idx = [((x * 3 + y * 5) % 256) for y in range(H) for x in range(W)]
    for rle_on in (False, True):
        t = 9 if rle_on else 1
        sfx = "_rle" if rle_on else ""
        order_idx = file_order(idx, False, False)
        for bits in (15, 16, 24, 32):
            if bits in (15, 16):
                entries = [struct.pack("<H", q5(c[0]) | q5(c[1]) << 5 | q5(c[2]) << 10 | (0x8000 if c[3] >= 128 else 0))
                           for c in palette]
                use_alpha = bits == 16
                exp_pal = [(e5(q5(c[0])), e5(q5(c[1])), e5(q5(c[2])), (255 if c[3] >= 128 else 0) if use_alpha else 255)
                           for c in palette]
                desc = 1 if use_alpha else 0
            elif bits == 24:
                entries, exp_pal, desc = [bytes(c[:3]) for c in palette], [c[:3] + (255,) for c in palette], 0
            else:
                entries, exp_pal, desc = [bytes(c) for c in palette], palette, 8
            out[f"cmap8_e{bits}{sfx}"] = (
                tga(t, 8, [bytes((i,)) for i in order_idx], descriptor=desc, cmap=b"".join(entries), cmap_bits=bits,
                    cmap_len=256), [exp_pal[i] for i in idx])
        # 16-bit indices with a non-zero first entry.
        first = 1000
        out[f"cmap16{sfx}"] = (
            tga(t, 16, [struct.pack("<H", i + first) for i in order_idx], cmap=b"".join(bytes(c[:3]) for c in palette),
                cmap_bits=24, cmap_len=256, cmap_first=first), [palette[i][:3] + (255,) for i in idx])

    # Orientations.
    for name, top, rtl in (("origin_top_left", True, False), ("origin_bottom_right", False, True),
                           ("origin_top_right", True, True)):
        desc = 8 | (0x20 if top else 0) | (0x10 if rtl else 0)
        out[name] = (tga(2, 32, [bytes(p) for p in file_order(ref, top, rtl)], descriptor=desc), ref)

    # Alpha rules.
    order = file_order(ref, False, False)
    out["alpha_all_zero"] = (tga(2, 32, [bytes(p[:3]) + b"\0" for p in order], descriptor=8),
                             [p[:3] + (255,) for p in ref])
    out["footer_alpha_ignored"] = (tga(2, 32, [bytes(p) for p in order], descriptor=8, footer_attr=0),
                                   [p[:3] + (255,) for p in ref])
    out["footer_alpha_useful"] = (tga(2, 32, [bytes(p) for p in order], descriptor=8, footer_attr=3), ref)
    premul = [tuple((c * p[3] + 127) // 255 for c in p[:3]) + (p[3],) for p in ref]
    unpremul = [tuple(min(255, (c * 255 + p[3] // 2) // p[3]) if p[3] not in (0, 255) else c for c in p[:3]) + (p[3],)
                for p in premul]
    out["footer_premultiplied"] = (
        tga(2, 32, [bytes(p) for p in file_order(premul, False, False)], descriptor=8, footer_attr=4), unpremul)

    # Image ID and a color map present on a true-color image must both be skipped.
    out["id_and_unused_cmap"] = (
        tga(2, 24, [bytes(p[:3]) for p in order], image_id=b"made by make_testdata.py",
            cmap=b"\x11\x22\x33" * 4, cmap_bits=24, cmap_len=4), [p[:3] + (255,) for p in ref])
    return out


def main():
    folder = sys.argv[1]
    all_cases = cases()
    if "--check" in sys.argv:
        out_dir = sys.argv[sys.argv.index("--check") + 1]
        bad = 0
        for name, (_, expected) in sorted(all_cases.items()):
            path = os.path.join(out_dir, name + ".png")
            if not os.path.exists(path):
                print(f"{name:24s} MISSING")
                bad += 1
                continue
            w, h, got = read_png(path)
            diff = max((abs(a - b) for g, e in zip(got, expected) for a, b in zip(g, e)), default=999)
            ok = (w, h) == (W, H) and diff == 0
            bad += not ok
            print(f"{name:24s} {'OK' if ok else 'DIFF max %d' % diff}")
        print(f"{len(all_cases) - bad}/{len(all_cases)} identical")
        return 1 if bad else 0

    os.makedirs(os.path.join(folder, "expected"), exist_ok=True)
    for name, (data, expected) in all_cases.items():
        with open(os.path.join(folder, name + ".tga"), "wb") as f:
            f.write(data)
        write_png(os.path.join(folder, "expected", name + ".png"), expected)
    print(f"wrote {len(all_cases)} TGA files to {folder}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
