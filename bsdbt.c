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
 * bsdbt -- Bluetooth adapters, connections and devices in range, from the
 * command line.  Runs as an ordinary user.
 */

#include <sys/types.h>

#include <bluetooth.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bt.h"

static void
usage(void)
{
	fprintf(stderr,
	    "usage: bsdbt [-a adapter] [list]\n"
	    "       bsdbt [-a adapter] [-t seconds] scan\n");
	exit(1);
}

/* Explain the usual reasons for having no adapter. */
static void
no_adapter(int error)
{
	if (error == EPROTONOSUPPORT || error == EAFNOSUPPORT)
		errx(1, "no Bluetooth sockets: kldload ng_btsocket");
	if (error != 0)
		errc(1, error, "cannot list Bluetooth adapters");
	errx(1, "no Bluetooth adapter (is its firmware loaded and "
	    "\"service bluetooth start <device>\" run?)");
}

static const char *
scan_str(int scan)
{
	switch (scan) {
	case 0:
		return ("hidden, not connectable");
	case 1:
		return ("discoverable, not connectable");
	case 2:
		return ("hidden, connectable");
	case 3:
		return ("discoverable, connectable");
	}
	return ("unknown");
}

static void
list_adapter(const struct bsdbt_adapter *a)
{
	struct bsdbt_conn *c;
	char addr[32], name[BSDBT_NAME_SIZE];
	int i, n;

	printf("%s\n", a->node);
	printf("\taddress:\t%s\n", bt_ntoa(&a->bdaddr, addr));
	if (!BSDBT_ADAPTER_UP(a)) {
		printf("\tstate:\t\tdown (%#x)\n", a->state);
		return;
	}
	printf("\tstate:\t\tup\n");
	printf("\tname:\t\t%s\n", a->name);
	printf("\tclass:\t\t%s\n", bsdbt_class_str(a->class));
	printf("\tscan:\t\t%s\n", scan_str(a->scan));

	n = bsdbt_connections(a->node, &c);
	if (n < 0) {
		warn("%s: connections", a->node);
		return;
	}
	printf("\tconnections:\t%d\n", n);
	for (i = 0; i < n; i++) {
		if (bsdbt_remote_name(a->node, &c[i].bdaddr, NULL, name,
		    sizeof(name)) < 0)
			name[0] = '\0';
		printf("\t  %s  %s  %s%s\n", bt_ntoa(&c[i].bdaddr, addr),
		    c[i].link_type == NG_HCI_LINK_ACL ? "ACL" : "SCO",
		    c[i].encryption ? "encrypted  " : "", name);
	}
	free(c);
}

static void
list(const char *node)
{
	struct bsdbt_adapter a, *ap;
	int i, n;

	if (node != NULL) {
		if (bsdbt_adapter(node, &a) < 0)
			err(1, "%s", node);
		list_adapter(&a);
		return;
	}
	n = bsdbt_adapters(&ap);
	if (n <= 0)
		no_adapter(n < 0 ? errno : 0);
	for (i = 0; i < n; i++)
		list_adapter(&ap[i]);
	free(ap);
}

static void
scan(const char *node, int seconds)
{
	struct bsdbt_adapter a, *ap;
	struct bsdbt_device *d;
	char addr[32], name[BSDBT_NAME_SIZE];
	int i, n;

	/* Without -a, the first adapter that is up. */
	if (node != NULL) {
		if (bsdbt_adapter(node, &a) < 0)
			err(1, "%s", node);
	} else {
		n = bsdbt_adapters(&ap);
		if (n <= 0)
			no_adapter(n < 0 ? errno : 0);
		for (i = 0; i < n; i++)
			if (BSDBT_ADAPTER_UP(&ap[i]))
				break;
		if (i == n)
			errx(1, "no Bluetooth adapter is up");
		a = ap[i];
		free(ap);
	}
	if (!BSDBT_ADAPTER_UP(&a))
		errx(1, "%s is down", a.node);

	fprintf(stderr, "Scanning on %s for %d seconds...\n", a.node,
	    seconds);
	n = bsdbt_scan(a.node, seconds, &d);
	if (n < 0)
		err(1, "%s: inquiry", a.node);
	if (n == 0) {
		fprintf(stderr, "No devices found.  Is the device in "
		    "pairing mode?\n");
		return;
	}
	for (i = 0; i < n; i++) {
		if (bsdbt_remote_name(a.node, &d[i].bdaddr, &d[i], name,
		    sizeof(name)) < 0)
			strlcpy(name, "(no name)", sizeof(name));
		printf("%s  %-14s  %s\n", bt_ntoa(&d[i].bdaddr, addr),
		    bsdbt_class_str(d[i].class), name);
	}
	free(d);
}

int
main(int argc, char *argv[])
{
	const char *node;
	char *end;
	long seconds;
	int ch;

	node = NULL;
	seconds = 10;
	while ((ch = getopt(argc, argv, "a:t:")) != -1) {
		switch (ch) {
		case 'a':
			node = optarg;
			break;
		case 't':
			seconds = strtol(optarg, &end, 10);
			if (*end != '\0' || seconds < 1 || seconds > 60)
				errx(1, "-t: 1 to 60 seconds");
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 0 || (argc == 1 && strcmp(argv[0], "list") == 0))
		list(node);
	else if (argc == 1 && strcmp(argv[0], "scan") == 0)
		scan(node, (int)seconds);
	else
		usage();
	return (0);
}
