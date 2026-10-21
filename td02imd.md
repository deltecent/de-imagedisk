# td02imd

`td02imd` converts a Teledisk `.TD0` image into an ImageDisk `.IMD` image. It reads both
normal and advanced-compression Teledisk files. [TD0.md](TD0.md) describes the Teledisk
format, and [IMD.md](IMD.md) the output.

```
td02imd file[.TD0] [options]
```

When no extension is given, `.TD0` is tried before `.td0`. The output is the name as
given, without its extension, plus `.imd`, in the same directory, unless `O=` names
another file. An existing output file is overwritten without asking. It's written only if
the whole conversion succeeds.

```
$ td02imd mor-adv.td0
TD 1.5 5.25 LD MFM S-step, 1 sides 16/08/2003 19:23:26
Adventure Game
43 tracks, 215 sectors converted.
```

The first line describes the Teledisk header: version, drive type, density, encoding,
stepping and sides. `ADV` is added for advanced compression and `DOS` for a DOS-mode
image. The creation date and comment follow when the image has them. This text also
becomes the IMD image's comment, as `IMD TD 1.5 5.25 LD MFM …`.

## Options

Options are case-insensitive. Switches can be written `/X` or `-X`.

| Option | Meaning |
|---|---|
| `O=file[.IMD]` | output file |
| `/Q` | quiet: don't print the header line or the final count (a comment is still shown) |
| `/F` | don't make the whole disk FM because the header's FM bit is set |
| `/L` | keep 300 kbps tracks at 300 kbps |
| `/RF` | remove duplicate sectors, keeping the first copy |
| `/RL` | remove duplicate sectors, keeping the last copy |
| `F=flags` | remove sectors with any of these Teledisk flags |
| `M=n` | remove sectors numbered above `n` |
| `W=flags` | don't print these warnings |
| `/Dx[,l[,h]]` | debug output |

### Data rate and encoding

A track's IMD mode comes from the header's data rate:

| Header rate | IMD mode |
|---|---|
| 250 kbps | 5 (250 kbps MFM) |
| 300 kbps | 5 (250 kbps MFM), or 4 (300 kbps MFM) with `/L` |
| 500 kbps | 3 (500 kbps MFM) |

Rate code 1 means a 250 kbps disk read in a 360 RPM high-density drive, so it is stored as
250 kbps by default. A track is FM instead (modes 2, 1 and 0) when the track header marks it
FM, or when the file header's FM bit is set. `/F` ignores the header bit, for images where
only some tracks are FM.

### Removing sectors

| `F=` letter | Removes sectors |
|---|---|
| `A` | with a deleted-data address mark |
| `C` | with a CRC error |
| `D` | skipped in DOS mode (unallocated) |
| `I` | found without an ID field |
| `N` | whose ID was found but no data |

For example, `F=IN` drops sectors that have no ID field or no data. `M=n` removes
sectors with an ID number above `n`, such as stray high-numbered sectors.

`/RF` and `/RL` handle a sector number that appears more than once in a track: `/RF`
keeps the first copy and `/RL` the last. The kept copy stays in the first copy's position.
Without either, duplicates are kept and a warning is printed. Removed sectors, duplicates
included, are counted in the final line: `…, 7 sectors removed.`

### Warnings

Problems in the source image are reported at most once per kind per track, prefixed with
`cylinder/head`:

```
40/0: CRC error (bad sector)
```

| `W=` letter | Suppresses |
|---|---|
| `C` | `CRC error (bad sector)` |
| `D` | `Duplicate sector numbers` |
| `H` | `Head not 0 or 1` |
| `I` | `No ID field - possible bogus sector` |
| `N` | `No DATA field` |

The warnings report what Teledisk recorded. The sectors are converted anyway: a CRC error
becomes an IMD bad sector, and a sector with no data becomes unavailable.

### Debug output

`/Dx,l,h` prints the TD0 structures as they are read, for cylinders `l` to `h` (all by
default; `/Dx,l` is one cylinder). `x` is a hex sum of:

| Bit | Prints |
|---|---|
| `80` | the file header |
| `40` | each track header |
| `20` | each sector header and data block header |
| `02` | each sector's IMD type and running size |
| `01` | stops after the first track |

For example, `/D60,0,1` shows the track and sector headers of cylinders 0 and 1.

## How sectors are converted

| Teledisk sector | IMD sector |
|---|---|
| normal data | data |
| deleted-data mark (`0x04`) | deleted data |
| CRC error (`0x02`) | bad data |
| DOS-skipped (`0x10`) | data, filled with `0xE5` |
| no data (`0x20`) | unavailable |

All-same sectors are stored compressed. A cylinder or head map is added to a track when a
sector's ID field doesn't match the track's position. Tracks with no sectors are left out.

## Errors

Errors go to stderr, `td02imd` exits with status 1, and no output is written. Run with no
file or a bad option, it prints its help text and exits with status 1. The main messages:

| Message | Cause |
|---|---|
| `No TD signature` | not a Teledisk file |
| `Header CRC failure` | the 12-byte header is damaged |
| `Comment CRC error!`, `Track header CRC error` | damaged or truncated file |
| `EOF reading sector data` (and similar) | the file ends in the middle of a record |
| `Unknown data mode: xx` | a sector uses an encoding method other than 0-2 |
| `Sector data size/offset mismatch` | a data block's length doesn't match its contents |
| `Unknown data rate`, `Unknown sector size`, `Unknown track-side indicator` | a value outside the known range |
| `Cannot do mixed sector size within track` | the track can't be stored as IMD |

Multi-volume Teledisk sets (one disk split across several files) aren't supported.

## Examples

```
td02imd DISK                         DISK.TD0 (or DISK.td0) to DISK.imd
td02imd disk.td0 O=out/disk          write out/disk.imd
td02imd disk.td0 /RF W=D             drop duplicate sectors, without the warnings
td02imd disk.td0 F=IN                drop sectors with no ID or no data
td02imd disk.td0 /Q                  only warnings and errors
```

Converting a whole directory from a shell:

```
for f in *.td0; do td02imd "$f" /Q || echo "failed: $f"; done
```
