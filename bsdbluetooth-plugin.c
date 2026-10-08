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
 * xfce4-bsdbluetooth-plugin -- an XFCE panel icon for FreeBSD Bluetooth.
 *
 * Its popup switches Bluetooth on and off, lists the devices set up
 * before (connected or not) and the devices in range, and pairs,
 * disconnects and removes devices.
 *
 * Reading state and searching run as the user (bt.c); changes go through
 * bsdbt-helper with pkexec.  Everything that can block -- HCI commands,
 * an inquiry, the helper -- runs in a GTask thread; the widgets are only
 * touched in the main loop.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <bluetooth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

#include "bt.h"

#ifndef BSDBT_HELPER
#define BSDBT_HELPER	"/usr/local/libexec/bsdbt-helper"
#endif

/* bsdbt-helper's exit status */
#define HELPER_NOHID	3
#define HELPER_REFUSED	4
#define HELPER_NOANSWER	5
/* pkexec's: the user dismissed the dialog, or polkit said no */
#define PKEXEC_DISMISSED 126
#define PKEXEC_NOTAUTH	127

#define REFRESH_OPEN_S	3	/* popup open: follow connections */
#define REFRESH_IDLE_S	15	/* closed: only the panel icon */
#define SCAN_SECONDS	8

#define POPUP_WIDTH	400	/* pixels, without the margins */

#define ICON_ON		"bluetooth-active-symbolic"
#define ICON_OFF	"bluetooth-disabled-symbolic"

/* What a refresh found. */
typedef struct {
	struct bsdbt_adapter *adapters;
	int		nadapters;
	bdaddr_t	*connected;	/* ACL connections, all adapters */
	int		nconnected;
	struct bsdbt_known *known;
	int		nknown;
	struct bsdbt_radio *radios;
	int		nradios;
} Snapshot;

/* A device found by a search. */
typedef struct {
	bdaddr_t	bdaddr;
	uint8_t		class[NG_HCI_CLASS_SIZE];
	char		name[BSDBT_NAME_SIZE];
} Nearby;

typedef struct {
	XfcePanelPlugin	*plugin;
	int		refs;		/* the plugin, and each running task */
	gboolean	dead;		/* the plugin is gone */

	GtkWidget	*button, *icon;
	GtkWidget	*popup;
	gboolean	grabbed;
	int		popup_width, popup_height;
	gint64		hidden_at;	/* when the popup was last closed */

	GtkWidget	*power, *adapter_label;
	GtkWidget	*known_box, *known_empty;
	GtkWidget	*nearby_section, *off_label;
	GtkWidget	*search, *spinner, *nearby_box, *nearby_empty;
	GtkWidget	*status;
	GtkWidget	*pin_row, *pin_label, *pin_entry, *pin_ok;
	bdaddr_t	pin_bdaddr;	/* the device that refused the PIN */
	char		*pin_name;

	Snapshot	snap;
	gboolean	refreshing;
	guint		timer;
	char		*known_key;	/* what the device list shows */

	bdaddr_t	expanded;	/* device whose actions are shown */
	bdaddr_t	pairing;	/* device being paired */

	GArray		*nearby;	/* of Nearby */
	gboolean	scanning;
	gboolean	busy;		/* the helper is running */
} Panel;

static void	refresh(Panel *);
static void	render(Panel *);

/* ---- lifetime ----------------------------------------------------------- */

static Panel *
panel_ref(Panel *p)
{
	g_atomic_int_inc(&p->refs);
	return (p);
}

static void
panel_unref(Panel *p)
{
	if (!g_atomic_int_dec_and_test(&p->refs))
		return;
	g_free(p->snap.adapters);
	g_free(p->snap.connected);
	g_free(p->snap.known);
	g_free(p->snap.radios);
	g_free(p->known_key);
	g_free(p->pin_name);
	g_array_free(p->nearby, TRUE);
	g_free(p);
}

/* ---- the helper --------------------------------------------------------- */

typedef struct {
	int		status;		/* exit status, -1 not run */
	char		*err;		/* what it wrote to stderr */
} HelperResult;

/* Run bsdbt-helper with pkexec, in a worker thread. */
static void
run_helper(const char *const *args, HelperResult *r)
{
	const char *argv[8];
	GError *error;
	char *err;
	int i, k, wstatus;

	k = 0;
	argv[k++] = "pkexec";
	argv[k++] = BSDBT_HELPER;
	for (i = 0; args[i] != NULL && k < (int)G_N_ELEMENTS(argv) - 1; i++)
		argv[k++] = args[i];
	argv[k] = NULL;

	r->status = -1;
	r->err = NULL;
	error = NULL;
	err = NULL;
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH |
	    G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, &err, &wstatus,
	    &error)) {
		r->err = g_strdup(error->message);
		g_error_free(error);
		return;
	}
	if (WIFEXITED(wstatus))
		r->status = WEXITSTATUS(wstatus);
	/* Its messages are "bsdbt-helper: text"; keep the text. */
	if (err != NULL) {
		g_strstrip(err);
		if (g_str_has_prefix(err, "bsdbt-helper: "))
			r->err = g_strdup(err + strlen("bsdbt-helper: "));
		else if (err[0] != '\0')
			r->err = g_strdup(err);
		g_free(err);
	}
}

static char *
helper_message(const HelperResult *r)
{
	if (r->status == PKEXEC_DISMISSED)
		return (g_strdup("Not authorized."));
	if (r->status == PKEXEC_NOTAUTH)
		return (g_strdup("Not authorized by polkit (only the user at "
		    "the console may do this)."));
	if (r->err != NULL)
		return (g_strdup(r->err));
	return (g_strdup_printf("bsdbt-helper failed (status %d).",
	    r->status));
}

/* ---- status line -------------------------------------------------------- */

static void
set_status(Panel *p, const char *markup)
{
	gtk_label_set_markup(GTK_LABEL(p->status), markup != NULL ? markup :
	    "");
	gtk_widget_set_visible(p->status, markup != NULL && markup[0] != '\0');
}

static void
set_status_text(Panel *p, const char *text)
{
	char *m;

	m = g_markup_escape_text(text, -1);
	set_status(p, m);
	g_free(m);
}

/* ---- refresh: state of adapters, connections, known devices ------------- */

static void
snapshot_free(Snapshot *s)
{
	g_free(s->adapters);
	g_free(s->connected);
	g_free(s->known);
	g_free(s->radios);
	memset(s, 0, sizeof(*s));
}

/*
 * bt.c allocates with malloc(); the snapshot is freed with g_free(),
 * which is free() since GLib 2.46.
 */
static void
refresh_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
	Snapshot *s;
	struct bsdbt_conn *conn;
	int i, j, n;

	(void)src;
	(void)data;
	(void)c;
	s = g_new0(Snapshot, 1);
	s->nadapters = bsdbt_adapters(&s->adapters);
	if (s->nadapters < 0)
		s->nadapters = 0;
	for (i = 0; i < s->nadapters; i++) {
		if (!BSDBT_ADAPTER_UP(&s->adapters[i]))
			continue;
		n = bsdbt_connections(s->adapters[i].node, &conn);
		for (j = 0; j < n; j++) {
			if (conn[j].link_type != NG_HCI_LINK_ACL)
				continue;
			s->connected = g_renew(bdaddr_t, s->connected,
			    s->nconnected + 1);
			bdaddr_copy(&s->connected[s->nconnected++],
			    &conn[j].bdaddr);
		}
		free(conn);
	}
	s->nknown = bsdbt_known(&s->known);
	if (s->nknown < 0)
		s->nknown = 0;
	s->nradios = bsdbt_radios(&s->radios);
	if (s->nradios < 0)
		s->nradios = 0;
	g_task_return_pointer(task, s, NULL);
}

static void
refresh_done(GObject *src, GAsyncResult *res, gpointer data)
{
	Panel *p = data;
	Snapshot *s;

	(void)src;
	s = g_task_propagate_pointer(G_TASK(res), NULL);
	p->refreshing = FALSE;
	if (!p->dead && s != NULL) {
		snapshot_free(&p->snap);
		p->snap = *s;
		render(p);
	} else if (s != NULL)
		snapshot_free(s);
	g_free(s);
	panel_unref(p);
}

static void
refresh(Panel *p)
{
	GTask *task;

	if (p->refreshing || p->dead)
		return;
	p->refreshing = TRUE;
	task = g_task_new(NULL, NULL, refresh_done, panel_ref(p));
	g_task_run_in_thread(task, refresh_thread);
	g_object_unref(task);
}

static gboolean
on_timer(gpointer data)
{
	refresh(data);
	return (G_SOURCE_CONTINUE);
}

static void
set_timer(Panel *p, int seconds)
{
	if (p->timer != 0)
		g_source_remove(p->timer);
	p->timer = g_timeout_add_seconds(seconds, on_timer, p);
}

/* ---- looking at the snapshot -------------------------------------------- */

static const struct bsdbt_adapter *
adapter_up(const Panel *p)
{
	int i;

	for (i = 0; i < p->snap.nadapters; i++)
		if (BSDBT_ADAPTER_UP(&p->snap.adapters[i]))
			return (&p->snap.adapters[i]);
	return (NULL);
}

static gboolean
is_connected(const Panel *p, const bdaddr_t *ba)
{
	int i;

	for (i = 0; i < p->snap.nconnected; i++)
		if (bdaddr_same(&p->snap.connected[i], ba))
			return (TRUE);
	return (FALSE);
}

static gboolean
is_known(const Panel *p, const bdaddr_t *ba)
{
	int i;

	for (i = 0; i < p->snap.nknown; i++)
		if (bdaddr_same(&p->snap.known[i].bdaddr, ba))
			return (TRUE);
	return (FALSE);
}

/* The device to switch on or off: that of an adapter, or of the radio. */
static gboolean
power_device(const Panel *p, char *dev, size_t len)
{
	size_t n;

	if (p->snap.nadapters > 0) {
		strlcpy(dev, p->snap.adapters[0].node, len);
		n = strlen(dev);
		if (n > 3 && strcmp(dev + n - 3, "hci") == 0)
			dev[n - 3] = '\0';
		return (TRUE);
	}
	if (p->snap.nradios > 0) {
		strlcpy(dev, p->snap.radios[0].dev, len);
		return (TRUE);
	}
	return (FALSE);
}

static const char *
class_icon(const uint8_t class[NG_HCI_CLASS_SIZE])
{
	const char *s;

	s = bsdbt_class_str(class);
	if (strcmp(s, "keyboard") == 0 || strcmp(s, "keyboard+mouse") == 0)
		return ("input-keyboard-symbolic");
	if (strcmp(s, "mouse") == 0)
		return ("input-mouse-symbolic");
	if (strcmp(s, "phone") == 0)
		return ("phone-symbolic");
	if (strcmp(s, "computer") == 0)
		return ("computer-symbolic");
	if (strcmp(s, "audio") == 0)
		return ("audio-headphones-symbolic");
	return ("bluetooth-symbolic");
}

/* Known devices have no class; an input device is a mouse or keyboard. */
static const char *
known_icon(const struct bsdbt_known *k)
{
	char *lower;
	const char *icon;

	if (!k->hid)
		return ("bluetooth-symbolic");
	lower = g_ascii_strdown(k->name, -1);
	icon = strstr(lower, "keyboard") != NULL || strstr(lower, "kbd") !=
	    NULL ? "input-keyboard-symbolic" : "input-mouse-symbolic";
	g_free(lower);
	return (icon);
}

/* ---- actions ------------------------------------------------------------ */

typedef struct {
	Panel		*p;
	char		*args[5];	/* for the helper */
	char		*name;		/* the device, for messages */
	gboolean	pair;		/* then "hid" */
	gboolean	keyboard;	/* the PIN was typed on it */
	HelperResult	r, hid;
} Action;

static void
action_free(Action *a)
{
	int i;

	for (i = 0; i < (int)G_N_ELEMENTS(a->args); i++)
		g_free(a->args[i]);
	g_free(a->name);
	g_free(a->r.err);
	g_free(a->hid.err);
	g_free(a);
}

static void
action_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
	Action *a = data;
	const char *hid[3];

	(void)src;
	(void)c;
	run_helper((const char *const *)a->args, &a->r);
	if (a->pair && a->r.status == 0) {
		hid[0] = "hid";
		hid[1] = a->args[1];
		hid[2] = NULL;
		run_helper(hid, &a->hid);
	}
	g_task_return_boolean(task, TRUE);
}

static void	pair_with_pin(Panel *, const bdaddr_t *, const char *,
		    const char *, gboolean);

/* The device refused the PIN: ask for the one in its manual, here. */
static void
ask_pin(Panel *p, const char *addr, const char *name)
{
	char *m;

	if (!bt_aton(addr, &p->pin_bdaddr))
		return;
	g_free(p->pin_name);
	p->pin_name = g_strdup(name);
	m = g_strdup_printf("%s refused PIN 0000.  Enter the PIN from its "
	    "manual (often 1234):", name);
	gtk_label_set_text(GTK_LABEL(p->pin_label), m);
	g_free(m);
	gtk_entry_set_text(GTK_ENTRY(p->pin_entry), "");
	gtk_widget_show(p->pin_row);
	gtk_widget_grab_focus(p->pin_entry);
}

static void
on_pin_ok(GtkWidget *w, Panel *p)
{
	char *pin;

	(void)w;
	pin = g_strdup(gtk_entry_get_text(GTK_ENTRY(p->pin_entry)));
	g_strstrip(pin);
	if (pin[0] != '\0' && !p->busy) {
		gtk_widget_hide(p->pin_row);
		pair_with_pin(p, &p->pin_bdaddr, p->pin_name, pin, FALSE);
	}
	g_free(pin);
}

static void
on_pin_cancel(GtkButton *b, Panel *p)
{
	(void)b;
	gtk_widget_hide(p->pin_row);
}

static void
nearby_remove(Panel *p, const bdaddr_t *ba)
{
	guint i;

	for (i = 0; i < p->nearby->len; i++)
		if (bdaddr_same(&g_array_index(p->nearby, Nearby, i).bdaddr,
		    ba)) {
			g_array_remove_index(p->nearby, i);
			return;
		}
}

static void
action_done(GObject *src, GAsyncResult *res, gpointer data)
{
	Action *a = data;
	Panel *p = a->p;
	char *msg, *m;
	bdaddr_t ba;

	(void)src;
	(void)res;
	p->busy = FALSE;
	memset(&p->pairing, 0, sizeof(p->pairing));
	gtk_spinner_stop(GTK_SPINNER(p->spinner));
	if (p->dead)
		goto out;

	if (a->r.status == 0) {
		if (a->pair) {
			if (a->hid.status == 0)
				msg = g_strdup_printf("%s is paired and set up "
				    "as an input device.", a->name);
			else if (a->hid.status == HELPER_NOHID)
				msg = g_strdup_printf("%s is paired.",
				    a->name);
			else {
				m = helper_message(&a->hid);
				msg = g_strdup_printf("%s is paired, but not "
				    "set up as an input device: %s", a->name,
				    m);
				g_free(m);
			}
			/* It is in the device list now. */
			if (bt_aton(a->args[1], &ba))
				nearby_remove(p, &ba);
			set_status_text(p, msg);
			g_free(msg);
		} else
			set_status(p, NULL);
	} else if (a->pair && a->r.status == HELPER_REFUSED && !a->keyboard) {
		set_status(p, NULL);
		ask_pin(p, a->args[1], a->name);
	} else if (a->pair && a->r.status == HELPER_NOANSWER) {
		msg = g_strdup_printf("%s did not answer.  Is it on and in "
		    "pairing mode?", a->name);
		set_status_text(p, msg);
		g_free(msg);
	} else {
		msg = helper_message(&a->r);
		set_status_text(p, msg);
		g_free(msg);
	}
	render(p);
	refresh(p);
out:
	action_free(a);
	panel_unref(p);
}

/* Start the helper; args are copied. */
static void
start_action(Panel *p, const char *const *args, const char *name,
    gboolean pair, gboolean keyboard)
{
	GTask *task;
	Action *a;
	int i;

	if (p->busy)
		return;
	a = g_new0(Action, 1);
	a->p = panel_ref(p);
	for (i = 0; args[i] != NULL && i < (int)G_N_ELEMENTS(a->args) - 1;
	    i++)
		a->args[i] = g_strdup(args[i]);
	a->name = g_strdup(name);
	a->pair = pair;
	a->keyboard = keyboard;
	p->busy = TRUE;
	gtk_spinner_start(GTK_SPINNER(p->spinner));
	render(p);
	task = g_task_new(NULL, NULL, action_done, a);
	g_task_set_task_data(task, a, NULL);
	g_task_run_in_thread(task, action_thread);
	g_object_unref(task);
}

static void
pair_with_pin(Panel *p, const bdaddr_t *ba, const char *name,
    const char *pin, gboolean keyboard)
{
	const char *args[5];
	char addr[32], *m;

	if (keyboard)
		m = g_markup_printf_escaped("Type <b><big>%s</big></b> on "
		    "%s, then press Enter.", pin, name);
	else
		m = g_markup_printf_escaped("Pairing with %s...", name);
	set_status(p, m);
	g_free(m);
	bdaddr_copy(&p->pairing, ba);
	args[0] = "pair";
	args[1] = bt_ntoa(ba, addr);
	args[2] = pin;
	args[3] = name[0] != '\0' ? name : NULL;
	args[4] = NULL;
	start_action(p, args, name[0] != '\0' ? name : addr, TRUE, keyboard);
}

/*
 * A keyboard is paired with a PIN typed on it; most other devices have a
 * fixed PIN, usually 0000.
 */
static void
on_nearby_activated(GtkListBox *box, GtkListBoxRow *row, Panel *p)
{
	const Nearby *n;
	const char *kind;
	char pin[8];
	gboolean keyboard;

	(void)box;
	n = g_object_get_data(G_OBJECT(row), "nearby");
	if (n == NULL || p->busy || p->scanning)
		return;
	kind = bsdbt_class_str(n->class);
	keyboard = strcmp(kind, "keyboard") == 0 ||
	    strcmp(kind, "keyboard+mouse") == 0;
	if (keyboard)
		snprintf(pin, sizeof(pin), "%06u", g_random_int_range(0,
		    1000000));
	else
		strlcpy(pin, "0000", sizeof(pin));
	pair_with_pin(p, &n->bdaddr, n->name, pin, keyboard);
}

static void
on_disconnect(GtkButton *b, Panel *p)
{
	const struct bsdbt_known *k;
	const char *args[3];
	char addr[32];

	k = g_object_get_data(G_OBJECT(b), "known");
	args[0] = "disconnect";
	args[1] = bt_ntoa(&k->bdaddr, addr);
	args[2] = NULL;
	set_status(p, NULL);
	start_action(p, args, k->name[0] != '\0' ? k->name : addr, FALSE,
	    FALSE);
}

static gboolean
disarm_remove(gpointer data)
{
	GtkWidget *b = data;

	if (g_object_get_data(G_OBJECT(b), "armed") != NULL) {
		g_object_set_data(G_OBJECT(b), "armed", NULL);
		gtk_button_set_label(GTK_BUTTON(b), "Remove");
		gtk_style_context_remove_class(gtk_widget_get_style_context(b),
		    "destructive-action");
	}
	g_object_unref(b);
	return (G_SOURCE_REMOVE);
}

/* The first click asks "Remove?"; a second one within 4 s removes. */
static void
on_remove(GtkButton *b, Panel *p)
{
	const struct bsdbt_known *k;
	const char *args[3];
	char addr[32];

	k = g_object_get_data(G_OBJECT(b), "known");
	if (g_object_get_data(G_OBJECT(b), "armed") == NULL) {
		g_object_set_data(G_OBJECT(b), "armed", GINT_TO_POINTER(1));
		gtk_button_set_label(b, "Remove?");
		gtk_style_context_add_class(gtk_widget_get_style_context(
		    GTK_WIDGET(b)), "destructive-action");
		g_timeout_add_seconds(4, disarm_remove, g_object_ref(b));
		return;
	}
	args[0] = "remove";
	args[1] = bt_ntoa(&k->bdaddr, addr);
	args[2] = NULL;
	set_status(p, NULL);
	memset(&p->expanded, 0, sizeof(p->expanded));
	start_action(p, args, k->name[0] != '\0' ? k->name : addr, FALSE,
	    FALSE);
}

static void
on_power(GObject *sw, GParamSpec *pspec, Panel *p)
{
	const char *args[4];
	char dev[16];
	gboolean on;

	(void)pspec;
	on = gtk_switch_get_active(GTK_SWITCH(sw));
	if (on == (adapter_up(p) != NULL) || !power_device(p, dev,
	    sizeof(dev))) {
		render(p);
		return;
	}
	args[0] = "power";
	args[1] = dev;
	args[2] = on ? "on" : "off";
	args[3] = NULL;
	set_status_text(p, on ? "Switching Bluetooth on..." :
	    "Switching Bluetooth off...");
	start_action(p, args, dev, FALSE, FALSE);
}

/* ---- search ------------------------------------------------------------- */

typedef struct {
	Panel		*p;
	Nearby		n;
} Found;

/* A device found: add it (main loop). */
static gboolean
found_idle(gpointer data)
{
	Found *f = data;
	Panel *p = f->p;
	guint i;

	if (!p->dead) {
		for (i = 0; i < p->nearby->len; i++)
			if (bdaddr_same(&g_array_index(p->nearby, Nearby,
			    i).bdaddr, &f->n.bdaddr))
				break;
		if (i == p->nearby->len)
			g_array_append_val(p->nearby, f->n);
		else
			g_array_index(p->nearby, Nearby, i) = f->n;
		render(p);
	}
	panel_unref(p);
	g_free(f);
	return (G_SOURCE_REMOVE);
}

typedef struct {
	Panel		*p;
	char		node[HCI_DEVNAME_SIZE];
} ScanJob;

static void
scan_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
	ScanJob *job = data;
	Panel *p = job->p;
	const char *node = job->node;
	struct bsdbt_device *d;
	Found *f;
	int i, n;

	(void)src;
	(void)c;
	n = bsdbt_scan(node, SCAN_SECONDS, &d);
	if (n < 0) {
		g_task_return_int(task, -1);
		return;
	}
	/* Names one by one, each shown as soon as it is known. */
	for (i = 0; i < n && !p->dead; i++) {
		f = g_new0(Found, 1);
		f->p = panel_ref(p);
		bdaddr_copy(&f->n.bdaddr, &d[i].bdaddr);
		memcpy(f->n.class, d[i].class, sizeof(f->n.class));
		if (bsdbt_remote_name(node, &d[i].bdaddr, &d[i], f->n.name,
		    sizeof(f->n.name)) < 0)
			f->n.name[0] = '\0';
		g_idle_add(found_idle, f);
	}
	free(d);
	g_task_return_int(task, n);
}

static void
scan_done(GObject *src, GAsyncResult *res, gpointer data)
{
	Panel *p = data;
	gssize n;

	(void)src;
	n = g_task_propagate_int(G_TASK(res), NULL);
	p->scanning = FALSE;
	if (!p->dead) {
		gtk_spinner_stop(GTK_SPINNER(p->spinner));
		/*
		 * Not p->nearby->len: the devices found are added from idle
		 * callbacks, which may run after this one.
		 */
		if (n < 0)
			set_status_text(p, "The search failed.");
		else if (n == 0)
			set_status_text(p, "Nothing found.  Put the device in "
			    "pairing mode and search again.");
		render(p);
	}
	panel_unref(p);
}

static void
on_search(GtkButton *b, Panel *p)
{
	const struct bsdbt_adapter *a;
	ScanJob *job;
	GTask *task;

	(void)b;
	a = adapter_up(p);
	if (p->scanning || p->busy || a == NULL)
		return;
	p->scanning = TRUE;
	g_array_set_size(p->nearby, 0);
	set_status(p, NULL);
	gtk_spinner_start(GTK_SPINNER(p->spinner));
	render(p);
	job = g_new0(ScanJob, 1);
	job->p = panel_ref(p);
	strlcpy(job->node, a->node, sizeof(job->node));
	task = g_task_new(NULL, NULL, scan_done, p);
	g_task_set_task_data(task, job, g_free);
	g_task_run_in_thread(task, scan_thread);
	g_object_unref(task);
}

/* ---- view --------------------------------------------------------------- */

static void
clear_box(GtkWidget *box)
{
	GList *children, *l;

	children = gtk_container_get_children(GTK_CONTAINER(box));
	for (l = children; l != NULL; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(children);
}

/* Icon, name over a dim second line, and `end' (or nothing) at the end. */
static GtkWidget *
device_line(const char *icon, const char *name, const char *sub,
    GtkWidget *end)
{
	GtkWidget *line, *img, *text, *l1, *l2;
	char *m;

	line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	l1 = gtk_label_new(name);
	gtk_label_set_xalign(GTK_LABEL(l1), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(l1), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(l1), 1);
	m = g_markup_printf_escaped("<small>%s</small>", sub);
	l2 = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(l2), m);
	g_free(m);
	gtk_label_set_xalign(GTK_LABEL(l2), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(l2), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(l2), 1);
	gtk_style_context_add_class(gtk_widget_get_style_context(l2),
	    "dim-label");
	gtk_box_pack_start(GTK_BOX(text), l1, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text), l2, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(line), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(line), text, TRUE, TRUE, 0);
	if (end != NULL) {
		gtk_widget_set_valign(end, GTK_ALIGN_CENTER);
		gtk_box_pack_end(GTK_BOX(line), end, FALSE, FALSE, 0);
	}
	return (line);
}

/*
 * A list row carrying a copy of its device: the snapshot or the list it
 * came from is replaced while the row stays.
 */
static GtkWidget *
list_row(GtkWidget *child, const char *key, gconstpointer data, gsize size,
    const char *tip)
{
	GtkWidget *row;

	row = gtk_list_box_row_new();
	gtk_container_add(GTK_CONTAINER(row), child);
	g_object_set_data_full(G_OBJECT(row), key, g_memdup2(data, size),
	    g_free);
	gtk_widget_set_tooltip_text(row, tip);
	gtk_widget_show_all(row);
	return (row);
}

/* A borderless button, for the actions under a device. */
static GtkWidget *
flat_button(const char *label, const char *tip, GCallback cb, Panel *p,
    gconstpointer known, gsize size)
{
	GtkWidget *b;

	b = gtk_button_new_with_label(label);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(b, tip);
	g_object_set_data_full(G_OBJECT(b), "known", g_memdup2(known, size),
	    g_free);
	g_signal_connect(b, "clicked", cb, p);
	return (b);
}

/* A click on a device shows its actions, or hides them again. */
static void
on_known_activated(GtkListBox *box, GtkListBoxRow *row, Panel *p)
{
	const struct bsdbt_known *k;

	(void)box;
	k = g_object_get_data(G_OBJECT(row), "known");
	if (k == NULL)
		return;
	if (bdaddr_same(&p->expanded, &k->bdaddr))
		memset(&p->expanded, 0, sizeof(p->expanded));
	else
		bdaddr_copy(&p->expanded, &k->bdaddr);
	g_free(p->known_key);
	p->known_key = NULL;
	render(p);
}

/*
 * The known devices: a line each, and under the one clicked, its actions.
 * Rebuilt only when what they show changed: a refresh every few seconds
 * must not take a button away under the pointer.
 */
static void
render_known(Panel *p)
{
	const struct bsdbt_known *k;
	GtkWidget *box, *actions, *arrow, *b;
	GString *key;
	char addr[32], sub[64];
	gboolean conn, open;
	int i;

	key = g_string_new(NULL);
	g_string_append_printf(key, "%d %s|", p->busy,
	    bt_ntoa(&p->expanded, addr));
	for (i = 0; i < p->snap.nknown; i++) {
		k = &p->snap.known[i];
		g_string_append_printf(key, "%s %d %d %d %s|",
		    bt_ntoa(&k->bdaddr, addr), k->paired, k->hid,
		    is_connected(p, &k->bdaddr), k->name);
	}
	if (g_strcmp0(key->str, p->known_key) == 0) {
		g_string_free(key, TRUE);
		return;
	}
	g_free(p->known_key);
	p->known_key = g_string_free(key, FALSE);

	clear_box(p->known_box);
	for (i = 0; i < p->snap.nknown; i++) {
		k = &p->snap.known[i];
		conn = is_connected(p, &k->bdaddr);
		open = bdaddr_same(&p->expanded, &k->bdaddr);
		snprintf(sub, sizeof(sub), "%s",
		    conn ? "Connected" : k->paired ? "Not connected" :
		    "Not paired");
		arrow = gtk_image_new_from_icon_name(open ?
		    "pan-down-symbolic" : "pan-end-symbolic",
		    GTK_ICON_SIZE_MENU);
		gtk_style_context_add_class(gtk_widget_get_style_context(arrow),
		    "dim-label");
		box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_box_pack_start(GTK_BOX(box), device_line(known_icon(k),
		    k->name[0] != '\0' ? k->name : bt_ntoa(&k->bdaddr, addr),
		    sub, arrow), FALSE, FALSE, 0);
		if (open) {
			actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
			gtk_widget_set_halign(actions, GTK_ALIGN_END);
			if (conn) {
				b = flat_button("Disconnect", "Close the "
				    "connection.  An input device connects "
				    "again when you use it.",
				    G_CALLBACK(on_disconnect), p, k,
				    sizeof(*k));
				gtk_widget_set_sensitive(b, !p->busy);
				gtk_box_pack_start(GTK_BOX(actions), b, FALSE,
				    FALSE, 0);
			}
			b = flat_button("Remove", "Forget the device: unpair "
			    "it and stop using it", G_CALLBACK(on_remove), p, k,
			    sizeof(*k));
			gtk_widget_set_sensitive(b, !p->busy);
			gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE,
			    0);
			gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE,
			    0);
		}
		bt_ntoa(&k->bdaddr, addr);
		gtk_container_add(GTK_CONTAINER(p->known_box), list_row(box,
		    "known", k, sizeof(*k), addr));
	}
	gtk_widget_set_visible(p->known_empty, p->snap.nknown == 0);
}

/* A click on a device in range pairs with it. */
static void
render_nearby(Panel *p)
{
	Nearby *n;
	GtkWidget *end;
	char addr[32], *tip, *sub;
	guint i;
	int shown;
	gboolean pairing;

	clear_box(p->nearby_box);
	shown = 0;
	for (i = 0; i < p->nearby->len; i++) {
		n = &g_array_index(p->nearby, Nearby, i);
		if (is_known(p, &n->bdaddr))
			continue;
		pairing = p->busy && bdaddr_same(&p->pairing, &n->bdaddr);
		end = NULL;
		/* What kind of device it is, as blueman shows. */
		sub = g_strdup_printf("%c%s · %s",
		    g_ascii_toupper(bsdbt_class_str(n->class)[0]),
		    bsdbt_class_str(n->class) + 1,
		    pairing ? "pairing..." : "click to pair");
		if (pairing) {
			end = gtk_spinner_new();
			gtk_spinner_start(GTK_SPINNER(end));
		}
		bt_ntoa(&n->bdaddr, addr);
		tip = g_strdup(addr);
		gtk_container_add(GTK_CONTAINER(p->nearby_box), list_row(
		    device_line(class_icon(n->class), n->name[0] != '\0' ?
		    n->name : addr, sub, end), "nearby", n, sizeof(*n), tip));
		g_free(tip);
		g_free(sub);
		shown++;
	}
	gtk_widget_set_sensitive(p->nearby_box, !p->busy && !p->scanning);
	gtk_widget_set_visible(p->nearby_empty, shown == 0 && !p->scanning);
}

static void
render_tooltip(Panel *p)
{
	const struct bsdbt_adapter *a;
	GString *tip;
	int i, nconn;

	a = adapter_up(p);
	tip = g_string_new(NULL);
	if (a == NULL)
		g_string_append(tip, p->snap.nradios > 0 ||
		    p->snap.nadapters > 0 ? "Bluetooth is off" :
		    "No Bluetooth adapter");
	else {
		g_string_append(tip, "Bluetooth is on");
		nconn = 0;
		for (i = 0; i < p->snap.nknown; i++)
			if (is_connected(p, &p->snap.known[i].bdaddr)) {
				g_string_append_printf(tip, "%s%s",
				    nconn++ == 0 ? "\nConnected: " : ", ",
				    p->snap.known[i].name);
			}
	}
	gtk_widget_set_tooltip_text(p->button, tip->str);
	g_string_free(tip, TRUE);
}

static void
render(Panel *p)
{
	const struct bsdbt_adapter *a;
	char *m;
	gboolean on, can_power;
	char dev[16];

	a = adapter_up(p);
	on = a != NULL;
	/* What a search found is of no use once Bluetooth is off. */
	if (!on && !p->scanning)
		g_array_set_size(p->nearby, 0);
	gtk_image_set_from_icon_name(GTK_IMAGE(p->icon), on ? ICON_ON :
	    ICON_OFF, GTK_ICON_SIZE_BUTTON);
	gtk_image_set_pixel_size(GTK_IMAGE(p->icon),
	    xfce_panel_plugin_get_icon_size(p->plugin));
	render_tooltip(p);

	can_power = power_device(p, dev, sizeof(dev));
	g_signal_handlers_block_by_func(p->power, on_power, p);
	gtk_switch_set_active(GTK_SWITCH(p->power), on);
	g_signal_handlers_unblock_by_func(p->power, on_power, p);
	gtk_widget_set_sensitive(p->power, can_power && !p->busy &&
	    !p->scanning);
	if (on)
		m = g_markup_printf_escaped("<small>%s</small>",
		    a->name[0] != '\0' ? a->name : a->node);
	else if (can_power)
		m = g_markup_printf_escaped("<small>%s</small>", dev);
	else
		m = g_strdup("<small>No Bluetooth adapter found</small>");
	gtk_label_set_markup(GTK_LABEL(p->adapter_label), m);
	g_free(m);

	gtk_widget_set_visible(p->nearby_section, on);
	gtk_widget_set_visible(p->off_label, !on && can_power);
	gtk_widget_set_sensitive(p->search, on && !p->busy && !p->scanning);
	gtk_button_set_label(GTK_BUTTON(p->search), p->scanning ?
	    "Searching..." : "Search");
	render_known(p);
	render_nearby(p);
}

/* ---- popup window (as in xfce4-bsdthinkpad-plugin) ---------------------- */

/* Next to the panel button, for the popup's current size. */
static void
popup_place(Panel *p)
{
	gint x, y;

	xfce_panel_plugin_position_widget(p->plugin, p->popup, p->button,
	    &x, &y);
	gtk_window_move(GTK_WINDOW(p->popup), x, y);
}

/*
 * Rows come and go; place the popup again for its new size, or it would
 * grow off the screen when the panel is at the bottom.
 */
static void
on_popup_size(GtkWidget *w, GdkRectangle *alloc, Panel *p)
{
	(void)w;
	if (alloc->width != p->popup_width ||
	    alloc->height != p->popup_height) {
		p->popup_width = alloc->width;
		p->popup_height = alloc->height;
		popup_place(p);
	}
}

static void
popup_show(Panel *p)
{
	xfce_panel_plugin_block_autohide(p->plugin, TRUE);
	gtk_widget_realize(p->popup);
	popup_place(p);
	gtk_window_present_with_time(GTK_WINDOW(p->popup),
	    gtk_get_current_event_time());
	set_timer(p, REFRESH_OPEN_S);
	refresh(p);
}

static void
popup_hide(Panel *p)
{
	if (p->grabbed) {
		gdk_seat_ungrab(gdk_display_get_default_seat(
		    gtk_widget_get_display(p->popup)));
		p->grabbed = FALSE;
	}
	if (gtk_widget_get_visible(p->popup)) {
		gtk_widget_hide(p->popup);
		xfce_panel_plugin_block_autohide(p->plugin, FALSE);
	}
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->button)))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->button),
		    FALSE);
	p->hidden_at = g_get_monotonic_time();
	/* Open again with the devices' actions put away. */
	if (!bdaddr_any(&p->expanded)) {
		memset(&p->expanded, 0, sizeof(p->expanded));
		render(p);
	}
	set_timer(p, REFRESH_IDLE_S);
}

/*
 * A click on the panel button while the popup is open closes the popup
 * (on_popup_button()).  Should the button get the same click as well, it
 * would open the popup again at once: a button that turns on right after
 * the popup closed is turned off.
 */
static void
on_toggled(GtkToggleButton *button, Panel *p)
{
	if (gtk_toggle_button_get_active(button)) {
		if (g_get_monotonic_time() - p->hidden_at < 300000) {
			g_signal_handlers_block_by_func(button, on_toggled, p);
			gtk_toggle_button_set_active(button, FALSE);
			g_signal_handlers_unblock_by_func(button, on_toggled,
			    p);
			return;
		}
		popup_show(p);
	} else
		popup_hide(p);
}

/* Grab once the window is on screen (a grab needs a viewable window). */
static gboolean
on_popup_map(GtkWidget *w, GdkEvent *ev, Panel *p)
{
	GdkSeat *seat;

	(void)ev;
	seat = gdk_display_get_default_seat(gtk_widget_get_display(w));
	p->grabbed = gdk_seat_grab(seat, gtk_widget_get_window(w),
	    GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL, NULL, NULL) ==
	    GDK_GRAB_SUCCESS;
	return (FALSE);
}

/*
 * A click outside the popup arrives here because of the grab.  Its x and
 * y are not always relative to the popup: a click on the panel button
 * comes with the button's own coordinates, which can fall inside the
 * popup's area.  So compare screen coordinates.
 */
static gboolean
on_popup_button(GtkWidget *w, GdkEventButton *ev, Panel *p)
{
	gint ox, oy;

	if (ev->window != gtk_widget_get_window(w))
		return (FALSE);
	gdk_window_get_origin(gtk_widget_get_window(w), &ox, &oy);
	if (ev->x_root >= ox && ev->y_root >= oy &&
	    ev->x_root < ox + gtk_widget_get_allocated_width(w) &&
	    ev->y_root < oy + gtk_widget_get_allocated_height(w))
		return (FALSE);
	popup_hide(p);
	return (TRUE);
}

/* Another program took the grab (e.g. a menu): close. */
static gboolean
on_popup_grab_broken(GtkWidget *w, GdkEventGrabBroken *ev, Panel *p)
{
	(void)w;
	if (ev->grab_window == NULL) {
		p->grabbed = FALSE;
		popup_hide(p);
	}
	return (FALSE);
}

static gboolean
on_popup_key(GtkWidget *w, GdkEventKey *ev, Panel *p)
{
	(void)w;
	if (ev->keyval != GDK_KEY_Escape)
		return (FALSE);
	popup_hide(p);
	return (TRUE);
}

static gboolean
on_popup_delete(GtkWidget *w, GdkEvent *ev, Panel *p)
{
	(void)w;
	(void)ev;
	popup_hide(p);
	return (TRUE);
}

static GtkWidget *
heading(const char *text)
{
	GtkWidget *l;
	char *m;

	m = g_markup_printf_escaped("<b>%s</b>", text);
	l = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(l), m);
	g_free(m);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0);
	return (l);
}

static GtkWidget *
dim_label(const char *text)
{
	GtkWidget *l;

	l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(l),
	    "dim-label");
	return (l);
}

/*
 * The lists sit on the popup's background, with rows that light up under
 * the pointer (Adwaita draws lists on a white, framed background).  The
 * plugin runs in a process of its own, so the CSS reaches nothing else.
 */
static void
load_css(void)
{
	static const char css[] =
	    ".bsdbt-list { background-color: transparent; }\n"
	    ".bsdbt-list row { padding: 4px 6px; border-radius: 6px; }\n";
	GtkCssProvider *provider;

	provider = gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
	    GTK_STYLE_PROVIDER(provider),
	    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
}

static GtkWidget *
device_list(void)
{
	GtkWidget *list;

	list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list),
	    GTK_SELECTION_NONE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(list),
	    "bsdbt-list");
	return (list);
}

static void
build_popup(Panel *p)
{
	GtkWidget *box, *row, *text;

	p->popup = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(p->popup), "Bluetooth");
	gtk_window_set_decorated(GTK_WINDOW(p->popup), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(p->popup), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(p->popup), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(p->popup), TRUE);
	gtk_window_set_keep_above(GTK_WINDOW(p->popup), TRUE);
	gtk_window_set_type_hint(GTK_WINDOW(p->popup),
	    GDK_WINDOW_TYPE_HINT_UTILITY);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	g_object_set(box, "margin", 12, NULL);
	/* A fixed width: rows with more buttons must not widen it. */
	gtk_widget_set_size_request(box, POPUP_WIDTH, -1);

	/* Bluetooth  [adapter]  (spinner) [switch] */
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	p->adapter_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->adapter_label), 0.0);
	gtk_style_context_add_class(gtk_widget_get_style_context(
	    p->adapter_label), "dim-label");
	gtk_box_pack_start(GTK_BOX(text), heading("Bluetooth"), FALSE, FALSE,
	    0);
	gtk_box_pack_start(GTK_BOX(text), p->adapter_label, FALSE, FALSE, 0);
	p->spinner = gtk_spinner_new();
	p->power = gtk_switch_new();
	gtk_widget_set_valign(p->power, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(p->power, "Start or stop the Bluetooth "
	    "stack on the adapter (service bluetooth)");
	gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name(ICON_ON,
	    GTK_ICON_SIZE_LARGE_TOOLBAR), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), p->power, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), p->spinner, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box),
	    gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

	/* Devices */
	gtk_box_pack_start(GTK_BOX(box), heading("Devices"), FALSE, FALSE, 0);
	p->known_box = device_list();
	g_signal_connect(p->known_box, "row-activated",
	    G_CALLBACK(on_known_activated), p);
	gtk_box_pack_start(GTK_BOX(box), p->known_box, FALSE, FALSE, 0);
	p->known_empty = dim_label("None yet.");
	gtk_box_pack_start(GTK_BOX(box), p->known_empty, FALSE, FALSE, 0);
	p->off_label = dim_label("Bluetooth is off.");
	gtk_box_pack_start(GTK_BOX(box), p->off_label, FALSE, FALSE, 0);

	/* Nearby  [Search], only while Bluetooth is on */
	p->nearby_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_box_pack_start(GTK_BOX(p->nearby_section),
	    gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	p->search = gtk_button_new_with_label("Search");
	gtk_button_set_relief(GTK_BUTTON(p->search), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(p->search, "Look for devices in pairing "
	    "mode (about 10 seconds)");
	g_signal_connect(p->search, "clicked", G_CALLBACK(on_search), p);
	gtk_box_pack_start(GTK_BOX(row), heading("Nearby"), FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), p->search, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p->nearby_section), row, FALSE, FALSE, 0);
	p->nearby_box = device_list();
	g_signal_connect(p->nearby_box, "row-activated",
	    G_CALLBACK(on_nearby_activated), p);
	gtk_box_pack_start(GTK_BOX(p->nearby_section), p->nearby_box, FALSE,
	    FALSE, 0);
	p->nearby_empty = dim_label("Put a device in pairing mode, then "
	    "search.");
	gtk_box_pack_start(GTK_BOX(p->nearby_section), p->nearby_empty, FALSE,
	    FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), p->nearby_section, FALSE, FALSE, 0);

	/* What is going on, or what went wrong */
	p->status = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->status), 0.0);
	gtk_label_set_line_wrap(GTK_LABEL(p->status), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(p->status), 1);
	gtk_box_pack_start(GTK_BOX(box), p->status, FALSE, FALSE, 0);

	/* PIN for a device that refused 0000 */
	p->pin_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	p->pin_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->pin_label), 0.0);
	gtk_label_set_line_wrap(GTK_LABEL(p->pin_label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(p->pin_label), 1);
	gtk_box_pack_start(GTK_BOX(p->pin_row), p->pin_label, FALSE, FALSE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	p->pin_entry = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(p->pin_entry), NG_HCI_PIN_SIZE);
	g_signal_connect(p->pin_entry, "activate", G_CALLBACK(on_pin_ok), p);
	p->pin_ok = gtk_button_new_with_label("Pair");
	g_signal_connect(p->pin_ok, "clicked", G_CALLBACK(on_pin_ok), p);
	text = gtk_button_new_with_label("Cancel");
	g_signal_connect(text, "clicked", G_CALLBACK(on_pin_cancel), p);
	gtk_box_pack_start(GTK_BOX(row), p->pin_entry, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), p->pin_ok, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), text, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p->pin_row), row, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), p->pin_row, FALSE, FALSE, 0);

	g_signal_connect(p->power, "notify::active", G_CALLBACK(on_power), p);

	gtk_container_add(GTK_CONTAINER(p->popup), box);
	gtk_widget_show_all(box);
	gtk_widget_hide(p->status);
	gtk_widget_hide(p->pin_row);
	gtk_widget_hide(p->off_label);
	gtk_widget_add_events(p->popup, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(p->popup, "map-event", G_CALLBACK(on_popup_map), p);
	g_signal_connect(p->popup, "size-allocate", G_CALLBACK(on_popup_size),
	    p);
	g_signal_connect(p->popup, "button-press-event",
	    G_CALLBACK(on_popup_button), p);
	g_signal_connect(p->popup, "grab-broken-event",
	    G_CALLBACK(on_popup_grab_broken), p);
	g_signal_connect(p->popup, "key-press-event", G_CALLBACK(on_popup_key),
	    p);
	g_signal_connect(p->popup, "delete-event", G_CALLBACK(on_popup_delete),
	    p);
}

/* ---- plugin ------------------------------------------------------------- */

static gboolean
on_size_changed(XfcePanelPlugin *plugin, gint size, Panel *p)
{
	size /= xfce_panel_plugin_get_nrows(plugin);
	gtk_widget_set_size_request(p->button, size, size);
	gtk_image_set_pixel_size(GTK_IMAGE(p->icon),
	    xfce_panel_plugin_get_icon_size(plugin));
	return (TRUE);
}

static void
on_free(XfcePanelPlugin *plugin, Panel *p)
{
	(void)plugin;
	p->dead = TRUE;
	if (p->timer != 0)
		g_source_remove(p->timer);
	gtk_widget_destroy(p->popup);
	/* Running tasks hold references; the last one frees. */
	panel_unref(p);
}

static void
construct(XfcePanelPlugin *plugin)
{
	Panel *p;

	p = g_new0(Panel, 1);
	p->refs = 1;
	p->plugin = plugin;
	p->nearby = g_array_new(FALSE, TRUE, sizeof(Nearby));
	p->button = xfce_panel_create_toggle_button();
	p->icon = gtk_image_new_from_icon_name(ICON_OFF, GTK_ICON_SIZE_BUTTON);
	gtk_container_add(GTK_CONTAINER(p->button), p->icon);
	g_signal_connect(p->button, "toggled", G_CALLBACK(on_toggled), p);
	gtk_container_add(GTK_CONTAINER(plugin), p->button);
	xfce_panel_plugin_add_action_widget(plugin, p->button);
	xfce_panel_plugin_set_small(plugin, TRUE);

	load_css();
	build_popup(p);
	g_signal_connect(plugin, "size-changed", G_CALLBACK(on_size_changed),
	    p);
	g_signal_connect(plugin, "free-data", G_CALLBACK(on_free), p);
	gtk_widget_show_all(p->button);
	set_timer(p, REFRESH_IDLE_S);
	refresh(p);
}

XFCE_PANEL_PLUGIN_REGISTER(construct);
