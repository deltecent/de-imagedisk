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
