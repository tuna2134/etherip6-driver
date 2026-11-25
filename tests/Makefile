# SPDX-License-Identifier: GPL-2.0
CC	?= cc
CFLAGS	?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS	?=

all: etherip6_selftest

# Static binaries are what the QEMU initramfs uses.
static: etherip6_selftest.static etherip6ctl.static

etherip6_selftest: etherip6_selftest.c
	$(CC) $(CFLAGS) -o $@ $<

etherip6_selftest.static: etherip6_selftest.c
	$(CC) $(CFLAGS) -static -o $@ $<

etherip6ctl.static: ../tools/etherip6ctl.c ../src/etherip6_uapi.h
	$(CC) $(CFLAGS) -I../src -static -o $@ ../tools/etherip6ctl.c

vm-test: all
	./vm/run-vm-test.sh

clean:
	rm -f etherip6_selftest etherip6_selftest.static etherip6ctl.static

.PHONY: all static vm-test clean
