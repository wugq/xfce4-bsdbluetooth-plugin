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
 * The HID service record of a device, read with libsdp(3) as
 * bthidcontrol(8) "query" does, and the bthidd.conf(5) block for it.
 * Attribute IDs: Bluetooth HID profile and Device ID profile.
 */

#include <sys/param.h>

#include <bluetooth.h>
#include <errno.h>
#include <sdp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <usbhid.h>

#include "conf.h"
#include "sdphid.h"

#define ATTR_HID_RECONNECT_INITIATE	0x0205
#define ATTR_HID_DESCRIPTOR_LIST	0x0206
#define ATTR_HID_BATTERY_POWER		0x0209
#define ATTR_HID_NORMALLY_CONNECTABLE	0x020d
#define ATTR_DEVID_VENDOR		0x0201
#define ATTR_DEVID_PRODUCT		0x0202
#define ATTR_DEVID_VERSION		0x0203

/* HID class descriptor type of the report descriptor (UDESC_REPORT) */
#define DESC_REPORT			0x22

#define NVALUES		8
#define VALUE_SIZE	2048

/* A cursor over SDP data elements, with bounds checks. */
struct cur {
	uint8_t	*p, *end;
};

static int
get8(struct cur *c, uint32_t *v)
{
	if (c->end - c->p < 1)
		return (-1);
	*v = *c->p++;
	return (0);
}

static int
get16(struct cur *c, uint32_t *v)
{
	if (c->end - c->p < 2)
		return (-1);
	*v = (uint32_t)c->p[0] << 8 | c->p[1];
	c->p += 2;
	return (0);
}

static int
get32(struct cur *c, uint32_t *v)
{
	if (c->end - c->p < 4)
		return (-1);
	*v = (uint32_t)c->p[0] << 24 | (uint32_t)c->p[1] << 16 |
	    (uint32_t)c->p[2] << 8 | c->p[3];
	c->p += 4;
	return (0);
}

/*
 * Enter a sequence (or read a string header): returns its length and
 * leaves the cursor at its first byte.
 */
static int
header(struct cur *c, int seq, uint32_t *len)
{
	uint32_t type;

	if (get8(c, &type) < 0)
		return (-1);
	switch (type) {
	case SDP_DATA_SEQ8:
	case SDP_DATA_STR8:
		if (seq != (type == SDP_DATA_SEQ8) || get8(c, len) < 0)
			return (-1);
		break;
	case SDP_DATA_SEQ16:
	case SDP_DATA_STR16:
		if (seq != (type == SDP_DATA_SEQ16) || get16(c, len) < 0)
			return (-1);
		break;
	case SDP_DATA_SEQ32:
	case SDP_DATA_STR32:
		if (seq != (type == SDP_DATA_SEQ32) || get32(c, len) < 0)
			return (-1);
		break;
	default:
		return (-1);
	}
	if (*len > (uint32_t)(c->end - c->p))
		return (-1);
	return (0);
}

/*
 * The L2CAP PSM in a protocol descriptor list:
 * seq { seq { uuid L2CAP, uint16 psm }, seq { uuid HIDP } }.
 * The additional list has one more sequence around it.
 */
static int
parse_psm(sdp_attr_t *a, int additional)
{
	struct cur c;
	uint32_t len, type, v;

	c.p = a->value;
	c.end = a->value + a->vlen;
	if (additional && header(&c, 1, &len) < 0)
		return (-1);
	if (header(&c, 1, &len) < 0 || header(&c, 1, &len) < 0)
		return (-1);
	if (get8(&c, &type) < 0 || type != SDP_DATA_UUID16 ||
	    get16(&c, &v) < 0 || v != SDP_UUID_PROTOCOL_L2CAP)
		return (-1);
	if (get8(&c, &type) < 0 || type != SDP_DATA_UINT16 ||
	    get16(&c, &v) < 0)
		return (-1);
	return ((int)v);
}

/*
 * The report descriptor in HIDDescriptorList:
 * seq { seq { uint8 type, string descriptor }, ... }; type 0x22 is the
 * report descriptor.
 */
static int
parse_descriptor(sdp_attr_t *a, uint8_t **desc, uint32_t *dlen)
{
	struct cur c, d;
	uint32_t len, type, dtype;

	c.p = a->value;
	c.end = a->value + a->vlen;
	if (header(&c, 1, &len) < 0)
		return (-1);
	c.end = c.p + len;
	while (c.p < c.end) {
		if (header(&c, 1, &len) < 0)
			return (-1);
		d.p = c.p;
		d.end = c.p + len;
		c.p += len;
		if (get8(&d, &type) < 0 || type != SDP_DATA_UINT8 ||
		    get8(&d, &dtype) < 0 || header(&d, 0, &len) < 0)
			return (-1);
		if (dtype == DESC_REPORT && len > 0) {
			*desc = d.p;
			*dlen = len;
			return (0);
		}
	}
	return (-1);
}

static int
parse_bool(sdp_attr_t *a)
{
	if (a->vlen != 2 || a->value[0] != SDP_DATA_BOOL)
		return (-1);
	return (a->value[1] != 0);
}

static int
parse_uint16(sdp_attr_t *a, uint16_t *v)
{
	if (a->vlen != 3 || a->value[0] != SDP_DATA_UINT16)
		return (-1);
	*v = (uint16_t)(a->value[1] << 8 | a->value[2]);
	return (0);
}

static void
init_values(sdp_attr_t *values, uint8_t (*buf)[VALUE_SIZE])
{
	int i;

	for (i = 0; i < NVALUES; i++) {
		values[i].flags = SDP_ATTR_INVALID;
		values[i].attr = 0;
		values[i].vlen = VALUE_SIZE;
		values[i].value = buf[i];
	}
}

int
sdphid_query(const bdaddr_t *bdaddr, struct sdphid *h)
{
	static uint32_t attrs[] = {
		SDP_ATTR_RANGE(SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST,
		    SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST),
		SDP_ATTR_RANGE(SDP_ATTR_ADDITIONAL_PROTOCOL_DESCRIPTOR_LISTS,
		    SDP_ATTR_ADDITIONAL_PROTOCOL_DESCRIPTOR_LISTS),
		SDP_ATTR_RANGE(ATTR_HID_RECONNECT_INITIATE,
		    ATTR_HID_RECONNECT_INITIATE),
		SDP_ATTR_RANGE(ATTR_HID_DESCRIPTOR_LIST,
		    ATTR_HID_DESCRIPTOR_LIST),
		SDP_ATTR_RANGE(ATTR_HID_BATTERY_POWER, ATTR_HID_BATTERY_POWER),
		SDP_ATTR_RANGE(ATTR_HID_NORMALLY_CONNECTABLE,
		    ATTR_HID_NORMALLY_CONNECTABLE),
	};
	uint32_t devid = SDP_ATTR_RANGE(ATTR_DEVID_VENDOR, ATTR_DEVID_VERSION);
	uint16_t hid = SDP_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE;
	uint16_t pnp = SDP_SERVICE_CLASS_PNP_INFORMATION;
	sdp_attr_t values[NVALUES];
	uint8_t (*buf)[VALUE_SIZE];
	report_desc_t rd;
	uint8_t *desc;
	uint32_t dlen;
	void *ss;
	int i, error, reconnect;

	memset(h, 0, sizeof(*h));
	buf = calloc(NVALUES, VALUE_SIZE);
	if (buf == NULL)
		return (-1);
	ss = sdp_open(NG_HCI_BDADDR_ANY, bdaddr);
	if (ss == NULL) {
		free(buf);
		errno = ENOMEM;
		return (-1);
	}
	if ((error = sdp_error(ss)) != 0)
		goto out;

	init_values(values, buf);
	if (sdp_search(ss, 1, &hid, nitems(attrs), attrs, NVALUES,
	    values) != 0) {
		error = sdp_error(ss);
		goto out;
	}
	h->control_psm = h->interrupt_psm = reconnect = -1;
	desc = NULL;
	dlen = 0;
	for (i = 0; i < NVALUES; i++) {
		if (values[i].flags != SDP_ATTR_OK)
			continue;
		switch (values[i].attr) {
		case SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST:
			h->control_psm = parse_psm(&values[i], 0);
			break;
		case SDP_ATTR_ADDITIONAL_PROTOCOL_DESCRIPTOR_LISTS:
			h->interrupt_psm = parse_psm(&values[i], 1);
			break;
		case ATTR_HID_RECONNECT_INITIATE:
			reconnect = parse_bool(&values[i]);
			break;
		case ATTR_HID_DESCRIPTOR_LIST:
			if (parse_descriptor(&values[i], &desc, &dlen) < 0)
				desc = NULL;
			break;
		case ATTR_HID_BATTERY_POWER:
			h->battery_power = parse_bool(&values[i]) == 1;
			break;
		case ATTR_HID_NORMALLY_CONNECTABLE:
			h->normally_connectable = parse_bool(&values[i]) == 1;
			break;
		}
	}
	/* No HID service, or not one bthidd could use. */
	if (h->control_psm < 0 || h->interrupt_psm < 0 || reconnect < 0 ||
	    desc == NULL || dlen > sizeof(h->desc)) {
		error = ENOATTR;
		goto out;
	}
	h->reconnect_initiate = reconnect;
	memcpy(h->desc, desc, dlen);
	h->desc_len = dlen;

	/* bthidd parses the descriptor with libusbhid; so do we, first. */
	rd = hid_use_report_desc(h->desc, h->desc_len);
	if (rd == NULL) {
		error = EINVAL;
		goto out;
	}
	hid_dispose_report_desc(rd);

	/* Vendor, product and version are optional. */
	init_values(values, buf);
	if (sdp_search(ss, 1, &pnp, 1, &devid, NVALUES, values) == 0)
		for (i = 0; i < NVALUES; i++) {
			if (values[i].flags != SDP_ATTR_OK)
				continue;
			switch (values[i].attr) {
			case ATTR_DEVID_VENDOR:
				parse_uint16(&values[i], &h->vendor_id);
				break;
			case ATTR_DEVID_PRODUCT:
				parse_uint16(&values[i], &h->product_id);
				break;
			case ATTR_DEVID_VERSION:
				parse_uint16(&values[i], &h->version);
				break;
			}
		}
	error = 0;
out:
	sdp_close(ss);
	free(buf);
	if (error != 0) {
		errno = error;
		return (-1);
	}
	return (0);
}

char *
sdphid_block(const bdaddr_t *bdaddr, const char *name, const struct sdphid *h)
{
	char addr[32], *s;
	FILE *f;
	size_t len;
	int i;

	s = NULL;
	f = open_memstream(&s, &len);
	if (f == NULL)
		return (NULL);
	fprintf(f, "device {\n\t%s\n", CONF_MARK);
	fprintf(f, "\tbdaddr\t\t\t%s;\n", bt_ntoa(bdaddr, addr));
	if (name != NULL && name[0] != '\0')
		fprintf(f, "\tname\t\t\t\"%s\";\n", name);
	fprintf(f, "\tvendor_id\t\t0x%04x;\n", h->vendor_id);
	fprintf(f, "\tproduct_id\t\t0x%04x;\n", h->product_id);
	fprintf(f, "\tversion\t\t\t0x%04x;\n", h->version);
	fprintf(f, "\tcontrol_psm\t\t0x%x;\n", h->control_psm);
	fprintf(f, "\tinterrupt_psm\t\t0x%x;\n", h->interrupt_psm);
	fprintf(f, "\treconnect_initiate\t%s;\n",
	    h->reconnect_initiate ? "true" : "false");
	fprintf(f, "\tbattery_power\t\t%s;\n",
	    h->battery_power ? "true" : "false");
	fprintf(f, "\tnormally_connectable\t%s;\n",
	    h->normally_connectable ? "true" : "false");
	fprintf(f, "\thid_descriptor\t\t{");
	for (i = 0; i < (int)h->desc_len; i++)
		fprintf(f, "%s0x%02x", i % 8 == 0 ? "\n\t\t" : " ",
		    h->desc[i]);
	fprintf(f, "\n\t};\n}\n");
	if (fclose(f) != 0) {
		free(s);
		return (NULL);
	}
	return (s);
}
