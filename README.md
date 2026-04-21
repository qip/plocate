# plocate (extended fork)

A fork of [plocate](https://plocate.sesse.net/) with file-size metadata, per-file checksums, metadata-based query filtering, and a WizTree-compatible CSV export tool.

Upstream plocate provides a fast file-locating database using trigram indexing with zstd compression. This fork extends both `updatedb` and `plocate` while maintaining full backward compatibility with upstream database readers.

## Building

Requires a C++17 compiler and development packages for [Zstd](https://facebook.github.io/zstd/). [liburing](https://github.com/axboe/liburing) with a kernel supporting io_uring (Linux 5.1+) is optional but recommended.

```bash
meson setup obj
cd obj
ninja
sudo ninja install
```

## updatedb

The database builder adds file-size, allocated-size, and checksum metadata alongside the standard filename index.

### Checksum options

| Option | Description |
|---|---|
| `--checksum-command CMD` | Compute checksums by running `CMD filepath` and reading the first hex token from stdout (e.g. `sha256sum`, or `zfssum.sh` from my other project). |
| `--checksum-xattr ATTR` | Read hex-encoded checksum from extended attribute `ATTR` (e.g. `user.checksum.sha256`, or `user.zfssum.checksum` from my other project). |
| `--checksum-xattr-mtime ATTR` | Read mtime-at-checksum-time from xattr `ATTR`; if older than the file's actual mtime, the xattr checksum is considered stale and the checksum command is used as fallback. |
| `--min-checksum-size N` | Skip checksum computation for files smaller than `N` bytes (default 0). |

When both `--checksum-xattr` and `--checksum-command` are set, xattr is tried first with command as fallback.

### Include/exclude filters

| Option | Description |
|---|---|
| `-I, --include PREFIX` | Only index paths starting with `PREFIX`. May be repeated. |
| `-X, --exclude PREFIX` | Skip paths starting with `PREFIX`. Evaluated before include. May be repeated. |

These allow partial database rebuilds — for example, re-indexing a single subtree without rescanning the entire filesystem.

### Verbose mode

| Option | Description |
|---|---|
| `-v` | Print each indexed file path to stdout. Report database status and output path to stderr. |
| `-vv` | Additionally print per-directory scan/reuse decisions and per-file checksum processing details to stderr. |

## plocate

### Metadata display

| Option | Description |
|---|---|
| `-S, --size` | Print file size (bytes) after each match. |
| `-a, --allocated` | Print allocated size (bytes) after each match. |
| `-C, --checksum` | Print checksum (hex) after each match. |

These flags are independent and can be combined. Output is comma-separated: `path,size,allocated,checksum` (only requested fields appear). Metadata streams are loaded on demand.

### Metadata filters

| Option | Description |
|---|---|
| `--match-size [+\|-]N` | Filter by file size: `+N` greater than, `-N` less than, bare `N` exact match. |
| `--match-checksum HEX` | Filter by exact checksum (hex-encoded). |

Filters are ANDed with the path pattern. Files lacking the required metadata are excluded.

### Examples

```bash
# Find files named "*.log" larger than 100 MB, showing size
plocate '*.log' --match-size +104857600 -S

# Find all copies of a file by checksum
plocate / --match-checksum a1b2c3d4... -S -C

# Standard search (unchanged from upstream)
plocate myfile.txt
```

## plocate2csv

Exports a plocate database to WizTree-compatible CSV format with folder aggregation.

```bash
plocate2csv -d /var/lib/plocate/plocate.db -f /data -m /data/:D: > output.csv
```

| Option | Description |
|---|---|
| `-d, --database DBPATH` | Path to the database file (default: `plocate.db`). |
| `-f, --filter PREFIX` | Only include paths under `PREFIX`. |
| `-m, --map FROM:TO` | Replace path prefix (e.g. `/data/:D:` for drive-letter mapping). |
| `-s, --replace FROM:TO` | Global string replacement in output paths. |

CSV columns: `File Name,Size,Allocated,Modified,Attributes,Files,Folders,Checksum`

Directories get aggregated size/file/folder counts. Path separators are converted to `\` for Windows compatibility.

## Database format

The database extends the upstream format with two version-gated sections:

- **Version 3**: zstd-compressed file-size stream (`size, allocated` pairs per file).
- **Version 4**: zstd-compressed checksum stream (length-prefixed binary checksums per file).

Upstream plocate readers ignore the extra header fields — the filename index and trigram posting lists are unchanged.

## License

plocate is Copyright 2020 Steinar H. Gunderson. Licensed under the GNU General Public License, version 2 or later. See COPYING.

updatedb is Copyright (C) 2005, 2007 Red Hat, Inc. Licensed under the GNU General Public License, version 2. See COPYING.
