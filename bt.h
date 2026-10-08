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
 * bt.h -- what bsdbt and the panel plugin ask of the FreeBSD Bluetooth
 * stack, through libbluetooth(3) and the HCI raw socket ioctls.
 *
 * Everything here works as an ordinary user: the kernel lets an
 * unprivileged HCI socket read node state and send inquiry, remote name
 * request and most "read" commands.  The functions block (an inquiry
 * takes seconds); the panel plugin calls them from a thread.
 *
 * Functions returning int return -1 and set errno on error.
 */
#ifndef BSDBT_BT_H
#define BSDBT_BT_H

#include <sys/types.h>
#include <bluetooth.h>

#define BSDBT_NAME_SIZE	(NG_HCI_UNIT_NAME_SIZE + 1)

/*
 * Devices set up with bsdbt-helper, one per line: address, "paired" or
 * "-", "hid" or "-", name.  The helper writes it after every change; the
 * daemons' own files are not readable by everyone (hcsecd.conf has PINs).
 */
#ifndef BSDBT_DEVICES
#define BSDBT_DEVICES	"/var/db/bsdbt.devices"
#endif

struct bsdbt_adapter {
	char		node[HCI_DEVNAME_SIZE];	/* e.g. "ubt0hci" */
	bdaddr_t	bdaddr;
	uint32_t	state;		/* NG_HCI_UNIT_* */
	int		scan;		/* scan enable, -1 when not known */
	uint8_t		class[NG_HCI_CLASS_SIZE];
	char		name[BSDBT_NAME_SIZE];	/* local name, may be "" */
};

/* An adapter is usable when its node is attached and initialized. */
#define BSDBT_ADAPTER_UP(a) \
	(((a)->state & NG_HCI_UNIT_READY) == NG_HCI_UNIT_READY)

struct bsdbt_conn {
	bdaddr_t	bdaddr;
	uint16_t	handle;
	uint8_t		link_type;	/* NG_HCI_LINK_* */
	uint8_t		encryption;	/* 0: off */
	uint8_t		role;		/* 0: we are master */
	uint16_t	state;		/* NG_HCI_CON_* */
};

struct bsdbt_device {
	bdaddr_t	bdaddr;
	uint8_t		class[NG_HCI_CLASS_SIZE];
	uint16_t	clock_offset;
	uint8_t		pscan_rep_mode;
};

/*
 * All Bluetooth adapters (HCI nodes), whatever driver they use.
 * *ap is malloc'ed; returns the number of adapters.
 */
int	bsdbt_adapters(struct bsdbt_adapter **ap);

/* One adapter by node name ("ubt0hci"); "ubt0" works too. */
int	bsdbt_adapter(const char *node, struct bsdbt_adapter *a);

/* Open connections of an adapter.  *cp is malloc'ed. */
int	bsdbt_connections(const char *node, struct bsdbt_conn **cp);

/*
 * Inquiry for about `seconds': devices in range that are discoverable.
 * Each device appears once.  *dp is malloc'ed.
 */
int	bsdbt_scan(const char *node, int seconds, struct bsdbt_device **dp);

/*
 * Ask a device for its name (the device must be in range; takes up to a
 * few seconds).  `d' may come from bsdbt_scan(), which makes it faster;
 * otherwise pass NULL and the address.
 */
int	bsdbt_remote_name(const char *node, const bdaddr_t *bdaddr,
	    const struct bsdbt_device *d, char *name, size_t len);

/*
 * What the adapter remembers of a device from the last inquiry (clock
 * offset, page scan mode); 0 when it remembers nothing.
 */
int	bsdbt_neighbor(const char *node, const bdaddr_t *bdaddr,
	    struct bsdbt_device *d);

/*
 * The adapter (node, at least HCI_DEVNAME_SIZE bytes) and the ACL
 * connection to a device; 0 when not connected.
 */
int	bsdbt_find_connection(const bdaddr_t *bdaddr, char *node,
	    struct bsdbt_conn *c);

struct bsdbt_known {
	bdaddr_t	bdaddr;
	int		paired;		/* hcsecd has a link key */
	int		hid;		/* in bthidd.conf */
	char		name[BSDBT_NAME_SIZE];
};

/* The devices in BSDBT_DEVICES.  *kp is malloc'ed. */
int	bsdbt_known(struct bsdbt_known **kp);

/* "mouse", "keyboard", "phone", ... from a Class of Device. */
const char *bsdbt_class_str(const uint8_t class[NG_HCI_CLASS_SIZE]);

#endif /* BSDBT_BT_H */
