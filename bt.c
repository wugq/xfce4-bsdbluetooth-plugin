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
 * Talking to the Bluetooth stack.  Adapters are HCI nodes of netgraph
 * (ubt0hci, ...), found by name through SIOC_HCI_RAW_NODE_LIST_NAMES
 * (bt_devenum()), so nothing here depends on the driver below them.
 * Node state, address and connections come from ioctls on an HCI raw
 * socket; the local name, class and scan mode from standard HCI "read"
 * commands (bt_devreq()); devices in range from an inquiry.
 */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>

#include <bluetooth.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bt.h"

/* Seconds to wait for the answer to a local HCI command. */
#define CMD_TIMEOUT	5
/* ... and for a remote name request, which has to reach the device. */
#define NAME_TIMEOUT	10

struct names {
	char	(*node)[HCI_DEVNAME_SIZE];
	int	n;
};

static int
collect_node(int s __unused, struct bt_devinfo const *di, void *arg)
{
	struct names *nm = arg;
	char (*p)[HCI_DEVNAME_SIZE];

	p = reallocarray(nm->node, nm->n + 1, sizeof(*nm->node));
	if (p == NULL)
		return (1);		/* stop; we return what we have */
	nm->node = p;
	strlcpy(nm->node[nm->n++], di->devname, sizeof(*nm->node));
	return (0);
}

int
bsdbt_adapters(struct bsdbt_adapter **ap)
{
	struct bsdbt_adapter *a;
	struct names nm;
	int i, n;

	*ap = NULL;
	nm.node = NULL;
	nm.n = 0;
	if (bt_devenum(collect_node, &nm) < 0) {
		free(nm.node);
		return (-1);
	}
	if (nm.n == 0) {
		free(nm.node);
		return (0);
	}

	a = calloc(nm.n, sizeof(*a));
	if (a == NULL) {
		free(nm.node);
		return (-1);
	}
	/* A node can go away between the two calls; skip it then. */
	for (i = n = 0; i < nm.n; i++)
		if (bsdbt_adapter(nm.node[i], &a[n]) == 0)
			n++;
	free(nm.node);
	*ap = a;
	return (n);
}

/* Send a local HCI command without parameters, wait for its result. */
static int
hci_read(int s, uint16_t ocf, void *rp, size_t rlen)
{
	struct bt_devreq r;

	memset(&r, 0, sizeof(r));
	r.opcode = NG_HCI_OPCODE(NG_HCI_OGF_HC_BASEBAND, ocf);
	r.rparam = rp;
	r.rlen = rlen;
	if (bt_devreq(s, &r, CMD_TIMEOUT) < 0)
		return (-1);
	/* The first byte of every result is the HCI status. */
	if (r.rlen < 1 || ((uint8_t *)rp)[0] != 0) {
		errno = EIO;
		return (-1);
	}
	return (0);
}

int
bsdbt_adapter(const char *node, struct bsdbt_adapter *a)
{
	struct bt_devinfo di;
	ng_hci_read_local_name_rp name;
	ng_hci_read_unit_class_rp class;
	ng_hci_read_scan_enable_rp scan;
	int s;

	memset(a, 0, sizeof(*a));
	memset(&di, 0, sizeof(di));
	strlcpy(di.devname, node, sizeof(di.devname));
	if (bt_devinfo(&di) < 0)
		return (-1);
	strlcpy(a->node, di.devname, sizeof(a->node));
	bdaddr_copy(&a->bdaddr, &di.bdaddr);
	a->state = di.state;
	a->scan = -1;

	/* HCI commands only work on a node that is up. */
	if (!BSDBT_ADAPTER_UP(a))
		return (0);
	s = bt_devopen(a->node);
	if (s < 0)
		return (0);
	if (hci_read(s, NG_HCI_OCF_READ_LOCAL_NAME, &name, sizeof(name)) == 0)
		strlcpy(a->name, name.name,
		    MIN(sizeof(a->name), sizeof(name.name) + 1));
	if (hci_read(s, NG_HCI_OCF_READ_UNIT_CLASS, &class, sizeof(class)) == 0)
		memcpy(a->class, class.uclass, sizeof(a->class));
	if (hci_read(s, NG_HCI_OCF_READ_SCAN_ENABLE, &scan, sizeof(scan)) == 0)
		a->scan = scan.scan_enable;
	bt_devclose(s);
	return (0);
}

int
bsdbt_connections(const char *node, struct bsdbt_conn **cp)
{
	struct ng_btsocket_hci_raw_con_list r;
	struct bsdbt_conn *c;
	int i, s, saved;

	*cp = NULL;
	s = bt_devopen(node);
	if (s < 0)
		return (-1);
	memset(&r, 0, sizeof(r));
	r.num_connections = NG_HCI_MAX_CON_NUM;
	r.connections = calloc(r.num_connections, sizeof(*r.connections));
	if (r.connections == NULL) {
		bt_devclose(s);
		return (-1);
	}
	if (ioctl(s, SIOC_HCI_RAW_NODE_GET_CON_LIST, &r, sizeof(r)) < 0) {
		saved = errno;
		free(r.connections);
		bt_devclose(s);
		errno = saved;
		return (-1);
	}
	bt_devclose(s);

	c = NULL;
	if (r.num_connections > 0) {
		c = calloc(r.num_connections, sizeof(*c));
		if (c == NULL) {
			free(r.connections);
			return (-1);
		}
	}
	for (i = 0; i < (int)r.num_connections; i++) {
		bdaddr_copy(&c[i].bdaddr, &r.connections[i].bdaddr);
		c[i].handle = r.connections[i].con_handle;
		c[i].link_type = r.connections[i].link_type;
		c[i].encryption = r.connections[i].encryption_mode;
		c[i].role = r.connections[i].role;
		c[i].state = r.connections[i].state;
	}
	free(r.connections);
	*cp = c;
	return (i);
}

int
bsdbt_scan(const char *node, int seconds, struct bsdbt_device **dp)
{
	struct bt_devinquiry *ii;
	struct bsdbt_device *d;
	int i, j, n, found;

	*dp = NULL;
	ii = NULL;
	/* Up to 255 responses; a device may answer more than once. */
	n = bt_devinquiry(node, seconds, 255, &ii);
	if (n < 0)
		return (-1);
	if (n == 0) {
		free(ii);
		return (0);
	}
	d = calloc(n, sizeof(*d));
	if (d == NULL) {
		free(ii);
		return (-1);
	}
	for (found = i = 0; i < n; i++) {
		for (j = 0; j < found; j++)
			if (bdaddr_same(&d[j].bdaddr, &ii[i].bdaddr))
				break;
		if (j < found)
			continue;
		bdaddr_copy(&d[found].bdaddr, &ii[i].bdaddr);
		memcpy(d[found].class, ii[i].dev_class, sizeof(d->class));
		d[found].clock_offset = ii[i].clock_offset;
		d[found].pscan_rep_mode = ii[i].pscan_rep_mode;
		found++;
	}
	free(ii);
	*dp = d;
	return (found);
}

/*
 * Not bt_devremote_name(): it returns an empty name when the request
 * fails, so a device out of range would look like one without a name.
 */
int
bsdbt_remote_name(const char *node, const bdaddr_t *bdaddr,
    const struct bsdbt_device *d, char *name, size_t len)
{
	struct bt_devreq r;
	ng_hci_remote_name_req_cp cp;
	ng_hci_remote_name_req_compl_ep ep;
	int s, saved;

	memset(&cp, 0, sizeof(cp));
	bdaddr_copy(&cp.bdaddr, bdaddr);
	/*
	 * The clock offset and page scan mode from the inquiry let the
	 * adapter find the device faster; bit 15 says the offset is valid.
	 */
	cp.page_scan_rep_mode = NG_HCI_SCAN_REP_MODE1;
	if (d != NULL) {
		cp.clock_offset = htole16(d->clock_offset | 0x8000);
		cp.page_scan_rep_mode = d->pscan_rep_mode;
	}

	memset(&ep, 0, sizeof(ep));
	memset(&r, 0, sizeof(r));
	r.opcode = NG_HCI_OPCODE(NG_HCI_OGF_LINK_CONTROL,
	    NG_HCI_OCF_REMOTE_NAME_REQ);
	r.event = NG_HCI_EVENT_REMOTE_NAME_REQ_COMPL;
	r.cparam = &cp;
	r.clen = sizeof(cp);
	r.rparam = &ep;
	r.rlen = sizeof(ep);

	s = bt_devopen(node);
	if (s < 0)
		return (-1);
	/* Paging a device can take its page timeout, 5.12 s by default. */
	if (bt_devreq(s, &r, NAME_TIMEOUT) < 0) {
		saved = errno;
		bt_devclose(s);
		errno = saved;
		return (-1);
	}
	bt_devclose(s);
	if (ep.status != 0) {
		/* Usually 0x04, page timeout: the device did not answer. */
		errno = ep.status == 0x04 ? EHOSTDOWN : EIO;
		return (-1);
	}
	strlcpy(name, ep.name, MIN(len, sizeof(ep.name) + 1));
	return (0);
}

int
bsdbt_neighbor(const char *node, const bdaddr_t *bdaddr,
    struct bsdbt_device *d)
{
	struct ng_btsocket_hci_raw_node_neighbor_cache r;
	int i, s, saved, found;

	s = bt_devopen(node);
	if (s < 0)
		return (-1);
	memset(&r, 0, sizeof(r));
	r.num_entries = NG_HCI_MAX_NEIGHBOR_NUM;
	r.entries = calloc(r.num_entries, sizeof(*r.entries));
	if (r.entries == NULL) {
		bt_devclose(s);
		return (-1);
	}
	if (ioctl(s, SIOC_HCI_RAW_NODE_GET_NEIGHBOR_CACHE, &r,
	    sizeof(r)) < 0) {
		saved = errno;
		free(r.entries);
		bt_devclose(s);
		errno = saved;
		return (-1);
	}
	bt_devclose(s);
	found = 0;
	for (i = 0; i < (int)r.num_entries; i++) {
		if (!bdaddr_same(&r.entries[i].bdaddr, bdaddr))
			continue;
		memset(d, 0, sizeof(*d));
		bdaddr_copy(&d->bdaddr, bdaddr);
		d->clock_offset = r.entries[i].clock_offset;
		d->pscan_rep_mode = r.entries[i].page_scan_rep_mode;
		found = 1;
		break;
	}
	free(r.entries);
	return (found);
}

int
bsdbt_find_connection(const bdaddr_t *bdaddr, char *node,
    struct bsdbt_conn *c)
{
	struct bsdbt_adapter *a;
	struct bsdbt_conn *cl;
	int i, j, n, nc, found;

	n = bsdbt_adapters(&a);
	if (n < 0)
		return (-1);
	found = 0;
	for (i = 0; i < n && !found; i++) {
		if (!BSDBT_ADAPTER_UP(&a[i]))
			continue;
		nc = bsdbt_connections(a[i].node, &cl);
		if (nc < 0)
			continue;
		for (j = 0; j < nc; j++)
			if (cl[j].link_type == NG_HCI_LINK_ACL &&
			    cl[j].state == NG_HCI_CON_OPEN &&
			    bdaddr_same(&cl[j].bdaddr, bdaddr)) {
				strlcpy(node, a[i].node, HCI_DEVNAME_SIZE);
				*c = cl[j];
				found = 1;
				break;
			}
		free(cl);
	}
	free(a);
	return (found);
}

int
bsdbt_known(struct bsdbt_known **kp)
{
	struct bsdbt_known *k, *p;
	FILE *f;
	char line[512], *s, *addr, *paired, *hid;
	int n;

	*kp = NULL;
	f = fopen(BSDBT_DEVICES, "r");
	if (f == NULL)
		return (errno == ENOENT ? 0 : -1);
	k = NULL;
	n = 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		s = line;
		addr = strsep(&s, "\t");
		paired = strsep(&s, "\t");
		hid = strsep(&s, "\t");
		if (addr == NULL || paired == NULL || hid == NULL || s == NULL)
			continue;
		p = reallocarray(k, n + 1, sizeof(*k));
		if (p == NULL)
			break;
		k = p;
		memset(&k[n], 0, sizeof(k[n]));
		if (!bt_aton(addr, &k[n].bdaddr))
			continue;
		k[n].paired = strcmp(paired, "paired") == 0;
		k[n].hid = strcmp(hid, "hid") == 0;
		strlcpy(k[n].name, s, sizeof(k[n].name));
		n++;
	}
	fclose(f);
	*kp = k;
	return (n);
}

int
bsdbt_radios(struct bsdbt_radio **rp)
{
	static const char *drivers[] = { "ubt" };
	struct bsdbt_radio *r, *p;
	char name[64], desc[128];
	size_t len;
	int d, n, unit, misses;

	*rp = NULL;
	r = NULL;
	n = 0;
	for (d = 0; d < (int)nitems(drivers); d++)
		/* Units can have gaps (unplugged devices); stop after a few. */
		for (unit = 0, misses = 0; misses < 4; unit++) {
			snprintf(name, sizeof(name), "dev.%s.%d.%%desc",
			    drivers[d], unit);
			len = sizeof(desc) - 1;
			if (sysctlbyname(name, desc, &len, NULL, 0) < 0) {
				misses++;
				continue;
			}
			desc[len] = '\0';
			p = reallocarray(r, n + 1, sizeof(*r));
			if (p == NULL)
				break;
			r = p;
			snprintf(r[n].dev, sizeof(r[n].dev), "%s%d",
			    drivers[d], unit);
			strlcpy(r[n].desc, desc, sizeof(r[n].desc));
			n++;
		}
	*rp = r;
	return (n);
}

/*
 * Class of Device, Bluetooth Assigned Numbers: bits 2-7 minor class,
 * bits 8-12 major class.  class[0] is the lowest byte.
 */
const char *
bsdbt_class_str(const uint8_t class[NG_HCI_CLASS_SIZE])
{
	static const char *major[] = {
		"misc", "computer", "phone", "network", "audio",
		"peripheral", "imaging", "wearable", "toy", "health",
	};
	int maj;

	maj = class[1] & 0x1f;
	if (maj == 5) {
		/* Peripheral: minor bits 6 and 7 are keyboard and pointer. */
		switch (class[0] & 0xc0) {
		case 0x40:
			return ("keyboard");
		case 0x80:
			return ("mouse");
		case 0xc0:
			return ("keyboard+mouse");
		}
	}
	if (maj < (int)nitems(major))
		return (major[maj]);
	return ("unknown");
}
