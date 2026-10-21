# bin2imd

`bin2imd` converts a raw binary disk image, such as an emulator's `.DSK` file holding just
the sector data, into an ImageDisk `.IMD` file. A raw image has no record of the disk's
format, so you describe it: the number of cylinders and sides, and for each track the
recording mode, sector size and sector numbering. [IMD.md](IMD.md) describes the output
format.

```
bin2imd input[.BIN] output[.IMD] [option-file[.B2I]] [options]
```

A file name without an extension gets the one shown. For an input file, `NAME.BIN` is tried
before `name.bin`. The output file is always overwritten without asking.

## How the input is read

`bin2imd` reads the input from the start, one sector at a time, in this order:

1. cylinder 0, head 0, sectors in sector-map order
2. cylinder 0, head 1 (two-sided images only)
3. cylinder 1, head 0, and so on up to `N=` cylinders

The sector map decides the order the input fills the sectors, not just their numbers. With
`SM=1,3,5,2,4`, the input's second sector becomes sector 3. Most emulator `.DSK` files
store sectors in number order, so use a sequential map like `SM=1-26`. To give the IMD
image a physical interleave afterwards, use `imdu IL=` (below), which moves each sector
with its data.

If the input runs out early, the remaining sectors are filled with the `F=` value and
`bin2imd` prints `Input file is smaller than output image (ends at CHS c/h/s)`, where `s`
is the position in the sector map. If input is left over when the image is complete, it
prints `Input file is larger than output image.` Neither is an error; check the geometry
when you see one.

## Options

Options can be given in any order and are case-insensitive. Switches can be written `/X`
or `-X`.

| Option | Meaning | Default |
|---|---|---|
| `N=n` | number of cylinders, 1-256 | required |
| `/1`, `/2` | one- or two-sided output | one-sided |
| `DM[s]=0-5` | data mode, see the table below | required |
| `SS[s]=n` | sector size in bytes: 128, 256, 512, 1024, 2048, 4096 or 8192 | required |
| `SM[s]=map` | sector numbering map, in physical order | required |
| `CM[s]=map` | cylinder map: the cylinder value in each sector's ID field | track's cylinder |
| `HM[s]=map` | head map: the head value in each sector's ID field (0 or 1) | track's head |
| `F=n` | fill value for sectors past the end of the input | 0 |
| `/C`, `/U` | store all-same sectors compressed, or every sector in full | `/C` |
| `C=text` | add a comment line; `~` stands for a space | none |
| `C=@file[.TXT]` | add the contents of a file to the comment | none |
| `/V` | verbose: print the track format when it's set, at cylinder 0 and at each option-file line | off |
| `/V1` | verbose, also printing the maps | off |
| `/V0` | verbose off | |

`DM`, `SS`, `SM`, `CM` and `HM` take an optional side digit `s`, for example `DM1=0`.
Without a digit the option sets side 0 and then copies **all** of side 0's settings to
side 1. So put the side-specific options after the general ones: `SM=1-26 SM1=27-52`
works, but in `SM1=27-52 SM=1-26` the second option replaces the first. Giving any option
with side digit 1 also makes the output two-sided.

| Mode | Data rate and encoding | Typical use |
|---|---|---|
| 0 | 500 kbps FM | 8" single density |
| 1 | 300 kbps FM | |
| 2 | 250 kbps FM | 5.25" single density |
| 3 | 500 kbps MFM | 8" double density, 5.25"/3.5" high density |
| 4 | 300 kbps MFM | 5.25" double density read in a high-density drive |
| 5 | 250 kbps MFM | 5.25"/3.5" double density |

### Numbers and maps

Numbers are decimal, or `$` hex, `@` octal, `%` binary: `$1A`, `@32` and `%11010` are all
26.

A map is a comma-separated list of values, with two shorthands:

| Map | Gives |
|---|---|
| `1-9` | 1 2 3 4 5 6 7 8 9 |
| `9-1` | 9 8 7 6 5 4 3 2 1 |
| `1,3,5,7,9,2,4,6,8` | the values as listed |
| `0.9` | 0 nine times, as for a cylinder map |
| `1-5,10-14` | 1 2 3 4 5 10 11 12 13 14 |

`n.k` repeats `n` until it appears `k` times in a row. A map holds at most 255 values.
Sector numbers must be unique within a track, and a cylinder or head map must have as many
values as the sector map.

### Comments

Every image starts with the line `IMD 1.20: dd/mm/yyyy hh:mm:ss`, the time it was made.
Each `C=text` adds one line after it. A shell needs quotes around text with spaces, or use
`~` for each space: `C=CP/M~2.2~system~disk`. `C=@notes.txt` adds the file's bytes as they
are, so the file should use CR LF line endings. The comment can be up to 32,767 bytes.

## Option files

A track format that changes partway through the disk needs an option file. Each line
starts with a cylinder number, followed by options that take effect when output reaches
that cylinder:

```
; comment lines start with a semicolon
0 N=77 DM=0 SS=128 SM=1-26
1 DM=3 SS=128 SM=1-51
```

- Lines must be in increasing cylinder order. Settings carry forward until a later line
  changes them.
- The cylinder 0 line is applied after the command-line options, so it overrides them.
- `N=` and `C=` are allowed only on the cylinder 0 line. Every other option, switches
  included, can appear on any line.
- Each option is one word: write `C=My~disk` rather than `C="My disk"`.

## Errors

Errors print a message to stderr, and `bin2imd` exits with status 1 without writing any
output. Messages name the option or side they concern, such as `SM=1x: Bad digit 'x'` or
`Side0: Sector Size must be defined`. With no input or output file, `bin2imd` prints its
help text and exits with status 1.

## Examples

### Tarbell single density

Tarbell's single-density controller uses standard 8" IBM 3740 disks: 77 cylinders, one
side, 26 sectors of 128 bytes per track, FM. The `.DSK` is 77 × 26 × 128 = 256,256 bytes,
with sectors in number order.

```
bin2imd TARBSD.DSK tarbsd.imd N=77 DM=0 SS=128 SM=1-26 C=Tarbell~single~density~CP/M~2.2
```

```
$ imdu tarbsd.imd
IMageDisk Utility 1.20 / Oct 20 2026
IMD 1.20: 20/10/2026 20:45:49
Tarbell single density CP/M 2.2
 0/0 500 kbps SD  26x128
77 tracks(77/0), 2002 sectors (90 Compressed)
```

### Tarbell double density

Tarbell's double-density controller uses mixed-density disks. Track 0 stays single
density (26 sectors of 128 bytes, FM) so the boot PROM can read it. Tracks 1-76 are double
density with 51 sectors of 128 bytes, MFM. The `.DSK` is 3,328 + 76 × 6,528 = 499,456
bytes. The format changes after cylinder 0, so use an option file, `tarbdd.b2i`:

```
; Tarbell double density: track 0 single density, tracks 1-76 double density
0 N=77 DM=0 SS=128 SM=1-26
1 DM=3 SS=128 SM=1-51
```

```
bin2imd TARBDD.DSK tarbdd.imd tarbdd.b2i C=Tarbell~double~density~CP/M~2.2
```

```
$ imdu tarbdd.imd
IMageDisk Utility 1.20 / Oct 20 2026
IMD 1.20: 20/10/2026 20:45:49
Tarbell double density CP/M 2.2
 0/0 500 kbps SD  26x128
 1/0 500 kbps DD  51x128
77 tracks(77/0), 3902 sectors (2270 Compressed)
```

A single-density disk mounted in the double-density controller is just the single-density
example. A 256,256-byte `.DSK` is all single density.

### Checking a conversion

Converting back to binary with `imdu /B` should give the original file:

```
imdu tarbdd.imd check.bin /B
cmp check.bin TARBDD.DSK
```

Both Tarbell examples above round-trip byte for byte.

### Setting a physical interleave

The examples write sectors in physical order 1, 2, 3 and so on (1:1). CP/M applies its
sector skew in software, so a `.DSK` doesn't carry one. To write the image to a real disk
in another interleave, change it with `imdu`, which keeps each sector's data:

```
imdu tarbsd.imd tarbsd6.imd IL=6
```

### A two-sided PC disk

A 1.44 MB 3.5" disk: 80 cylinders, two sides, 18 sectors of 512 bytes, high-density MFM.

```
bin2imd DISK.IMG disk.imd N=80 /2 DM=3 SS=512 SM=1-18
```
