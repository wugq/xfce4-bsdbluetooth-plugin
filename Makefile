# Makefile for xfce4-bsdbluetooth-plugin -- works with FreeBSD make and GNU make
#
#   make                   build bsdbt and bsdbt-helper
#   make install           install under PREFIX (default /usr/local); with
#                          DESTDIR for staging (ports, packages)
#   make dist              source tarball of HEAD (needs git)
#   make CFLAGS="-O0 -g3"  build for debugging
#   make clean

PACKAGE    = xfce4-bsdbluetooth-plugin
VERSION    = 0.0.1

CLI        = bsdbt
HELPER     = bsdbt-helper

PREFIX    ?= /usr/local
DESTDIR   ?=
BINDIR     = $(PREFIX)/bin
LIBEXECDIR = $(PREFIX)/libexec
POLKITDIR  = $(PREFIX)/share/polkit-1/actions

CC        ?= cc
CFLAGS    ?= -O2 -pipe
CFLAGS    += -Wall -Wextra
BT_CFLAGS  = -DBSDBT_HELPER='"$(LIBEXECDIR)/$(HELPER)"'

# Files made from templates: @LIBEXECDIR@ filled in
GENERATED  = org.bsdbt.helper.policy
SUBST      = sed -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g'

all: $(CLI) $(HELPER) $(GENERATED)

$(CLI): bsdbt.c bt.c bt.h
	$(CC) $(CFLAGS) $(BT_CFLAGS) -o $(CLI) bsdbt.c bt.c -lbluetooth

$(HELPER): bsdbt-helper.c bt.c bt.h conf.c conf.h sdphid.c sdphid.h
	$(CC) $(CFLAGS) -o $(HELPER) bsdbt-helper.c bt.c conf.c sdphid.c \
	    -lbluetooth -lsdp -lusbhid

org.bsdbt.helper.policy: org.bsdbt.helper.policy.in
	$(SUBST) org.bsdbt.helper.policy.in > $@

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBEXECDIR) \
	    $(DESTDIR)$(POLKITDIR)
	install -m 755 $(CLI) $(DESTDIR)$(BINDIR)/$(CLI)
	install -m 755 $(HELPER) $(DESTDIR)$(LIBEXECDIR)/$(HELPER)
	install -m 644 org.bsdbt.helper.policy \
	    $(DESTDIR)$(POLKITDIR)/org.bsdbt.helper.policy

dist:
	git archive --prefix=$(PACKAGE)-$(VERSION)/ \
	    -o $(PACKAGE)-$(VERSION).tar.gz HEAD

clean:
	rm -f $(CLI) $(HELPER) $(GENERATED)

.PHONY: all install dist clean
