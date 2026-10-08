# Makefile for xfce4-bsdbluetooth-plugin -- works with FreeBSD make and GNU make
#
#   make                   build bsdbt, bsdbt-helper and the XFCE panel
#                          plugin libbsdbluetooth-plugin.so
#   make install           install under PREFIX (default /usr/local); with
#                          DESTDIR for staging (ports, packages)
#   make dist              source tarball of HEAD (needs git)
#   make CFLAGS="-O0 -g3"  build for debugging
#   make clean

PACKAGE    = xfce4-bsdbluetooth-plugin
VERSION    = 0.1.0

CLI        = bsdbt
HELPER     = bsdbt-helper
PLUGIN     = libbsdbluetooth-plugin.so

PREFIX    ?= /usr/local
DESTDIR   ?=
BINDIR     = $(PREFIX)/bin
LIBEXECDIR = $(PREFIX)/libexec
POLKITDIR  = $(PREFIX)/share/polkit-1/actions
MANDIR     = $(PREFIX)/share/man
PLUGINDIR  = $(PREFIX)/lib/xfce4/panel/plugins
PLUGINDATA = $(PREFIX)/share/xfce4/panel/plugins

CC        ?= cc
CFLAGS    ?= -O2 -pipe
CFLAGS    += -Wall -Wextra
BT_CFLAGS  = -DBSDBT_HELPER='"$(LIBEXECDIR)/$(HELPER)"' \
	     -DPACKAGE_VERSION='"$(VERSION)"'

PANEL_CFLAGS != pkg-config --cflags libxfce4panel-2.0 gtk+-3.0
PANEL_LIBS   != pkg-config --libs libxfce4panel-2.0 gtk+-3.0

# Files made from templates: @LIBEXECDIR@ filled in
GENERATED  = org.bsdbt.helper.policy man/bsdbt.1 man/bsdbt-helper.8
SUBST      = sed -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g'

all: $(CLI) $(HELPER) $(PLUGIN) $(GENERATED)

$(CLI): bsdbt.c bt.c bt.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BT_CFLAGS) $(LDFLAGS) -o $(CLI) \
	    bsdbt.c bt.c -lbluetooth

$(HELPER): bsdbt-helper.c bt.c bt.h conf.c conf.h sdphid.c sdphid.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $(HELPER) bsdbt-helper.c \
	    bt.c conf.c sdphid.c -lbluetooth -lsdp -lusbhid

$(PLUGIN): bsdbluetooth-plugin.c bt.c bt.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(BT_CFLAGS) $(PANEL_CFLAGS) \
	    $(LDFLAGS) -o $(PLUGIN) bsdbluetooth-plugin.c bt.c $(PANEL_LIBS) \
	    -lbluetooth

org.bsdbt.helper.policy: org.bsdbt.helper.policy.in
	$(SUBST) org.bsdbt.helper.policy.in > $@

man/bsdbt.1: man/bsdbt.1.in
	$(SUBST) man/bsdbt.1.in > $@
man/bsdbt-helper.8: man/bsdbt-helper.8.in
	$(SUBST) man/bsdbt-helper.8.in > $@

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBEXECDIR) \
	    $(DESTDIR)$(POLKITDIR) $(DESTDIR)$(PLUGINDIR) \
	    $(DESTDIR)$(PLUGINDATA) $(DESTDIR)$(MANDIR)/man1 \
	    $(DESTDIR)$(MANDIR)/man8
	install -m 755 $(CLI) $(DESTDIR)$(BINDIR)/$(CLI)
	install -m 755 $(HELPER) $(DESTDIR)$(LIBEXECDIR)/$(HELPER)
	install -m 644 org.bsdbt.helper.policy \
	    $(DESTDIR)$(POLKITDIR)/org.bsdbt.helper.policy
	install -m 755 $(PLUGIN) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN)
	install -m 644 bsdbluetooth-plugin.desktop \
	    $(DESTDIR)$(PLUGINDATA)/bsdbluetooth-plugin.desktop
	install -m 644 man/bsdbt.1 $(DESTDIR)$(MANDIR)/man1/bsdbt.1
	install -m 644 man/bsdbt-helper.8 $(DESTDIR)$(MANDIR)/man8/bsdbt-helper.8

dist:
	git archive --prefix=$(PACKAGE)-$(VERSION)/ \
	    -o $(PACKAGE)-$(VERSION).tar.gz HEAD

clean:
	rm -f $(CLI) $(HELPER) $(PLUGIN) $(GENERATED)

.PHONY: all install dist clean
