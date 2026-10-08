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
 * Its menu switches Bluetooth on and off, lists the devices set up before
 * (connected or not) and the devices in range, and pairs, disconnects and
 * removes devices.  It is a GtkMenu opened with
 * xfce_panel_plugin_popup_menu(), as the pulseaudio plugin's and the power
 * manager's are, so GTK and the panel do the grabbing and closing.  What
 * needs typing or confirming (a PIN, removing a device) gets a dialog, and
 * results come as desktop notifications.
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
#include <libnotify/notify.h>
#include <libxfce4panel/libxfce4panel.h>

#include "bt.h"

#ifndef BSDBT_HELPER
#define BSDBT_HELPER	"/usr/local/libexec/bsdbt-helper"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION	"unknown"
#endif

/* bsdbt-helper's exit status */
#define HELPER_NOHID	3
#define HELPER_REFUSED	4
#define HELPER_NOANSWER	5
/* pkexec's: the user dismissed the dialog, or polkit said no */
#define PKEXEC_DISMISSED 126
#define PKEXEC_NOTAUTH	127

#define REFRESH_OPEN_S	3	/* menu open: follow connections */
#define REFRESH_IDLE_S	15	/* closed: only the panel icon */
#define SCAN_SECONDS	8

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
	GtkWidget	*menu;
	GtkWidget	*power_item, *power, *known_header, *known_empty;
	GtkWidget	*nearby_sep, *nearby_header;
	GtkWidget	*search_item, *search_label, *search_spinner;
	GtkWidget	*status_sep, *status_item, *status;
	GList		*known_items, *nearby_items;
	char		*known_key;	/* what the device items show */
	char		*nearby_key;

	GtkWidget	*pin_dialog;	/* the PIN to type on a keyboard */
	NotifyNotification *note;

	Snapshot	snap;
	gboolean	refreshing;
	guint		timer;
	guint		reopen;		/* idle source */
	gboolean	reopening;
	GdkEvent	*opened_by;	/* the click that opened the menu */

	GArray		*nearby;	/* of Nearby */
	gboolean	scanning;
	gboolean	busy;		/* the helper is running */
} Panel;

static void	refresh(Panel *);
static void	render(Panel *);
static void	set_timer(Panel *, int);
static void	set_status(Panel *, const char *);

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
	g_free(p->nearby_key);
	g_list_free(p->known_items);
	g_list_free(p->nearby_items);
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

/* ---- telling the user --------------------------------------------------- */

/*
 * Items came or went while the menu is open.  A GtkMenu keeps the size it
 * had when it opened (GTK fixes its window's size once placed), so it would
 * scroll, or grow off the screen above a bottom panel: close it and open it
 * again, and GTK places it for its new size.  With the click that opened
 * it, as the menu needs an event to open with.
 *
 * In two steps: opened again at once, the menu took the events of its own
 * closing (the end of its grab) for a reason to close, and did.
 */
static gboolean
reopen_show(gpointer data)
{
	Panel *p = data;

	p->reopen = 0;
	p->reopening = FALSE;
	if (p->opened_by == NULL || gtk_widget_get_visible(p->menu))
		return (G_SOURCE_REMOVE);
	/* The grab must be newer than the one just released. */
	p->opened_by->button.time = GDK_CURRENT_TIME;
	xfce_panel_plugin_popup_menu(p->plugin, GTK_MENU(p->menu), p->button,
	    p->opened_by);
	if (!gtk_widget_get_visible(p->menu)) {
		g_debug("menu: not shown again (no pointer grab?)");
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->button),
		    FALSE);
		set_status(p, NULL);
		set_timer(p, REFRESH_IDLE_S);
	}
	return (G_SOURCE_REMOVE);
}

static gboolean
reopen_idle(gpointer data)
{
	Panel *p = data;

	p->reopen = 0;
	if (!gtk_widget_get_visible(p->menu) || p->opened_by == NULL)
		return (G_SOURCE_REMOVE);
	g_debug("menu: open again for its new size");
	p->reopening = TRUE;
	gtk_menu_popdown(GTK_MENU(p->menu));
	p->reopen = g_timeout_add(50, reopen_show, p);
	return (G_SOURCE_REMOVE);
}

/* After GTK has worked out the new size: idle sources run after that. */
static void
reposition(Panel *p)
{
	if (gtk_widget_get_visible(p->menu) && p->reopen == 0)
		p->reopen = g_idle_add(reopen_idle, p);
}

/*
 * The last line of the menu: what is going on while it is open (pairing,
 * searching, an error).  Cleared when the menu closes.
 */
static void
set_status(Panel *p, const char *text)
{
	gboolean show, was;

	show = text != NULL && text[0] != '\0';
	was = gtk_widget_get_visible(p->status_item);
	gtk_label_set_text(GTK_LABEL(p->status), show ? text : "");
	gtk_widget_set_visible(p->status_item, show);
	gtk_widget_set_visible(p->status_sep, show);
	if (show || was)
		reposition(p);
}

/*
 * The result of something that goes on after the menu closed (pairing,
 * removing): a notification, updated in place as the pulseaudio plugin's
 * are, so that several results do not pile up.
 */
static void
notify(Panel *p, const char *body, gboolean error)
{
	GError *e;

	g_debug("notify: %s", body);
	if (!notify_is_initted())
		return;
	if (p->note == NULL)
		p->note = notify_notification_new("Bluetooth", body, NULL);
	notify_notification_update(p->note, "Bluetooth", body,
	    error ? "dialog-error" : ICON_ON);
	notify_notification_set_hint(p->note, "transient",
	    g_variant_new_boolean(TRUE));
	e = NULL;
	if (!notify_notification_show(p->note, &e)) {
		g_debug("notify: %s", e->message);
		g_error_free(e);
	}
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

/* A dialog asking for the PIN in a device's manual. */
typedef struct {
	Panel		*p;
	bdaddr_t	bdaddr;
	char		*name;
	GtkWidget	*entry;
} PinAsk;

static void
on_pin_response(GtkDialog *dialog, gint response, PinAsk *a)
{
	char *pin;

	if (response == GTK_RESPONSE_OK && !a->p->dead) {
		pin = g_strdup(gtk_entry_get_text(GTK_ENTRY(a->entry)));
		g_strstrip(pin);
		if (pin[0] != '\0')
			pair_with_pin(a->p, &a->bdaddr, a->name, pin, FALSE);
		g_free(pin);
	}
	gtk_widget_destroy(GTK_WIDGET(dialog));
	panel_unref(a->p);
	g_free(a->name);
	g_free(a);
}

static void
ask_pin(Panel *p, const bdaddr_t *ba, const char *name)
{
	GtkWidget *dialog, *box, *label;
	PinAsk *a;
	char *text;

	a = g_new0(PinAsk, 1);
	a->p = panel_ref(p);
	bdaddr_copy(&a->bdaddr, ba);
	a->name = g_strdup(name);
	dialog = gtk_dialog_new_with_buttons("Bluetooth PIN", NULL, 0,
	    "_Cancel", GTK_RESPONSE_CANCEL, "_Pair", GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
	gtk_window_set_keep_above(GTK_WINDOW(dialog), TRUE);
	box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	g_object_set(box, "margin", 12, "spacing", 8, NULL);
	text = g_strdup_printf("%s refused PIN 0000.  Enter the PIN from its "
	    "manual (often 1234):", name);
	label = gtk_label_new(text);
	g_free(text);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	a->entry = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(a->entry), NG_HCI_PIN_SIZE);
	gtk_entry_set_activates_default(GTK_ENTRY(a->entry), TRUE);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), a->entry, FALSE, FALSE, 0);
	g_signal_connect(dialog, "response", G_CALLBACK(on_pin_response), a);
	gtk_widget_show_all(dialog);
	gtk_window_present(GTK_WINDOW(dialog));
}

/* The PIN to type on a keyboard; closes by itself when pairing ends. */
static void
show_keyboard_pin(Panel *p, const char *name, const char *pin)
{
	GtkWidget *dialog;
	char *m;

	if (p->pin_dialog != NULL)
		gtk_widget_destroy(p->pin_dialog);
	dialog = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_INFO,
	    GTK_BUTTONS_NONE, NULL);
	m = g_markup_printf_escaped("Type <b><big>%s</big></b> on %s, then "
	    "press Enter.", pin, name);
	gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog), m);
	g_free(m);
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
	    "This window closes when pairing is done.");
	gtk_dialog_add_button(GTK_DIALOG(dialog), "_Hide", GTK_RESPONSE_CLOSE);
	gtk_window_set_title(GTK_WINDOW(dialog), "Bluetooth");
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
	gtk_window_set_keep_above(GTK_WINDOW(dialog), TRUE);
	g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy),
	    NULL);
	p->pin_dialog = dialog;
	g_object_add_weak_pointer(G_OBJECT(dialog), (gpointer *)&p->pin_dialog);
	gtk_widget_show_all(dialog);
	gtk_window_present(GTK_WINDOW(dialog));
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
	g_debug("helper %s %s: status %d%s%s", a->args[0], a->args[1],
	    a->r.status, a->r.err != NULL ? ", " : "",
	    a->r.err != NULL ? a->r.err : "");
	p->busy = FALSE;
	if (p->dead)
		goto out;
	if (p->pin_dialog != NULL)
		gtk_widget_destroy(p->pin_dialog);
	set_status(p, NULL);

	if (a->r.status == 0 && a->pair) {
		if (a->hid.status == 0)
			msg = g_strdup_printf("%s is paired and set up as an "
			    "input device.", a->name);
		else if (a->hid.status == HELPER_NOHID)
			msg = g_strdup_printf("%s is paired.", a->name);
		else {
			m = helper_message(&a->hid);
			msg = g_strdup_printf("%s is paired, but not set up as "
			    "an input device: %s", a->name, m);
			g_free(m);
		}
		/* It is a known device now. */
		if (bt_aton(a->args[1], &ba))
			nearby_remove(p, &ba);
		notify(p, msg, FALSE);
		g_free(msg);
	} else if (a->r.status == 0 && strcmp(a->args[0], "remove") == 0) {
		msg = g_strdup_printf("%s is removed.", a->name);
		notify(p, msg, FALSE);
		g_free(msg);
	} else if (a->r.status == 0)
		;	/* disconnect, power: the menu and icon show it */
	else if (a->pair && a->r.status == HELPER_REFUSED && !a->keyboard) {
		if (bt_aton(a->args[1], &ba))
			ask_pin(p, &ba, a->name);
	} else if (a->pair && a->r.status == HELPER_REFUSED) {
		msg = g_strdup_printf("%s refused the PIN.  Try again, and "
		    "type the PIN on it before it times out.", a->name);
		notify(p, msg, TRUE);
		g_free(msg);
	} else if (a->pair && a->r.status == HELPER_NOANSWER) {
		msg = g_strdup_printf("%s did not answer.  Is it on and in "
		    "pairing mode?", a->name);
		notify(p, msg, TRUE);
		g_free(msg);
	} else {
		msg = helper_message(&a->r);
		notify(p, msg, TRUE);
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
	g_debug("helper %s %s", a->args[0], a->args[1]);
	p->busy = TRUE;
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

	if (p->busy)
		return;
	bt_ntoa(ba, addr);
	if (name[0] == '\0')
		name = addr;
	if (keyboard)
		show_keyboard_pin(p, name, pin);
	m = g_strdup_printf("Pairing with %s...", name);
	set_status(p, m);
	if (!keyboard)
		notify(p, m, FALSE);
	g_free(m);
	args[0] = "pair";
	args[1] = addr;
	args[2] = pin;
	args[3] = name != addr ? name : NULL;
	args[4] = NULL;
	start_action(p, args, name, TRUE, keyboard);
}

/*
 * A keyboard is paired with a PIN typed on it; most other devices have a
 * fixed PIN, usually 0000.
 */
static void
on_nearby_activate(GtkMenuItem *item, Panel *p)
{
	const Nearby *n;
	const char *kind;
	char pin[8];
	gboolean keyboard;

	n = g_object_get_data(G_OBJECT(item), "nearby");
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
on_disconnect(GtkMenuItem *item, Panel *p)
{
	const struct bsdbt_known *k;
	const char *args[3];
	char addr[32];

	k = g_object_get_data(G_OBJECT(item), "known");
	args[0] = "disconnect";
	args[1] = bt_ntoa(&k->bdaddr, addr);
	args[2] = NULL;
	start_action(p, args, k->name[0] != '\0' ? k->name : addr, FALSE,
	    FALSE);
}

static void
on_remove_response(GtkDialog *dialog, gint response, Panel *p)
{
	const struct bsdbt_known *k;
	const char *args[3];
	char addr[32];

	k = g_object_get_data(G_OBJECT(dialog), "known");
	if (response == GTK_RESPONSE_OK && !p->dead) {
		args[0] = "remove";
		args[1] = bt_ntoa(&k->bdaddr, addr);
		args[2] = NULL;
		start_action(p, args, k->name[0] != '\0' ? k->name : addr,
		    FALSE, FALSE);
	}
	gtk_widget_destroy(GTK_WIDGET(dialog));
	panel_unref(p);
}

/* Removing a device is asked for again; it would have to be paired again. */
static void
on_remove(GtkMenuItem *item, Panel *p)
{
	const struct bsdbt_known *k;
	GtkWidget *dialog;
	char addr[32];

	k = g_object_get_data(G_OBJECT(item), "known");
	dialog = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_QUESTION,
	    GTK_BUTTONS_NONE, "Remove %s?", k->name[0] != '\0' ? k->name :
	    bt_ntoa(&k->bdaddr, addr));
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
	    "To use it again, it has to be paired again.");
	gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Cancel",
	    GTK_RESPONSE_CANCEL, "_Remove", GTK_RESPONSE_OK, NULL);
	gtk_style_context_add_class(gtk_widget_get_style_context(
	    gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
	    GTK_RESPONSE_OK)), "destructive-action");
	gtk_window_set_title(GTK_WINDOW(dialog), "Bluetooth");
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
	gtk_window_set_keep_above(GTK_WINDOW(dialog), TRUE);
	g_object_set_data_full(G_OBJECT(dialog), "known", g_memdup2(k,
	    sizeof(*k)), g_free);
	g_signal_connect(dialog, "response", G_CALLBACK(on_remove_response),
	    panel_ref(p));
	gtk_widget_show_all(dialog);
	gtk_window_present(GTK_WINDOW(dialog));
}

static void
on_power(GObject *sw, GParamSpec *pspec, Panel *p)
{
	const char *args[4];
	char dev[16];
	gboolean on;

	(void)pspec;
	on = gtk_switch_get_active(GTK_SWITCH(sw));
	if (on == (adapter_up(p) != NULL) || p->busy || !power_device(p, dev,
	    sizeof(dev))) {
		render(p);
		return;
	}
	args[0] = "power";
	args[1] = dev;
	args[2] = on ? "on" : "off";
	args[3] = NULL;
	set_status(p, on ? "Switching Bluetooth on..." :
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
	g_debug("search: %d devices", (int)n);
	p->scanning = FALSE;
	if (!p->dead) {
		/*
		 * Not p->nearby->len: the devices found are added from idle
		 * callbacks, which may run after this one.
		 */
		if (n < 0)
			set_status(p, "The search failed.");
		else if (n == 0)
			set_status(p, "Nothing found.  Put the device in "
			    "pairing mode and search again.");
		render(p);
	}
	panel_unref(p);
}

static void
start_search(Panel *p)
{
	const struct bsdbt_adapter *a;
	ScanJob *job;
	GTask *task;

	a = adapter_up(p);
	if (p->scanning || p->busy || a == NULL)
		return;
	g_debug("search: on %s", a->node);
	p->scanning = TRUE;
	g_array_set_size(p->nearby, 0);
	set_status(p, NULL);
	render(p);
	job = g_new0(ScanJob, 1);
	job->p = panel_ref(p);
	strlcpy(job->node, a->node, sizeof(job->node));
	task = g_task_new(NULL, NULL, scan_done, p);
	g_task_set_task_data(task, job, g_free);
	g_task_run_in_thread(task, scan_thread);
	g_object_unref(task);
}

/*
 * A click on "Search" keeps the menu open, so that the devices can be seen
 * coming in; Enter (the item's "activate") searches too, and closes it.
 */
static gboolean
on_search_release(GtkWidget *item, GdkEventButton *ev, Panel *p)
{
	(void)item;
	(void)ev;
	start_search(p);
	return (TRUE);
}

static void
on_search_activate(GtkMenuItem *item, Panel *p)
{
	(void)item;
	start_search(p);
}

/* ---- the menu: items ---------------------------------------------------- */

/* A menu item holding a row of widgets. */
static GtkWidget *
row_item(GtkWidget *row, const char *tip)
{
	GtkWidget *item;

	item = gtk_menu_item_new();
	gtk_container_add(GTK_CONTAINER(item), row);
	gtk_widget_set_tooltip_text(item, tip);
	gtk_widget_show_all(item);
	return (item);
}

static GtkWidget *
dim_label(const char *text)
{
	GtkWidget *l;

	l = gtk_label_new(text);
	gtk_style_context_add_class(gtk_widget_get_style_context(l),
	    "dim-label");
	return (l);
}

/* Icon, name and, dimmed at the end, what it is or its state. */
static GtkWidget *
device_item(const char *icon, const char *name, const char *note,
    const char *tip)
{
	GtkWidget *row, *label;

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	label = gtk_label_new(name);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 34);
	gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name(icon,
	    GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), dim_label(note), FALSE, FALSE, 0);
	return (row_item(row, tip));
}

/* A section's title, which cannot be chosen. */
static GtkWidget *
header_item(const char *text)
{
	GtkWidget *item, *label;
	char *m;

	m = g_markup_printf_escaped("<b>%s</b>", text);
	label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), m);
	g_free(m);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	item = row_item(label, NULL);
	gtk_widget_set_sensitive(item, FALSE);
	return (item);
}

static int
menu_index(Panel *p, GtkWidget *item)
{
	GList *children;
	int i;

	children = gtk_container_get_children(GTK_CONTAINER(p->menu));
	i = g_list_index(children, item);
	g_list_free(children);
	return (i);
}

static void
destroy_items(GList **items)
{
	GList *l;

	for (l = *items; l != NULL; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(*items);
	*items = NULL;
}

static GtkWidget *
action_item(const char *label, const char *tip, GCallback cb, Panel *p,
    const struct bsdbt_known *k, gboolean sensitive)
{
	GtkWidget *mi;

	mi = gtk_menu_item_new_with_mnemonic(label);
	gtk_widget_set_tooltip_text(mi, tip);
	gtk_widget_set_sensitive(mi, sensitive);
	g_object_set_data_full(G_OBJECT(mi), "known", g_memdup2(k,
	    sizeof(*k)), g_free);
	g_signal_connect(mi, "activate", cb, p);
	return (mi);
}

/*
 * The known devices, each with its actions in a submenu.  Rebuilt only
 * when what they show changed: a refresh every few seconds must not take
 * an item away under the pointer.
 */
static void
render_known(Panel *p)
{
	const struct bsdbt_known *k;
	GtkWidget *item, *sub;
	GString *key;
	char addr[32];
	gboolean conn;
	int i, at;

	key = g_string_new(NULL);
	g_string_append_printf(key, "%d|", p->busy);
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

	destroy_items(&p->known_items);
	at = menu_index(p, p->known_header) + 1;
	for (i = 0; i < p->snap.nknown; i++) {
		k = &p->snap.known[i];
		conn = is_connected(p, &k->bdaddr);
		bt_ntoa(&k->bdaddr, addr);
		item = device_item(known_icon(k), k->name[0] != '\0' ?
		    k->name : addr, conn ? "Connected" : k->paired ? "" :
		    "Not paired", addr);
		sub = gtk_menu_new();
		gtk_menu_set_reserve_toggle_size(GTK_MENU(sub), FALSE);
		gtk_menu_shell_append(GTK_MENU_SHELL(sub), action_item(
		    "_Disconnect", "Close the connection.  An input device "
		    "connects again when you use it.",
		    G_CALLBACK(on_disconnect), p, k, conn && !p->busy));
		gtk_menu_shell_append(GTK_MENU_SHELL(sub), action_item(
		    "_Remove...", "Forget the device: unpair it and stop "
		    "using it", G_CALLBACK(on_remove), p, k, !p->busy));
		gtk_widget_show_all(sub);
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
		gtk_menu_shell_insert(GTK_MENU_SHELL(p->menu), item, at + i);
		p->known_items = g_list_append(p->known_items, item);
	}
	gtk_widget_set_visible(p->known_empty, p->snap.nknown == 0);
	reposition(p);
}

/* The devices a search found; choosing one pairs with it. */
static void
render_nearby(Panel *p)
{
	const Nearby *n;
	GtkWidget *item;
	GString *key;
	char addr[32], *kind;
	guint i;
	int at;

	/* Shown when the search is over: the menu opens again once only. */
	if (p->scanning)
		return;
	key = g_string_new(NULL);
	g_string_append_printf(key, "%d|", p->busy);
	for (i = 0; i < p->nearby->len; i++) {
		n = &g_array_index(p->nearby, Nearby, i);
		g_string_append_printf(key, "%s %d %s|",
		    bt_ntoa(&n->bdaddr, addr), is_known(p, &n->bdaddr),
		    n->name);
	}
	if (g_strcmp0(key->str, p->nearby_key) == 0) {
		g_string_free(key, TRUE);
		return;
	}
	g_free(p->nearby_key);
	p->nearby_key = g_string_free(key, FALSE);

	destroy_items(&p->nearby_items);
	at = menu_index(p, p->nearby_header) + 1;
	for (i = 0; i < p->nearby->len; i++) {
		n = &g_array_index(p->nearby, Nearby, i);
		if (is_known(p, &n->bdaddr))
			continue;
		bt_ntoa(&n->bdaddr, addr);
		/* What kind of device it is, as blueman shows. */
		kind = g_strdup(bsdbt_class_str(n->class));
		kind[0] = g_ascii_toupper(kind[0]);
		item = device_item(class_icon(n->class), n->name[0] != '\0' ?
		    n->name : addr, kind, "Pair with this device");
		g_free(kind);
		gtk_widget_set_sensitive(item, !p->busy);
		g_object_set_data_full(G_OBJECT(item), "nearby", g_memdup2(n,
		    sizeof(*n)), g_free);
		g_signal_connect(item, "activate",
		    G_CALLBACK(on_nearby_activate), p);
		gtk_menu_shell_insert(GTK_MENU_SHELL(p->menu), item, at++);
		p->nearby_items = g_list_append(p->nearby_items, item);
	}
	reposition(p);
}

static void
render_tooltip(Panel *p)
{
	GString *tip;
	int i, nconn;

	tip = g_string_new(NULL);
	if (adapter_up(p) == NULL)
		g_string_append(tip, p->snap.nradios > 0 ||
		    p->snap.nadapters > 0 ? "Bluetooth is off" :
		    "No Bluetooth adapter");
	else {
		g_string_append(tip, "Bluetooth is on");
		nconn = 0;
		for (i = 0; i < p->snap.nknown; i++)
			if (is_connected(p, &p->snap.known[i].bdaddr))
				g_string_append_printf(tip, "%s%s",
				    nconn++ == 0 ? "\nConnected: " : ", ",
				    p->snap.known[i].name);
	}
	gtk_widget_set_tooltip_text(p->button, tip->str);
	g_string_free(tip, TRUE);
}

static void
render(Panel *p)
{
	const struct bsdbt_adapter *a;
	gboolean on, can_power;
	char dev[16], *tip;

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
	gtk_widget_set_sensitive(p->power_item, can_power && !p->busy &&
	    !p->scanning);
	tip = on ? g_strdup_printf("Adapter %s (%s)", a->node, a->name) :
	    can_power ? g_strdup_printf("Adapter %s, switched off", dev) :
	    g_strdup("No Bluetooth adapter (is its firmware loaded?)");
	gtk_widget_set_tooltip_text(p->power_item, tip);
	g_free(tip);

	gtk_widget_set_visible(p->nearby_sep, on);
	gtk_widget_set_visible(p->nearby_header, on);
	gtk_widget_set_visible(p->search_item, on);
	if (p->scanning && p->nearby->len > 0) {
		tip = g_strdup_printf("Searching... %u found", p->nearby->len);
		gtk_label_set_text(GTK_LABEL(p->search_label), tip);
		g_free(tip);
	} else
		gtk_label_set_text(GTK_LABEL(p->search_label), p->scanning ?
		    "Searching..." : "Search for devices");
	gtk_widget_set_visible(p->search_spinner, p->scanning);
	if (p->scanning)
		gtk_spinner_start(GTK_SPINNER(p->search_spinner));
	else
		gtk_spinner_stop(GTK_SPINNER(p->search_spinner));
	gtk_widget_set_sensitive(p->search_item, on && !p->busy &&
	    !p->scanning);
	render_known(p);
	render_nearby(p);
}

/* ---- the menu: opening and closing -------------------------------------- */

/*
 * Left button on the panel icon: open the menu.  Handled on the press, as
 * the pulseaudio and clock plugins do, and not passed on, so the toggle
 * button does not toggle itself.  While the menu is open a click on the
 * icon goes to the menu, which closes; the second branch is a fallback.
 */
static gboolean
on_button_press(GtkWidget *w, GdkEventButton *ev, Panel *p)
{
	if (ev->button != 1)
		return (FALSE);
	if (ev->type != GDK_BUTTON_PRESS)
		return (TRUE);			/* double and triple clicks */
	if (gtk_widget_get_visible(p->menu))
		gtk_menu_popdown(GTK_MENU(p->menu));
	else {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), TRUE);
		if (p->opened_by != NULL)
			gdk_event_free(p->opened_by);
		p->opened_by = gdk_event_copy((GdkEvent *)ev);
		xfce_panel_plugin_popup_menu(p->plugin, GTK_MENU(p->menu), w,
		    (GdkEvent *)ev);
		/*
		 * GTK does not show the menu, and says nothing, when it can
		 * not grab the pointer: do not leave the button pressed.
		 */
		if (!gtk_widget_get_visible(p->menu)) {
			g_debug("menu: not shown (no pointer grab?)");
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),
			    FALSE);
		} else {
			g_debug("menu: open");
			set_timer(p, REFRESH_OPEN_S);
			refresh(p);
		}
	}
	return (TRUE);
}

static void
on_menu_hide(GtkWidget *menu, Panel *p)
{
	(void)menu;
	if (p->reopening)
		return;		/* opens again in a moment */
	g_debug("menu: closed");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->button), FALSE);
	/* Results that come later are notifications. */
	set_status(p, NULL);
	set_timer(p, REFRESH_IDLE_S);
}

static void
flip_power(GtkWidget *sw)
{
	if (gtk_widget_is_sensitive(sw))
		gtk_switch_set_active(GTK_SWITCH(sw),
		    !gtk_switch_get_active(GTK_SWITCH(sw)));
}

/*
 * A click on the Bluetooth row flips its switch and keeps the menu open,
 * as the power manager's presentation mode does; Enter flips it too.
 */
static gboolean
on_power_release(GtkWidget *item, GdkEventButton *ev, Panel *p)
{
	(void)item;
	(void)ev;
	flip_power(p->power);
	return (TRUE);
}

static void
on_power_activate(GtkMenuItem *item, Panel *p)
{
	(void)item;
	flip_power(p->power);
}

static GtkWidget *
separator(GtkWidget *menu)
{
	GtkWidget *sep;

	sep = gtk_separator_menu_item_new();
	gtk_widget_show(sep);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
	return (sep);
}

/*
 * Built once; the device items between the headers come and go.
 *
 *	[icon] Bluetooth               [switch]
 *	---------------------------------------
 *	Devices
 *	  device                  state     >	(Disconnect, Remove...)
 *	---------------------------------------
 *	Nearby
 *	  device                  kind		(choose: pair)
 *	  Search for devices
 *	---------------------------------------
 *	what is going on
 */
static void
build_menu(Panel *p)
{
	GtkWidget *row, *label;

	p->menu = gtk_menu_new();
	gtk_menu_attach_to_widget(GTK_MENU(p->menu), p->button, NULL);
	/* No room for check marks on the left: our items have their icons */
	gtk_menu_set_reserve_toggle_size(GTK_MENU(p->menu), FALSE);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	label = gtk_label_new("Bluetooth");
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	p->power = gtk_switch_new();
	gtk_widget_set_valign(p->power, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name(ICON_ON,
	    GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), p->power, FALSE, FALSE, 0);
	p->power_item = row_item(row, NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(p->menu), p->power_item);
	g_signal_connect(p->power, "notify::active", G_CALLBACK(on_power), p);
	g_signal_connect(p->power_item, "button-release-event",
	    G_CALLBACK(on_power_release), p);
	g_signal_connect(p->power_item, "activate",
	    G_CALLBACK(on_power_activate), p);
	separator(p->menu);

	p->known_header = header_item("Devices");
	gtk_menu_shell_append(GTK_MENU_SHELL(p->menu), p->known_header);
	p->known_empty = row_item(dim_label("None yet"), NULL);
	gtk_widget_set_sensitive(p->known_empty, FALSE);
	gtk_menu_shell_append(GTK_MENU_SHELL(p->menu), p->known_empty);

	p->nearby_sep = separator(p->menu);
	p->nearby_header = header_item("Nearby");
	gtk_menu_shell_append(GTK_MENU_SHELL(p->menu), p->nearby_header);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	p->search_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->search_label), 0.0);
	p->search_spinner = gtk_spinner_new();
	gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name(
	    "system-search-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), p->search_label, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), p->search_spinner, FALSE, FALSE, 0);
	p->search_item = row_item(row, "Look for devices in pairing mode "
	    "(about 10 seconds)");
	gtk_menu_shell_append(GTK_MENU_SHELL(p->menu), p->search_item);
	g_signal_connect(p->search_item, "button-release-event",
	    G_CALLBACK(on_search_release), p);
	g_signal_connect(p->search_item, "activate",
	    G_CALLBACK(on_search_activate), p);

	p->status_sep = separator(p->menu);
	p->status = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->status), 0.0);
	gtk_label_set_line_wrap(GTK_LABEL(p->status), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(p->status), 36);
	p->status_item = row_item(p->status, NULL);
	gtk_widget_set_sensitive(p->status_item, FALSE);
	gtk_menu_shell_append(GTK_MENU_SHELL(p->menu), p->status_item);
	gtk_widget_hide(p->status_sep);
	gtk_widget_hide(p->status_item);

	g_signal_connect(p->menu, "hide", G_CALLBACK(on_menu_hide), p);
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

/* "About" in the panel's menu for the plugin */
static void
on_about(XfcePanelPlugin *plugin, Panel *p)
{
	static const char *authors[] = { "wugq <wugq.dev@gmail.com>", NULL };

	(void)plugin;
	(void)p;
	gtk_show_about_dialog(NULL,
	    "program-name", "Bluetooth (FreeBSD)",
	    "version", PACKAGE_VERSION,
	    "comments", "Switch Bluetooth on and off, pair and connect "
	    "devices with the FreeBSD Bluetooth stack.",
	    "website", "https://github.com/wugq/xfce4-bsdbluetooth-plugin",
	    "license-type", GTK_LICENSE_BSD,
	    "authors", authors,
	    "logo-icon-name", ICON_ON,
	    NULL);
}

static void
on_free(XfcePanelPlugin *plugin, Panel *p)
{
	(void)plugin;
	p->dead = TRUE;
	if (p->timer != 0)
		g_source_remove(p->timer);
	if (p->reopen != 0)
		g_source_remove(p->reopen);
	if (p->opened_by != NULL)
		gdk_event_free(p->opened_by);
	if (p->pin_dialog != NULL)
		gtk_widget_destroy(p->pin_dialog);
	g_clear_object(&p->note);
	gtk_widget_destroy(p->menu);
	/* Running tasks hold references; the last one frees. */
	panel_unref(p);
}

/*
 * Debug messages (g_debug(), log domain "bsdbluetooth-plugin") are off
 * unless asked for, as with the other panel plugins: PANEL_DEBUG=all or
 * PANEL_DEBUG=bsdbluetooth-plugin (a comma separated list, as the panel
 * takes it), or GLib's own G_MESSAGES_DEBUG=bsdbluetooth-plugin.  They go
 * to the panel's standard error.
 */
static void
init_debug(void)
{
	const char *env;
	char **domains, *value;
	int i;

	env = g_getenv("PANEL_DEBUG");
	if (env == NULL)
		return;
	domains = g_strsplit(env, ",", -1);
	for (i = 0; domains[i] != NULL; i++) {
		g_strstrip(domains[i]);
		if (!g_str_equal(domains[i], "all") &&
		    !g_str_equal(domains[i], G_LOG_DOMAIN))
			continue;
		value = g_strjoin(" ", G_LOG_DOMAIN,
		    g_getenv("G_MESSAGES_DEBUG"), NULL);
#if GLIB_CHECK_VERSION(2, 80, 0)
		/*
		 * GLib reads G_MESSAGES_DEBUG once, at the first message, and
		 * the wrapper process has logged already.
		 */
		{
			char **list = g_strsplit(value, " ", -1);

			g_log_writer_default_set_debug_domains(
			    (const char * const *)list);
			g_strfreev(list);
		}
#else
		g_setenv("G_MESSAGES_DEBUG", value, TRUE);
#endif
		g_free(value);
		break;
	}
	g_strfreev(domains);
}

static void
construct(XfcePanelPlugin *plugin)
{
	Panel *p;

	init_debug();
	g_debug("version %s", PACKAGE_VERSION);
	notify_init("xfce4-bsdbluetooth-plugin");

	p = g_new0(Panel, 1);
	p->refs = 1;
	p->plugin = plugin;
	p->nearby = g_array_new(FALSE, TRUE, sizeof(Nearby));
	p->button = xfce_panel_create_toggle_button();
	p->icon = gtk_image_new_from_icon_name(ICON_OFF, GTK_ICON_SIZE_BUTTON);
	gtk_container_add(GTK_CONTAINER(p->button), p->icon);
	g_signal_connect(p->button, "button-press-event",
	    G_CALLBACK(on_button_press), p);
	gtk_container_add(GTK_CONTAINER(plugin), p->button);
	xfce_panel_plugin_add_action_widget(plugin, p->button);
	xfce_panel_plugin_set_small(plugin, TRUE);

	build_menu(p);
	g_signal_connect(plugin, "size-changed", G_CALLBACK(on_size_changed),
	    p);
	g_signal_connect(plugin, "free-data", G_CALLBACK(on_free), p);
	xfce_panel_plugin_menu_show_about(plugin);
	g_signal_connect(plugin, "about", G_CALLBACK(on_about), p);
	gtk_widget_show_all(p->button);
	render(p);
	set_timer(p, REFRESH_IDLE_S);
	refresh(p);
}

XFCE_PANEL_PLUGIN_REGISTER(construct);
