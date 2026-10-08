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
 * bsdbt-helper -- the root side of bsdbt and the panel plugin, run with
 * pkexec(1).  Each call does one thing:
 *
 *	pair ADDR PIN [NAME]	let hcsecd(8) answer the device with PIN,
 *				connect and authenticate (pair), keep the
 *				link key
 *	hid ADDR [NAME]		read the device's HID record over SDP and
 *				give it to bthidd(8)
 *	remove ADDR		forget the device: hcsecd, bthidd, link key
 *	disconnect ADDR		close the connection to the device
 *	power DEVICE on|off	start or stop the Bluetooth stack on DEVICE
 *				(ubt0, ...) with rc.d
 *
 * hcsecd and bthidd are driven through their configuration files and
 * rc.d.  hcsecd keeps link keys in memory and writes /var/db/hcsecd.keys
 * on SIGHUP and when it stops; bthidd rereads nothing, it is restarted.
 *
 * Exit status: 0 done, 1 error, 2 usage, 3 the device has no HID service
 * bthidd can use (printing nothing), 4 pairing refused (wrong PIN, ...),
 * 5 the device did not answer.
 */

#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <bluetooth.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "bt.h"
#include "conf.h"
#include "sdphid.h"

#define HCSECD_CONF	"/etc/bluetooth/hcsecd.conf"
#define HCSECD_KEYS	"/var/db/hcsecd.keys"
#define HCSECD_PID	"/var/run/hcsecd.pid"
#define BTHIDD_CONF	"/etc/bluetooth/bthidd.conf"
#define BTHIDD_HIDS	"/var/db/bthidd.hids"
#define BTHIDD_PID	"/var/run/bthidd.pid"
#define SERVICE		"/usr/sbin/service"
#define LOCK_FILE	"/var/run/bsdbt-helper.lock"

#define EX_NOHID	3
#define EX_REFUSED	4
#define EX_NOANSWER	5

/* Seconds: paging a device, and the user typing a PIN on a keyboard. */
#define CONNECT_TIMEOUT	20
#define AUTH_TIMEOUT	60

static char *service_env[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };

static void
usage(void)
{
	fprintf(stderr,
	    "usage: bsdbt-helper pair address pin [name]\n"
	    "       bsdbt-helper hid address [name]\n"
	    "       bsdbt-helper remove address\n"
	    "       bsdbt-helper disconnect address\n"
	    "       bsdbt-helper power device on|off\n");
	exit(2);
}

/* service(8) NAME VERB [ARG], quietly; its exit status, or -1. */
static int
service(const char *name, const char *verb, const char *arg)
{
	char *argv[] = { SERVICE, (char *)name, (char *)verb, (char *)arg,
	    NULL };
	posix_spawn_file_actions_t fa;
	pid_t pid;
	int status, rv;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null",
	    O_WRONLY, 0);
	rv = -1;
	if (posix_spawn(&pid, SERVICE, &fa, NULL, argv, service_env) == 0) {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
		if (WIFEXITED(status))
			rv = WEXITSTATUS(status);
	}
	posix_spawn_file_actions_destroy(&fa);
	return (rv);
}

/* The pid of a running daemon, from its pid file; 0 when not running. */
static pid_t
running(const char *pidfile)
{
	FILE *f;
	long pid;

	f = fopen(pidfile, "r");
	if (f == NULL)
		return (0);
	if (fscanf(f, "%ld", &pid) != 1 || pid <= 1)
		pid = 0;
	fclose(f);
	if (pid != 0 && kill((pid_t)pid, 0) < 0)
		pid = 0;
	return ((pid_t)pid);
}

/* Enable in rc.conf, so that it starts at boot too. */
static void
enable(const char *name)
{
	if (service(name, "enabled", NULL) != 0 &&
	    service(name, "enable", NULL) != 0)
		warnx("could not enable %s in rc.conf", name);
}

/*
 * Change hcsecd's entry for a device (block NULL: remove it).  hcsecd is
 * stopped meanwhile: on SIGHUP it would write back the link key it has in
 * memory, so an old key could not be dropped.
 */
static int
hcsecd_set(const bdaddr_t *ba, const char *block)
{
	int was, rv;

	was = running(HCSECD_PID) != 0;
	if (was && service("hcsecd", "onestop", NULL) != 0)
		warnx("could not stop hcsecd");
	rv = 0;
	if (conf_set_device(HCSECD_CONF, ba, block, 0600) < 0) {
		warn("%s", HCSECD_CONF);
		rv = -1;
	}
	if (conf_remove_line(HCSECD_KEYS, ba) < 0) {
		warn("%s", HCSECD_KEYS);
		rv = -1;
	}
	if (block != NULL)
		enable("hcsecd");
	if ((was || block != NULL) && service("hcsecd", "onestart",
	    NULL) != 0) {
		warnx("could not start hcsecd");
		rv = -1;
	}
	return (rv);
}

/* Have hcsecd write the link keys it holds to its keys file. */
static void
hcsecd_save(void)
{
	pid_t pid;

	pid = running(HCSECD_PID);
	if (pid != 0 && kill(pid, SIGHUP) == 0)
		usleep(500000);
}

/*
 * Change bthidd's entry for a device (block NULL: remove it) and restart
 * bthidd.  Dropping the device from the hids file makes bthidd connect
 * to it as to a new device.  bthidd refuses to start with no devices.
 */
static int
bthidd_set(const bdaddr_t *ba, const char *block)
{
	struct conf_entry *e;
	int n, rv;

	if (running(BTHIDD_PID) != 0 &&
	    service("bthidd", "onestop", NULL) != 0)
		warnx("could not stop bthidd");
	rv = 0;
	if (conf_set_device(BTHIDD_CONF, ba, block, 0644) < 0) {
		warn("%s", BTHIDD_CONF);
		rv = -1;
	}
	if (conf_remove_line(BTHIDD_HIDS, ba) < 0) {
		warn("%s", BTHIDD_HIDS);
		rv = -1;
	}
	n = conf_devices(BTHIDD_CONF, &e);
	free(e);
	if (n > 0) {
		enable("bthidd");
		if (service("bthidd", "onestart", NULL) != 0) {
			warnx("could not start bthidd");
			rv = -1;
		}
	} else if (n == 0 && service("bthidd", "enabled", NULL) == 0)
		service("bthidd", "disable", NULL);
	return (rv);
}

static struct conf_entry *
find_entry(struct conf_entry *e, int n, const bdaddr_t *ba)
{
	int i;

	for (i = 0; i < n; i++)
		if (bdaddr_same(&e[i].bdaddr, ba))
			return (&e[i]);
	return (NULL);
}

/*
 * Rewrite BSDBT_DEVICES from the daemons' files: the hcsecd entries bsdbt
 * made or that got a link key by pairing, and every bthidd device.  A key
 * written in hcsecd.conf (as in its example entries) is not a pairing.
 */
static void
update_devices(void)
{
	struct conf_entry *h, *b, *x;
	char addr[32], *buf;
	size_t len;
	FILE *f;
	int i, nh, nb, key;

	nh = conf_devices(HCSECD_CONF, &h);
	nb = conf_devices(BTHIDD_CONF, &b);
	if (nh < 0 || nb < 0) {
		warn("cannot read the daemons' configuration");
		goto out;
	}
	buf = NULL;
	f = open_memstream(&buf, &len);
	if (f == NULL)
		goto out;
	for (i = 0; i < nh; i++) {
		if (bdaddr_any(&h[i].bdaddr))
			continue;
		key = conf_has_line(HCSECD_KEYS, &h[i].bdaddr) == 1 &&
		    !h[i].fixed_key;
		x = find_entry(b, nb, &h[i].bdaddr);
		if (!key && !h[i].managed && x == NULL)
			continue;
		fprintf(f, "%s\t%s\t%s\t%s\n", bt_ntoa(&h[i].bdaddr, addr),
		    key ? "paired" : "-", x != NULL ? "hid" : "-",
		    h[i].name[0] != '\0' ? h[i].name :
		    x != NULL ? x->name : "");
	}
	for (i = 0; i < nb; i++)
		if (find_entry(h, nh, &b[i].bdaddr) == NULL)
			fprintf(f, "%s\t-\thid\t%s\n",
			    bt_ntoa(&b[i].bdaddr, addr), b[i].name);
	if (fclose(f) == 0 && conf_write(BSDBT_DEVICES, buf, len, 0644) < 0)
		warn("%s", BSDBT_DEVICES);
	free(buf);
out:
	free(h);
	free(b);
}

static void
parse_addr(const char *s, bdaddr_t *ba)
{
	if (!bt_aton(s, ba) || bdaddr_any(ba))
		errx(2, "%s: not a Bluetooth address", s);
}

/* An adapter to use: the first one that is up. */
static void
first_adapter(char *node)
{
	struct bsdbt_adapter *a;
	int i, n;

	n = bsdbt_adapters(&a);
	for (i = 0; i < n; i++)
		if (BSDBT_ADAPTER_UP(&a[i]))
			break;
	if (n <= 0 || i == n)
		errx(1, "no Bluetooth adapter is up");
	strlcpy(node, a[i].node, HCI_DEVNAME_SIZE);
	free(a);
}

/* HCI command and the event that completes it; the event's status. */
static int
hci_cmd(int s, uint16_t ocf, void *cp, size_t clen, uint8_t event,
    void *ep, size_t elen, time_t timeout)
{
	struct bt_devreq r;

	memset(&r, 0, sizeof(r));
	r.opcode = NG_HCI_OPCODE(NG_HCI_OGF_LINK_CONTROL, ocf);
	r.event = event;
	r.cparam = cp;
	r.clen = clen;
	r.rparam = ep;
	r.rlen = elen;
	if (bt_devreq(s, &r, timeout) < 0)
		return (-1);
	return (((uint8_t *)ep)[0]);
}

/* Open an ACL connection; returns its handle. */
static int
connect_device(int s, const char *node, const bdaddr_t *ba)
{
	ng_hci_create_con_cp cp;
	ng_hci_con_compl_ep ep;
	struct bsdbt_device d;
	int status;

	memset(&cp, 0, sizeof(cp));
	bdaddr_copy(&cp.bdaddr, ba);
	cp.pkt_type = htole16(NG_HCI_PKT_DM1 | NG_HCI_PKT_DH1 |
	    NG_HCI_PKT_DM3 | NG_HCI_PKT_DH3 | NG_HCI_PKT_DM5 |
	    NG_HCI_PKT_DH5);
	cp.page_scan_rep_mode = NG_HCI_SCAN_REP_MODE1;
	cp.accept_role_switch = 1;
	/* From the last inquiry, if any: finds the device faster. */
	if (bsdbt_neighbor(node, ba, &d) == 1) {
		cp.page_scan_rep_mode = d.pscan_rep_mode;
		cp.clock_offset = htole16(d.clock_offset | 0x8000);
	}
	memset(&ep, 0, sizeof(ep));
	status = hci_cmd(s, NG_HCI_OCF_CREATE_CON, &cp, sizeof(cp),
	    NG_HCI_EVENT_CON_COMPL, &ep, sizeof(ep), CONNECT_TIMEOUT);
	if (status < 0) {
		if (errno == ETIMEDOUT)
			errx(EX_NOANSWER, "the device did not answer");
		err(1, "connect");
	}
	/* 0x04 page timeout, 0x08 connection timeout */
	if (status == 0x04 || status == 0x08)
		errx(EX_NOANSWER, "the device did not answer (is it on and "
		    "in pairing mode?)");
	if (status != 0)
		errx(1, "connect: HCI error %#x", status);
	return (NG_HCI_CON_HANDLE(le16toh(ep.con_handle)));
}

static void
disconnect_handle(int s, int handle)
{
	ng_hci_discon_cp cp;
	ng_hci_discon_compl_ep ep;

	memset(&cp, 0, sizeof(cp));
	cp.con_handle = htole16(handle);
	cp.reason = 0x13;	/* remote user terminated connection */
	memset(&ep, 0, sizeof(ep));
	hci_cmd(s, NG_HCI_OCF_DISCON, &cp, sizeof(cp),
	    NG_HCI_EVENT_DISCON_COMPL, &ep, sizeof(ep), 10);
}

static int
check_pin(const char *pin)
{
	size_t i, len;

	len = strlen(pin);
	if (len < 1 || len > NG_HCI_PIN_SIZE)
		return (-1);
	for (i = 0; i < len; i++)
		if (!isprint((unsigned char)pin[i]) || pin[i] == '"' ||
		    pin[i] == '\\')
			return (-1);
	return (0);
}

static int
cmd_pair(const char *addr, const char *pin, const char *rawname)
{
	ng_hci_auth_req_cp cp;
	ng_hci_auth_compl_ep ep;
	struct bsdbt_conn c;
	bdaddr_t ba;
	char node[HCI_DEVNAME_SIZE], name[64], *block;
	int s, handle, status;

	parse_addr(addr, &ba);
	if (check_pin(pin) < 0)
		errx(2, "the PIN must be 1 to %d printable characters, "
		    "no quotes", NG_HCI_PIN_SIZE);
	conf_clean_name(name, rawname != NULL ? rawname : "", sizeof(name));

	if (asprintf(&block, "device {\n\t%s\n\tbdaddr\t%s;\n%s%s%s"
	    "\tkey\tnokey;\n\tpin\t\"%s\";\n}\n", CONF_MARK, addr,
	    name[0] != '\0' ? "\tname\t\"" : "", name,
	    name[0] != '\0' ? "\";\n" : "", pin) < 0)
		err(1, "asprintf");
	if (hcsecd_set(&ba, block) < 0)
		errx(1, "could not set up hcsecd");
	free(block);

	/*
	 * A fresh connection, so that the device is asked again even if the
	 * link was authenticated before.
	 */
	if (bsdbt_find_connection(&ba, node, &c) == 1) {
		s = bt_devopen(node);
		if (s < 0)
			err(1, "%s", node);
		disconnect_handle(s, c.handle);
		usleep(500000);
	} else {
		first_adapter(node);
		s = bt_devopen(node);
		if (s < 0)
			err(1, "%s", node);
	}
	handle = connect_device(s, node, &ba);

	/*
	 * Authentication makes the adapter ask for the link key, then for
	 * the PIN; hcsecd answers both.  The resulting link key comes to
	 * hcsecd in a Link_Key_Notification.
	 */
	memset(&cp, 0, sizeof(cp));
	cp.con_handle = htole16(handle);
	memset(&ep, 0, sizeof(ep));
	status = hci_cmd(s, NG_HCI_OCF_AUTH_REQ, &cp, sizeof(cp),
	    NG_HCI_EVENT_AUTH_COMPL, &ep, sizeof(ep), AUTH_TIMEOUT);
	if (status != 0) {
		disconnect_handle(s, handle);
		bt_devclose(s);
		hcsecd_set(&ba, NULL);
		update_devices();
		if (status < 0)
			errx(EX_NOANSWER, "pairing: %s", strerror(errno));
		/* 0x05 authentication failure, 0x06 PIN or key missing */
		if (status == 0x05 || status == 0x06)
			errx(EX_REFUSED, "pairing refused (wrong PIN?)");
		if (status == 0x08 || status == 0x22)
			errx(EX_NOANSWER, "pairing timed out");
		errx(EX_REFUSED, "pairing failed: HCI error %#x", status);
	}
	bt_devclose(s);
	hcsecd_save();
	update_devices();
	syslog(LOG_NOTICE, "paired %s", addr);
	return (0);
}

static int
cmd_hid(const char *addr, const char *rawname)
{
	struct conf_entry *e, *x;
	struct sdphid h;
	bdaddr_t ba;
	char name[64], *block;
	int n;

	parse_addr(addr, &ba);
	if (sdphid_query(&ba, &h) < 0) {
		/* Not an error for the caller to report; just say so. */
		if (errno == ENOATTR)
			exit(EX_NOHID);
		if (errno == EHOSTDOWN || errno == ETIMEDOUT)
			errx(EX_NOANSWER, "the device did not answer");
		err(1, "SDP query");
	}
	/* Without a name, the one given when pairing. */
	name[0] = '\0';
	if (rawname != NULL)
		conf_clean_name(name, rawname, sizeof(name));
	else {
		n = conf_devices(HCSECD_CONF, &e);
		if (n > 0 && (x = find_entry(e, n, &ba)) != NULL)
			conf_clean_name(name, x->name, sizeof(name));
		free(e);
	}
	block = sdphid_block(&ba, name, &h);
	if (block == NULL)
		err(1, "sdphid_block");
	n = bthidd_set(&ba, block);
	free(block);
	update_devices();
	if (n < 0)
		errx(1, "could not set up bthidd");
	syslog(LOG_NOTICE, "HID device %s added", addr);
	return (0);
}

static int
cmd_disconnect(const char *addr, int quiet)
{
	struct bsdbt_conn c;
	bdaddr_t ba;
	char node[HCI_DEVNAME_SIZE];
	int s;

	parse_addr(addr, &ba);
	if (bsdbt_find_connection(&ba, node, &c) != 1) {
		if (quiet)
			return (0);
		errx(1, "%s is not connected", addr);
	}
	s = bt_devopen(node);
	if (s < 0)
		err(1, "%s", node);
	disconnect_handle(s, c.handle);
	bt_devclose(s);
	return (0);
}

static int
cmd_remove(const char *addr)
{
	struct conf_entry *e;
	bdaddr_t ba;
	int n, rv;

	parse_addr(addr, &ba);
	cmd_disconnect(addr, 1);
	rv = 0;
	n = conf_devices(HCSECD_CONF, &e);
	if ((n > 0 && find_entry(e, n, &ba) != NULL) ||
	    conf_has_line(HCSECD_KEYS, &ba) == 1)
		rv |= hcsecd_set(&ba, NULL);
	free(e);
	n = conf_devices(BTHIDD_CONF, &e);
	if (n > 0 && find_entry(e, n, &ba) != NULL)
		rv |= bthidd_set(&ba, NULL);
	free(e);
	update_devices();
	if (rv != 0)
		errx(1, "could not remove %s everywhere", addr);
	syslog(LOG_NOTICE, "removed %s", addr);
	return (0);
}

static int
cmd_power(const char *dev, const char *onoff)
{
	size_t i;

	/* A device name: letters, then the unit number (ubt0). */
	for (i = 0; isalpha((unsigned char)dev[i]); i++)
		;
	if (i == 0 || dev[i] == '\0' || strlen(dev) > 16)
		errx(2, "%s: not a device name", dev);
	for (; dev[i] != '\0'; i++)
		if (!isdigit((unsigned char)dev[i]))
			errx(2, "%s: not a device name", dev);

	if (strcmp(onoff, "on") == 0) {
		if (service("bluetooth", "start", dev) != 0)
			errx(1, "could not start Bluetooth on %s", dev);
	} else if (strcmp(onoff, "off") == 0) {
		if (service("bluetooth", "stop", dev) != 0)
			errx(1, "could not stop Bluetooth on %s", dev);
	} else
		usage();
	syslog(LOG_NOTICE, "Bluetooth %s on %s", onoff, dev);
	return (0);
}

int
main(int argc, char *argv[])
{
	const char *uid;
	int lock;

	if (argc < 2)
		usage();
	if (geteuid() != 0)
		errx(1, "must be run as root (pkexec bsdbt-helper ...)");
	umask(022);
	uid = getenv("PKEXEC_UID");
	openlog("bsdbt-helper", LOG_PID, LOG_AUTH);
	syslog(LOG_INFO, "uid %s: %s %s", uid != NULL ? uid : "0", argv[1],
	    argc > 2 ? argv[2] : "");

	/* One change at a time. */
	lock = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX) < 0)
		err(1, "%s", LOCK_FILE);

	if (strcmp(argv[1], "pair") == 0 && (argc == 4 || argc == 5))
		return (cmd_pair(argv[2], argv[3], argc == 5 ? argv[4] : NULL));
	if (strcmp(argv[1], "hid") == 0 && (argc == 3 || argc == 4))
		return (cmd_hid(argv[2], argc == 4 ? argv[3] : NULL));
	if (strcmp(argv[1], "remove") == 0 && argc == 3)
		return (cmd_remove(argv[2]));
	if (strcmp(argv[1], "disconnect") == 0 && argc == 3)
		return (cmd_disconnect(argv[2], 0));
	if (strcmp(argv[1], "power") == 0 && argc == 4)
		return (cmd_power(argv[2], argv[3]));
	usage();
	return (2);
}
