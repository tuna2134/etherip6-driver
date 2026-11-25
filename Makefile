# SPDX-License-Identifier: GPL-2.0
#
# etherip6 - EtherIP (RFC 3378) over IPv6 driver.
#
#   make            build the kernel module and the userspace tool
#   make check      build with extra warnings (W=1)
#   make tests      build the userspace selftest
#   make vm-test    boot a QEMU test VM (see tests/vm/run-vm-test.sh)
#   make clean
#
KDIR	?= /lib/modules/$(shell uname -r)/build
PWD	:= $(shell pwd)
MAKE	?= make

.PHONY: all module tool tests check clean install vm-test

all: module tool

module:
	$(MAKE) -C $(KDIR) M=$(PWD)/src modules

check:
	$(MAKE) -C $(KDIR) M=$(PWD)/src W=1 modules

tool:
	$(MAKE) -C tools

tests:
	$(MAKE) -C tests

vm-test: all tests
	$(MAKE) -C tests vm-test

install: all
	$(MAKE) -C $(KDIR) M=$(PWD)/src modules_install
	install -D -m 0755 tools/etherip6ctl $(DESTDIR)/usr/local/sbin/etherip6ctl

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/src clean
	$(MAKE) -C tools clean
	$(MAKE) -C tests clean
