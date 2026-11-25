// SPDX-License-Identifier: GPL-2.0
/*
 * etherip6_selftest - functional test for the etherip6 driver.
 *
 * Two etherip6 devices (e.g. eip0 and eip1) are connected back-to-back in
 * the same network namespace through the IPv6 loopback route.  Frames are
 * injected with AF_PACKET on one device and must come out, unmodified, on
 * the other one.
 *
 * The test covers:
 *   - small and jumbo (9000 byte MTU, 9014 byte frame) Ethernet frames
 *   - IPv6 fragmentation of the outer packets (forced by lowering the
 *     loopback MTU in the test VM) and reassembly on the receive side
 *   - checksums? no: EtherIP has no outer checksum, the frame is verified
 *     byte for byte
 *   - the driver's TX/RX statistics
 *
 * Exit code 0 and the line "SELFTEST PASS" on success.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TEST_ETHERTYPE	0x88b5
#define TEST_MAGIC	0x45504936U	/* "EPI6" */
#define TEST_HDR_LEN	16
#define MAX_FRAME	65536

struct __attribute__((packed)) test_hdr {
	uint32_t magic;
	uint32_t seq;
	uint32_t len;
	uint32_t rsvd;
};

static unsigned int next_seq;
static int failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	failures++;
}

static unsigned long long read_u64_file(const char *path)
{
	unsigned long long v;
	FILE *f = fopen(path, "r");

	if (!f)
		return 0;
	if (fscanf(f, "%llu", &v) != 1)
		v = 0;
	fclose(f);
	return v;
}

static unsigned long long read_snmp6(const char *key)
{
	char name[64];
	unsigned long long value;
	FILE *f = fopen("/proc/net/snmp6", "r");

	if (!f)
		return 0;

	while (fscanf(f, "%63s %llu", name, &value) == 2) {
		if (!strcmp(name, key)) {
			fclose(f);
			return value;
		}
	}

	fclose(f);
	return 0;
}

static int open_packet(const char *ifname)
{
	struct sockaddr_ll sll;
	struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
	int fd;

	fd = socket(AF_PACKET, SOCK_RAW, htons(TEST_ETHERTYPE));
	if (fd < 0) {
		perror("socket(AF_PACKET)");
		return -1;
	}

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(TEST_ETHERTYPE);
	sll.sll_ifindex = (int)if_nametoindex(ifname);
	if (!sll.sll_ifindex) {
		fprintf(stderr, "FAIL: %s: %s\n", ifname, strerror(ENODEV));
		close(fd);
		return -1;
	}

	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		perror("bind(AF_PACKET)");
		close(fd);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
		perror("setsockopt(SO_RCVTIMEO)");

	return fd;
}

static void fill_frame(unsigned char *buf, size_t len, unsigned int seq)
{
	struct ethhdr *eth = (struct ethhdr *)buf;
	struct test_hdr *hdr;
	unsigned char *p;
	size_t i, plen;

	memset(buf, 0, len);
	memset(eth->h_dest, 0xff, ETH_ALEN);
	eth->h_source[0] = 0x02;
	eth->h_source[1] = 0x00;
	eth->h_source[2] = 0x00;
	eth->h_source[3] = 0x00;
	eth->h_source[4] = 0x00;
	eth->h_source[5] = 0x01;
	eth->h_proto = htons(TEST_ETHERTYPE);

	p = buf + ETH_HLEN;
	plen = len - ETH_HLEN;

	if (plen >= TEST_HDR_LEN) {
		hdr = (struct test_hdr *)p;
		hdr->magic = htonl(TEST_MAGIC);
		hdr->seq = htonl(seq);
		hdr->len = htonl((uint32_t)len);
		hdr->rsvd = 0;
		p += TEST_HDR_LEN;
		plen -= TEST_HDR_LEN;
	}

	for (i = 0; i < plen; i++)
		p[i] = (unsigned char)((seq * 31U + i * 7U) & 0xffU);
}

static int verify_frame(const unsigned char *buf, size_t len,
			unsigned int seq, const char *what)
{
	const struct ethhdr *eth = (const struct ethhdr *)buf;
	const struct test_hdr *hdr;
	const unsigned char *p;
	size_t i, plen;

	if (len < ETH_HLEN + TEST_HDR_LEN) {
		fail("%s: short frame (%zu)", what, len);
		return -1;
	}

	if (ntohs(eth->h_proto) != TEST_ETHERTYPE) {
		fail("%s: wrong ethertype %#x", what, ntohs(eth->h_proto));
		return -1;
	}

	hdr = (const struct test_hdr *)(buf + ETH_HLEN);
	if (ntohl(hdr->magic) != TEST_MAGIC) {
		fail("%s: bad magic %#x", what, ntohl(hdr->magic));
		return -1;
	}

	if (ntohl(hdr->seq) != seq) {
		fail("%s: bad sequence %u (expected %u)", what,
		     ntohl(hdr->seq), seq);
		return -1;
	}

	if (ntohl(hdr->len) != (uint32_t)len) {
		fail("%s: length field %u, received %zu", what,
		     ntohl(hdr->len), len);
		return -1;
	}

	p = buf + ETH_HLEN + TEST_HDR_LEN;
	plen = len - ETH_HLEN - TEST_HDR_LEN;
	for (i = 0; i < plen; i++) {
		unsigned char expect = (unsigned char)((seq * 31U + i * 7U) &
						       0xffU);

		if (p[i] != expect) {
			fail("%s: payload corruption at offset %zu (%#x != %#x)",
			     what, i, p[i], expect);
			return -1;
		}
	}

	return 0;
}

static int send_frame(int fd, int ifindex, const unsigned char *frame,
		      size_t len)
{
	struct sockaddr_ll sll;
	ssize_t n;

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(TEST_ETHERTYPE);
	sll.sll_ifindex = ifindex;
	sll.sll_halen = ETH_ALEN;
	memset(sll.sll_addr, 0xff, ETH_ALEN);

	n = sendto(fd, frame, len, 0, (struct sockaddr *)&sll, sizeof(sll));
	if (n < 0) {
		fail("sendto(%zu): %s", len, strerror(errno));
		return -1;
	}
	if ((size_t)n != len) {
		fail("sendto short write: %zd/%zu", n, len);
		return -1;
	}

	return 0;
}

static int recv_frame(int fd, unsigned char *buf, size_t cap, size_t *len)
{
	ssize_t n;

	for (;;) {
		n = recv(fd, buf, cap, 0);
		if (n >= 0) {
			*len = (size_t)n;
			return 0;
		}
		if (errno == EINTR)
			continue;
		fail("recv: %s", strerror(errno));
		return -1;
	}
}

static int run_single(int txf, int rxf, int tx_ifindex, size_t len,
		      unsigned int count, const char *what)
{
	unsigned char *frame = malloc(len);
	unsigned char *rbuf = malloc(MAX_FRAME);
	unsigned int i;
	int ret = 0;

	if (!frame || !rbuf) {
		fail("out of memory");
		free(frame);
		free(rbuf);
		return -1;
	}

	for (i = 0; i < count; i++) {
		unsigned int seq = next_seq++;
		size_t rlen;

		fill_frame(frame, len, seq);
		if (send_frame(txf, tx_ifindex, frame, len) < 0) {
			ret = -1;
			break;
		}
		if (recv_frame(rxf, rbuf, MAX_FRAME, &rlen) < 0) {
			ret = -1;
			break;
		}
		if (verify_frame(rbuf, rlen, seq, what) < 0) {
			ret = -1;
			break;
		}
	}

	free(frame);
	free(rbuf);
	return ret;
}

static int run_burst(int txf, int rxf, int tx_ifindex, size_t len,
		     unsigned int count, const char *what)
{
	unsigned char *frame = malloc(len);
	unsigned char *rbuf = malloc(MAX_FRAME);
	unsigned char *seen = calloc(count, 1);
	struct timespec t0, t1;
	unsigned int sent = 0, received = 0;
	int rcvbuf = 32 * 1024 * 1024;
	double secs;
	int ret = 0;

	if (!frame || !rbuf || !seen) {
		fail("out of memory");
		ret = -1;
		goto out;
	}

	/* Best effort: raise the receive queue for the burst. */
	if (setsockopt(rxf, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf,
		       sizeof(rcvbuf)) < 0)
		setsockopt(rxf, SOL_SOCKET, SO_RCVBUF, &rcvbuf,
			   sizeof(rcvbuf));

	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (sent = 0; sent < count; sent++) {
		fill_frame(frame, len, next_seq + sent);
		if (send_frame(txf, tx_ifindex, frame, len) < 0) {
			ret = -1;
			goto out;
		}
	}

	while (received < count) {
		size_t rlen;
		unsigned int seq;

		if (recv_frame(rxf, rbuf, MAX_FRAME, &rlen) < 0) {
			ret = -1;
			goto out;
		}
		if (rlen < ETH_HLEN + TEST_HDR_LEN ||
		    ntohl(((struct test_hdr *)(rbuf + ETH_HLEN))->magic) !=
			    TEST_MAGIC) {
			fail("%s: bad burst frame", what);
			ret = -1;
			goto out;
		}
		seq = ntohl(((struct test_hdr *)(rbuf + ETH_HLEN))->seq);
		if (seq < next_seq || seq >= next_seq + count) {
			fail("%s: burst sequence %u out of range", what, seq);
			ret = -1;
			goto out;
		}
		if (seen[seq - next_seq]) {
			fail("%s: duplicate burst sequence %u", what, seq);
			ret = -1;
			goto out;
		}
		if (verify_frame(rbuf, rlen, seq, what) < 0) {
			ret = -1;
			goto out;
		}
		seen[seq - next_seq] = 1;
		received++;
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("[burst] %s: %u x %zu bytes in %.2fs (%.0f pps)\n",
	       what, count, len, secs, secs > 0 ? count / secs : 0);

	next_seq += count;
out:
	free(frame);
	free(rbuf);
	free(seen);
	return ret;
}

int main(int argc, char **argv)
{
	const char *txname, *rxname;
	bool expect_frag = false;
	bool no_frag = false;
	int txf, rxf, tx_ifindex;
	unsigned long long tx_before, rx_before, tx_after, rx_after;
	unsigned long long fc0, fr0, fc1, fr1;
	int i;

	if (argc < 3) {
		fprintf(stderr,
			"usage: %s <tx-dev> <rx-dev> [--expect-frag|--no-frag]\n",
			argv[0]);
		return 2;
	}

	txname = argv[1];
	rxname = argv[2];

	for (i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--expect-frag"))
			expect_frag = true;
		else if (!strcmp(argv[i], "--no-frag"))
			no_frag = true;
		else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			return 2;
		}
	}

	txf = open_packet(txname);
	rxf = open_packet(rxname);
	if (txf < 0 || rxf < 0)
		return 1;

	tx_ifindex = (int)if_nametoindex(txname);

	{
		char path[256];

		snprintf(path, sizeof(path),
			 "/sys/class/net/%s/statistics/tx_packets", txname);
		tx_before = read_u64_file(path);
		snprintf(path, sizeof(path),
			 "/sys/class/net/%s/statistics/rx_packets", rxname);
		rx_before = read_u64_file(path);
	}

	printf("== etherip6 selftest: %s -> %s ==\n", txname, rxname);

	/* Small frame, no fragmentation anywhere. */
	if (run_single(txf, rxf, tx_ifindex, 64, 5, txname) < 0)
		goto out;
	printf("[ok] 64 byte frames\n");

	/* 1500 byte MTU + Ethernet header (+ VLAN in the next step). */
	if (run_single(txf, rxf, tx_ifindex, 1518, 5, txname) < 0)
		goto out;
	printf("[ok] 1518 byte frames\n");

	fc0 = read_snmp6("Ip6FragCreates");
	fr0 = read_snmp6("Ip6ReasmOKs");

	/* Jumbo: a full 9000 byte MTU IP packet (9000 + 14 byte Ethernet
	 * header).  The outer packets need fragmentation when the outer
	 * path MTU is smaller (the VM sets lo to 1280).
	 */
	if (run_single(txf, rxf, tx_ifindex, 9000, 5, txname) < 0)
		goto out;
	printf("[ok] 9000 byte frames\n");

	if (run_single(txf, rxf, tx_ifindex, 9014, 5, txname) < 0)
		goto out;
	printf("[ok] 9014 byte frames (mtu 9000 + Ethernet header)\n");

	fc1 = read_snmp6("Ip6FragCreates");
	fr1 = read_snmp6("Ip6ReasmOKs");
	printf("[stats] Ip6FragCreates +%llu, Ip6ReasmOKs +%llu\n",
	       fc1 - fc0, fr1 - fr0);

	if (expect_frag) {
		if (fc1 - fc0 < 10) {
			fail("expected outer IPv6 fragmentation (creates +%llu)",
			     fc1 - fc0);
			goto out;
		}
		if (fr1 - fr0 < 10) {
			fail("expected outer IPv6 reassembly (ok +%llu)",
			     fr1 - fr0);
			goto out;
		}
		printf("[ok] outer fragments were created and reassembled\n");
	} else if (no_frag) {
		if (fc1 - fc0 != 0 || fr1 - fr0 != 0) {
			fail("unexpected fragmentation (creates +%llu, ok +%llu)",
			     fc1 - fc0, fr1 - fr0);
			goto out;
		}
		printf("[ok] no fragmentation on a jumbo path\n");
	}

	if (run_burst(txf, rxf, tx_ifindex, 1518, 500, txname) < 0)
		goto out;
	printf("[ok] burst of 1518 byte frames\n");

	if (run_burst(txf, rxf, tx_ifindex, 9014, 50, txname) < 0)
		goto out;
	printf("[ok] burst of 9014 byte frames\n");

	{
		char path[256];

		snprintf(path, sizeof(path),
			 "/sys/class/net/%s/statistics/tx_packets", txname);
		tx_after = read_u64_file(path);
		snprintf(path, sizeof(path),
			 "/sys/class/net/%s/statistics/rx_packets", rxname);
		rx_after = read_u64_file(path);
	}

	printf("[stats] %s tx_packets +%llu, %s rx_packets +%llu\n",
	       txname, tx_after - tx_before, rxname, rx_after - rx_before);

	if (tx_after - tx_before < 560) {
		fail("tx_packets did not increase as expected");
		goto out;
	}
	if (rx_after - rx_before < 560) {
		fail("rx_packets did not increase as expected");
		goto out;
	}

	printf("SELFTEST PASS\n");
	close(txf);
	close(rxf);
	return failures ? 1 : 0;

out:
	printf("SELFTEST FAIL\n");
	close(txf);
	close(rxf);
	return 1;
}
