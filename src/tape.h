/* SPDX-License-Identifier: MIT */
/*
 * tape.h - fixed-capacity byte ring on a backing file.
 *
 * The stream has an absolute byte address space: byte N is the N-th byte
 * ever written and is never renumbered.  Only the last `capacity` bytes of
 * that address space live on disk; older bytes are overwritten in place.
 *
 * Layout of the backing file:
 *
 *   0                 4096  8192                         8192+capacity
 *   +-----------------+-----+-----------------------------+
 *   | superblock A    | B   | circular data region        |
 *   +-----------------+-----+-----------------------------+
 *
 * Absolute position p lives at file offset 8192 + (p % capacity).
 * A sidecar file `<backing>.idx` holds a ring of (abs, timestamp) samples
 * used for time-based positioning.
 */
#ifndef ENDLESS_TAPE_H
#define ENDLESS_TAPE_H

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define TAPE_MAGIC 0x454e444cu /* "ENDL" */
#define TAPE_VERSION 1u
#define TAPE_SECTOR 4096ull
#define TAPE_DATA_OFF 8192ull
#define TAPE_MIN_CAPACITY (1ull << 20)
#define TAPE_MAX_CAPACITY (1ull << 50)
#define TAPE_MAX_SLOTS (1u << 20)
#define TAPE_DEFAULT_STEP (1ull << 20)
#define TAPE_DEFAULT_COMMIT (4ull << 20)

struct tape_cfg {
	const char *path;
	uint64_t capacity; /* 0: use the formatted capacity */
	uint64_t step;     /* 0: use the formatted step (or default) */
	uint64_t commit;   /* 0: TAPE_DEFAULT_COMMIT */
	bool format;
	bool prealloc;
};

struct tape_idx {
	uint64_t abs;
	uint64_t ts_ns;
};

struct tape {
	int fd;
	int ifd;
	char *path;
	char *ipath;
	uint64_t capacity;
	uint64_t step;
	uint64_t commit_bytes;
	uint64_t head;        /* absolute stream position of the write end */
	uint64_t last_commit; /* head value covered by the durable superblock */
	uint64_t dropped;     /* bytes readers skipped because they were overwritten */
	uint64_t next_sample; /* next absolute index sample position */
	uint64_t sb_gen;
	uint32_t nslots;
	struct tape_idx *idx;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	volatile sig_atomic_t *stop; /* set from a signal handler to break waits */
	bool shutdown;
};

/* Open (and optionally create/format) a tape.  Returns 0 or -1 with errno set
 * and a message in err. */
int tape_open(struct tape *t, const struct tape_cfg *cfg, char *err, size_t errlen);
/* Flush and close.  Safe to call on a partially opened tape. */
void tape_close(struct tape *t);

/* Append bytes.  Never blocks on readers.  Returns the byte count or -errno. */
ssize_t tape_write(struct tape *t, const void *buf, size_t len);

/* Read at *pos and advance it.  Bytes that were overwritten while the reader
 * was away are skipped (and counted in t->dropped).  With nonblock set and no
 * data available, returns -EAGAIN; otherwise blocks until data arrives. */
ssize_t tape_read(struct tape *t, uint64_t *pos, void *buf, size_t len, bool nonblock);

/* Absolute position of the byte `secs_back` seconds before now. */
uint64_t tape_seek_time(struct tape *t, int64_t secs_back);

uint64_t tape_head(struct tape *t);
uint64_t tape_tail(struct tape *t);
uint64_t tape_used(struct tape *t);

/* Make everything written so far durable. */
int tape_sync(struct tape *t);

/* Wake blocked readers and make them fail with -ESHUTDOWN. */
void tape_shutdown(struct tape *t);

/* Render key=value lines.  Returns the number of bytes written (excluding the
 * terminating NUL) or -1 if the buffer is too small. */
int tape_status(struct tape *t, char *out, size_t outlen);

#endif
