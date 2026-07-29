"""Leftmost x of the white box on the row through its centre.

Pure-stdlib PNG reader: the verify script must not need a pip install to
check its own output.
"""
import sys, zlib, struct

def read_png(path):
    d = open(path, 'rb').read()
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]
        typ = d[pos+4:pos+8]
        if typ == b'IHDR':
            w, h = struct.unpack('>II', d[pos+8:pos+16])
        elif typ == b'IDAT':
            idat += d[pos+8:pos+8+ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride, prev, rows, i = w * 3, bytearray(w * 3), [], 0
    for _ in range(h):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i+stride]); i += stride
        for x in range(stride):
            a = line[x-3] if x >= 3 else 0
            b = prev[x]
            c = prev[x-3] if x >= 3 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(bytes(line)); prev = line
    return w, h, rows

w, h, rows = read_png(sys.argv[1])
y = h // 2
xs = [x for x in range(w)
      if (rows[y][x*3], rows[y][x*3+1], rows[y][x*3+2]) == (255, 255, 255)]
print(min(xs) if xs else -1)
