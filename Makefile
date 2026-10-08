# Makefile for xfce4-bsdbluetooth-plugin -- works with FreeBSD make and GNU make
#
#   make                   build bsdbt
#   make install           install under PREFIX (default /usr/local); with
#                          DESTDIR for staging (ports, packages)
#   make dist              source tarball of HEAD (needs git)
#   make CFLAGS="-O0 -g3"  build for debugging
#   make clean

PACKAGE    = xfce4-bsdbluetooth-plugin
VERSION    = 0.0.1

CLI        = bsdbt

PREFIX    ?= /usr/local
DESTDIR   ?=
BINDIR     = $(PREFIX)/bin

CC        ?= cc
CFLAGS    ?= -O2 -pipe
CFLAGS    += -Wall -Wextra

all: $(CLI)

$(CLI): bsdbt.c bt.c bt.h
	$(CC) $(CFLAGS) -o $(CLI) bsdbt.c bt.c -lbluetooth

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(CLI) $(DESTDIR)$(BINDIR)/$(CLI)

dist:
	git archive --prefix=$(PACKAGE)-$(VERSION)/ \
	    -o $(PACKAGE)-$(VERSION).tar.gz HEAD

clean:
	rm -f $(CLI)

.PHONY: all install dist clean
