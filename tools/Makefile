# SPDX-License-Identifier: GPL-2.0
CC	?= cc
CFLAGS	?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I../src

all: etherip6ctl

etherip6ctl: etherip6ctl.c ../src/etherip6_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ etherip6ctl.c

clean:
	rm -f etherip6ctl

.PHONY: all clean
