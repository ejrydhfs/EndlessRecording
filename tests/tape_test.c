/* SPDX-License-Identifier: MIT */
/* Unit tests for the ring core (no FUSE involved). */
#define _GNU_SOURCE
#include "../src/tape.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                           \
	do {                                                                  \
		if (!(cond)) {                                                \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
				#cond);                                       \
			failures++;                                           \
		}                                                             \
	} while (0)

static unsigned char pat(uint64_t i)
{
	return (unsigned char)(i * 31u + (i >> 9) + 7u);
}

static int read_verify(struct tape *t, uint64_t from, uint64_t n)
{
	unsigned char buf[65536];
	uint64_t pos = from, got = 0;

	while (got < n) {
		size_t want = n - got > sizeof buf ? sizeof buf : (size_t)(n - got);
		ssize_t r = tape_read(t, &pos, buf, want, true);

		if (r <= 0)
			return -1;
		for (size_t i = 0; i < (size_t)r; i++)
			if (buf[i] != pat(pos - (uint64_t)r + i))
				return -2;
		got += (uint64_t)r;
	}
	return 0;
}

int main(void)
{
	char dir[] = "/tmp/endless-test-XXXXXX";
	char path[512], err[512];
	struct tape t;
	struct tape_cfg cfg;
	unsigned char *buf;
	uint64_t total = 3ull << 20, off = 0, pos;
	const uint64_t cap = 1ull << 20;

	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(path, sizeof path, "%s/tape.bin", dir);

	memset(&cfg, 0, sizeof cfg);
	cfg.path = path;
	cfg.capacity = cap;
	cfg.step = 64u << 10;
	cfg.commit = 64u << 10;
	cfg.format = true;
	cfg.prealloc = true;

	CHECK(tape_open(&t, &cfg, err, sizeof err) == 0);
	CHECK(tape_head(&t) == 0);
	CHECK(tape_used(&t) == 0);
	CHECK(tape_seek_time(&t, 0) == 0);

	/* Ragged writes so records and the wrap point land off any boundary. */
	buf = malloc(1 << 20);
	for (;;) {
		size_t n = 1 + (size_t)((off * 2654435761u) % 70000);

		if (!buf || off >= total)
			break;
		if (n > total - off)
			n = (size_t)(total - off);
		for (size_t i = 0; i < n; i++)
			buf[i] = pat(off + i);
		CHECK(tape_write(&t, buf, n) == (ssize_t)n);
		off += n;
	}
	CHECK(tape_head(&t) == total);
	CHECK(tape_tail(&t) == total - cap);
	CHECK(tape_used(&t) == cap);

	/* The retained window is exactly the last `cap` bytes. */
	CHECK(read_verify(&t, tape_tail(&t), cap) == 0);

	/* A reader at the write end has nothing to read yet. */
	pos = tape_head(&t);
	CHECK(tape_read(&t, &pos, buf, 4096, true) == -EAGAIN);

	/* A reader left behind is dragged forward to the oldest byte. */
	pos = 0;
	CHECK(tape_read(&t, &pos, buf, 4096, true) == 4096);
	CHECK(pos == tape_tail(&t) + 4096);
	CHECK(t.dropped == tape_tail(&t));

	/* Time index: now -> the write end, an hour ago -> the oldest byte. */
	CHECK(tape_seek_time(&t, 0) == tape_head(&t));
	CHECK(tape_seek_time(&t, 3600) == tape_tail(&t));

	/* Close commits; reopen resumes at the same position. */
	tape_close(&t);
	cfg.format = false;
	CHECK(tape_open(&t, &cfg, err, sizeof err) == 0);
	CHECK(tape_head(&t) == total);
	CHECK(tape_used(&t) == cap);
	CHECK(read_verify(&t, tape_tail(&t), cap) == 0);

	/* status reports the geometry we asked for. */
	{
		char st[2048];

		CHECK(tape_status(&t, st, sizeof st) > 0);
		CHECK(strstr(st, "wrapped=1"));
		CHECK(strstr(st, "capacity=1048576"));
	}
	tape_close(&t);

	/* Re-formatting with a different size starts from zero. */
	cfg.format = true;
	cfg.capacity = 2ull << 20;
	CHECK(tape_open(&t, &cfg, err, sizeof err) == 0);
	CHECK(tape_head(&t) == 0);
	CHECK(tape_used(&t) == 0);
	{
		char st[2048];

		CHECK(tape_status(&t, st, sizeof st) > 0);
		CHECK(strstr(st, "oldest_ts_ns=0") != NULL);
	}
	tape_close(&t);

	free(buf);
	unlink(path);
	{
		char ipath[600];

		snprintf(ipath, sizeof ipath, "%s.idx", path);
		unlink(ipath);
	}
	rmdir(dir);

	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	else
		printf("tape_test: ok (capacity %llu, %llu bytes written, wrapped)\n",
		       (unsigned long long)cap, (unsigned long long)total);
	return failures ? 1 : 0;
}
