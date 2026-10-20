#!/usr/bin/env python3
"""End-to-end tests for imdu, bin2imd and td02imd.

Usage: tool_tests.py <bin-dir> <test-name>
Each test runs in its own temporary directory with stdin from /dev/null.
"""
import os
import random
import subprocess
import sys
import tempfile

BIN = None
TESTS = {}


def test(fn):
    TESTS[fn.__name__] = fn
    return fn


class Failure(Exception):
    pass


def check(cond, msg="check failed"):
    if not cond:
        raise Failure(msg)


def run(tool, *args, expect=0):
    p = subprocess.run([os.path.join(BIN, tool), *args], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    out, err = p.stdout.decode("latin-1"), p.stderr.decode("latin-1")
    if expect is not None and p.returncode != expect:
        raise Failure(f"{tool} {' '.join(args)}: exit {p.returncode}, expected {expect}\n"
                      f"stdout:\n{out}\nstderr:\n{err}")
    return out, err


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def parse_imd(data):
    """Independent IMD parser: returns (comment, [track dicts])."""
    end = data.index(0x1A)
    comment, pos, tracks = data[:end], end + 1, []
    while pos < len(data):
        mode, cyl, head, nsec, size = data[pos:pos + 5]
        pos += 5
        smap = list(data[pos:pos + nsec]); pos += nsec
        cmap = hmap = None
        if head & 0x80:
            cmap = list(data[pos:pos + nsec]); pos += nsec
        if head & 0x40:
            hmap = list(data[pos:pos + nsec]); pos += nsec
        ssize = 128 << size
        sectors = []
        for _ in range(nsec):
            t = data[pos]; pos += 1
            if t == 0:
                sectors.append((0, None))
            elif (t - 1) & 1:
                sectors.append((t, bytes([data[pos]]) * ssize)); pos += 1
            else:
                sectors.append((t, data[pos:pos + ssize])); pos += ssize
        tracks.append(dict(mode=mode, cyl=cyl, head=head & 0x0F, flags=head & 0xC0, size=size,
                           smap=smap, cmap=cmap, hmap=hmap, sectors=sectors))
    check(pos == len(data), "trailing data in IMD")
    return comment, tracks


def build_imd(comment, tracks):
    """Independent IMD writer. Track: (mode, cyl, head, size, smap, sectors[, cmap, hmap]);
    sector: (type, fill-or-bytes)."""
    out = bytearray(comment) + b"\x1a"
    for t in tracks:
        mode, cyl, head, size, smap, sectors = t[:6]
        cmap = t[6] if len(t) > 6 else None
        hmap = t[7] if len(t) > 7 else None
        flags = (0x80 if cmap else 0) | (0x40 if hmap else 0)
        out += bytes([mode, cyl, head | flags, len(smap), size]) + bytes(smap)
        if cmap:
            out += bytes(cmap)
        if hmap:
            out += bytes(hmap)
        for typ, val in sectors:
            out.append(typ)
            if typ == 0:
                continue
            if (typ - 1) & 1:
                out.append(val)
            else:
                check(len(val) == 128 << size)
                out += val
    return bytes(out)


def sector_bytes(tag, size=0):
    return bytes((tag + i) & 0xFF for i in range(128 << size))


def make_bin(path, size, seed=1):
    rnd = random.Random(seed)
    data = bytearray(rnd.getrandbits(8) for _ in range(size))
    # A few all-same sectors so compression is exercised.
    for s in (0, 5, 77):
        data[s * 512:(s + 1) * 512] = bytes([0xE5]) * 512
    write(path, bytes(data))
    return bytes(data)


def bin_from_tracks(tracks):
    """Sector data in ascending sector-ID order per track."""
    out = bytearray()
    for t in tracks:
        for _, (_, d) in sorted(zip(t["smap"], t["sectors"]), key=lambda x: x[0]):
            out += d if d is not None else bytes(128 << t["size"])
    return bytes(out)


# ---------------------------------------------------------------- bin2imd

@test
def bin2imd_144m():
    data = make_bin("x.bin", 1474560)
    out, _ = run("bin2imd", "x.bin", "x.imd", "N=80", "/2", "DM=3", "SS=512", "SM=1-18",
                 "C=Test~comment")
    comment, tracks = parse_imd(read("x.imd"))
    check(comment.startswith(b"IMD 1.20: "), comment)
    check(comment.endswith(b"\r\nTest comment\r\n"), comment)
    check(len(tracks) == 160)
    check([(t["cyl"], t["head"]) for t in tracks[:3]] == [(0, 0), (0, 1), (1, 0)])
    check(all(t["mode"] == 3 and t["size"] == 2 and t["smap"] == list(range(1, 19)) for t in tracks))
    check(tracks[0]["sectors"][0][0] == 2 and tracks[0]["sectors"][1][0] == 1)
    check(bin_from_tracks(tracks) == data)
    check("smaller" not in out and "larger" not in out, out)


@test
def bin2imd_uncompressed_and_size_messages():
    make_bin("x.bin", 10 * 9 * 512 + 100)
    out, _ = run("bin2imd", "x", "y", "N=10", "DM=5", "SS=512", "SM=1-9", "/U")
    check("larger than output image" in out, out)
    _, tracks = parse_imd(read("y.imd"))
    check(all(s[0] == 1 for t in tracks for s in t["sectors"]))
    out, _ = run("bin2imd", "x.bin", "z.imd", "N=11", "DM=5", "SS=512", "SM=1-9", "F=$E5")
    check("smaller than output image (ends at CHS 10/0/0)" in out, out)
    _, tracks = parse_imd(read("z.imd"))
    check(tracks[10]["sectors"][1] == (2, bytes([0xE5]) * 512))


@test
def bin2imd_maps():
    make_bin("x.bin", 2 * 2 * 8 * 256)
    run("bin2imd", "x.bin", "x.imd", "N=2", "DM=4", "SS=256", "SM=8-5,1.1,2,3,4", "SM1=%1,2-8",
        "CM1=@7.8", "HM1=0.8")
    _, tracks = parse_imd(read("x.imd"))
    check(len(tracks) == 4)
    check(tracks[0]["smap"] == [8, 7, 6, 5, 1, 2, 3, 4], tracks[0]["smap"])
    check(tracks[0]["flags"] == 0)
    check(tracks[1]["smap"] == list(range(1, 9)))
    check(tracks[1]["flags"] == 0xC0 and tracks[1]["cmap"] == [7] * 8 and tracks[1]["hmap"] == [0] * 8)


@test
def bin2imd_option_file():
    data = make_bin("x.bin", 3 * 26 * 128 + 4 * 2 * 9 * 512)
    write("fmt.b2i", b"; mixed format\n"
                     b"0 DM=0 SS=128 SM=1-26 C=Mixed\n"
                     b"\n"
                     b"3 DM=3 SS=512 SM=1-9 SM1=1-9 DM1=3\n")
    out, _ = run("bin2imd", "x.bin", "x.imd", "fmt", "N=7", "/V")
    check("3/1:  Mode:3  Sectors:9  Ssize:2/512" in out, out)
    comment, tracks = parse_imd(read("x.imd"))
    check(comment.endswith(b"\r\nMixed\r\n"), comment)
    check([(t["cyl"], t["head"], t["mode"], len(t["smap"])) for t in tracks] ==
          [(0, 0, 0, 26), (1, 0, 0, 26), (2, 0, 0, 26)] +
          [(c, h, 3, 9) for c in range(3, 7) for h in (0, 1)])
    check(bin_from_tracks(tracks) == data)
    write("bad.b2i", b"0 DM=3 SS=512 SM=1-9\n2 N=5\n")
    _, err = run("bin2imd", "x.bin", "x.imd", "bad.b2i", "N=3", expect=1)
    check("N=5: Not allowed in track records" in err, err)


@test
def bin2imd_errors():
    write("x.bin", b"")
    cases = [
        (["SS=512", "SM=1", "N=1"], "Side0: Data Mode must be defined."),
        (["DM=3", "SM=1", "N=1"], "Side0: Sector Size must be defined"),
        (["DM=3", "SS=512", "N=1"], "Side0: At least 1 sector must be defined"),
        (["DM=3", "SS=512", "SM=1-3", "CM=1,2", "N=1"], "Cylinder/Head maps must match sector map size"),
        (["DM=3", "SS=512", "SM=1,2,1", "N=1"], "Side0: Duplicate sector map entry 1"),
        (["DM=3", "SS=512", "SM=1"], "N= (cylinders) not specified"),
        (["DM=9"], "DM=9: Value 9 out of range 0-5"),
        (["SS=500"], "SS=500: Bad sector size"),
        (["SM=1x"], "SM=1x: Bad digit 'x'"),
        (["DM=3", "SS=512", "SM=1", "SM1=1", "SS1=128", "DM1=1", "HM1=0,0", "N=1"],
         "Side1: Cylinder/Head maps must match sector map size"),
    ]
    for args, msg in cases:
        _, err = run("bin2imd", "x.bin", "x.imd", *args, expect=1)
        check(msg in err, f"{args}: {err!r}")
    out, _ = run("bin2imd", expect=1)
    check("Use: bin2imd" in out)


# ---------------------------------------------------------------- imdu

def make_imd(name="x", seed=1, extra=()):
    data = make_bin(name + ".bin", 80 * 2 * 18 * 512, seed)
    run("bin2imd", name + ".bin", name + ".imd", "N=80", "/2", "DM=3", "SS=512", "SM=1-18",
        "C=Sample", *extra)
    return data


@test
def imdu_binary_round_trip():
    data = make_imd()
    out, _ = run("imdu", "x.imd", "out", "/B", "/Y")
    check(read("out.bin") == data)
    check("80 kbps" not in out)
    check(" 0/0 500 kbps DD  18x512" in out, out)
    check("160 tracks(80/80), 2880 sectors (3 Compressed)" in out, out)


@test
def imdu_identity_and_compression():
    make_imd()
    # bin2imd already compressed the all-same sectors, so /C changes nothing.
    run("imdu", "x.imd", "y.imd", "/C", "/Y")
    check(read("x.imd") == read("y.imd"))
    run("imdu", "x.imd", "e.imd", "/E", "/Y")
    _, tracks = parse_imd(read("e.imd"))
    check(all(s[0] == 1 for t in tracks for s in t["sectors"]))
    check(len(read("e.imd")) > len(read("x.imd")))
    run("imdu", "e.imd", "c.imd", "/C", "/Y")
    check(read("c.imd") == read("x.imd"))


@test
def imdu_overwrite_never_blocks():
    make_imd()
    write("y.imd", b"old")
    _, err = run("imdu", "x.imd", "y.imd", "/C", expect=1)
    check("already exists" in err, err)
    check(read("y.imd") == b"old")
    run("imdu", "x.imd", "y.imd", "/C", "/Y")
    check(read("y.imd") != b"old")
    _, err = run("imdu", "x.imd", "/C", expect=1)
    check("No output file." in err, err)


@test
def imdu_comments():
    make_imd()
    out, _ = run("imdu", "x.imd", "EC=cmt")
    check("\nSample\n" in out, out)
    cmt = read("cmt.txt")
    check(cmt.startswith(b"IMD 1.20: ") and cmt.endswith(b"\r\nSample\r\n"), cmt)
    write("new.txt", b"Replaced\r\n")
    run("imdu", "x.imd", "r.imd", "RC=new", "/Y")
    check(parse_imd(read("r.imd"))[0] == b"Replaced\r\n")
    run("imdu", "x.imd", "a.imd", "AC=new.txt", "/Y")
    check(parse_imd(read("a.imd"))[0] == cmt + b"Replaced\r\n")


@test
def imdu_detail():
    make_imd()
    out, _ = run("imdu", "x.imd", "/D")
    lines = out.splitlines()
    i = lines.index(" 0/0 500 kbps DD  18x512")
    check(lines[i + 1].startswith("      1   2   3   "), lines[i + 1])
    check(lines[i + 2].startswith("      De5 D   D   D   "), lines[i + 2])
    run("imdu", "x.imd", expect=0)
    _, err = run("imdu", "missing.imd", expect=1)
    check("missing.imd" in err, err)
    write("bad.imd", b"comment only")
    _, err = run("imdu", "bad.imd", expect=1)
    check("EOF in comment" in err, err)


@test
def imdu_merge():
    full = lambda c, h, ids: [(1, sector_bytes(c * 16 + h * 8 + i)) for i in ids]
    a = build_imd(b"A\r\n", [
        (2, 0, 0, 0, [1, 2, 3], full(0, 0, [1, 2, 3])),
        (2, 1, 0, 0, [1, 2, 3], full(1, 0, [1, 2]) + [(0, None)]),
    ])
    b = build_imd(b"B\r\n", [
        (2, 0, 0, 0, [1, 2, 3], full(0, 0, [1, 2]) + [(1, sector_bytes(99))]),
        (2, 0, 1, 0, [1, 2], full(0, 1, [1, 2])),
        (2, 1, 0, 0, [3, 1, 2, 10], full(1, 0, [3, 1, 2, 10])),
        (2, 2, 0, 0, [1], full(2, 0, [1])),
    ])
    write("a.imd", a)
    write("b.imd", b)
    out, _ = run("imdu", "a.imd", "b.imd", "m.imd", "/Y")
    for msg in [" 0/0 Merge data differs from original 2 2",
                " 1/0 Adding track 0/1 from mergefile",
                "     Filling in sector 2/3",
                "     Adding sector 3/10",
                "Adding track 2/0 from mergefile",
                "4 tracks(3/1), 10 sectors"]:
        check(msg in out, f"{msg!r} not in:\n{out}")
    comment, tracks = parse_imd(read("m.imd"))
    check(comment == b"A\r\n")
    check([(t["cyl"], t["head"]) for t in tracks] == [(0, 0), (0, 1), (1, 0), (2, 0)])
    t = tracks[2]
    check(t["smap"] == [1, 2, 3, 10])
    check([d for _, d in t["sectors"]] == [sector_bytes(16 + i) for i in (1, 2, 3, 10)])
    check(tracks[0]["sectors"][2][1] == sector_bytes(3), "input data kept")

    out, _ = run("imdu", "a.imd", "b.imd")
    check("No output file - compare only." in out and "Filling in sector 2/3" in out, out)
    out, _ = run("imdu", "a.imd", "b.imd", "/Q")
    check("Filling" not in out and "compare only" not in out, out)

    # Mode mismatch is reported unless /M is given.
    write("c.imd", build_imd(b"C\r\n", [(5, 0, 0, 0, [1, 2, 3], full(0, 0, [1, 2, 3]))]))
    out, _ = run("imdu", "a.imd", "c.imd")
    check(" 0/0 Incompatible track mode" in out and "Track not found in merge file" in out, out)
    out, _ = run("imdu", "a.imd", "c.imd", "/M")
    check("Incompatible" not in out, out)


@test
def imdu_merge_identity():
    make_imd()
    out, _ = run("imdu", "x.imd", "x.imd", "y.imd", "/Y")
    check(read("x.imd") == read("y.imd"))
    for real in REAL_IMAGES:
        if os.path.exists(real):
            run("imdu", real, real, "r.imd", "/Y")
            check(read(real) == read("r.imd"), real)


def interleave_image():
    smap = [1, 4, 7, 2, 5, 8, 3, 6, 9]
    write("i.imd", build_imd(b"I\r\n", [(5, 0, 0, 0, smap, [(1, sector_bytes(i)) for i in smap])]))


@test
def imdu_interleave():
    interleave_image()
    run("imdu", "i.imd", "g.imd", "IL=", "/Y")
    check(read("g.imd") == read("i.imd"), "best guess keeps 3:1")
    run("imdu", "i.imd", "o.imd", "IL=1", "/Y")
    t = parse_imd(read("o.imd"))[1][0]
    check(t["smap"] == list(range(1, 10)) and t["sectors"][4][1] == sector_bytes(5))
    run("imdu", "i.imd", "t.imd", "il=2", "/Y")
    t = parse_imd(read("t.imd"))[1][0]
    check(t["smap"] == [1, 6, 2, 7, 3, 8, 4, 9, 5], t["smap"])
    check([d for _, d in t["sectors"]] == [sector_bytes(i) for i in t["smap"]])
    out, _ = run("imdu", "i.imd", "b", "/B", "/Y")
    check("Assuming 1:1 for Binary output" in out, out)
    check(read("b.bin") == b"".join(sector_bytes(i) for i in range(1, 10)))
    run("imdu", "i.imd", "b2", "/B", "IL=3", "/Y")
    check(read("b2.bin") == b"".join(sector_bytes(i) for i in [1, 4, 7, 2, 5, 8, 3, 6, 9]))
    _, err = run("imdu", "i.imd", "o.imd", "IL=100", "/Y", expect=1)
    check("Value (100) out of range: 1-99" in err, err)


@test
def imdu_translate_exclude_fill():
    tracks = []
    for c in range(5):
        for h in (0, 1):
            tracks.append((2 if c == 0 else 5, c, h, 0, [1, 2],
                           [(1, sector_bytes(c)), (0, None) if c == 4 else (3, sector_bytes(9))]))
    write("t.imd", build_imd(b"T\r\n", tracks))
    run("imdu", "t.imd", "o.imd", "T2=500", "X=1", "X0=2", "X1=3,3", "/Y")
    _, out = parse_imd(read("o.imd"))
    check([(t["cyl"], t["head"]) for t in out] == [(0, 0), (0, 1), (2, 1), (3, 0), (4, 0), (4, 1)])
    check(out[0]["mode"] == 0 and out[2]["mode"] == 3, [t["mode"] for t in out])
    check(out[0]["sectors"][1][0] == 3, "deleted mark kept by default")
    check(out[4]["sectors"][1] == (0, None))
    run("imdu", "t.imd", "f.imd", "F=e5", "/ND", "/Y")
    _, out = parse_imd(read("f.imd"))
    check(out[0]["sectors"][1][0] == 1, "/ND drops deleted mark")
    check(out[8]["sectors"][1] == (1, bytes([0xE5]) * 128))
    run("imdu", "t.imd", "fc.imd", "F=0", "/C", "/Y")
    _, out = parse_imd(read("fc.imd"))
    check(out[8]["sectors"][1] == (2, bytes(128)))
    run("imdu", "t.imd", "t5.imd", "T5=250", "T3=500", "/Y")
    check(read("t5.imd") == read("t.imd"))
    _, err = run("imdu", "t.imd", "o.imd", "X=1,2,3", "/Y", expect=1)
    check("Syntax error" in err, err)
    _, err = run("imdu", "t.imd", "o.imd", "F=1G", "/Y", expect=1)
    check("Bad number" in err, err)
    out, _ = run("imdu", "t.imd", "o.imd", "T2=123", "/Y", expect=1)
    check("Use: imdu" in out)


@test
def imdu_json():
    import json
    tracks = [(5, 0, 0, 1, [1, 2, 3], [(2, 0xE5), (0, None), (8, 0x11)]),
              (5, 0, 1, 1, [1], [(3, sector_bytes(1, 1))], [7], [0])]
    write("j.imd", build_imd(b"Hi \"there\"\r\n", tracks))
    out, err = run("imdu", "j.imd", "--json")
    doc = json.loads(out)
    check(doc["comment"] == 'Hi "there"\r\n', doc["comment"])
    t0, t1 = doc["tracks"]
    check(t0["mode"] == 5 and t0["rate_kbps"] == 250 and t0["encoding"] == "MFM" and t0["sector_size"] == 256)
    check(t0["sectors"][0] == {"id": 1, "cylinder": 0, "head": 0, "available": True, "compressed": True,
                               "deleted": False, "bad": False, "fill": 229}, t0["sectors"][0])
    check(t0["sectors"][1]["available"] is False)
    check(t0["sectors"][2]["deleted"] and t0["sectors"][2]["bad"])
    check(t1["cylinder_map"] and t1["sectors"][0]["cylinder"] == 7)
    check(doc["totals"] == {"tracks": 2, "head0_tracks": 1, "head1_tracks": 1, "sectors": 4,
                            "compressed": 2, "deleted": 2, "bad": 1, "unavailable": 1}, doc["totals"])


REAL_IMAGES = [os.path.expanduser("~/Downloads/CDOSMBD.IMD"), os.path.expanduser("~/Downloads/CPM80-22.IMD")]


def main():
    global BIN
    BIN = os.path.abspath(sys.argv[1])
    name = sys.argv[2]
    with tempfile.TemporaryDirectory() as d:
        os.chdir(d)
        try:
            TESTS[name]()
        except Failure as e:
            print(f"FAIL {name}: {e}")
            return 1
    print(f"ok {name}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--list":
        print(";".join(TESTS))
        sys.exit(0)
    sys.exit(main())
