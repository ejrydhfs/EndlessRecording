#!/usr/bin/env python3
"""End-to-end tests: real FUSE mount, real writes, real wrap."""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "endless")
CAP = 8 << 20
STEP = 64 << 10

fails = []


def check(cond, msg):
    if cond:
        print(f"  ok   {msg}")
    else:
        print(f"  FAIL {msg}")
        fails.append(msg)


def read_fd(fd, n=-1):
    out = []
    while n != 0:
        try:
            chunk = os.read(fd, 65536 if n < 0 else min(65536, n))
        except BlockingIOError:
            break
        if not chunk:
            break
        out.append(chunk)
        if n > 0:
            n -= len(chunk)
    return b"".join(out)


def read_path(path, follow=False, limit=None):
    fd = os.open(path, os.O_RDONLY | (0 if follow else os.O_NONBLOCK))
    try:
        return read_fd(fd, -1 if limit is None else limit)
    finally:
        os.close(fd)


class Daemon:
    def __init__(self, backing, mnt, fmt=False, size="8M", extra=()):
        self.log = open(backing + ".log", "ab")
        args = [EXE, "mount", "--file", backing]
        if fmt:
            args.append("--format")
        args += ["--size", size, "--step", str(STEP), "--commit", str(STEP)]
        args += list(extra) + [mnt]
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=self.log)
        self.mnt = mnt
        self.ready()

    def ready(self, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                raise RuntimeError("daemon exited: " + self.logtext())
            try:
                read_path(os.path.join(self.mnt, "status"))
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("mount never became ready")

    def logtext(self):
        self.log.flush()
        with open(self.log.name) as f:
            return f.read()

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            subprocess.run(["fusermount3", "-u", self.mnt],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        clear_mount(self.mnt)
        self.log.close()


def mounted(mnt):
    try:
        with open("/proc/self/mountinfo") as f:
            return any(line.split()[4] == mnt for line in f if len(line.split()) > 4)
    except OSError:
        return os.path.ismount(mnt)


def clear_mount(mnt, tries=30):
    for _ in range(tries):
        if not mounted(mnt):
            return
        subprocess.run(["fusermount3", "-uz", mnt],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.05)
    if mounted(mnt):
        raise RuntimeError(f"stale mount survived: {mnt}")


def status(mnt):
    out = {}
    for line in read_path(os.path.join(mnt, "status")).decode().splitlines():
        k, _, v = line.partition("=")
        out[k] = v
    return out


def main():
    tmp = tempfile.mkdtemp(prefix="endless-e2e-")
    backing = os.path.join(tmp, "tape.bin")
    mnt = os.path.join(tmp, "mnt")
    os.mkdir(mnt)
    d = None
    try:
        d = Daemon(backing, mnt, fmt=True)
        print("mount + format")
        check(status(mnt)["capacity"] == str(CAP), "capacity reported")

        print("append/wrap")
        chunk = 64 << 10
        data = b"".join(os.urandom(chunk) for _ in range(192))  # 12 MiB
        fd = os.open(os.path.join(mnt, "tape"), os.O_WRONLY)
        t0 = time.time()
        for off in range(0, len(data), chunk):
            os.write(fd, data[off:off + chunk])
        os.close(fd)
        dt = time.time() - t0
        s = status(mnt)
        check(s["used"] == str(CAP), "ring filled to capacity")
        check(s["wrapped"] == "1", "wrap flagged")
        check(int(s["head"]) == len(data), "absolute position == bytes written")
        check(read_path(os.path.join(mnt, "tape")) == data[-CAP:],
              "reads return exactly the last capacity bytes")
        print(f"       ({len(data)>>20} MiB in {dt:.2f}s, {len(data)/dt/2**20:.0f} MiB/s)")

        print("timestamp index / time travel")
        a = b"A" * chunk
        b = b"B" * chunk
        fd = os.open(os.path.join(mnt, "tape"), os.O_WRONLY)
        os.write(fd, a)
        time.sleep(1.2)
        os.write(fd, b)
        os.close(fd)
        got = read_path(os.path.join(mnt, "last-1"))
        check(got.endswith(b), "last-1 starts inside the most recent data")
        check(got == b, f"last-1 returns only data newer than 1s ({len(got)} bytes)")
        check(a not in got, "data older than the requested age is excluded")

        print("slow reader is overtaken, not blocked")
        rd = os.open(os.path.join(mnt, "tape"), os.O_RDONLY)
        first = read_fd(rd, 4096)
        check(len(first) == 4096, "reader got a prefix")
        head_before = int(status(mnt)["head"])
        fd = os.open(os.path.join(mnt, "tape"), os.O_WRONLY)
        fresh = os.urandom(10 << 20)  # wraps the 8 MiB ring
        for off in range(0, len(fresh), chunk):
            os.write(fd, fresh[off:off + chunk])
        os.close(fd)
        after = read_fd(rd, 4096)          # must skip, not stall
        os.close(rd)
        tail = int(status(mnt)["tail"])
        check(len(after) == 4096, "reader moved on after being overtaken")
        check(after == fresh[tail - head_before:tail - head_before + 4096],
              "skipped to the oldest retained byte")
        check(int(status(mnt)["dropped_bytes"]) > 0, "drop counter advanced")

        print("following the live edge past the ring size")
        got = bytearray()
        err = []
        live = os.urandom(14 << 20)   # offsets run past the 8 MiB ring

        def follower():
            fd = os.open(os.path.join(mnt, "live"), os.O_RDONLY)
            try:
                while len(got) < len(live):
                    piece = read_fd(fd, 65536)
                    if not piece:
                        break
                    got.extend(piece)
            except OSError as e:
                err.append(e)
            finally:
                os.close(fd)

        th = threading.Thread(target=follower, daemon=True)
        th.start()
        time.sleep(0.2)
        fd = os.open(os.path.join(mnt, "tape"), os.O_WRONLY)
        for off in range(0, len(live), 1 << 20):
            os.write(fd, live[off:off + (1 << 20)])
            time.sleep(0.01)          # keep the reader ahead of the overwrite
        os.close(fd)
        th.join(10)
        check(not th.is_alive(), "follower returned")
        check(not err, f"follower saw no error ({err})")
        check(len(got) == len(live), f"follower received {len(got)} of {len(live)} bytes")
        check(bytes(got) == live, "follower content matches")

        print("O_TRUNC does not erase")
        before = int(status(mnt)["head"])
        fd = os.open(os.path.join(mnt, "tape"), os.O_WRONLY | os.O_TRUNC)
        os.close(fd)
        check(int(status(mnt)["head"]) == before, "tape unchanged by O_TRUNC")

        print("non-blocking read at the live edge")
        fd = os.open(os.path.join(mnt, "live"), os.O_RDONLY | os.O_NONBLOCK)
        try:
            os.read(fd, 1024)
            check(False, "expected EAGAIN")
        except BlockingIOError:
            check(True, "EAGAIN at the live edge")
        finally:
            os.close(fd)

        s = status(mnt)
        head = int(s["head"])
        expect = live[-CAP:]
        d.stop()
        d = None

        print("remount recovers position and data")
        d = Daemon(backing, mnt, fmt=False)
        s = status(mnt)
        check(int(s["head"]) == head, "head survived restart")
        check(read_path(os.path.join(mnt, "tape")) == expect,
              "retained data survived restart")
        d.stop()
        d = None

        print("kill -9 loses at most one commit interval")
        d = Daemon(backing, mnt, fmt=True)
        payload = os.urandom((1 << 20) + 5000)   # ends mid-commit-interval
        fd = os.open(os.path.join(mnt, "tape"), os.O_WRONLY)
        for off in range(0, len(payload), 4096):   # many small appended chunks
            os.write(fd, payload[off:off + 4096])
        os.close(fd)
        head = int(status(mnt)["head"])
        d.proc.kill()
        d.proc.wait()
        d.log.close()
        d = None
        clear_mount(mnt)
        check(not mounted(mnt), "stale mount cleaned")
        d = Daemon(backing, mnt, fmt=False)
        sh = int(status(mnt)["head"])
        check(sh > 0, "recovered a committed position")
        check(0 < head - sh <= (64 << 10) + 4096,
              f"recovered head {sh} lags {head - sh} bytes (< one commit)")
        check(read_path(os.path.join(mnt, "tape")) == payload[:sh],
              "every byte below the committed position survived")
        d.stop()
        d = None

        print("ffmpeg round trip")
        d = Daemon(backing, mnt, fmt=True)
        ts = os.path.join(mnt, "tape")
        subprocess.run(
            ["ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i",
             "testsrc2=size=320x240:rate=25", "-t", "4",
             "-c:v", "mpeg2video", "-f", "mpegts", ts],
            check=True)
        clip = read_path(os.path.join(mnt, "last-60"))
        check(len(clip) > 10000, f"recorded clip is {len(clip)} bytes")
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "stream=codec_name",
             "-of", "csv=p=0", "-"],
            input=clip, capture_output=True, check=True)
        check(b"mpeg2video" in probe.stdout,
              f"ffprobe reads the retained clip ({probe.stdout.strip()!r})")
        d.stop()
        d = None
    finally:
        if d:
            d.stop()
        shutil.rmtree(tmp, ignore_errors=True)

    if fails:
        print(f"\n{len(fails)} failure(s)")
        return 1
    print("\ne2e: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
