# EndlessRecording

An endless tape on a fixed-size file. Write a byte stream into it forever;
once it is full the oldest bytes are overwritten. Read back the last N
minutes, or follow the live edge, with ordinary `read()`/`write()` calls.

Everything lives in one userspace daemon exposing a FUSE directory, so no
kernel module and no root are needed:

```
/srv/dvr/tape      <- recorder writes here (any format, any bitrate)
/srv/dvr/live      <- player reads from the write edge and follows it
/srv/dvr/last-5m   <- player reads from five minutes ago
/srv/dvr/status    <- capacity, used bytes, wrap state, timestamps
```

## Build

Dependencies: `fuse3` (runtime and headers), `gcc`, `make`. Tests also use
`python3` and, for one round-trip check, `ffmpeg`/`ffprobe`.

```
make
sudo make install          # /usr/local/bin/endless
```

## Quick start

```
mkdir -p /srv/dvr
endless mount --file /var/lib/endless/dvr.bin --size 64G --format /srv/dvr
```

`--format` creates (or wipes) the backing file and reserves 64 GiB, aligned to
4 KiB. Leave it out when restarting an existing tape.

Record, from a camera, a screen, an audio device, or anything else:

```
ffmpeg -y -f v4l2 -i /dev/video0 -c:v libx264 -preset ultrafast \
       -f mpegts /srv/dvr/tape

parec --raw | ffmpeg -y -f s16le -ar 48000 -ac 2 -i - -f mpegts /srv/dvr/tape
```

Play the last ten minutes, or dump everything still on the tape:

```
endless cat /srv/dvr --last 10m | ffplay -i -
endless cat /srv/dvr --snapshot > clip.ts      # stops at the write edge
endless status /srv/dvr
```

## The files

| path            | mode | read starts at                                             | write        |
|-----------------|------|------------------------------------------------------------|--------------|
| `tape`          | 0666 | the oldest byte still on the tape                          | appends      |
| `live`          | 0444 | the write edge, then follows it                            | –            |
| `last-<DUR>`    | 0444 | the position `DUR` before now (`30`, `30s`, `5m`, `2h`, `1d`) | –          |
| `status`        | 0444 | –                                                          | –            |

`oldest` and `all` are aliases for `tape`; `now` for `live`.

Stream semantics:

* Reads never return EOF. When a reader catches up with the writer it blocks
  until more data arrives. Open `O_NONBLOCK` (or use `cat --snapshot`) to get
  `EAGAIN` instead.
* Reads are not seekable: the tape has no stable offsets. `lseek()` fails with
  `ESPIPE`. Reopen to move.
* `stat()` reports size 0 for `tape` (a sink has no length; this also keeps
  tools from prompting "file exists, overwrite?") and the retained byte count
  for `live`/`last-*`. Reads ignore that number.
* `O_TRUNC` is accepted and ignored, so opening a recorder twice, or a player
  touching the stream, cannot erase the recording.
* If a reader falls more than `capacity` bytes behind, the oldest data it was
  about to read has already been overwritten: that read skips forward to the
  oldest byte that still exists. The skipped amount is reported as
  `dropped_bytes` in `status`.
* Writes never block on readers and never fail because the tape is "full".

Timestamps come from `CLOCK_REALTIME` and are indexed every `--step` bytes
(1 MiB by default), so `last-<DUR>` starts at the newest index sample at or
before the requested instant — at most one step of extra history.

## How it works

The backing file is a ring with an absolute address space. Byte *N* is the
*N*-th byte ever written and keeps that identity forever; only the most recent
`capacity` bytes of that address space still exist on disk.

```
offset 0        4 KiB      8 KiB                         8 KiB + capacity
       +--------+----------+-----------------------------+
       |  sb A  |   sb B   |   circular data region      |
       +--------+----------+-----------------------------+
                            ^                             ^
                            | p % capacity                |
```

* `head` is the absolute position of the write edge. It only ever grows.
* `tail = max(0, head - capacity)` is the oldest byte that still exists.
* Absolute position `p` is stored at `8 KiB + (p % capacity)`, so a write that
  crosses the end is split into two `pwrite()`s and overwrites exactly the
  bytes that are now `capacity` old.

A sidecar file `BACKING.idx` holds a fixed array of `(absolute position,
timestamp)` samples; the sample slot is `(abs / step) % nslots`, so recording
for weeks costs constant space and one 16-byte `pwrite()` per step. That array
is what turns "five minutes ago" into a byte offset, including across restarts.

Durability is a two-phase commit every `--commit` bytes (4 MiB by default,
clamped to the capacity):

1. `fdatasync()` the data region,
2. write a superblock with a new generation number into slot A or B,
3. `fdatasync()` again.

On start the daemon picks the valid superblock with the highest generation.
Anything written after it — data and index samples alike — is treated as
uncommitted and discarded on the next write, so a crash can lose at most the
last commit interval, never corrupt what the tape claims to hold. Both
superblock slots are checksummed, so a torn write cannot win.

`endless` is a FUSE filesystem because the object being modelled is a byte
stream, not a file with an extent map:

* `direct_io` bypasses the page cache (the same offset would otherwise serve
  stale bytes after a wrap) and makes every read/write a request to the daemon.
* `nonseekable` makes `lseek()` fail cleanly instead of silently mapping to the
  wrong data.
* Reads and writes are plain `pread()`/`pwrite()` on the backing file; there is
  no read-modify-write because only whole bytes at the write edge are ever
  replaced.

Measured on an ordinary ext4 laptop: ~800 MiB/s of appended stream through the
mount, which is far above any camera or audio bitrate.

## Tuning

| option          | default | meaning                                                        |
|-----------------|---------|----------------------------------------------------------------|
| `--size`        | –       | ring capacity, e.g. `512M`, `64G`; multiple of 4096, ≥ 1 MiB    |
| `--step`        | `1M`    | timestamp index granularity = time-seek precision              |
| `--commit`      | `4M`    | bytes between durability points (crash exposure)                |
| `--sparse`      | off     | do not reserve the capacity; the file grows as it is written    |
| `-o allow_other`| off     | let other users read the mount (`user_allow_other` in `/etc/fuse.conf`) |

The capacity is fixed at format time: a ring cannot grow without moving data.
To change it, copy the retained window out and format again.

By default the whole capacity is reserved with `posix_fallocate()` at format
time, so a recorder never runs out of disk halfway through the night. Use
`--sparse` on filesystems where that is expensive and you accept `ENOSPC`
later.

## Running it as a service

```
[Unit]
Description=Endless recording tape
After=local-fs.target

[Service]
# Run once by hand with --format to create the tape, then drop --format.
ExecStart=/usr/local/bin/endless mount --file /var/lib/endless/dvr.bin \
          --size 64G /srv/dvr
Restart=always

[Install]
WantedBy=multi-user.target
```

One daemon per backing file: the file is `flock()`ed, and a second mount
fails with "in use". Stop the daemon with `SIGTERM` (`systemctl stop`): it
wakes blocked readers, unmounts and commits. Unmounting underneath a running
daemon with `fusermount3 -u` fails with `EBUSY` while a reader is blocked.
After a `kill -9` the mountpoint is left stale; clean it with
`fusermount3 -uz /srv/dvr`.

## Notes and limits

* Use streaming container formats (MPEG-TS, Matroska, raw audio). MP4 writes
  its index by seeking backwards, which a tape cannot do.
* `ffmpeg` prompts before writing to an existing path; pass `-y` (or record to
  a path that never exists, which is always the case for `tape`).
* A player that opens `last-5m` starts mid-packet; MPEG-TS resynchronises on
  the next 0x47. Raw PCM just starts mid-sample.
* Multiple writers are allowed but their bytes interleave; use one.
* The index is a hint, not a journal: if `--step` changes or the sidecar is
  deleted, old data is still intact but only newly written data gets
  timestamps (`oldest`/`newest` show `-`).
* No filesystem is mounted on the tape: it is a byte stream, not a disk.

## Tests

```
make check
```

`tests/tape_test.c` exercises the ring directly: ragged writes, wrap
arithmetic, retained-window reads, skip-on-overwrite, time index, commit and
reopen. `tests/e2e.py` mounts a real FUSE filesystem and checks 12 MiB written
into 8 MiB, exact replay of the retained window, `last-1` windows, an
overtaken reader, a follower reading 14 MiB past the ring size, `O_TRUNC`,
`EAGAIN`, restart recovery, and an `ffmpeg` → `ffprobe` round trip.
