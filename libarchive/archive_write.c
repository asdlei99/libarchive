/*-
 * Copyright (c) 2003-2010 Tim Kientzle
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "archive_platform.h"
__FBSDID("$FreeBSD: head/lib/libarchive/archive_write.c 201099 2009-12-28 03:03:00Z kientzle $");

/*
 * This file contains the "essential" portions of the write API, that
 * is, stuff that will essentially always be used by any client that
 * actually needs to write a archive.  Optional pieces have been, as
 * far as possible, separated out into separate files to reduce
 * needlessly bloating statically-linked clients.
 */

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "archive.h"
#include "archive_entry.h"
#include "archive_private.h"
#include "archive_write_private.h"

static struct archive_vtable *archive_write_vtable(void);

static int	_archive_filter_code(struct archive *, int);
static const char *_archive_filter_name(struct archive *, int);
static int64_t	_archive_filter_bytes(struct archive *, int);
static int	_archive_write_close(struct archive *);
static int	_archive_write_free(struct archive *);
static int	_archive_write_header(struct archive *, struct archive_entry *);
static int	_archive_write_finish_entry(struct archive *);
static ssize_t	_archive_write_data(struct archive *, const void *, size_t);

static struct archive_vtable *
archive_write_vtable(void)
{
	static struct archive_vtable av;
	static int inited = 0;

	if (!inited) {
		av.archive_close = _archive_write_close;
		av.archive_filter_bytes = _archive_filter_bytes;
		av.archive_filter_code = _archive_filter_code;
		av.archive_filter_name = _archive_filter_name;
		av.archive_free = _archive_write_free;
		av.archive_write_header = _archive_write_header;
		av.archive_write_finish_entry = _archive_write_finish_entry;
		av.archive_write_data = _archive_write_data;
	}
	return (&av);
}

/*
 * Allocate, initialize and return an archive object.
 */
struct archive *
archive_write_new(void)
{
	struct archive_write *a;
	unsigned char *nulls;

	a = (struct archive_write *)malloc(sizeof(*a));
	if (a == NULL)
		return (NULL);
	memset(a, 0, sizeof(*a));
	a->archive.magic = ARCHIVE_WRITE_MAGIC;
	a->archive.state = ARCHIVE_STATE_NEW;
	a->archive.vtable = archive_write_vtable();
	/*
	 * The value 10240 here matches the traditional tar default,
	 * but is otherwise arbitrary.
	 * TODO: Set the default block size from the format selected.
	 */
	a->bytes_per_block = 10240;
	a->bytes_in_last_block = -1;	/* Default */

	/* Initialize a block of nulls for padding purposes. */
	a->null_length = 1024;
	nulls = (unsigned char *)malloc(a->null_length);
	if (nulls == NULL) {
		free(a);
		return (NULL);
	}
	memset(nulls, 0, a->null_length);
	a->nulls = nulls;
	return (&a->archive);
}

/*
 * Set write options for the format. Returns 0 if successful.
 */
int
archive_write_set_format_options(struct archive *_a, const char *s)
{
	struct archive_write *a = (struct archive_write *)_a;
	char key[64], val[64];
	int len, r, ret = ARCHIVE_OK;

	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_NEW, "archive_write_set_format_options");
	archive_clear_error(&a->archive);

	if (s == NULL || *s == '\0')
		return (ARCHIVE_OK);
	if (a->format_options == NULL)
		/* This format does not support option. */
		return (ARCHIVE_OK);

	while ((len = __archive_parse_options(s, a->format_name,
	    sizeof(key), key, sizeof(val), val)) > 0) {
		if (val[0] == '\0')
			r = a->format_options(a, key, NULL);
		else
			r = a->format_options(a, key, val);
		if (r == ARCHIVE_FATAL)
			return (r);
		if (r < ARCHIVE_OK) { /* This key was not handled. */
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Unsupported option ``%s''", key);
			ret = ARCHIVE_WARN;
		}
		s += len;
	}
	if (len < 0) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Malformed options string.");
		return (ARCHIVE_WARN);
	}
	return (ret);
}

/*
 * Set write options for the compressor. Returns 0 if successful.
 */
int
archive_write_set_compressor_options(struct archive *_a, const char *s)
{
	struct archive_write *a = (struct archive_write *)_a;
	struct archive_write_filter *filter;
	char key[64], val[64];
	int len, r;
	int ret = ARCHIVE_OK;

	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_NEW, "archive_write_set_compressor_options");
	archive_clear_error(&a->archive);

	if (s == NULL || *s == '\0')
		return (ARCHIVE_OK);
	len = 0;
	for (filter = a->filter_first; filter != NULL; filter = filter->next_filter) {
		if (filter->options == NULL)
			/* This filter does not have any options */
			continue;
		while ((len = __archive_parse_options(s,
			    a->archive.compression_name,
			    sizeof(key), key, sizeof(val), val)) > 0) {
			if (val[0] == '\0')
				r = filter->options(filter, key, NULL);
			else
				r = filter->options(filter, key, val);
			if (r == ARCHIVE_FATAL)
				return (r);
			if (r < ARCHIVE_OK) {
				archive_set_error(&a->archive,
				    ARCHIVE_ERRNO_MISC,
				    "Unsupported option ``%s''", key);
				ret = ARCHIVE_WARN;
			}
			s += len;
		}
	}
	if (len < 0) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Illegal format options.");
		return (ARCHIVE_WARN);
	}
	return (ret);
}

/*
 * Set write options for the format and the compressor. Returns 0 if successful.
 */
int
archive_write_set_options(struct archive *_a, const char *s)
{
	int r1, r2;

	r1 = archive_write_set_format_options(_a, s);
	if (r1 < ARCHIVE_WARN)
		return (r1);
	r2 = archive_write_set_compressor_options(_a, s);
	if (r2 < ARCHIVE_WARN)
		return (r2);
	if (r1 == ARCHIVE_WARN && r2 == ARCHIVE_WARN)
		return (ARCHIVE_WARN);
	return (ARCHIVE_OK);
}

/*
 * Set the block size.  Returns 0 if successful.
 */
int
archive_write_set_bytes_per_block(struct archive *_a, int bytes_per_block)
{
	struct archive_write *a = (struct archive_write *)_a;
	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_NEW, "archive_write_set_bytes_per_block");
	a->bytes_per_block = bytes_per_block;
	return (ARCHIVE_OK);
}

/*
 * Get the current block size.  -1 if it has never been set.
 */
int
archive_write_get_bytes_per_block(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_write_get_bytes_per_block");
	return (a->bytes_per_block);
}

/*
 * Set the size for the last block.
 * Returns 0 if successful.
 */
int
archive_write_set_bytes_in_last_block(struct archive *_a, int bytes)
{
	struct archive_write *a = (struct archive_write *)_a;
	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_write_set_bytes_in_last_block");
	a->bytes_in_last_block = bytes;
	return (ARCHIVE_OK);
}

/*
 * Return the value set above.  -1 indicates it has not been set.
 */
int
archive_write_get_bytes_in_last_block(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_write_get_bytes_in_last_block");
	return (a->bytes_in_last_block);
}


/*
 * dev/ino of a file to be rejected.  Used to prevent adding
 * an archive to itself recursively.
 */
int
archive_write_set_skip_file(struct archive *_a, dev_t d, ino_t i)
{
	struct archive_write *a = (struct archive_write *)_a;
	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_write_set_skip_file");
	a->skip_file_dev = d;
	a->skip_file_ino = i;
	return (ARCHIVE_OK);
}

/*
 * Allocate and return the next filter structure.
 */
struct archive_write_filter *
__archive_write_allocate_filter(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	struct archive_write_filter *f;
	__archive_check_magic(_a, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_NEW, "archive_write_allocate_filter");
	f = calloc(1, sizeof(*f));
	f->archive = _a;
	if (a->filter_first == NULL)
		a->filter_first = f;
	else
		a->filter_last->next_filter = f;
	a->filter_last = f;
	return f;
}

/*
 * Write data to a particular filter.
 */
int
__archive_write_filter(struct archive_write_filter *f,
    const void *buff, size_t length)
{
	int r;
	r = (f->write)(f, buff, length);
	f->bytes_written += length;
	return (r);
}

/*
 * Open a filter.
 */
int
__archive_write_open_filter(struct archive_write_filter *f)
{
	if (f->open == NULL)
		return (ARCHIVE_OK);
	return (f->open)(f);
}

/*
 * Close a filter.
 */
int
__archive_write_close_filter(struct archive_write_filter *f)
{
	if (f->close == NULL)
		return (ARCHIVE_OK);
	return (f->close)(f);
}

int
__archive_write_output(struct archive_write *a, const void *buff, size_t length)
{
	return (__archive_write_filter(a->filter_first, buff, length));
}

static int
archive_write_client_open(struct archive_write_filter *f)
{
	struct archive_write *a = (struct archive_write *)f->archive;
	if (a->client_opener == NULL)
		return (ARCHIVE_OK);
	return (a->client_opener(f->archive, f->data));
}

static int
archive_write_client_write(struct archive_write_filter *f,
    const void *_buff, size_t length)
{
	struct archive_write *a = (struct archive_write *)f->archive;
	const char *buff = _buff;

	while (length > 0) {
		ssize_t written
		    = (a->client_writer(f->archive, f->data, buff, length));
		if (written < 0)
			return ((int)written);
		if (written == 0)
			return (ARCHIVE_FATAL);
		buff += written;
		length -= written;
	}
	return (ARCHIVE_OK);
}

static int
archive_write_client_close(struct archive_write_filter *f)
{
	struct archive_write *a = (struct archive_write *)f->archive;
	if (a->client_closer == NULL)
		return (ARCHIVE_OK);
	return (a->client_closer(f->archive, f->data));
}

/*
 * Open the archive using the current settings.
 */
int
archive_write_open(struct archive *_a, void *client_data,
    archive_open_callback *opener, archive_write_callback *writer,
    archive_close_callback *closer)
{
	struct archive_write *a = (struct archive_write *)_a;
	struct archive_write_filter *client_filter;
	int ret;

	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_NEW, "archive_write_open");
	archive_clear_error(&a->archive);

	if (a->filter_first == NULL)
		archive_write_set_compression_none(_a);

	a->client_writer = writer;
	a->client_opener = opener;
	a->client_closer = closer;

	client_filter = __archive_write_allocate_filter(_a);
	client_filter->data = client_data;
	client_filter->open = archive_write_client_open;
	client_filter->write = archive_write_client_write;
	client_filter->close = archive_write_client_close;

	ret = __archive_write_open_filter(a->filter_first);

	a->archive.state = ARCHIVE_STATE_HEADER;

	if (a->format_init && ret == ARCHIVE_OK)
		ret = (a->format_init)(a);
	return (ret);
}

/*
 * Close out the archive.
 */
static int
_archive_write_close(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	int r = ARCHIVE_OK, r1 = ARCHIVE_OK;

	/*
	 * It's perfectly reasonable to call close() as part of
	 * routine cleanup, even after an error, so we're a little
	 * tolerant of being called in odd states.
	 */
	if (a->archive.state & ARCHIVE_STATE_FATAL)
		return (ARCHIVE_FATAL);
	archive_clear_error(&a->archive);
	if (a->archive.state & (ARCHIVE_STATE_NEW | ARCHIVE_STATE_CLOSED))
		return (ARCHIVE_OK);

	__archive_check_magic(&a->archive,
	    ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
	    "archive_write_close");

	/* Finish the last entry. */
	if (a->archive.state & ARCHIVE_STATE_DATA)
		r = ((a->format_finish_entry)(a));

	/* Finish off the archive. */
	/* TODO: Rename format_finish to format_close, have
	 * format closers invoke compression close. */
	if (a->format_finish != NULL) {
		r1 = (a->format_finish)(a);
		if (r1 < r)
			r = r1;
	}

	/* Finish the compression and close the stream. */
	r1 = __archive_write_close_filter(a->filter_first);
	if (r1 < r)
		r = r1;

	a->archive.state = ARCHIVE_STATE_CLOSED;
	return (r);
}

void
__archive_write_filters_free(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	int r = ARCHIVE_OK, r1;

	while (a->filter_first != NULL) {
		struct archive_write_filter *next
		    = a->filter_first->next_filter;
		if (a->filter_first->free != NULL) {
			r1 = (*a->filter_first->free)(a->filter_first);
			if (r > r1)
				r = r1;
		}
		free(a->filter_first);
		a->filter_first = next;
	}
	a->filter_last = NULL;
}

/*
 * Destroy the archive structure.
 *
 * Be careful: user might just call write_new and then write_free.
 * Don't assume we actually wrote anything or performed any non-trivial
 * initialization.
 */
static int
_archive_write_free(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	int r = ARCHIVE_OK, r1;

	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_write_free");
	if (a->archive.state != ARCHIVE_STATE_CLOSED
	    && a->archive.state != ARCHIVE_STATE_FATAL)
		r = archive_write_close(&a->archive);

	/* Release format resources. */
	/* TODO: Rename format_destroy to format_free */
	if (a->format_destroy != NULL) {
		r1 = (a->format_destroy)(a);
		if (r1 < r)
			r = r1;
	}

	__archive_write_filters_free(_a);

	/* Release various dynamic buffers. */
	free((void *)(uintptr_t)(const void *)a->nulls);
	archive_string_free(&a->archive.error_string);
	a->archive.magic = 0;
	free(a);
	return (r);
}

/*
 * Write the appropriate header.
 */
static int
_archive_write_header(struct archive *_a, struct archive_entry *entry)
{
	struct archive_write *a = (struct archive_write *)_a;
	int ret, r2;

	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_DATA | ARCHIVE_STATE_HEADER, "archive_write_header");
	archive_clear_error(&a->archive);

	/* In particular, "retry" and "fatal" get returned immediately. */
	ret = archive_write_finish_entry(&a->archive);
	if (ret == ARCHIVE_FATAL) {
		a->archive.state = ARCHIVE_STATE_FATAL;
		return (ARCHIVE_FATAL);
	}
	if (ret < ARCHIVE_OK && ret != ARCHIVE_WARN)
		return (ret);

	if (a->skip_file_dev != 0 &&
	    archive_entry_dev(entry) == a->skip_file_dev &&
	    a->skip_file_ino != 0 &&
	    archive_entry_ino64(entry) == a->skip_file_ino) {
		archive_set_error(&a->archive, 0,
		    "Can't add archive to itself");
		return (ARCHIVE_FAILED);
	}

	/* Format and write header. */
	r2 = ((a->format_write_header)(a, entry));
	if (r2 == ARCHIVE_FATAL) {
		a->archive.state = ARCHIVE_STATE_FATAL;
		return (ARCHIVE_FATAL);
	}
	if (r2 < ret)
		ret = r2;

	a->archive.state = ARCHIVE_STATE_DATA;
	return (ret);
}

static int
_archive_write_finish_entry(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	int ret = ARCHIVE_OK;

	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
	    "archive_write_finish_entry");
	if (a->archive.state & ARCHIVE_STATE_DATA)
		ret = (a->format_finish_entry)(a);
	a->archive.state = ARCHIVE_STATE_HEADER;
	return (ret);
}

/*
 * Note that the compressor is responsible for blocking.
 */
static ssize_t
_archive_write_data(struct archive *_a, const void *buff, size_t s)
{
	struct archive_write *a = (struct archive_write *)_a;
	__archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_DATA, "archive_write_data");
	archive_clear_error(&a->archive);
	return ((a->format_write_data)(a, buff, s));
}

static struct archive_write_filter *
filter_lookup(struct archive *_a, int n)
{
	struct archive_write *a = (struct archive_write *)_a;
	struct archive_write_filter *f = a->filter_first;
	if (n == -1)
		return a->filter_last;
	if (n < 0)
		return NULL;
	while (n > 0 && f != NULL) {
		f = f->next_filter;
		--n;
	}
	return f;
}

static int
_archive_filter_code(struct archive *_a, int n)
{
	struct archive_write_filter *f = filter_lookup(_a, n);
	return f == NULL ? -1 : f->code;
}

static const char *
_archive_filter_name(struct archive *_a, int n)
{
	struct archive_write_filter *f = filter_lookup(_a, n);
	return f == NULL ? NULL : f->name;
}

static int64_t
_archive_filter_bytes(struct archive *_a, int n)
{
	struct archive_write_filter *f = filter_lookup(_a, n);
	return f == NULL ? -1 : f->bytes_written;
}
