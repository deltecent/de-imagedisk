# imdu

`imdu` is the ImageDisk utility. It shows what an `.IMD` image holds, and it can rewrite
the image with changes: compress or expand sectors, change the comment, re-interleave,
translate data rates, drop tracks, fill missing sectors, merge in a second read of the same
disk, or write the sector data out as a raw binary file. [IMD.md](IMD.md) describes the
format.

```
imdu image[.IMD] [[merge-image[.IMD]] [output-image]] [options]
```

`NAME.IMD` is tried before `name.imd` for input files. An output file gets a lowercase
`.imd` extension, or `.bin` with `/B`, unless it already has one.

## What the file arguments mean

| Command | Does |
|---|---|
| `imdu a.imd` | show `a.imd` |
| `imdu a.imd b.imd` | compare `b.imd` with `a.imd` (no options that change the image) |
| `imdu a.imd b.imd /C` | write `a.imd`, changed, to `b.imd` |
| `imdu a.imd b.imd c.imd` | merge `b.imd` into `a.imd` and write the result to `c.imd` |

With two files, the second one is the output if any option changes the image: `/B /C /E
/NB /ND AC= RC= F= IL= T2= T3= T5= X= X0= X1=`. Otherwise it's an image to compare with.
A changing option with no output file is an error (`No output file.`).

`imdu` reads all its input before writing, so the output can be the input file itself:
`imdu a.imd a.imd /C /Y`. If the output exists and `/Y` isn't given, `imdu` asks
`proceed(Y/N)?` when run from a terminal. From a script, it exits with
`… already exists - use /Y to overwrite` and leaves the file alone.

## Display

`imdu` prints its version, the image comment, one line each time the track format changes,
and a total:

```
$ imdu tarbdd.imd
IMageDisk Utility 1.20 / Oct 20 2026
IMD 1.20: 20/10/2026 20:45:49
Tarbell double density CP/M 2.2
 0/0 500 kbps SD  26x128
 1/0 500 kbps DD  51x128
77 tracks(77/0), 3902 sectors (2270 Compressed)
```

A format line shows `cylinder/head`, the data rate, `SD` (FM) or `DD` (MFM), and
`sectors x bytes`. The total gives the tracks on head 0 and head 1, then the sectors.
Nonzero counts of compressed, deleted, bad and unavailable sectors follow in parentheses.

`/D` adds the detail of every track. The sector numbering map is shown whenever it
changes, then one status per sector, in physical order:

```
 0/0 500 kbps SD  26x128
     1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26
     D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D  D
 1/0 500 kbps DD  51x128
     ...
     D  D  ... De5De5De5
```

| Status | Meaning |
|---|---|
| `D` | data |
| `B` | bad: the data was read with a CRC error |
| `U` | unavailable: no data could be read |
| lowercase `d`, `b` | the sector has a deleted-data address mark |
| two hex digits after it | the sector is compressed: every byte has this value (`De5`) |

Tracks that use a cylinder or head map also show the map, on lines starting `CL:` and `HD:`.
With more than 18 sectors per track, the columns are packed closer together.

## Options

Options are case-insensitive. Switches can be written `/X` or `-X`. An argument starting
with `/` counts as an option only if it is exactly a known one, so an absolute path like
`/home/me/disk.imd` is a file name.

### Output format

| Option | Meaning |
|---|---|
| `/C` | compress every all-same sector |
| `/E` | expand every compressed sector to full data |
| `/B` | write a raw binary file of the sector data instead of an IMD image |

Without `/C` or `/E`, the output keeps each sector as the input stored it.

`/B` writes the tracks in image order, each track's sectors one after another. Unless
`IL=` is given, the sectors are sorted by number first (`Assuming 1:1 for Binary output`),
so the result matches the usual emulator `.DSK` layout. Unavailable sectors are written as
the `F=` value, 0 by default. All the other options apply too: `X=` leaves tracks out of
the binary, for example.

### Sector status

| Option | Meaning |
|---|---|
| `/NB` | clear the bad (CRC error) mark from every sector |
| `/ND` | clear the deleted-data mark from every sector |
| `F=xx` | make every unavailable sector available, filled with hex byte `xx` |

### Comment

| Option | Meaning |
|---|---|
| `EC=file[.TXT]` | extract the image's comment to a file |
| `RC=file[.TXT]` | replace the comment with a file's contents |
| `AC=file[.TXT]` | append a file's contents to the comment |

`EC=` always writes the comment as it was in the input, before `RC=` or `AC=`. It doesn't
change the image, so `imdu a.imd EC=a` needs no output file. Comment files are copied
byte for byte, so use CR LF line endings to match other images.

### Interleave

| Option | Meaning |
|---|---|
| `IL=n` | reorder each track's sectors to an n:1 interleave, 1-99 |
| `IL=` | reorder to the most likely original interleave |

`IL=n` places consecutive sector numbers `n` positions apart, skipping positions already
used. `IL=1` puts them in order, and on a 26-sector track `IL=6` gives
`1 14 10 23 6 19 2 …`. Each sector keeps its data, status and ID fields. `IL=` alone
finds the most common gap between consecutive sector numbers in each track and rebuilds
the track evenly with it. That's useful after a merge, which adds new sectors at the end
of a track.

### Data rate

| Option | Meaning |
|---|---|
| `T5=rate` | write 500 kbps tracks at `rate` |
| `T3=rate` | write 300 kbps tracks at `rate` |
| `T2=rate` | write 250 kbps tracks at `rate` |

`rate` is 250, 300 or 500. FM stays FM and MFM stays MFM. `T3=250` is the usual one: it
turns a 5.25" double-density disk read in a high-density drive (300 kbps) back into a
standard 250 kbps image.

### Excluding tracks

| Option | Meaning |
|---|---|
| `X=a[,b]` | leave out cylinders `a` to `b`, both heads |
| `X0=a[,b]` | leave out cylinders `a` to `b`, head 0 only |
| `X1=a[,b]` | leave out cylinders `a` to `b`, head 1 only |

The options can be repeated and combined: `X=0 X1=40,79`.

### Other

| Option | Meaning |
|---|---|
| `/M` | ignore mode (data rate and encoding) differences when merging or comparing |
| `/Q` | quiet: don't print warnings |
| `/Y` | overwrite the output file without asking |
| `--json` | print the result as JSON instead of the display |

## Merging and comparing

Reading the same disk twice and merging the reads recovers sectors that failed one time
and not the other. `imdu a.imd b.imd out.imd` goes through `a.imd` track by track and
looks for the same cylinder and head in `b.imd`. Then:

- A sector unavailable in `a` but readable in `b` is taken from `b` (`Filling in sector`).
- A sector found only in `b` is added to the track (`Adding sector`). A cylinder or head
  map is added if needed to keep its ID.
- A track found only in `b` is added (`Adding track c/h from mergefile`).
- Sectors in both whose data or status differ are kept from `a`, with a warning
  (`Merge data differs from original`, `Bad sector status differs`).
- A track whose sector size differs, or mode unless `/M` is given, isn't merged
  (`Incompatible sector size`, `Incompatible track mode`).

With no output file (`imdu a.imd b.imd`), the same checks run as a comparison and print
the same messages, starting with `No output file - compare only.`. No messages means the
images match. Each message is printed under the `cylinder/head` it concerns. Sector
references like `3/7` mean position 3 in the track, sector number 7.

## JSON

`imdu image --json` prints one JSON object on stdout and sends any messages to stderr, for
scripts and tools:

```json
{
  "file": "tarbdd.imd",
  "comment": "IMD 1.20: 20/10/2026 20:45:49\r\nTarbell double density CP/M 2.2\r\n",
  "tracks": [
    {"cylinder": 0, "head": 0, "mode": 0, "rate_kbps": 500, "encoding": "FM", "sector_size": 128,
     "cylinder_map": false, "head_map": false,
     "sectors": [
       {"id": 1, "cylinder": 0, "head": 0, "available": true, "compressed": false, "deleted": false, "bad": false},
       ...
     ]},
    ...
  ],
  "totals": {"tracks": 77, "head0_tracks": 77, "head1_tracks": 0, "sectors": 3902,
             "compressed": 2270, "deleted": 0, "bad": 0, "unavailable": 0}
}
```

A compressed sector also has `"fill"`, its byte value. An unavailable sector has only
`id`, `cylinder`, `head` and `"available": false`. With conversion or merge options, the
JSON describes the result, the image that is written.

## Exit status and errors

`imdu` exits with status 0 on success and 1 on any error. A damaged image is reported with
the track it was reading, for example `disk.imd:  3/1 EOF in sector 5/6 data`, and
nothing is written. Run with no arguments or a bad option, it prints its help text and
exits with status 1.

## Examples

```
imdu disk.imd /D                     show every sector
imdu disk.imd disk.imd /C /Y         compress disk.imd in place
imdu disk.imd EC=disk                save the comment to disk.txt
imdu disk.imd new.imd RC=notes       replace the comment with notes.txt
imdu read1.imd read2.imd best.imd    merge two reads of a disk
imdu read1.imd read2.imd             compare two reads
imdu disk.imd disk.bin /B            raw sector data, sorted by sector number
imdu disk.imd fixed.imd T3=250       300 kbps tracks to 250 kbps
imdu disk.imd ss.imd X1=0,79         drop side 1 of an 80-cylinder disk
imdu disk.imd --json                 machine-readable summary
```
