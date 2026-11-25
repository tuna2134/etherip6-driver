#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot the exact kernel used to build the module inside QEMU (TCG, no KVM
# required) with a minimal initramfs and run the etherip6 selftest.
#
# The kernel image is downloaded from the distribution package matching
# KVER, so the module vermagic and symbol CRCs always match.
#
# Usage: tests/vm/run-vm-test.sh [--keep]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=${KVER:-$(uname -r)}
BUILD=${VM_BUILD_DIR:-$ROOT/.vmtest}
VMLINUZ=$BUILD/root/boot/vmlinuz-$KVER
STAGE=$BUILD/initramfs
LOG=$BUILD/console.log
TIMEOUT=${VM_TIMEOUT:-900}

say() { printf '>>> %s\n' "$*"; }

for cmd in qemu-system-x86_64 cpio gzip ldd dpkg-deb apt-get gcc make zstd ethtool; do
	command -v "$cmd" >/dev/null || {
		echo "missing required command: $cmd" >&2
		exit 1
	}
done

mkdir -p "$BUILD"

say "building module, tool and selftest"
make -C "$ROOT" module tool
make -C "$ROOT/tests" static

if [ ! -f "$VMLINUZ" ]; then
	say "downloading linux-image-$KVER"
	(
		cd "$BUILD"
		rm -rf root *.deb
		apt-get download "linux-image-$KVER"
		mkdir -p root
		dpkg-deb -x ./*.deb root
	)
fi

if [ ! -f "$VMLINUZ" ]; then
	echo "kernel image not found: $VMLINUZ" >&2
	exit 1
fi

say "building initramfs"
rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,sbin,lib,lib64,proc,sys,dev,tmp,run,usr/bin,usr/sbin,usr/lib/x86_64-linux-gnu}
cp /bin/busybox "$STAGE/bin/busybox"
for applet in sh mount insmod dmesg poweroff reboot sleep cat grep mkdir \
	      ls uname sync ping awk head killall timeout; do
	ln -sf /bin/busybox "$STAGE/bin/$applet"
done

# Real iproute2 is used for addresses/MTU and for interoperability checks.
IP_BIN=$(command -v ip)
cp "$IP_BIN" "$STAGE/sbin/ip"
ldd "$IP_BIN" |
	awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\//) print $i }' |
	sort -u |
	while read -r lib; do
		[ -n "$lib" ] || continue
		mkdir -p "$STAGE$(dirname "$lib")"
		cp -L "$lib" "$STAGE$lib"
	done

# veth is needed for the netns + iperf3 test.
VETH_SRC=/lib/modules/$KVER/kernel/drivers/net/veth.ko.zst
if [ ! -f "$VETH_SRC" ]; then
	echo "veth module not found: $VETH_SRC" >&2
	exit 1
fi
zstd -d -c "$VETH_SRC" > "$STAGE/veth.ko"

# iperf3 (and its shared libraries) for the netns throughput test.
IPERF_DIR=$BUILD/pkgs
if [ ! -x "$IPERF_DIR/root/usr/bin/iperf3" ]; then
	mkdir -p "$IPERF_DIR"
	(
		cd "$IPERF_DIR"
		apt-get download iperf3 libiperf0 libsctp1
		mkdir -p root
		for deb in ./*.deb; do
			dpkg-deb -x "$deb" root
		done
	)
fi
install -m 0755 "$IPERF_DIR/root/usr/bin/iperf3" "$STAGE/usr/bin/iperf3"
install -m 0755 "$(command -v ethtool)" "$STAGE/usr/sbin/ethtool"
for lib in libiperf.so.0 libsctp.so.1; do
	src=$(find "$IPERF_DIR/root" -name "$lib" | head -1)
	[ -n "$src" ] || { echo "missing $lib" >&2; exit 1; }
	cp -L "$src" "$STAGE/lib/x86_64-linux-gnu/"
done
cp -L /lib/x86_64-linux-gnu/libcrypto.so.3 /lib/x86_64-linux-gnu/libm.so.6 \
	/lib/x86_64-linux-gnu/libgcc_s.so.1 "$STAGE/lib/x86_64-linux-gnu/"
if LD_LIBRARY_PATH="$STAGE/lib/x86_64-linux-gnu:$STAGE/usr/lib/x86_64-linux-gnu" \
	ldd "$STAGE/usr/bin/iperf3" | grep -q "not found"; then
	echo "iperf3 dependencies are not satisfied" >&2
	exit 1
fi

install -m 0644 "$ROOT/src/etherip6.ko" "$STAGE/etherip6.ko"
install -m 0755 "$ROOT/tests/etherip6ctl.static" "$STAGE/etherip6ctl"
install -m 0755 "$ROOT/tests/etherip6_selftest.static" "$STAGE/etherip6_selftest"
# Minimal /etc files for iperf3 (getaddrinfo / getprotobyname / services).
mkdir -p "$STAGE/etc"
cp /etc/protocols /etc/services "$STAGE/etc/" 2>/dev/null || true
printf 'hosts: files\n' > "$STAGE/etc/nsswitch.conf"
printf '127.0.0.1 localhost\n::1 localhost\n' > "$STAGE/etc/hosts"
: > "$STAGE/etc/resolv.conf"
if [ -f /usr/lib/ssl/openssl.cnf ]; then
	mkdir -p "$STAGE/usr/lib/ssl"
	cp -L /usr/lib/ssl/openssl.cnf "$STAGE/usr/lib/ssl/"
fi

install -m 0755 "$ROOT/tests/vm/init" "$STAGE/init"

(
	cd "$STAGE"
	find . -print0 | cpio --null -o -H newc --quiet
) | gzip -9 > "$BUILD/initramfs.cpio.gz"

say "booting QEMU (TCG) with $VMLINUZ"
rm -f "$LOG"
set +e
timeout "$TIMEOUT" qemu-system-x86_64 \
	-kernel "$VMLINUZ" \
	-initrd "$BUILD/initramfs.cpio.gz" \
	-append "console=ttyS0,115200 rdinit=/init panic=-1 loglevel=7" \
	-nographic -no-reboot -m 1024 -smp 2 \
	-accel tcg,thread=multi \
	2>&1 | tee "$LOG"
set -e

say "checking results"
if grep -q "INIT FAIL" "$LOG"; then
	echo "VM TEST FAILED (init)" >&2
	tail -60 "$LOG" >&2
	exit 1
fi
if ! grep -q "SELFTEST PASS" "$LOG"; then
	echo "VM TEST FAILED (no SELFTEST PASS marker)" >&2
	tail -80 "$LOG" >&2
	exit 1
fi
if ! grep -q "INIT OK" "$LOG"; then
	echo "VM TEST FAILED (no INIT OK marker)" >&2
	tail -80 "$LOG" >&2
	exit 1
fi
if ! grep -q "FRAG_TCP_OK" "$LOG"; then
	echo "VM TEST FAILED (no FRAG_TCP_OK marker)" >&2
	tail -120 "$LOG" >&2
	exit 1
fi
if ! grep -q "NETNS_IPERF_OK" "$LOG"; then
	echo "VM TEST FAILED (no NETNS_IPERF_OK marker)" >&2
	tail -120 "$LOG" >&2
	exit 1
fi

say "VM TEST PASSED"
grep -E "SELFTEST PASS|\[ok\]|\[stats\]|\[burst\]|JUMBO OK|RECONFIG OK|NL CONFIG OK|IPROUTE2_CREATE|OUTER-MTU OK|VALIDATION OK|IPERF |RATIO|NETNS_IPERF_OK|NETNS_SUMMARY|FRAG" "$LOG" | tail -80

if [ "${1:-}" != "--keep" ]; then
	rm -f "$BUILD/initramfs.cpio.gz"
fi
