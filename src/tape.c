/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "tape.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct sb {
	uint32_t magic;
	uint32_t version;
	uint64_t capacity;
	uint64_t data_off;
	uint64_t step;
	uint64_t head;
	uint64_t gen;
	uint64_t crc;
};

struct ihdr {
	uint32_t magic;
	uint32_t version;
	uint64_t step;
	uint64_t capacity;
	uint32_t nslots;
	uint32_t pad;
	uint64_t crc;
};

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (!err || !errlen)
		return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

static uint64_t csum(const void *p, size_t n)
{
	const unsigned char *b = p;
	uint64_t h = 0xcbf29ce484222325ull;

	while (n--) {
		h ^= *b++;
		h *= 0x100000001b3ull;
	}
	return h;
}

static uint64_t sb_csum(const struct sb *s)
{
	struct sb c = *s;

	c.crc = 0;
	return csum(&c, sizeof c);
}

static uint64_t ihdr_csum(const struct ihdr *h)
{
	struct ihdr c = *h;

	c.crc = 0;
	return csum(&c, sizeof c);
}

static int pwrite_full(int fd, const void *buf, size_t n, off_t off)
{
	const char *p = buf;

	while (n) {
		ssize_t w = pwrite(fd, p, n, off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (w == 0) {
			errno = EIO;
			return -1;
		}
		p += w;
		n -= (size_t)w;
		off += w;
	}
	return 0;
}

static int pread_full(int fd, void *buf, size_t n, off_t off)
{
	char *p = buf;

	while (n) {
		ssize_t r = pread(fd, p, n, off);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0) {
			errno = EIO;
			return -1;
		}
		p += r;
		n -= (size_t)r;
		off += r;
	}
	return 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool sb_valid(const struct sb *s)
{
	return s->magic == TAPE_MAGIC && s->version == TAPE_VERSION &&
	       s->data_off == TAPE_DATA_OFF && s->capacity >= TAPE_MIN_CAPACITY &&
	       s->capacity <= TAPE_MAX_CAPACITY &&
	       s->capacity % TAPE_SECTOR == 0 && s->step >= TAPE_SECTOR &&
	       s->step <= s->capacity && s->crc == sb_csum(s);
}

/* Index slots hold the timestamp at an absolute position; a zero timestamp
 * marks a never-used slot. */
static void idx_put_locked(struct tape *t, uint64_t abs, uint64_t ts)
{
	uint32_t slot = (uint32_t)((abs / t->step) % t->nslots);

	t->idx[slot].abs = abs;
	t->idx[slot].ts_ns = ts;
	if (t->ifd >= 0)
		pwrite_full(t->ifd, &t->idx[slot], sizeof(struct tape_idx),
			    (off_t)TAPE_SECTOR + (off_t)slot * (off_t)sizeof(struct tape_idx));
}

static void idx_sample_locked(struct tape *t, uint64_t ts)
{
	while (t->next_sample <= t->head) {
		idx_put_locked(t, t->next_sample, ts);
		t->next_sample += t->step;
	}
}

static int idx_open(struct tape *t, bool reset, char *err, size_t errlen)
{
	struct ihdr h;
	struct stat st;
	off_t want;
	bool ok = false;

	if (asprintf(&t->ipath, "%s.idx", t->path) < 0) {
		seterr(err, errlen, "out of memory");
		return -1;
	}
	t->ifd = open(t->ipath, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (t->ifd < 0) {
		seterr(err, errlen, "%s: %s", t->ipath, strerror(errno));
		return -1;
	}
	t->idx = calloc(t->nslots, sizeof(struct tape_idx));
	if (!t->idx) {
		seterr(err, errlen, "out of memory");
		return -1;
	}
	want = (off_t)TAPE_SECTOR +
	       (off_t)t->nslots * (off_t)sizeof(struct tape_idx);

	if (fstat(t->ifd, &st) == 0 && st.st_size >= want &&
	    pread_full(t->ifd, &h, sizeof h, 0) == 0 && h.magic == TAPE_MAGIC &&
	    h.version == TAPE_VERSION && h.step == t->step &&
	    h.capacity == t->capacity && h.nslots == t->nslots &&
	    h.crc == ihdr_csum(&h))
		ok = true;

	if (ok && !reset) {
		if (pread_full(t->ifd, t->idx, (size_t)t->nslots * sizeof(struct tape_idx),
			       (off_t)TAPE_SECTOR) < 0) {
			seterr(err, errlen, "%s: %s", t->ipath, strerror(errno));
			return -1;
		}
		return 0;
	}

	/* Missing, stale (different geometry), corrupt or freshly formatted:
	 * start an empty index.
	 * Timestamps of already recorded data are lost, the data itself is not. */
	memset(&h, 0, sizeof h);
	h.magic = TAPE_MAGIC;
	h.version = TAPE_VERSION;
	h.step = t->step;
	h.capacity = t->capacity;
	h.nslots = t->nslots;
	h.crc = ihdr_csum(&h);
	if (ftruncate(t->ifd, want) < 0 || pwrite_full(t->ifd, &h, sizeof h, 0) < 0 ||
	    pwrite_full(t->ifd, t->idx, (size_t)t->nslots * sizeof(struct tape_idx),
			(off_t)TAPE_SECTOR) < 0) {
		seterr(err, errlen, "%s: %s", t->ipath, strerror(errno));
		return -1;
	}
	return 0;
}

static uint64_t round_step(uint64_t capacity, uint64_t step)
{
	if (!step)
		step = TAPE_DEFAULT_STEP;
	if (step < TAPE_SECTOR)
		step = TAPE_SECTOR;
	while (capacity / step + 2 > TAPE_MAX_SLOTS)
		step *= 2;
	if (step > capacity)
		step = capacity;
	return step;
}

/* Write a superblock describing t->head as durable.  Data must already be on
 * stable storage; two alternating copies make a torn write harmless. */
static int commit_locked(struct tape *t)
{
	struct sb sb;
	off_t slot;
	int rc = 0;

	if (t->head == t->last_commit)
		return 0;
	if (fdatasync(t->fd) < 0)
		return -errno;

	memset(&sb, 0, sizeof sb);
	sb.magic = TAPE_MAGIC;
	sb.version = TAPE_VERSION;
	sb.capacity = t->capacity;
	sb.data_off = TAPE_DATA_OFF;
	sb.step = t->step;
	sb.head = t->head;
	sb.gen = ++t->sb_gen;
	sb.crc = sb_csum(&sb);
	slot = (off_t)(t->sb_gen & 1) * (off_t)TAPE_SECTOR;
	if (pwrite_full(t->fd, &sb, sizeof sb, slot) < 0)
		rc = -errno;
	else if (fdatasync(t->fd) < 0)
		rc = -errno;
	if (!rc && t->ifd >= 0 && fdatasync(t->ifd) < 0)
		rc = -errno;
	if (!rc)
		t->last_commit = t->head;
	return rc;
}

static int format_new(struct tape *t, const struct tape_cfg *cfg, char *err,
		      size_t errlen)
{
	uint64_t total = TAPE_DATA_OFF + t->capacity;
	struct sb sb;
	char zero[TAPE_SECTOR] = {0};
	int rc;

	if (cfg->prealloc) {
		rc = posix_fallocate(t->fd, 0, (off_t)total);
		if (rc) {
			seterr(err, errlen, "cannot reserve %llu bytes: %s",
			       (unsigned long long)total, strerror(rc));
			return -1;
		}
	}
	if (ftruncate(t->fd, (off_t)total) < 0) {
		seterr(err, errlen, "truncate: %s", strerror(errno));
		return -1;
	}
	if (pwrite_full(t->fd, zero, sizeof zero, 0) < 0 ||
	    pwrite_full(t->fd, zero, sizeof zero, (off_t)TAPE_SECTOR) < 0) {
		seterr(err, errlen, "write: %s", strerror(errno));
		return -1;
	}
	memset(&sb, 0, sizeof sb);
	sb.magic = TAPE_MAGIC;
	sb.version = TAPE_VERSION;
	sb.capacity = t->capacity;
	sb.data_off = TAPE_DATA_OFF;
	sb.step = t->step;
	sb.head = 0;
	sb.gen = 1;
	sb.crc = sb_csum(&sb);
	if (pwrite_full(t->fd, &sb, sizeof sb, 0) < 0 ||
	    pwrite_full(t->fd, &sb, sizeof sb, (off_t)TAPE_SECTOR) < 0 ||
	    fdatasync(t->fd) < 0) {
		seterr(err, errlen, "write: %s", strerror(errno));
		return -1;
	}
	t->head = 0;
	t->last_commit = 0;
	t->sb_gen = 1;
	return 0;
}

int tape_open(struct tape *t, const struct tape_cfg *cfg, char *err, size_t errlen)
{
	struct sb a, b;
	struct stat st;
	bool oka = false, okb = false, have_sb = false;
	int flags = O_RDWR | O_CLOEXEC;

	memset(t, 0, sizeof *t);
	t->fd = -1;
	t->ifd = -1;
	pthread_mutex_init(&t->lock, NULL);
	pthread_cond_init(&t->cond, NULL);

	t->path = strdup(cfg->path);
	if (!t->path) {
		seterr(err, errlen, "out of memory");
		return -1;
	}
	if (cfg->format)
		flags |= O_CREAT;
	t->fd = open(t->path, flags, 0644);
	if (t->fd < 0) {
		seterr(err, errlen, "%s: %s", t->path, strerror(errno));
		return -1;
	}
	if (flock(t->fd, LOCK_EX | LOCK_NB) < 0) {
		seterr(err, errlen, "%s is in use by another endless daemon", t->path);
		return -1;
	}

	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	oka = pread_full(t->fd, &a, sizeof a, 0) == 0 && sb_valid(&a);
	okb = pread_full(t->fd, &b, sizeof b, (off_t)TAPE_SECTOR) == 0 && sb_valid(&b);

	if (cfg->format) {
		struct sb *old = NULL;

		if (!cfg->capacity) {
			seterr(err, errlen, "--size is required with --format");
			return -1;
		}
		if (cfg->capacity % TAPE_SECTOR) {
			seterr(err, errlen, "size must be a multiple of %llu bytes",
			       (unsigned long long)TAPE_SECTOR);
			return -1;
		}
		if (cfg->capacity < TAPE_MIN_CAPACITY ||
		    cfg->capacity > TAPE_MAX_CAPACITY) {
			seterr(err, errlen, "size must be between %llu and %llu bytes",
			       (unsigned long long)TAPE_MIN_CAPACITY,
			       (unsigned long long)TAPE_MAX_CAPACITY);
			return -1;
		}
		if (oka)
			old = &a;
		if (okb && (!old || b.gen > old->gen))
			old = &b;
		t->capacity = cfg->capacity;
		t->step = round_step(t->capacity,
				     cfg->step ? cfg->step : (old ? old->step : 0));
		if (format_new(t, cfg, err, errlen) < 0)
			return -1;
		have_sb = true;
	} else {
		struct sb *sb;

		if (!oka && !okb) {
			seterr(err, errlen,
			       "%s: not an endless tape (pass --format to create one)",
			       t->path);
			return -1;
		}
		sb = (okb && (!oka || b.gen > a.gen)) ? &b : &a;
		if (cfg->capacity && cfg->capacity != sb->capacity) {
			seterr(err, errlen,
			       "%s: formatted for %llu bytes, --size asks for %llu",
			       t->path, (unsigned long long)sb->capacity,
			       (unsigned long long)cfg->capacity);
			return -1;
		}
		if (cfg->step && cfg->step != sb->step) {
			seterr(err, errlen,
			       "%s: formatted with step %llu, refusing step %llu",
			       t->path, (unsigned long long)sb->step,
			       (unsigned long long)cfg->step);
			return -1;
		}
		t->capacity = sb->capacity;
		t->step = sb->step;
		t->head = sb->head;
		t->last_commit = sb->head;
		t->sb_gen = sb->gen;
		have_sb = true;
	}

	if (!have_sb)
		return -1;

	t->commit_bytes = cfg->commit ? cfg->commit : TAPE_DEFAULT_COMMIT;
	if (t->commit_bytes > t->capacity)
		t->commit_bytes = t->capacity;
	t->nslots = (uint32_t)(t->capacity / t->step + 2);
	/* First sample is the next step boundary: an entry must describe data
	 * written from that position on, never retroactively relabel old bytes. */
	t->next_sample = (t->head / t->step + 1) * t->step;

	if (fstat(t->fd, &st) < 0) {
		seterr(err, errlen, "%s: %s", t->path, strerror(errno));
		return -1;
	}
	if (st.st_size < (off_t)(TAPE_DATA_OFF + t->capacity)) {
		seterr(err, errlen, "%s: backing file is shorter than its superblock",
		       t->path);
		return -1;
	}
	if (idx_open(t, cfg->format, err, errlen) < 0)
		return -1;

	return 0;
}

void tape_close(struct tape *t)
{
	if (t->fd >= 0) {
		pthread_mutex_lock(&t->lock);
		commit_locked(t);
		pthread_mutex_unlock(&t->lock);
	}
	if (t->fd >= 0)
		close(t->fd);
	if (t->ifd >= 0)
		close(t->ifd);
	free(t->idx);
	free(t->path);
	free(t->ipath);
	pthread_mutex_destroy(&t->lock);
	pthread_cond_destroy(&t->cond);
	memset(t, 0, sizeof *t);
	t->fd = -1;
	t->ifd = -1;
}

ssize_t tape_write(struct tape *t, const void *buf, size_t len)
{
	const char *p = buf;
	size_t done = 0;
	uint64_t ts = now_ns();
	ssize_t rc = 0;

	if (!len)
		return 0;

	pthread_mutex_lock(&t->lock);
	while (done < len) {
		size_t off = (size_t)((t->head + done) % t->capacity);
		size_t n = len - done;

		if (n > t->capacity - off)
			n = (size_t)(t->capacity - off);
		if (pwrite_full(t->fd, p + done, n,
				(off_t)(TAPE_DATA_OFF + off)) < 0) {
			rc = -errno;
			break;
		}
		done += n;
	}
	t->head += done;
	if (done) {
		if (t->head == done)
			idx_put_locked(t, 0, ts); /* first byte of a fresh tape */
		idx_sample_locked(t, ts);
	}
	if (t->head - t->last_commit >= t->commit_bytes) {
		int cr = commit_locked(t);

		if (!rc && cr && done == len)
			rc = cr;
	}
	pthread_cond_broadcast(&t->cond);
	pthread_mutex_unlock(&t->lock);

	if (rc)
		return rc;
	return (ssize_t)done;
}

ssize_t tape_read(struct tape *t, uint64_t *pos, void *buf, size_t len, bool nonblock)
{
	size_t n, n1;
	uint64_t tail;

	if (!len)
		return 0;

	pthread_mutex_lock(&t->lock);
	for (;;) {
		tail = t->head > t->capacity ? t->head - t->capacity : 0;
		if (*pos < tail) {
			t->dropped += tail - *pos;
			*pos = tail;
		}
		if (*pos < t->head)
			break;
		if (t->shutdown || (t->stop && *t->stop)) {
			pthread_mutex_unlock(&t->lock);
			return -ESHUTDOWN;
		}
		if (nonblock) {
			pthread_mutex_unlock(&t->lock);
			return -EAGAIN;
		}
		{
			struct timespec ts;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 100000000;
			if (ts.tv_nsec >= 1000000000) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&t->cond, &t->lock, &ts);
		}
	}
	n = len;
	if (n > t->head - *pos)
		n = (size_t)(t->head - *pos);
	n1 = (size_t)(t->capacity - (*pos % t->capacity));
	if (n1 > n)
		n1 = n;
	if (pread_full(t->fd, buf, n1, (off_t)(TAPE_DATA_OFF + *pos % t->capacity)) < 0 ||
	    pread_full(t->fd, (char *)buf + n1, n - n1,
		       (off_t)TAPE_DATA_OFF) < 0) {
		pthread_mutex_unlock(&t->lock);
		return -EIO;
	}
	*pos += n;
	pthread_mutex_unlock(&t->lock);
	return (ssize_t)n;
}

uint64_t tape_seek_time(struct tape *t, int64_t secs_back)
{
	uint64_t target, tail, head, best = 0;
	bool found = false;
	uint32_t i;

	pthread_mutex_lock(&t->lock);
	head = t->head;
	tail = head > t->capacity ? head - t->capacity : 0;
	target = now_ns();
	if (secs_back > 0) {
		uint64_t back = (uint64_t)secs_back * 1000000000ull;

		target = target > back ? target - back : 0;
	}
	for (i = 0; i < t->nslots; i++) {
		struct tape_idx *e = &t->idx[i];

		if (!e->ts_ns || e->abs > head || e->abs < tail ||
		    e->abs % t->step)
			continue;
		if (e->ts_ns <= target && (!found || e->abs >= best)) {
			best = e->abs;
			found = true;
		}
	}
	pthread_mutex_unlock(&t->lock);

	return found ? best : tail;
}

uint64_t tape_head(struct tape *t)
{
	uint64_t v;

	pthread_mutex_lock(&t->lock);
	v = t->head;
	pthread_mutex_unlock(&t->lock);
	return v;
}

uint64_t tape_tail(struct tape *t)
{
	uint64_t v;

	pthread_mutex_lock(&t->lock);
	v = t->head > t->capacity ? t->head - t->capacity : 0;
	pthread_mutex_unlock(&t->lock);
	return v;
}

uint64_t tape_used(struct tape *t)
{
	uint64_t v;

	pthread_mutex_lock(&t->lock);
	v = t->head - (t->head > t->capacity ? t->head - t->capacity : 0);
	pthread_mutex_unlock(&t->lock);
	return v;
}

int tape_sync(struct tape *t)
{
	int rc;

	pthread_mutex_lock(&t->lock);
	rc = commit_locked(t);
	pthread_mutex_unlock(&t->lock);
	return rc;
}

void tape_shutdown(struct tape *t)
{
	pthread_mutex_lock(&t->lock);
	t->shutdown = true;
	pthread_cond_broadcast(&t->cond);
	pthread_mutex_unlock(&t->lock);
}

int tape_status(struct tape *t, char *out, size_t outlen)
{
	uint64_t head, tail, dropped, oldest_abs = 0, newest_abs = 0;
	uint64_t oldest_ts = 0, newest_ts = 0;
	bool have = false;
	uint32_t i;
	int n;

	pthread_mutex_lock(&t->lock);
	head = t->head;
	tail = head > t->capacity ? head - t->capacity : 0;
	dropped = t->dropped;
	for (i = 0; i < t->nslots; i++) {
		struct tape_idx *e = &t->idx[i];

		if (!e->ts_ns || e->abs > head || e->abs < tail ||
		    e->abs % t->step)
			continue;
		if (!have || e->abs < oldest_abs) {
			oldest_abs = e->abs;
			oldest_ts = e->ts_ns;
		}
		if (!have || e->abs >= newest_abs) {
			newest_abs = e->abs;
			newest_ts = e->ts_ns;
		}
		have = true;
	}
	n = snprintf(out, outlen,
		     "capacity=%llu\n"
		     "used=%llu\n"
		     "head=%llu\n"
		     "tail=%llu\n"
		     "wrapped=%d\n"
		     "bytes_written=%llu\n"
		     "dropped_bytes=%llu\n"
		     "oldest_ts_ns=%llu\n"
		     "newest_ts_ns=%llu\n"
		     "step=%llu\n"
		     "commit_bytes=%llu\n"
		     "backing=%s\n",
		     (unsigned long long)t->capacity,
		     (unsigned long long)(head - tail),
		     (unsigned long long)head, (unsigned long long)tail,
		     head > t->capacity, (unsigned long long)head,
		     (unsigned long long)dropped,
		     (unsigned long long)oldest_ts, (unsigned long long)newest_ts,
		     (unsigned long long)t->step,
		     (unsigned long long)t->commit_bytes, t->path);
	pthread_mutex_unlock(&t->lock);
	if (n < 0 || (size_t)n >= outlen)
		return -1;
	return n;
}
