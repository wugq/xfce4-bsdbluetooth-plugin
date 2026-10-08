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
 * bsdbt -- Bluetooth from the command line: adapters, connections and
 * devices in range (as an ordinary user), and pairing, removing and
 * disconnecting devices through bsdbt-helper (with pkexec).
 */

#include <sys/param.h>
#include <sys/wait.h>

#include <bluetooth.h>
#include <err.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bt.h"

#ifndef BSDBT_HELPER
#define BSDBT_HELPER	"/usr/local/libexec/bsdbt-helper"
#endif

/* bsdbt-helper's exit status for "the device has no HID service" */
#define HELPER_NOHID	3

extern char **environ;

static void
usage(void)
{
	fprintf(stderr,
	    "usage: bsdbt [-a adapter] [list]\n"
	    "       bsdbt [-a adapter] [-t seconds] scan\n"
	    "       bsdbt [-a adapter] [-k | -p pin] pair address\n"
	    "       bsdbt remove address\n"
	    "       bsdbt disconnect address\n"
	    "       bsdbt -a device power on|off\n");
	exit(2);
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

/* The adapter given with -a, or the first one that is up. */
static void
pick_adapter(const char *node, struct bsdbt_adapter *a)
{
	struct bsdbt_adapter *ap;
	int i, n;

	if (node != NULL) {
		if (bsdbt_adapter(node, a) < 0)
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
		*a = ap[i];
		free(ap);
	}
	if (!BSDBT_ADAPTER_UP(a))
		errx(1, "%s is down", a->node);
}

/*
 * Run bsdbt-helper with `args' (NULL-terminated), through pkexec unless
 * we are root.  BSDBT_HELPER in the environment overrides its path (for
 * trying a build before installing it).  Returns its exit status.
 */
static int
helper(const char **args)
{
	const char *argv[8], *path;
	pid_t pid;
	int i, k, status;

	path = getenv("BSDBT_HELPER");
	if (path == NULL)
		path = BSDBT_HELPER;
	k = 0;
	if (geteuid() != 0)
		argv[k++] = "pkexec";
	argv[k++] = path;
	for (i = 0; args[i] != NULL && k < (int)nitems(argv) - 1; i++)
		argv[k++] = args[i];
	argv[k] = NULL;
	fflush(stdout);
	errno = posix_spawnp(&pid, argv[0], NULL, NULL, (char **)argv,
	    environ);
	if (errno != 0)
		err(1, "%s", argv[0]);
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			err(1, "waitpid");
	if (!WIFEXITED(status))
		errx(1, "%s: killed", path);
	return (WEXITSTATUS(status));
}

static void
parse_addr(const char *s, bdaddr_t *ba)
{
	if (!bt_aton(s, ba) || bdaddr_any(ba))
		errx(2, "%s: not a Bluetooth address", s);
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

/* The devices set up with bsdbt-helper. */
static void
list_known(void)
{
	struct bsdbt_known *k;
	struct bsdbt_conn c;
	char addr[32], node[HCI_DEVNAME_SIZE];
	int i, n;

	n = bsdbt_known(&k);
	if (n < 0) {
		warn("%s", BSDBT_DEVICES);
		return;
	}
	if (n == 0)
		return;
	printf("devices\n");
	for (i = 0; i < n; i++)
		printf("\t%s  %-10s %-6s %-10s %s\n",
		    bt_ntoa(&k[i].bdaddr, addr),
		    k[i].paired ? "paired" : "not paired",
		    k[i].hid ? "input" : "",
		    bsdbt_find_connection(&k[i].bdaddr, node, &c) == 1 ?
		    "connected" : "", k[i].name);
	free(k);
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
	} else {
		n = bsdbt_adapters(&ap);
		if (n <= 0)
			no_adapter(n < 0 ? errno : 0);
		for (i = 0; i < n; i++)
			list_adapter(&ap[i]);
		free(ap);
	}
	list_known();
}

static void
scan(const char *node, int seconds)
{
	struct bsdbt_adapter a;
	struct bsdbt_device *d;
	char addr[32], name[BSDBT_NAME_SIZE];
	int i, n;

	pick_adapter(node, &a);
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

/*
 * Pair, then hand the device to bthidd if it is a keyboard, mouse, ...
 * A keyboard needs a PIN typed on it (-k makes one up); most other
 * devices have a fixed one, usually 0000.
 */
static int
pair(const char *node, const char *addr, const char *pin, int keyboard)
{
	struct bsdbt_adapter a;
	struct bsdbt_device d;
	bdaddr_t ba;
	char name[BSDBT_NAME_SIZE], pinbuf[8];
	const char *args[6];
	int rv;

	parse_addr(addr, &ba);
	pick_adapter(node, &a);
	if (bsdbt_neighbor(a.node, &ba, &d) != 1 ||
	    bsdbt_remote_name(a.node, &ba, &d, name, sizeof(name)) < 0) {
		if (bsdbt_remote_name(a.node, &ba, NULL, name,
		    sizeof(name)) < 0) {
			if (errno == EHOSTDOWN)
				errx(5, "%s does not answer; is it on and in "
				    "pairing mode?", addr);
			err(1, "%s", addr);
		}
	}
	if (keyboard) {
		snprintf(pinbuf, sizeof(pinbuf), "%06u",
		    arc4random_uniform(1000000));
		pin = pinbuf;
		printf("Type %s on the keyboard, then press Enter.\n", pin);
	} else if (pin == NULL)
		pin = "0000";
	printf("Pairing with %s (%s)...\n", name, addr);

	args[0] = "pair";
	args[1] = addr;
	args[2] = pin;
	args[3] = name[0] != '\0' ? name : NULL;
	args[4] = NULL;
	rv = helper(args);
	if (rv != 0)
		return (rv);
	printf("Paired.\n");

	args[0] = "hid";
	args[1] = addr;
	args[2] = NULL;
	rv = helper(args);
	if (rv == HELPER_NOHID) {
		printf("Not an input device; nothing more to set up.\n");
		return (0);
	}
	if (rv == 0)
		printf("Added as an input device; bthidd connects to it.\n");
	return (rv);
}

static int
simple(const char *cmd, const char *addr)
{
	const char *args[3];
	bdaddr_t ba;

	parse_addr(addr, &ba);
	args[0] = cmd;
	args[1] = addr;
	args[2] = NULL;
	return (helper(args));
}

static int
power(const char *node, const char *onoff)
{
	const char *args[4];
	char dev[HCI_DEVNAME_SIZE];
	size_t len;

	if (node == NULL)
		errx(2, "power: give the device with -a (ubt0, ...)");
	if (strcmp(onoff, "on") != 0 && strcmp(onoff, "off") != 0)
		usage();
	/* The device of node ubt0hci is ubt0. */
	strlcpy(dev, node, sizeof(dev));
	len = strlen(dev);
	if (len > 3 && strcmp(dev + len - 3, "hci") == 0)
		dev[len - 3] = '\0';
	args[0] = "power";
	args[1] = dev;
	args[2] = onoff;
	args[3] = NULL;
	return (helper(args));
}

int
main(int argc, char *argv[])
{
	const char *node, *pin;
	char *end;
	long seconds;
	int ch, keyboard;

	node = NULL;
	pin = NULL;
	keyboard = 0;
	seconds = 10;
	while ((ch = getopt(argc, argv, "a:kp:t:")) != -1) {
		switch (ch) {
		case 'a':
			node = optarg;
			break;
		case 'k':
			keyboard = 1;
			break;
		case 'p':
			pin = optarg;
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
	else if (argc == 2 && strcmp(argv[0], "pair") == 0)
		return (pair(node, argv[1], pin, keyboard));
	else if (argc == 2 && strcmp(argv[0], "remove") == 0)
		return (simple("remove", argv[1]));
	else if (argc == 2 && strcmp(argv[0], "disconnect") == 0)
		return (simple("disconnect", argv[1]));
	else if (argc == 2 && strcmp(argv[0], "power") == 0)
		return (power(node, argv[1]));
	else
		usage();
	return (0);
}
