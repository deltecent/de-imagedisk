# The Teledisk (.TD0) file format

Teledisk was Sydex's DOS disk-imaging program. Its file format was never published. This
description comes from Dave Dunfield's TD0 notes, Haruhiko Okumura's LZHUF algorithm, and
checking real images. It covers what `td02imd` reads (`src/core/td0_reader.cpp`). All 103
TD0 files in one collection convert without error. 22 of them had IMD files made by DOS
TD02IMD, 7 of those with advanced compression, and `td02imd` reproduces all 22
byte for byte.

Multi-byte values are little-endian.

## Layout

```
+--------------------------------+
| header, 12 bytes               |  never compressed
+--------------------------------+
| comment block (optional)       |  everything from here on is
| track 0: track header          |  LZHUF compressed in an
|          sector records ...    |  "advanced compression" image
| track 1: ...                   |
| ...                            |
| end marker (0xFF)              |
+--------------------------------+
```

## CRC

Teledisk uses one 16-bit CRC throughout: polynomial `0xA097`, initial value 0, processed
most significant bit first, with no reflection and no final XOR.

```c
uint16_t td0_crc(const uint8_t *p, size_t n, uint16_t crc)
{
    while (n--) {
        crc ^= *p++ << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0xA097 : crc << 1;
    }
    return crc;
}
```

Where it is stored:

| Item | Stored | Covers |
|---|---|---|
| header | 2 bytes | header bytes 0-9 |
| comment block | 2 bytes | comment block bytes 2-9 and the comment text |
| track header | low byte | track header bytes 0-2 |
| sector header | low byte | the decoded sector data (not the header) |

The sector CRC byte is informational: `td02imd` doesn't check it.

## Header (12 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | signature | `TD` (`54 44`) normal; `td` (`74 64`) advanced compression |
| 2 | 1 | volume sequence | 0 for the first file of a multi-volume set |
| 3 | 1 | check sequence | identifies the files of one multi-volume set |
| 4 | 1 | version | Teledisk version, as two nibbles: `0x15` is shown as 1.5 |
| 5 | 1 | data rate | bits 0-1: 0 = 250 kbps, 1 = 300 kbps, 2 = 500 kbps; bit 7: the whole disk is FM |
| 6 | 1 | drive type | see below |
| 7 | 1 | stepping | bits 0-1: 0 = single, 1 = double, 2 = even-only; bit 7: a comment block follows |
| 8 | 1 | DOS mode | nonzero: only sectors allocated by DOS were stored |
| 9 | 1 | sides | number of sides |
| 10 | 2 | CRC | of bytes 0-9 |

Drive type as `td02imd` names it in the IMD comment:

| Value | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| Drive | 5.25-96/48 | 5.25 | 5.25-96 | 3.5 | 3.5 | 8" | 3.5 |

Data rate 0 and 1 are both labelled LD (low density) and 2 HD. Rate 1 is a 250 kbps disk
read in a 360 RPM drive; `td02imd` stores it as 250 kbps unless `/L` is given. Data rate
values above 2 are rejected.

Multi-volume sets (an image split across several files) aren't supported: the sequence
bytes are read but ignored.

## Advanced compression

With the `td` signature, every byte after the 12-byte header is one continuous LZHUF
stream. The decoded stream has exactly the layout of a normal image from offset 12 on.
The compression doesn't restart per track, so the whole stream must be decoded from the
start.

LZHUF is LZSS (back references into a window of recent output) with the literals and
match lengths coded by an adaptive Huffman code. Okumura's LZHUF.C (1988) is the reference.
Teledisk uses its standard parameters:

| Parameter | Value |
|---|---|
| window (ring buffer) | 4096 bytes, initially filled with spaces (`0x20`) |
| first output position in the ring | 4096 - 60 = 4036 |
| longest match | 60 bytes |
| threshold | 2 (matches are 3-60 bytes long) |
| symbols | 314: 0-255 literal bytes, 256-313 match lengths 3-60 |
| Huffman tree | 627 nodes, adaptive; rebuilt when the root count reaches `0x8000` |

Bits are read from each byte most significant bit first. Past the end of the file the
input reads as zero bits, and the decoded stream ends there.

Decoding one step:

1. Decode a symbol `c` by walking the Huffman tree from the root, one bit per node, then
   update the tree for `c`.
2. If `c < 256`, output byte `c` and store it in the ring.
3. Otherwise decode a match position `p`, then copy `c - 253` bytes. The copy starts at ring
   index `(r - p - 1) mod 4096`, where `r` is the current ring position. Each copied byte
   is output and also stored in the ring, so a match can overlap its own output.

The tree starts with all 314 leaves at count 1, paired bottom up. After each symbol, its
count is increased along the path to the root. Nodes are swapped as needed to keep the
counts in non-decreasing order. When the root count reaches `0x8000`, every leaf count is
halved (rounding up) and the tree is rebuilt from the leaves. The encoder and decoder must
do exactly the same updates, so follow the reference algorithm precisely.

A match position is 12 bits. The upper 6 bits use a fixed prefix code, and the lower 6
bits are stored as plain bits:

1. Read 8 bits as `i`.
2. Look up the upper 6 bits and the code length `L` for `i`. The table is built in order
   from these groups of upper-bit values:

   | Upper values | Count | Code length L |
   |---|---|---|
   | 0 | 1 | 3 |
   | 1-3 | 3 | 4 |
   | 4-11 | 8 | 5 |
   | 12-23 | 12 | 6 |
   | 24-47 | 24 | 7 |
   | 48-63 | 16 | 8 |

   Each value covers `256 >> L` consecutive entries of the 256-entry table.
3. Read `L - 2` more bits, shifting each one into `i` from the right.
4. The position is `upper << 6 | (i & 0x3F)`.

## Comment block

Present when stepping bit 7 is set.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | CRC of bytes 2-9 and the text |
| 2 | 2 | text length in bytes |
| 4 | 1 | year - 1900 |
| 5 | 1 | month, 0-11 |
| 6 | 1 | day, 1-31 |
| 7 | 1 | hour |
| 8 | 1 | minute |
| 9 | 1 | second |
| 10 | length | text |

In the text, a NUL byte ends each line, and the text is often padded with extra NULs.
`td02imd` turns each NUL into a line break, drops the trailing ones, and adds the date to
the IMD comment as ` d/mm/yyyy h:mm:ss`.

## Track header (4 bytes)

| Offset | Field | Notes |
|---|---|---|
| 0 | sector count | `0xFF` marks the end of the image; nothing follows it |
| 1 | cylinder | physical cylinder |
| 2 | side | bit 0: physical head; bit 7: this track is FM; other bits must be zero |
| 3 | CRC | low byte of the CRC of bytes 0-2 |

The end marker is a single `0xFF` byte. Readers should also accept a file that simply ends
where the next track header would begin. A track can have zero sectors.

## Sector record

Each sector is a 6-byte header, followed by a data block unless the flags say there is no
data.

| Offset | Field | Notes |
|---|---|---|
| 0 | cylinder | from the sector's ID field |
| 1 | head | from the ID field |
| 2 | sector number | from the ID field |
| 3 | size code | `128 << code` bytes, 0-6 |
| 4 | flags | below |
| 5 | CRC | low byte of the CRC of the decoded sector data |

| Flag | Meaning |
|---|---|
| `0x01` | sector number appears more than once in the track |
| `0x02` | data read with a CRC error |
| `0x04` | deleted-data address mark |
| `0x10` | skipped in DOS mode (unallocated); no data block |
| `0x20` | ID field found but no data; no data block |
| `0x40` | data found without an ID field |

### Data block

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | length: the method byte plus the encoded data that follows |
| 2 | 1 | method |
| 3 | length - 1 | encoded data |

The encoded data must decode to at least the sector size. Decoders discard any extra
output, and `td02imd` rejects a block whose length doesn't match what was consumed.

| Method | Encoding |
|---|---|
| 0 | raw: the sector data, sector size bytes |
| 1 | repeated pair: a 2-byte count, then 2 bytes repeated count times |
| 2 | run-length blocks, repeated until the sector is full |

A method 2 block starts with a byte `n`:

- `n = 0`: literal block. A count byte, then that many data bytes.
- `n > 0`: repeated block. A count byte, then a pattern of `1 << n` bytes, output count
  times. Dunfield's notes give the pattern length as `2 * n`, but TD02IMD uses `1 << n`.
  The two agree for n = 1 and n = 2, the only values real images use.

## Example

The start of a normal (`TD`) image, `mor-adv.td0`:

```
54 44                    signature "TD": not compressed
00                       volume sequence 0
4F                       check sequence
15                       version 1.5
00                       250 kbps, MFM
01                       drive type 1: 5.25"
80                       single step, comment present
00                       not DOS mode
01                       1 side
DA 79                    header CRC 0x79DA

90 5D                    comment CRC 0x5D90
16 00                    22 bytes of text
67 07 10 13 17 1A        2003-08-16 19:23:26 (year 103, month 7 = August)
"Adventure Game" 00 x8   text and NUL padding

05 00 00 C4              track: 5 sectors, cylinder 0, side 0, CRC 0xC4
00 00 01 03 00 C6        sector: C0 H0 R1, 1024 bytes, no flags, data CRC 0xC6
65 00                    data block length 101
02                       method 2: run-length blocks
00 3C ...                literal block of 60 bytes, and so on
```

## Converting to IMD

`td02imd` maps each Teledisk track to one IMD track:

- **Mode:** from the header data rate (250, 300 or 500 kbps). It's FM if the track's side
  bit 7 is set, or the header's FM bit is set (unless `/F` is given). 300 kbps becomes
  250 kbps unless `/L` is given.
- **Sectors:** sectors with flag `0x20` become unavailable (IMD type 0). DOS-skipped
  sectors (`0x10`) are filled with `0xE5`. Flags `0x04` and `0x02` become IMD's
  deleted-data and data-error types.
- **Maps:** a cylinder or head map is added when any sector's ID differs from the track's
  cylinder or head.
- **Limits:** a track with mixed sector sizes can't be stored in IMD and stops the
  conversion. Tracks with no sectors are left out.
