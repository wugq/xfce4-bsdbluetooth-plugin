/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 wugq <wugq.dev@gmail.com>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * conf.h -- edit the configuration files of hcsecd(8) and bthidd(8).
 *
 * Both are lists of "device { ... }" blocks, each with a "bdaddr" line.
 * These functions replace or remove the block of one device and leave
 * everything else in the file -- other devices, comments -- as it is.
 * Files are replaced atomically (written to a temporary file, renamed).
 */
#ifndef BSDBT_CONF_H
#define BSDBT_CONF_H

#include <sys/types.h>
#include <bluetooth.h>

/* Marks the blocks bsdbt wrote; a comment, so both daemons ignore it. */
#define CONF_MARK	"# managed by bsdbt"

struct conf_entry {
	bdaddr_t	bdaddr;
	int		managed;	/* has CONF_MARK */
	int		fixed_key;	/* hcsecd: "key 0x..." in the file */
	char		name[64];	/* "" when the block has none */
};

/*
 * Replace the block for `bdaddr' by `block', or add it at the end when
 * there is none; `block' NULL removes it.  A missing file is created with
 * `mode'.  Returns 1 when the file changed, 0 when not, -1 on error.
 */
int	conf_set_device(const char *path, const bdaddr_t *bdaddr,
	    const char *block, mode_t mode);

/*
 * Replace `path' with `buf' through a temporary file and rename(2),
 * keeping the owner and mode of the old file (or giving it `mode').
 */
int	conf_write(const char *path, const char *buf, size_t len, mode_t mode);

/* The device blocks of a file; *ep is malloc'ed.  A missing file has none. */
int	conf_devices(const char *path, struct conf_entry **ep);

/*
 * Remove the lines starting with `bdaddr' from a file with one device per
 * line (hcsecd.keys, bthidd.hids).  Returns as conf_set_device().
 */
int	conf_remove_line(const char *path, const bdaddr_t *bdaddr);

/* Whether a line-per-device file has a line for `bdaddr'. */
int	conf_has_line(const char *path, const bdaddr_t *bdaddr);

/*
 * Copy `name' so that it can go between double quotes in either file:
 * no quotes, backslashes or control characters.  Empty if nothing is left.
 */
void	conf_clean_name(char *dst, const char *name, size_t len);

#endif /* BSDBT_CONF_H */
