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
 */

/*
 * The files are read the way the daemons' lexers read them: "#" starts
 * a comment up to the end of the line, and a string runs from a double
 * quote to the last double quote on the same line (\".+\").
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <bluetooth.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "conf.h"

struct span {
	size_t		start, end;	/* [start, end) in the file */
	struct conf_entry e;
	int		has_bdaddr;
};

/* Whole file in memory, NUL-terminated.  A missing file is empty. */
static int
read_file(const char *path, char **bufp, size_t *lenp)
{
	struct stat st;
	char *buf;
	ssize_t n;
	int fd, saved;

	*bufp = NULL;
	*lenp = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno != ENOENT)
			return (-1);
		*bufp = strdup("");
		return (*bufp == NULL ? -1 : 0);
	}
	if (fstat(fd, &st) < 0)
		goto bad;
	if (st.st_size > 4 * 1024 * 1024) {
		errno = EFBIG;
		goto bad;
	}
	buf = malloc(st.st_size + 1);
	if (buf == NULL)
		goto bad;
	n = read(fd, buf, st.st_size);
	if (n < 0) {
		saved = errno;
		free(buf);
		errno = saved;
		goto bad;
	}
	buf[n] = '\0';
	close(fd);
	*bufp = buf;
	*lenp = n;
	return (0);
bad:
	saved = errno;
	close(fd);
	errno = saved;
	return (-1);
}

int
conf_write(const char *path, const char *buf, size_t len, mode_t mode)
{
	char tmp[PATH_MAX];
	struct stat st;
	uid_t uid;
	gid_t gid;
	ssize_t n;
	size_t off;
	int fd, saved;

	uid = (uid_t)-1;	/* a new file: ours */
	gid = (gid_t)-1;
	if (stat(path, &st) == 0) {
		mode = st.st_mode & 07777;
		uid = st.st_uid;
		gid = st.st_gid;
	}
	if (snprintf(tmp, sizeof(tmp), "%s.bsdbt.XXXXXX", path) >=
	    (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	fd = mkstemp(tmp);
	if (fd < 0)
		return (-1);
	for (off = 0; off < len; off += n) {
		n = write(fd, buf + off, len - off);
		if (n < 0)
			goto bad;
	}
	if (fchown(fd, uid, gid) < 0 || fchmod(fd, mode) < 0 ||
	    fsync(fd) < 0)
		goto bad;
	if (close(fd) < 0) {
		fd = -1;
		goto bad;
	}
	if (rename(tmp, path) < 0) {
		saved = errno;
		unlink(tmp);
		errno = saved;
		return (-1);
	}
	return (0);
bad:
	saved = errno;
	if (fd >= 0)
		close(fd);
	unlink(tmp);
	errno = saved;
	return (-1);
}

/* End of the string that starts with the quote at buf[i], or 0. */
static size_t
string_end(const char *buf, size_t len, size_t i)
{
	size_t j, last;

	last = 0;
	for (j = i + 1; j < len && buf[j] != '\n'; j++)
		if (buf[j] == '"')
			last = j;
	return (last > i + 1 ? last + 1 : 0);
}

/*
 * Find the "device { ... }" blocks.  A block's span starts at the
 * beginning of the line of "device" and ends after the newline that
 * follows its closing brace, so that removing it leaves no gap.
 */
static int
scan(const char *buf, size_t len, struct span **sp)
{
	struct span *s, *p, cur;
	size_t i, j, k, end;
	char word[32], addr[32];
	int n, depth, inblock, want_name;

	s = NULL;
	n = depth = inblock = want_name = 0;
	memset(&cur, 0, sizeof(cur));
	for (i = 0; i < len; ) {
		if (buf[i] == '#') {
			if (inblock && strncmp(buf + i, CONF_MARK,
			    strlen(CONF_MARK)) == 0)
				cur.e.managed = 1;
			while (i < len && buf[i] != '\n')
				i++;
			continue;
		}
		if (buf[i] == '"') {
			end = string_end(buf, len, i);
			if (end == 0) {
				i++;
				continue;
			}
			if (inblock && depth == 1 && want_name) {
				k = MIN(end - i - 2, sizeof(cur.e.name) - 1);
				memcpy(cur.e.name, buf + i + 1, k);
				cur.e.name[k] = '\0';
			}
			want_name = 0;
			i = end;
			continue;
		}
		if (buf[i] == '{') {
			depth++;
			i++;
			continue;
		}
		if (buf[i] == '}') {
			if (depth > 0)
				depth--;
			i++;
			if (depth == 0 && inblock) {
				/* Take the rest of the line if it is blank. */
				for (j = i; j < len && (buf[j] == ' ' ||
				    buf[j] == '\t' || buf[j] == ';'); j++)
					;
				if (j == len || buf[j] == '\n')
					i = j < len ? j + 1 : j;
				cur.end = i;
				p = reallocarray(s, n + 1, sizeof(*s));
				if (p == NULL) {
					free(s);
					return (-1);
				}
				s = p;
				s[n++] = cur;
				inblock = 0;
			}
			continue;
		}
		if (isalpha((unsigned char)buf[i]) || buf[i] == '_') {
			for (j = i, k = 0; j < len &&
			    (isalnum((unsigned char)buf[j]) || buf[j] == '_');
			    j++)
				if (k < sizeof(word) - 1)
					word[k++] = buf[j];
			word[k] = '\0';
			if (depth == 0 && strcmp(word, "device") == 0) {
				memset(&cur, 0, sizeof(cur));
				inblock = 1;
				for (k = i; k > 0 && (buf[k - 1] == ' ' ||
				    buf[k - 1] == '\t'); k--)
					;
				cur.start = (k == 0 || buf[k - 1] == '\n') ?
				    k : i;
			} else if (inblock && depth == 1 &&
			    strcmp(word, "bdaddr") == 0) {
				while (j < len && (buf[j] == ' ' ||
				    buf[j] == '\t'))
					j++;
				for (k = 0; j < len && k < sizeof(addr) - 1 &&
				    (isxdigit((unsigned char)buf[j]) ||
				    buf[j] == ':'); j++)
					addr[k++] = buf[j];
				addr[k] = '\0';
				if (bt_aton(addr, &cur.e.bdaddr))
					cur.has_bdaddr = 1;
			} else if (inblock && depth == 1 &&
			    strcmp(word, "key") == 0) {
				while (j < len && (buf[j] == ' ' ||
				    buf[j] == '\t'))
					j++;
				if (len - j > 2 && buf[j] == '0' &&
				    buf[j + 1] == 'x')
					cur.e.fixed_key = 1;
			} else if (inblock && depth == 1)
				want_name = strcmp(word, "name") == 0;
			i = j;
			continue;
		}
		i++;
	}
	*sp = s;
	return (n);
}

int
conf_set_device(const char *path, const bdaddr_t *bdaddr, const char *block,
    mode_t mode)
{
	struct span *s;
	char *buf, *out;
	size_t len, olen, pos, blen, start;
	int i, n, placed, rv;

	if (read_file(path, &buf, &len) < 0)
		return (-1);
	n = scan(buf, len, &s);
	if (n < 0) {
		free(buf);
		return (-1);
	}
	blen = block != NULL ? strlen(block) : 0;
	/* Old file, the new block and a blank line around it at most. */
	out = malloc(len + blen + 3);
	if (out == NULL) {
		free(s);
		free(buf);
		return (-1);
	}

	/* Copy the file, dropping blocks for bdaddr; the first is replaced. */
	olen = pos = 0;
	placed = 0;
	for (i = 0; i < n; i++) {
		if (!s[i].has_bdaddr || !bdaddr_same(&s[i].e.bdaddr, bdaddr))
			continue;
		start = s[i].start;
		/*
		 * Removing a block between blank lines: take the one before
		 * it too.
		 */
		if ((placed || block == NULL) && start >= pos + 2 &&
		    buf[start - 1] == '\n' && buf[start - 2] == '\n' &&
		    s[i].end < len && buf[s[i].end] == '\n')
			start--;
		memcpy(out + olen, buf + pos, start - pos);
		olen += start - pos;
		if (!placed && block != NULL) {
			memcpy(out + olen, block, blen);
			olen += blen;
		}
		placed = 1;
		pos = s[i].end;
	}
	memcpy(out + olen, buf + pos, len - pos);
	olen += len - pos;
	if (!placed && block != NULL) {
		/* Separate it from what comes before by one blank line. */
		if (olen > 0 && out[olen - 1] != '\n')
			out[olen++] = '\n';
		if (olen == 1 || (olen > 1 && out[olen - 2] != '\n'))
			out[olen++] = '\n';
		memcpy(out + olen, block, blen);
		olen += blen;
	}

	rv = 0;
	if (olen != len || memcmp(out, buf, len) != 0)
		rv = conf_write(path, out, olen, mode) < 0 ? -1 : 1;
	free(out);
	free(s);
	free(buf);
	return (rv);
}

int
conf_devices(const char *path, struct conf_entry **ep)
{
	struct conf_entry *e;
	struct span *s;
	char *buf;
	size_t len;
	int i, k, n;

	*ep = NULL;
	if (read_file(path, &buf, &len) < 0)
		return (-1);
	n = scan(buf, len, &s);
	free(buf);
	if (n <= 0)
		return (n);
	e = calloc(n, sizeof(*e));
	if (e == NULL) {
		free(s);
		return (-1);
	}
	for (i = k = 0; i < n; i++)
		if (s[i].has_bdaddr)
			e[k++] = s[i].e;
	free(s);
	*ep = e;
	return (k);
}

/* The address at the start of a line, as both daemons read it. */
static int
line_bdaddr(const char *line, bdaddr_t *ba)
{
	char addr[32];
	size_t k;

	while (*line == ' ' || *line == '\t')
		line++;
	for (k = 0; k < sizeof(addr) - 1 && line[k] != '\0' &&
	    strchr(" \t\r\n", line[k]) == NULL; k++)
		addr[k] = line[k];
	addr[k] = '\0';
	return (bt_aton(addr, ba));
}

int
conf_remove_line(const char *path, const bdaddr_t *bdaddr)
{
	bdaddr_t ba;
	char *buf, *out, *line, *nl;
	size_t len, olen, l;
	int rv;

	if (read_file(path, &buf, &len) < 0)
		return (-1);
	out = malloc(len + 1);
	if (out == NULL) {
		free(buf);
		return (-1);
	}
	olen = 0;
	for (line = buf; *line != '\0'; line += l) {
		nl = strchr(line, '\n');
		l = nl != NULL ? (size_t)(nl - line) + 1 : strlen(line);
		if (line_bdaddr(line, &ba) && bdaddr_same(&ba, bdaddr))
			continue;
		memcpy(out + olen, line, l);
		olen += l;
	}
	rv = 0;
	if (olen != len)
		rv = conf_write(path, out, olen, 0600) < 0 ? -1 : 1;
	free(out);
	free(buf);
	return (rv);
}

int
conf_has_line(const char *path, const bdaddr_t *bdaddr)
{
	bdaddr_t ba;
	char *buf, *line, *nl;
	size_t len;
	int found;

	if (read_file(path, &buf, &len) < 0)
		return (-1);
	found = 0;
	for (line = buf; line != NULL && *line != '\0' && !found;
	    line = nl != NULL ? nl + 1 : NULL) {
		nl = strchr(line, '\n');
		if (line_bdaddr(line, &ba) && bdaddr_same(&ba, bdaddr))
			found = 1;
	}
	free(buf);
	return (found);
}

void
conf_clean_name(char *dst, const char *name, size_t len)
{
	size_t k;
	unsigned char c;

	if (len == 0)
		return;
	while (*name == ' ')
		name++;
	for (k = 0; *name != '\0' && k < len - 1; name++) {
		c = *name;
		if (c < 0x20 || c == 0x7f || c == '"' || c == '\\')
			continue;
		dst[k++] = c;
	}
	/* Do not leave half a UTF-8 sequence when the name was cut. */
	if (*name != '\0')
		while (k > 0 && ((unsigned char)dst[k - 1] & 0xc0) == 0x80)
			k--;
	if (k > 0 && *name != '\0' && ((unsigned char)dst[k - 1] & 0xc0) ==
	    0xc0)
		k--;
	while (k > 0 && dst[k - 1] == ' ')
		k--;
	dst[k] = '\0';
}
