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
 * sdphid.h -- the HID service record of a Bluetooth device (SDP) and the
 * bthidd.conf(5) block made from it.
 */
#ifndef BSDBT_SDPHID_H
#define BSDBT_SDPHID_H

#include <sys/types.h>
#include <bluetooth.h>

/* bthidd's parser takes descriptors shorter than its 1024-byte buffer. */
#define SDPHID_DESC_MAX	1023

struct sdphid {
	int		control_psm;
	int		interrupt_psm;
	int		reconnect_initiate;
	int		battery_power;
	int		normally_connectable;
	uint16_t	vendor_id;
	uint16_t	product_id;
	uint16_t	version;
	uint8_t		desc[SDPHID_DESC_MAX];	/* HID report descriptor */
	size_t		desc_len;
};

/*
 * Ask the device.  Fails with ENOATTR when it has no HID service that
 * bthidd can use (not a keyboard, mouse, ...).
 */
int	sdphid_query(const bdaddr_t *bdaddr, struct sdphid *h);

/* The bthidd.conf block, malloc'ed; `name' as conf_clean_name() made it. */
char	*sdphid_block(const bdaddr_t *bdaddr, const char *name,
	    const struct sdphid *h);

#endif /* BSDBT_SDPHID_H */
