#!/usr/bin/env python3
"""Generator for synthetic Teledisk (.TD0) images, used by the tests.

Includes an LZHUF encoder (adaptive Huffman + LZSS, after Okumura) for "advanced
compression" images.

Command line: td0gen.py stream <out.td0> <out.raw> [seed]
  writes an advanced-compression file whose body decodes to the bytes in out.raw.
"""
import random
import sys

# ---------------------------------------------------------------- CRC


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0xA097) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


# ---------------------------------------------------------------- LZHUF encoder

RING, LOOKAHEAD, THRESHOLD = 4096, 60, 2
NCHAR = 256 - THRESHOLD + LOOKAHEAD
TABLE = NCHAR * 2 - 1
ROOT = TABLE - 1
MAXFREQ = 0x8000
# (number of position values, code length) for the upper 6 bits of a match position
POSITION_GROUPS = [(1, 3), (3, 4), (8, 5), (12, 6), (24, 7), (16, 8)]


def position_codes():
    """For each upper-6-bit value: (code length, code bits)."""
    codes, first = [], 0
    for count, length in POSITION_GROUPS:
        for _ in range(count):
            codes.append((length, first >> (8 - length)))
            first += 256 >> length
    assert first == 256
    return codes


class Huffman:
    def __init__(self):
        self.freq = [0] * (TABLE + 1)
        self.parent = [0] * (TABLE + NCHAR)
        self.son = [0] * TABLE
        for i in range(NCHAR):
            self.freq[i] = 1
            self.son[i] = i + TABLE
            self.parent[i + TABLE] = i
        i, j = 0, NCHAR
        while j <= ROOT:
            self.freq[j] = self.freq[i] + self.freq[i + 1]
            self.son[j] = i
            self.parent[i] = self.parent[i + 1] = j
            i += 2
            j += 1
        self.freq[TABLE] = 0xFFFF
        self.parent[ROOT] = 0

    rebuilds = 0

    def rebuild(self):
        Huffman.rebuilds += 1
        freq, son, parent = self.freq, self.son, self.parent
        j = 0
        for i in range(TABLE):
            if son[i] >= TABLE:
                freq[j] = (freq[i] + 1) // 2
                son[j] = son[i]
                j += 1
        i, j = 0, NCHAR
        while j < TABLE:
            f = freq[i] + freq[i + 1]
            k = j - 1
            while f < freq[k]:
                k -= 1
            k += 1
            freq[k + 1:j + 1] = freq[k:j]
            freq[k] = f
            son[k + 1:j + 1] = son[k:j]
            son[k] = i
            i += 2
            j += 1
        for i in range(TABLE):
            k = son[i]
            if k >= TABLE:
                parent[k] = i
            else:
                parent[k] = parent[k + 1] = i

    def update(self, c):
        freq, son, parent = self.freq, self.son, self.parent
        if freq[ROOT] == MAXFREQ:
            self.rebuild()
        c = parent[c + TABLE]
        while True:
            freq[c] += 1
            k = freq[c]
            if k > freq[c + 1]:
                l = c + 1
                while k > freq[l + 1]:
                    l += 1
                freq[c] = freq[l]
                freq[l] = k
                i = son[c]
                parent[i] = l
                if i < TABLE:
                    parent[i + 1] = l
                j = son[l]
                son[l] = i
                parent[j] = c
                if j < TABLE:
                    parent[j + 1] = c
                son[c] = j
                c = l
            c = parent[c]
            if c == 0:
                break

    def code(self, c):
        """Bits (root first) for symbol c."""
        bits = []
        k = self.parent[c + TABLE]
        while True:
            bits.append(k & 1)
            k = self.parent[k]
            if k == ROOT:
                break
        bits.reverse()
        return bits


def lzhuf_encode(data):
    huff, codes, bits = Huffman(), position_codes(), []

    def put(value, n):
        for i in range(n - 1, -1, -1):
            bits.append((value >> i) & 1)

    def symbol(c):
        bits.extend(huff.code(c))
        huff.update(c)

    recent = {}
    pos = 0
    while pos < len(data):
        best_len = best_dist = 0
        key = bytes(data[pos:pos + 3])
        if len(key) == 3:
            for a in reversed(recent.get(key, [])[-48:]):
                dist = pos - a
                if dist > RING - LOOKAHEAD - 8:
                    break
                n = 0
                limit = min(LOOKAHEAD, len(data) - pos)
                while n < limit and data[a + n] == data[pos + n]:
                    n += 1
                if n > best_len:
                    best_len, best_dist = n, dist
        if best_len > THRESHOLD:
            symbol(best_len + 255 - THRESHOLD)
            p = best_dist - 1
            length, code = codes[p >> 6]
            put(code, length)
            put(p & 0x3F, 6)
            step = best_len
        else:
            symbol(data[pos])
            step = 1
        for q in range(pos, pos + step):
            recent.setdefault(bytes(data[q:q + 3]), []).append(q)
        pos += step
    while len(bits) % 8:
        bits.append(0)
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


# ---------------------------------------------------------------- TD0 builder

def encode_data(data, method):
    """Encode sector data with method 0 (raw), 1 (2-byte pattern) or 2 (RLE blocks)."""
    if method == 0:
        return bytes(data)
    if method == 1:
        assert data[0::2] == data[0:1] * (len(data) // 2) and data[1::2] == data[1:2] * (len(data) // 2)
        n = len(data) // 2
        return bytes([n & 0xFF, n >> 8, data[0], data[1]])
    out, i = bytearray(), 0
    while i < len(data):
        # Prefer a run of a 2- or 4-byte pattern; otherwise a short literal block.
        for n in (1, 2):
            plen = 1 << n
            pat = data[i:i + plen]
            count = 0
            while count < 255 and data[i + count * plen:i + (count + 1) * plen] == pat:
                count += 1
            if count >= 3:
                out += bytes([n, count]) + pat
                i += count * plen
                break
        else:
            lit = data[i:i + 7]
            out += bytes([0, len(lit)]) + lit
            i += len(lit)
    return bytes(out)


def build_td0(tracks, *, advanced=False, rate=0, drive=1, stepping=0, dos=0, sides=2,
              version=0x15, comment=None, date=(107, 4, 1, 12, 34, 56), terminate=True):
    """tracks: list of (cyl, side, sectors); sector: dict(cyl, head, id, size, flags, method,
    data). `data` None means no data block (flags 0x10/0x20 must then be set)."""
    body = bytearray()
    if comment is not None:
        text = bytes(comment)
        block = bytes([len(text) & 0xFF, len(text) >> 8, *date]) + text
        c = crc16(block)
        body += bytes([c & 0xFF, c >> 8]) + block
        stepping |= 0x80
    for cyl, side, sectors in tracks:
        th = bytes([len(sectors), cyl, side])
        body += th + bytes([crc16(th) & 0xFF])
        for s in sectors:
            sh = bytes([s["cyl"], s["head"], s["id"], s["size"], s["flags"]])
            payload = b""
            if s.get("data") is not None:
                enc = encode_data(s["data"], s["method"])
                payload = bytes([(len(enc) + 1) & 0xFF, (len(enc) + 1) >> 8, s["method"]]) + enc
            body += sh + bytes([crc16(sh + payload) & 0xFF]) + payload
    if terminate:
        body += b"\xff"
    sig = b"td" if advanced else b"TD"
    head = sig + bytes([0, 0, version, rate, drive, stepping, dos, sides])
    c = crc16(head)
    head += bytes([c & 0xFF, c >> 8])
    return head + (lzhuf_encode(body) if advanced else bytes(body)), bytes(body)


def stream_sample(seed):
    rnd = random.Random(seed)
    parts = []
    for i in range(40):
        kind = i % 4
        if kind == 0:
            parts.append(bytes(rnd.getrandbits(8) for _ in range(rnd.randint(3000, 5000))))
        elif kind == 1:
            parts.append((b"The quick brown fox %d jumps over the lazy dog. " % i) * rnd.randint(5, 40))
        elif kind == 2:
            parts.append(bytes([rnd.getrandbits(8)]) * rnd.randint(1, 3000))
        else:
            parts.append(parts[rnd.randrange(len(parts))][:rnd.randint(10, 400)])
    return b"".join(parts)


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "stream":
        seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
        raw = stream_sample(seed)
        head = b"td" + bytes([0, 0, 0x15, 0, 1, 0, 0, 2])
        c = crc16(head)
        with open(sys.argv[2], "wb") as f:
            f.write(head + bytes([c & 0xFF, c >> 8]) + lzhuf_encode(raw))
        with open(sys.argv[3], "wb") as f:
            f.write(raw)
        print(f"{len(raw)} bytes, {Huffman.rebuilds} tree rebuild(s)")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
