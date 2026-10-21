# The ImageDisk (.IMD) file format

ImageDisk images store a floppy disk as it was read, one track at a time: the recording
mode and data rate, the sector ID fields, and the sector data with per-sector status. The
format was defined by Dave Dunfield for ImageDisk and is public domain. This document
describes it as `imdu`, `bin2imd` and `td02imd` read and write it. The reader is
`parseImage()` in `src/core/imd_image.cpp`.

All values are single bytes; the format has no multi-byte integers.

## Layout

```
+-------------------------+
| comment (ASCII text)    |
| 0x1A                    |
+-------------------------+
| track record            |  repeated until end of file;
| track record            |  there is no track count
| ...                     |
+-------------------------+
```

## Comment

The file starts with free-form ASCII text ending at the first `0x1A` byte (Ctrl-Z, the DOS
end-of-file mark). This means `TYPE disk.imd` in DOS showed only the comment. Lines end
with CR LF.

By convention the first line identifies the program and the time the image was made:

```
IMD 1.20: 20/10/2026 14:03:07
```

The date is day/month/year. Programs that write the line differently still produce a
valid image; readers should treat the whole comment as text. `td02imd` writes a
description of the Teledisk source instead (`IMD TD 1.5 8" HD MFM S-step, 1 sides ADV`),
followed by the Teledisk comment.

## Track record

| Bytes | Field | Notes |
|---|---|---|
| 1 | mode | data rate and encoding, 0-5 (below) |
| 1 | cylinder | physical cylinder the track was read from, 0-255 |
| 1 | head | bits 0-3: physical head (0 or 1); bit 7: cylinder map present; bit 6: head map present |
| 1 | sector count | number of sectors in the track, 0-255 |
| 1 | size code | sector size for every sector in the track, 0-6 (below) |
| count | sector numbering map | sector ID number of each sector, in the order they were read |
| count | cylinder map | only if head bit 7: the cylinder value in each sector's ID field |
| count | head map | only if head bit 6: the head value in each sector's ID field |
| varies | sector data records | one per sector, in map order |

Bits 4-5 of the head byte are unused and should be zero. The reader rejects a track
whose head (bits 0-3) is not 0 or 1.

### Mode

| Mode | Data rate | Encoding |
|---|---|---|
| 0 | 500 kbps | FM (single density) |
| 1 | 300 kbps | FM |
| 2 | 250 kbps | FM |
| 3 | 500 kbps | MFM (double density) |
| 4 | 300 kbps | MFM |
| 5 | 250 kbps | MFM |

500 kbps is the 8" and 5.25" high-density rate, and 250 kbps the standard 5.25"/3.5"
double-density rate. 300 kbps is what a 250 kbps disk reads at in a 360 RPM high-density
5.25" drive.

### Size code

| Code | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| Bytes per sector | 128 | 256 | 512 | 1024 | 2048 | 4096 | 8192 |

The size is `128 << code`. Every sector in a track has the same size, so a track with
mixed sector sizes can't be represented.

### Maps

The sector numbering map lists sector IDs in physical order, which records the
interleave: `01 03 05 07 09 02 04 06 08` is a 2:1 interleave.

The cylinder and head maps are needed only when the ID fields disagree with the physical
position. Examples: a disk whose ID fields number the second side as head 0, or a track
written with a different cylinder number for copy protection. Without a map, every
sector's ID cylinder and head equal the track's cylinder and head.

### Sector data record

Each record starts with a type byte:

| Type | Data that follows | Meaning |
|---|---|---|
| 0 | none | sector unavailable: its data could not be read |
| 1 | sector size bytes | normal data |
| 2 | 1 byte | compressed: every byte of the sector has this value |
| 3 | sector size bytes | normal data, deleted-data address mark |
| 4 | 1 byte | compressed, deleted-data address mark |
| 5 | sector size bytes | data read with a CRC error |
| 6 | 1 byte | compressed, CRC error |
| 7 | sector size bytes | deleted-data address mark, CRC error |
| 8 | 1 byte | compressed, deleted-data address mark, CRC error |

For types 1-8, `type - 1` is a bit set:

| Bit | Value | Meaning |
|---|---|---|
| 0 | 1 | compressed (one fill byte instead of the data) |
| 1 | 2 | deleted-data address mark |
| 2 | 4 | data error (CRC error when read) |

Types above 8 are invalid. Compression is only a space saving: a writer may store an
all-same sector either way, and the decoded image is the same.

## Example

The first track of a CompuPro CP/M 2.2 8" disk, starting at the `0x1A` that ends the
comment:

```
1A                       end of comment
00                       mode 0: 500 kbps FM
00                       cylinder 0
00                       head 0, no cylinder or head map
1A                       26 sectors
00                       size code 0: 128 bytes
01 02 03 ... 19 1A       sector numbering map: 1 to 26 in order
01 C3 1A 01 00 ...       sector 1: type 1, then 128 bytes of data
```

## Reading and writing rules used by these tools

- Tracks appear in the order they were read, normally cylinder by cylinder, alternating
  heads on two-sided disks. Readers shouldn't assume a fixed geometry: a track may be
  missing, repeated or out of order, and its sector count may be 0.
- The reader rejects mode > 5, head > 1, size code > 6 and sector type > 8. A file that
  ends inside a record is an error ("EOF in sector 3/4 data" and similar). An empty file
  or one with no `0x1A` is an error ("EOF in comment").
- An unavailable sector (type 0) has no data. When `imdu` writes a raw binary (`/B`) or
  fills missing sectors (`F=xx`), the sector's bytes are the `F=` value (0 by default).
- The writers store a sector compressed when all its bytes are the same, unless asked to
  keep the input's choice (`imdu` default) or expand everything (`imdu /E`, `bin2imd /U`).
- Cylinder and head maps are written only when some sector's ID differs from the track's
  position.
