# de-imagedisk

New C++17 implementations of three ImageDisk 1.20 utilities for macOS and Linux:

| Tool      | Purpose                                                        |
|-----------|----------------------------------------------------------------|
| `imdu`    | Display, convert, compress/expand, re-interleave and merge `.IMD` images |
| `bin2imd` | Convert a raw binary disk image into `.IMD` format             |
| `td02imd` | Convert a Teledisk `.TD0` image (normal or advanced compression) into `.IMD` |

The command lines, messages and output files follow Dave Dunfield's DOS ImageDisk 1.20
tools. The code is written from scratch: none of the original source is copied or
translated. Its license allows reusing the ideas and algorithms, but not redistributing the
code. The `.IMD` format is public domain. The Teledisk format comes from Dunfield's TD0
notes, and the advanced-compression decoder from Haruhiko Okumura's LZHUF algorithm.

## Building

Requires CMake 3.16+ and a C++17 compiler (Apple clang or GCC).

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build          # needs Python 3 for the end-to-end tests
    cmake --install build --prefix /usr/local

`-DIMD_WERROR=ON` turns compiler warnings into errors.

The `real_images` test also checks `~/Downloads/CDOSMBD.IMD` and `~/Downloads/CPM80-22.IMD`
if they exist. Set `IMD_REAL_IMAGES=a.imd:b.imd` to use other images. With no images, the
test is reported as skipped.

## Options

Options are case-insensitive and can be written `/X` (DOS style) or `-X`. An argument that
starts with `/` counts as an option only if it exactly matches a known one, so absolute
paths such as `/Users/me/disk.imd` are always taken as file names. When a file name has no
extension, the default one is added. For input files, the uppercase extension is tried
first (`disk.IMD`), then the lowercase one (`disk.imd`). Output files always get a
lowercase extension.

### imdu

    imdu image [[merge-image] [output-image]] [options]

| Option | Meaning |
|---|---|
| `/B` | write a Binary image (sector data only; defaults to 1:1 interleave) |
| `/C` | Compress all-same sectors |
| `/D` | display track/sector Detail |
| `/E` | Expand compressed sectors |
| `/M` | ignore Mode differences when merging/comparing |
| `/NB` | drop bad-sector (data error) marks |
| `/ND` | drop deleted-data marks |
| `/Q` | Quiet: no warnings |
| `/Y` | overwrite the output without asking |
| `--json` | print the image summary as JSON (see below) |
| `AC=file[.TXT]` | Append comment from file |
| `EC=file[.TXT]` | Extract comment to file |
| `RC=file[.TXT]` | Replace comment from file |
| `F=xx` | Fill missing sectors with hex value `xx` |
| `IL=[1-99]` | re-interleave; blank = best guess of the original interleave |
| `T2=` `T3=` `T5=` | translate 250/300/500 kbps tracks to another rate (250, 300 or 500) |
| `X=a[,b]` `X0=` `X1=` | exclude cylinders a..b (both sides, side 0, side 1) |

If a third file is given, the second file is the merge image. With only two files, the
second one is the output if any option changes the image (`/B /C /E /NB /ND AC= RC= F= IL=
T*= X*=`), and the merge image otherwise. Without an output file, a merge image is only
compared against.

Each track is shown as `cc/h <rate> kbps <SD|DD>  <count>x<size>`. The line appears when
the format changes. `/D` adds the sector numbering map and, when present, the cylinder
(`CL:`) and head (`HD:`) maps. It also adds one status per sector: `D` data, `B` bad, `U`
unavailable, lowercase if deleted, followed by the fill byte if the sector is compressed.

### bin2imd

    bin2imd input[.BIN] output[.IMD] [option-file[.B2I]] [options]

| Option | Meaning |
|---|---|
| `/1` `/2` | one- or two-sided output |
| `/C` `/U` | compress all-same sectors (default) / write all sectors uncompressed |
| `/V` `/V1` `/V0` | verbose: track summary / summary and maps / off |
| `C=text` | comment line (`~` stands for a space); may be repeated |
| `C=@file[.TXT]` | comment from a file |
| `N=cyls` | number of cylinders (required) |
| `F=n` | fill value for data past the end of the input |
| `DM[s]=0-5` | data mode (0-2 = 500/300/250 kbps FM, 3-5 = 500/300/250 kbps MFM) |
| `SS[s]=n` | sector size, 128-8192 |
| `SM[s]=map` `CM[s]=map` `HM[s]=map` | sector, cylinder and head numbering maps |

`[s]` is an optional side digit. With no digit the setting applies to both sides. A `1`
also turns on two-sided output. Numbers can be decimal, `$hex`, `@octal` or `%binary`. In
a map, `a-b` adds an ascending or descending range, and `n.k` makes the value `n` appear
`k` times in a row. For example, `SM=1-9` gives 1 to 9, and `SM=1,3,5,7,9,2,4,6,8` gives a
2:1 interleave.

The option file holds lines of the form `<cylinder> options...`, and lines starting with
`;` are comments. A line takes effect when output reaches its cylinder. `C=` and `N=` are
allowed only on the cylinder 0 line. Example, with FM on cylinder 0 and MFM from cylinder 1:

    0 DM=0 SS=128 SM=1-26
    1 DM=3 SS=256 SM=1-26

### td02imd

    td02imd file[.TD0] [options]

| Option | Meaning |
|---|---|
| `/Dx[,l[,h]]` | debug dumps for cylinders l..h; `x` (hex) = 80 header, 40 track headers, 20 sector headers, 02 sectors, 01 first track only |
| `/F` | don't treat the whole disk as FM because of the header FM bit |
| `/L` | don't convert 300 kbps (LD disk in HD drive) to 250 kbps |
| `/Q` | quiet |
| `/RF` `/RL` | remove duplicate sectors, keeping the first / last |
| `F=ACDIN` | remove sectors flagged A deleted, C CRC error, D DOS-skipped, I no ID, N no data |
| `M=n` | remove sectors numbered above `n` |
| `O=file` | output file (default: input name with `.imd`) |
| `W=CDHIN` | suppress warnings: C CRC error, D duplicates, H head not 0/1, I no ID, N no data |

## Use from scripts and agents

The tools are plain command-line programs and never wait for input unless stdin is a
terminal:

- Exit status 0 means success, 1 means an error. Error messages go to stderr; normal output
  and warnings go to stdout.
- If the output file exists and `/Y` wasn't given, imdu asks `proceed(Y/N)?` only when stdin
  is a terminal. Otherwise it exits with an error and leaves the file alone. bin2imd and
  td02imd overwrite without asking, like the originals.
- `imdu image --json` writes only JSON to stdout. Any messages go to stderr. The JSON
  contains the file name, the comment, and every track: cylinder, head, mode, rate,
  encoding, sector size, and whether cylinder or head maps are present. Each sector entry
  has its ID, cylinder, head, whether it is available, and its compressed, deleted and bad
  flags, plus the fill byte when compressed. Totals come last. `--json` can be combined
  with conversion and merge options; the JSON then describes the merged image.

## Differences from the DOS 1.20 tools

Most of these fix apparent bugs in 1.20:

1. **imdu `/ND`**: deleted-data marks are kept unless `/ND` is given, as the help text says.
   1.20 drops them by default and keeps them only with `/ND`.
2. **imdu `T2=`** works. 1.20 updates the wrong table entry.
3. **imdu** keeps tracks that have no sectors in its output. 1.20 shows "No data" and drops
   them.
4. **imdu `/D`** shows deleted sectors in lowercase, as intended. 1.20 tests the wrong bit.
5. **imdu** strips CR from CRLF when printing the comment. The image's comment bytes are
   unchanged.
6. **imdu** reads the whole image before writing anything, so a damaged image is reported
   before any output. Input and output can be the same file.
7. **imdu merge**: when an added sector's cylinder or head differs from the track's, a
   cylinder or head map is turned on so the value is kept.
8. **td02imd**: 300 kbps tracks become mode 5 (250 kbps MFM). 1.20 produces mode 3
   (500 kbps MFM).
9. **td02imd**: sectors without data are written as "unavailable" (type 0). 1.20 omits the
   type byte, which corrupts the image.
10. **td02imd `/RL`** keeps the right sector when an earlier one was removed. Duplicates
    removed by `/RF`/`/RL` are included in the "sectors removed" count.
11. **td02imd**: the data-length check only applies to sectors that have a data block. An
    unknown encoding method is an error; 1.20 loops forever.
12. **td02imd**: the output file is written only when conversion succeeds.
13. **bin2imd**: `%` numbers are binary (1.20 rejects every digit), and `F=0` is allowed.
14. **bin2imd**: each option must be a whole word. Trailing characters are an error, and a
    map holds at most 255 entries.
15. **All tools**: lowercase output extensions; errors go to stderr with exit status 1
    (not 255). Declining the overwrite prompt also exits with 1.
16. **TD0 RLE**: a repeated pattern is `1<<n` bytes long, as in 1.20's code. TD0NOTES says
    `n*2`. The two agree for n ≤ 2, which is all real images use.

## Layout

    src/core/      imdcore library: CLI helpers, IMD model/reader/writer, TD0 reader + LZHUF
    src/imdu/      imdu
    src/bin2imd/   bin2imd
    src/td02imd/   td02imd
    tests/         unit tests, end-to-end tests (tool_tests.py) and a synthetic TD0
                   generator with an LZHUF encoder (td0gen.py)
