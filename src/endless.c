/* SPDX-License-Identifier: MIT */
/*
 * endless - endless recording on a fixed-size file.
 *
 * A FUSE filesystem exposing a directory of "tapes":
 *
 *   tape        the stream: writes append, reads replay from the oldest byte
 *   live        reads start at the write end and follow it
 *   last-<N>    reads start N seconds before now (s/m/h/d suffix allowed)
 *   status      key=value counters, timestamps and geometry
 *
 * Reads are streams: they block when they catch up with the writer unless the
 * file was opened O_NONBLOCK.  lseek() is not supported; reopen instead.
 */
#define FUSE_USE_VERSION 312
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "tape.h"

#define STATUS_MAX 4096

struct state {
	struct tape t;
};

enum win { W_TAPE, W_LIVE, W_STATUS, W_LAST };

struct handle {
	uint64_t pos;
	int flags;
	enum win win;
};

static struct state *STATE;
static struct fuse *G_FUSE;
static volatile sig_atomic_t g_stop;

/* Async-signal-safe: a flag store and the same call libfuse's own handler makes.
 * Readers poll g_stop, so a blocked read does not hold the unmount hostage. */
static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
	if (G_FUSE)
		fuse_exit(G_FUSE);
}

/* ---------------------------------------------------------------- helpers */

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("endless: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

static uint64_t parse_size(const char *s)
{
	char *end;
	unsigned long long v = strtoull(s, &end, 10);
	uint64_t mult = 1;

	if (end == s)
		die("bad size '%s'", s);
	if (*end) {
		char u = *end;

		if (u >= 'a' && u <= 'z')
			u -= 32;
		switch (u) {
		case 'K': mult = 1ull << 10; break;
		case 'M': mult = 1ull << 20; break;
		case 'G': mult = 1ull << 30; break;
		case 'T': mult = 1ull << 40; break;
		default: die("bad size '%s'", s);
		}
		end++;
		if (*end == 'i' || *end == 'I')
			end++;
		if (*end == 'B' || *end == 'b')
			end++;
		if (*end)
			die("bad size '%s'", s);
	}
	if (v > UINT64_MAX / mult)
		die("size '%s' is too large", s);
	return v * mult;
}

static void fmt_size(uint64_t v, char *out, size_t n)
{
	static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
	double d = (double)v;
	int i = 0;

	while (d >= 1024.0 && i < 5) {
		d /= 1024.0;
		i++;
	}
	snprintf(out, n, i ? "%.2f %s" : "%.0f %s", d, unit[i]);
}

static void fmt_time(uint64_t ns, char *out, size_t n)
{
	time_t secs;
	struct tm tm;

	if (!ns) {
		snprintf(out, n, "-");
		return;
	}
	secs = (time_t)(ns / 1000000000ull);
	localtime_r(&secs, &tm);
	strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

static void fmt_dur(uint64_t ns, char *out, size_t n)
{
	uint64_t s = ns / 1000000000ull;

	snprintf(out, n, "%lluh%02llum%02llus", (unsigned long long)(s / 3600),
		 (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
}

static int write_full(int fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* ------------------------------------------------------------ path parsing */

static int parse_window(const char *path, enum win *win, int64_t *secs)
{
	if (!strcmp(path, "/tape") || !strcmp(path, "/all")) {
		*win = W_TAPE;
		return 0;
	}
	if (!strcmp(path, "/live") || !strcmp(path, "/now")) {
		*win = W_LIVE;
		return 0;
	}
	if (!strcmp(path, "/status")) {
		*win = W_STATUS;
		return 0;
	}
	if (!strncmp(path, "/last-", 6)) {
		const char *p = path + 6;
		char *end;
		long long v;
		int64_t mult = 1;

		v = strtoll(p, &end, 10);
		if (end == p || v < 0)
			return -ENOENT;
		if (*end) {
			switch (*end) {
			case 's': mult = 1; break;
			case 'm': mult = 60; break;
			case 'h': mult = 3600; break;
			case 'd': mult = 86400; break;
			default: return -ENOENT;
			}
			if (end[1])
				return -ENOENT;
		}
		if (v > INT64_MAX / mult)
			return -ENOENT;
		*win = W_LAST;
		*secs = (int64_t)v * mult;
		return 0;
	}
	return -ENOENT;
}

/* --------------------------------------------------------------- fuse ops */

static int e_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	enum win win;
	int64_t secs;

	(void)fi;
	memset(st, 0, sizeof *st);
	if (!strcmp(path, "/")) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		return 0;
	}
	if (parse_window(path, &win, &secs) < 0)
		return -ENOENT;

	st->st_nlink = 1;
	st->st_blksize = (blksize_t)TAPE_SECTOR;
	st->st_mtime = st->st_ctime = st->st_atime = time(NULL);
	if (win == W_STATUS) {
		char buf[STATUS_MAX];
		int n = tape_status(&STATE->t, buf, sizeof buf);

		st->st_mode = S_IFREG | 0444;
		st->st_size = n > 0 ? n : 0;
	} else if (win == W_TAPE) {
		/* A recorder target must look empty, otherwise tools refuse to
		 * overwrite it.  Reads are not limited by this size: the tape has no
		 * end, it either returns data or blocks at the write edge. */
		st->st_mode = S_IFREG | 0666;
		st->st_size = 0;
	} else {
		st->st_mode = S_IFREG | 0444;
		st->st_size = (off_t)tape_used(&STATE->t);
	}
	st->st_blocks = (st->st_size + 511) / 512;
	return 0;
}

static int e_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		     off_t off, struct fuse_file_info *fi,
		     enum fuse_readdir_flags flags)
{
	(void)off;
	(void)fi;
	(void)flags;
	if (strcmp(path, "/"))
		return -ENOENT;
	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);
	filler(buf, "tape", NULL, 0, 0);
	filler(buf, "live", NULL, 0, 0);
	filler(buf, "status", NULL, 0, 0);
	return 0;
}

static int e_open(const char *path, struct fuse_file_info *fi)
{
	struct handle *h;
	enum win win;
	int64_t secs;
	int acc = fi->flags & O_ACCMODE;

	if (parse_window(path, &win, &secs) < 0)
		return -ENOENT;

	if (win != W_TAPE && acc != O_RDONLY)
		return -EACCES;
	if (win == W_STATUS && acc != O_RDONLY)
		return -EACCES;

	h = calloc(1, sizeof *h);
	if (!h)
		return -ENOMEM;
	h->flags = fi->flags;
	h->win = win;
	switch (win) {
	case W_LIVE:
		h->pos = tape_head(&STATE->t);
		break;
	case W_LAST:
		h->pos = tape_seek_time(&STATE->t, secs);
		break;
	case W_STATUS:
	case W_TAPE:
	default:
		h->pos = tape_tail(&STATE->t);
		break;
	}

	fi->fh = (uintptr_t)h;
	/* No page cache: the same offset holds different bytes after a wrap. */
	fi->direct_io = 1;
	fi->keep_cache = 0;
	/* The tape is a stream; there is nothing to seek to. */
	fi->nonseekable = 1;
	return 0;
}

static int e_read(const char *path, char *buf, size_t size, off_t off,
		  struct fuse_file_info *fi)
{
	struct handle *h = (struct handle *)(uintptr_t)fi->fh;
	ssize_t n;

	(void)path;
	if (h->win == W_STATUS) {
		char tmp[STATUS_MAX];
		int len = tape_status(&STATE->t, tmp, sizeof tmp);

		if (len < 0)
			return -EIO;
		if (off >= len)
			return 0;
		if ((size_t)(len - off) < size)
			size = (size_t)(len - off);
		memcpy(buf, tmp + off, size);
		return (int)size;
	}
	if ((h->flags & O_ACCMODE) == O_WRONLY)
		return -EBADF;
	n = tape_read(&STATE->t, &h->pos, buf, size, (h->flags & O_NONBLOCK) != 0);
	return (int)n;
}

static int e_write(const char *path, const char *buf, size_t size, off_t off,
		   struct fuse_file_info *fi)
{
	struct handle *h = (struct handle *)(uintptr_t)fi->fh;
	ssize_t n;

	(void)path;
	(void)off;
	if (h->win != W_TAPE)
		return -EBADF;
	n = tape_write(&STATE->t, buf, size);
	return (int)n;
}

static int e_release(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	free((struct handle *)(uintptr_t)fi->fh);
	fi->fh = 0;
	return 0;
}

static int e_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	enum win win;
	int64_t secs;

	(void)size;
	(void)fi;
	if (parse_window(path, &win, &secs) < 0)
		return -ENOENT;
	/* O_TRUNC must not erase a recording: opening a recorder twice, or a
	 * player touching the file, must be harmless. */
	return 0;
}

static int e_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	(void)path;
	(void)datasync;
	(void)fi;
	return tape_sync(&STATE->t);
}

static int e_statfs(const char *path, struct statvfs *st)
{
	uint64_t used;

	(void)path;
	memset(st, 0, sizeof *st);
	used = tape_used(&STATE->t);
	st->f_bsize = TAPE_SECTOR;
	st->f_frsize = TAPE_SECTOR;
	st->f_blocks = STATE->t.capacity / TAPE_SECTOR;
	st->f_bfree = (STATE->t.capacity - used) / TAPE_SECTOR;
	st->f_bavail = st->f_bfree;
	st->f_files = 3;
	st->f_namemax = 255;
	return 0;
}

static const struct fuse_operations endless_ops = {
	.getattr = e_getattr,
	.readdir = e_readdir,
	.open = e_open,
	.read = e_read,
	.write = e_write,
	.release = e_release,
	.truncate = e_truncate,
	.fsync = e_fsync,
	.statfs = e_statfs,
};

/* --------------------------------------------------------------------- cli */

static void usage(FILE *out)
{
	fprintf(out,
		"usage:\n"
		"  endless mount --file PATH [--size SIZE] [--format] [--step SIZE]\n"
		"                [--commit SIZE] [--sparse] [-o FUSE_OPTS] MOUNTPOINT\n"
		"  endless status MOUNTPOINT [--raw]\n"
		"  endless cat MOUNTPOINT [--last DUR] [--snapshot]\n"
		"  endless version\n"
		"\n"
		"mount:\n"
		"  --file PATH    backing file holding the ring (required)\n"
		"  --size SIZE    ring capacity, e.g. 512M, 8G (required with --format)\n"
		"  --format       create or wipe PATH, then format it\n"
		"  --step SIZE    timestamp index granularity (default 1MiB)\n"
		"  --commit SIZE  bytes between durability points (default 4MiB)\n"
		"  --sparse       do not reserve the capacity up front\n"
		"  -o OPTS        extra FUSE mount options (e.g. allow_other)\n"
		"\n"
		"cat:\n"
		"  --last DUR     start DUR before now (30, 30s, 5m, 2h, 1d)\n"
		"  --snapshot     stop at the current write end instead of following\n"
		"\n"
		"files inside MOUNTPOINT:\n"
		"  tape           write here; reads replay from the oldest byte\n"
		"  live           reads start at the write end\n"
		"  last-<DUR>     reads start DUR before now, e.g. last-300, last-5m\n"
		"  status         capacity, used bytes, wrap state, timestamps\n");
}

static int cmd_mount(int argc, char **argv)
{
	const char *file = NULL, *mountpoint = NULL;
	char *fuse_opts = NULL;
	struct tape_cfg cfg;
	struct state st;
	char err[512];
	int i, rc;

	memset(&cfg, 0, sizeof cfg);
	cfg.prealloc = true;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--file")) {
			if (++i == argc)
				die("--file needs an argument");
			file = argv[i];
		} else if (!strcmp(argv[i], "--size")) {
			if (++i == argc)
				die("--size needs an argument");
			cfg.capacity = parse_size(argv[i]);
		} else if (!strcmp(argv[i], "--step")) {
			if (++i == argc)
				die("--step needs an argument");
			cfg.step = parse_size(argv[i]);
		} else if (!strcmp(argv[i], "--commit")) {
			if (++i == argc)
				die("--commit needs an argument");
			cfg.commit = parse_size(argv[i]);
		} else if (!strcmp(argv[i], "--format")) {
			cfg.format = true;
		} else if (!strcmp(argv[i], "--sparse")) {
			cfg.prealloc = false;
		} else if (!strcmp(argv[i], "-o")) {
			char *merged;

			if (++i == argc)
				die("-o needs an argument");
			if (asprintf(&merged, "%s%s%s", fuse_opts ? fuse_opts : "",
				     fuse_opts ? "," : "", argv[i]) < 0)
				die("out of memory");
			free(fuse_opts);
			fuse_opts = merged;
		} else if (argv[i][0] == '-') {
			die("unknown option '%s'", argv[i]);
		} else if (!mountpoint) {
			mountpoint = argv[i];
		} else {
			die("unexpected argument '%s'", argv[i]);
		}
	}
	if (!file)
		die("--file is required");
	if (!mountpoint)
		die("mountpoint is required");
	{
		struct stat mst;

		if (stat(mountpoint, &mst) < 0)
			die("%s: %s", mountpoint, strerror(errno));
		if (!S_ISDIR(mst.st_mode))
			die("%s: not a directory", mountpoint);
	}

	cfg.path = file;

	memset(&st, 0, sizeof st);
	if (tape_open(&st.t, &cfg, err, sizeof err) < 0) {
		tape_close(&st.t);
		die("%s", err);
	}
	STATE = &st;

	{
		struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
		struct fuse_loop_config *lcfg;
		struct fuse *f;
		struct sigaction sa;
		char *opts;

		if (asprintf(&opts,
			     "fsname=endless,subtype=endless,attr_timeout=0,"
			     "entry_timeout=0,negative_timeout=0%s%s",
			     fuse_opts ? "," : "", fuse_opts ? fuse_opts : "") < 0)
			die("out of memory");
		fuse_opt_add_arg(&args, "endless");
		fuse_opt_add_arg(&args, "-o");
		fuse_opt_add_arg(&args, opts);
		f = fuse_new(&args, &endless_ops, sizeof endless_ops, &st);
		if (!f)
			die("cannot create the FUSE filesystem");
		if (fuse_mount(f, mountpoint) != 0)
			die("cannot mount %s (stale mountpoint? fusermount3 -uz %s)",
			    mountpoint, mountpoint);

		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_signal;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGHUP, &sa, NULL);

		G_FUSE = f;
		st.t.stop = &g_stop;
		fprintf(stderr,
			"endless: %s: %llu bytes, step %llu, commit %llu; "
			"write to %s/tape\n",
			file, (unsigned long long)st.t.capacity,
			(unsigned long long)st.t.step,
			(unsigned long long)st.t.commit_bytes, mountpoint);

		lcfg = fuse_loop_cfg_create();
		if (!lcfg)
			die("out of memory");
		rc = fuse_loop_mt(f, lcfg);
		fuse_loop_cfg_destroy(lcfg);
		fuse_unmount(f);
		fuse_destroy(f);
		fuse_opt_free_args(&args);
		free(opts);
	}
	STATE = NULL;
	G_FUSE = NULL;
	fprintf(stderr, "endless: stopping at %llu bytes\n",
		(unsigned long long)tape_head(&st.t));
	tape_shutdown(&st.t);
	tape_close(&st.t);
	free(fuse_opts);
	return rc ? 1 : 0;
}

struct status {
	uint64_t capacity, used, head, tail, bytes_written, dropped;
	uint64_t oldest_ts, newest_ts, step, commit_bytes;
	int wrapped;
	char backing[PATH_MAX + 64];
};

static uint64_t kv(const char *line, const char *key, uint64_t dflt)
{
	size_t n = strlen(key);
	unsigned long long v;

	if (!strncmp(line, key, n) && line[n] == '=' &&
	    sscanf(line + n + 1, "%llu", &v) == 1)
		return v;
	return dflt;
}

static bool parse_status(const char *text, struct status *s)
{
	char line[PATH_MAX + 64];
	const char *p = text;

	memset(s, 0, sizeof *s);
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);

		if (len >= sizeof line)
			return false;
		memcpy(line, p, len);
		line[len] = 0;
		s->capacity = kv(line, "capacity", s->capacity);
		s->used = kv(line, "used", s->used);
		s->head = kv(line, "head", s->head);
		s->tail = kv(line, "tail", s->tail);
		s->bytes_written = kv(line, "bytes_written", s->bytes_written);
		s->dropped = kv(line, "dropped_bytes", s->dropped);
		s->oldest_ts = kv(line, "oldest_ts_ns", s->oldest_ts);
		s->newest_ts = kv(line, "newest_ts_ns", s->newest_ts);
		s->step = kv(line, "step", s->step);
		s->commit_bytes = kv(line, "commit_bytes", s->commit_bytes);
		s->wrapped = (int)kv(line, "wrapped", (uint64_t)s->wrapped);
		if (!strncmp(line, "backing=", 8))
			snprintf(s->backing, sizeof s->backing, "%s", line + 8);
		if (!nl)
			break;
		p = nl + 1;
	}
	return s->capacity != 0;
}

static int cmd_status(int argc, char **argv)
{
	const char *mountpoint = NULL;
	bool raw = false;
	char path[PATH_MAX], text[STATUS_MAX];
	struct status s;
	ssize_t n;
	int fd, i;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--raw"))
			raw = true;
		else if (argv[i][0] == '-')
			die("unknown option '%s'", argv[i]);
		else if (!mountpoint)
			mountpoint = argv[i];
		else
			die("unexpected argument '%s'", argv[i]);
	}
	if (!mountpoint)
		die("mountpoint is required");
	snprintf(path, sizeof path, "%s/status", mountpoint);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		die("%s: %s", path, strerror(errno));
	n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n < 0)
		die("%s: %s", path, strerror(errno));
	text[n] = 0;
	if (raw) {
		fputs(text, stdout);
		return 0;
	}
	if (!parse_status(text, &s))
		die("%s: unexpected content", path);
	{
		char cap[32], used[32], written[32], step[32], commit[32];
		char old[32], new[32], dur[32];

		fmt_size(s.capacity, cap, sizeof cap);
		fmt_size(s.used, used, sizeof used);
		fmt_size(s.bytes_written, written, sizeof written);
		fmt_size(s.step, step, sizeof step);
		fmt_size(s.commit_bytes, commit, sizeof commit);
		fmt_time(s.oldest_ts, old, sizeof old);
		fmt_time(s.newest_ts, new, sizeof new);
		fmt_dur(s.newest_ts > s.oldest_ts ? s.newest_ts - s.oldest_ts : 0,
			dur, sizeof dur);
		printf("capacity       %s\n", cap);
		printf("used           %s (%.1f%%)\n", used,
		       s.capacity ? 100.0 * (double)s.used / (double)s.capacity : 0.0);
		printf("written        %s\n", written);
		printf("wrapped        %s\n", s.wrapped ? "yes (oldest data overwritten)" : "no");
		printf("head / tail    %llu / %llu\n",
		       (unsigned long long)s.head, (unsigned long long)s.tail);
		printf("oldest         %s\n", old);
		printf("newest         %s\n", new);
		printf("retained       %s\n", dur);
		printf("dropped        %llu bytes (readers overtaken)\n",
		       (unsigned long long)s.dropped);
		printf("step / commit  %s / %s\n", step, commit);
		printf("backing        %s\n", s.backing);
	}
	return 0;
}

static int cmd_cat(int argc, char **argv)
{
	const char *mountpoint = NULL, *last = NULL;
	bool snapshot = false;
	char path[PATH_MAX];
	char buf[256 * 1024];
	int fd, i;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--last")) {
			if (++i == argc)
				die("--last needs an argument");
			last = argv[i];
		} else if (!strcmp(argv[i], "--snapshot")) {
			snapshot = true;
		} else if (argv[i][0] == '-') {
			die("unknown option '%s'", argv[i]);
		} else if (!mountpoint) {
			mountpoint = argv[i];
		} else {
			die("unexpected argument '%s'", argv[i]);
		}
	}
	if (!mountpoint)
		die("mountpoint is required");
	if (last)
		snprintf(path, sizeof path, "%s/last-%s", mountpoint, last);
	else
		snprintf(path, sizeof path, "%s/tape", mountpoint);

	fd = open(path, O_RDONLY | (snapshot ? O_NONBLOCK : 0));
	if (fd < 0)
		die("%s: %s", path, strerror(errno));
	for (;;) {
		ssize_t n = read(fd, buf, sizeof buf);

		if (n < 0) {
			if (errno == EAGAIN)
				break;
			if (errno == EINTR)
				continue;
			die("%s: %s", path, strerror(errno));
		}
		if (!n)
			break;
		if (write_full(STDOUT_FILENO, buf, (size_t)n) < 0)
			die("stdout: %s", strerror(errno));
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	if (!strcmp(argv[1], "mount"))
		return cmd_mount(argc - 2, argv + 2);
	if (!strcmp(argv[1], "status"))
		return cmd_status(argc - 2, argv + 2);
	if (!strcmp(argv[1], "cat"))
		return cmd_cat(argc - 2, argv + 2);
	if (!strcmp(argv[1], "version")) {
		printf("endless %s\n", "0.1.0");
		return 0;
	}
	if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") ||
	    !strcmp(argv[1], "-h")) {
		usage(stdout);
		return 0;
	}
	fprintf(stderr, "endless: unknown command '%s'\n", argv[1]);
	usage(stderr);
	return 2;
}
