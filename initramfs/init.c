/*
 * Minimal init for mainline bring-up on ALPS k50v1_64_bsp (MT6750).
 *
 * No shell, no busybox: mount the pseudo filesystems, dump a hardware
 * census to the serial console, prove the eMMC read path works, then
 * idle so the log stays visible.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#if defined(__has_include)
# if __has_include("build_time.h")
#  include "build_time.h"
# endif
#endif
#ifndef BUILD_EPOCH
# define BUILD_EPOCH 1794000000ULL
#endif
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

static void cat(const char *path)
{
	int fd = open(path, O_RDONLY);
	char buf[4096];
	ssize_t n;

	printf("\n===== %s =====\n", path);
	if (fd < 0) {
		printf("(unavailable)\n");
		return;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		write(STDOUT_FILENO, buf, (size_t)n);
	close(fd);
}

static void listdir(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *de;
	int col = 0;

	printf("\n===== %s =====\n", path);
	if (!d) {
		printf("(unavailable)\n");
		return;
	}
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		printf("%-24s", de->d_name);
		if (++col % 3 == 0)
			printf("\n");
	}
	if (col % 3)
		printf("\n");
	closedir(d);
}

/*
 * Read the first @len bytes of @path and print them as hex+ascii.
 * Used to prove the eMMC block layer really returns data: mmcblk0p3 is
 * the expdb partition, whose contents we already dumped from Android.
 */
static void hexdump_dev(const char *path, int len)
{
	unsigned char buf[512];
	int fd, i, n;

	if (len > (int)sizeof(buf))
		len = sizeof(buf);

	printf("\n===== hexdump %s (first %d bytes) =====\n", path, len);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("(open failed)\n");
		return;
	}
	n = read(fd, buf, (size_t)len);
	close(fd);
	if (n <= 0) {
		printf("(read returned %d)\n", n);
		return;
	}

	for (i = 0; i < n; i++) {
		if (i % 16 == 0)
			printf("%04x  ", i);
		printf("%02x ", buf[i]);
		if (i % 16 == 15) {
			int j;
			printf(" |");
			for (j = i - 15; j <= i; j++)
				putchar(buf[j] >= 32 && buf[j] < 127 ? buf[j] : '.');
			printf("|\n");
		}
	}
	if (i % 16) {
		int j = i % 16;
		while (j++ < 16)
			printf("   ");
		printf(" |");
		for (j = i - (i % 16); j < i; j++)
			putchar(buf[j] >= 32 && buf[j] < 127 ? buf[j] : '.');
		printf("|\n");
	}
	printf("(read %d bytes)\n", n);
}

/*
 * devtmpfs in the initramfs does not reliably carry nodes for devices
 * probed before /dev was mounted. Instead of hardcoding block major/minor
 * (partition numbering is not guaranteed to match the GPT order), read the
 * authoritative dev_t and partition name from sysfs uevent.
 *
 * Returns 0 on success and fills @name with PARTNAME when present.
 */
static int make_node_from_sysfs(int part, const char *node, char *name, size_t namelen)
{
	char path[128], buf[512], *p;
	int fd, n, ret = -1;
	unsigned int maj = 0, min = 0;

	name[0] = 0;
	snprintf(path, sizeof(path), "/sys/class/block/mmcblk0p%d/uevent", part);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;

	p = strstr(buf, "PARTNAME=");
	if (p) {
		char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - (p + 9)) : strlen(p + 9);
		if (len >= namelen)
			len = namelen - 1;
		memcpy(name, p + 9, len);
		name[len] = 0;
	}

	p = strstr(buf, "MAJOR=");
	if (p)
		maj = (unsigned int)strtoul(p + 6, NULL, 10);
	p = strstr(buf, "MINOR=");
	if (p)
		min = (unsigned int)strtoul(p + 6, NULL, 10);

	if (maj && (mknod(node, S_IFBLK | 0600, makedev(maj, min)) == 0 ||
		    errno == EEXIST))
		ret = 0;
	return ret;
}

static int try_mount(const char *dev, const char *mnt)
{
	int r;

	mkdir(mnt, 0755);
	r = mount(dev, mnt, "ext4", MS_RDONLY, "");
	if (r == 0)
		printf("mount %s -> %s : OK\n", dev, mnt);
	else
		printf("mount %s -> %s : FAILED errno %d (%s)\n",
		       dev, mnt, errno, strerror(errno));
	if (r == 0)
		listdir(mnt);
	return r;
}

/*
 * Probe an i2c bus by talking to every valid 7-bit address. A device that
 * ACKs but cannot be read still counts as present (many chips need a
 * register address written first, so read() returns -EREMOTEIO).
 */
static void i2c_scan(int bus)
{
	char path[32], node[32];
	int fd, addr, hits = 0;
	int e_nxio = 0, e_timeout = 0, e_other = 0;
	unsigned char buf;

	/* i2c-dev is char major 89, /dev/i2c-N is minor N */
	snprintf(node, sizeof(node), "/dev/i2c-%d", bus);
	mknod(node, S_IFCHR | 0600, makedev(89, bus));

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	printf("\n===== i2c scan bus %d =====\n", bus);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		printf("(open %s failed: errno %d: %s)\n", path, errno, strerror(errno));
		return;
	}

	for (addr = 0x08; addr <= 0x77; addr++) {
		int r;

		/* progress marker: tells us exactly which address wedges */
		if ((addr & 0x07) == 0) {
			printf("[%02x]", addr);
			fflush(stdout);
		}
		if (ioctl(fd, I2C_SLAVE, addr) < 0)
			continue;
		r = read(fd, &buf, 1);
		if (r == 1) {
			printf("  0x%02x ACK, read 0x%02x\n", addr, buf);
			hits++;
		} else if (errno == EREMOTEIO || errno == EIO) {
			printf("  0x%02x ACK (no data, errno %d)\n", addr, errno);
			hits++;
		} else {
			if (errno == ENXIO)
				e_nxio++;
			else if (errno == ETIMEDOUT)
				e_timeout++;
			else {
				e_other++;
				if (e_other <= 3)
					printf("  0x%02x errno %d (%s)\n",
					       addr, errno, strerror(errno));
			}
		}
	}
	close(fd);
	printf("bus %d: %d ACK | ENXIO=%d ETIMEDOUT=%d other=%d\n",
	       bus, hits, e_nxio, e_timeout, e_other);
}

/*
 * Run the scan in a child so a wedged i2c transfer cannot take the whole
 * census down with it: the parent keeps the console and reports the stall.
 */
static void run_guarded(void (*fn)(int, int), int a, int b, const char *what)
{
	pid_t p, r;
	int i, st;

	p = fork();
	if (p < 0) {
		printf("fork failed: errno %d\n", errno);
		return;
	}
	if (p == 0) {
		fn(a, b);
		_exit(0);
	}
	for (i = 0; i < 150; i++) {
		r = waitpid(p, &st, WNOHANG);
		if (r == p)
			return;
		usleep(100000);
	}
	printf("\n%s: STALLED for 15 s, killing the child\n", what);
	kill(p, SIGKILL);
	waitpid(p, &st, 0);
}

/*
 * One address, one step at a time, with the label printed *before* the
 * syscall and flushed. The step that never returns is the culprit: the
 * driver's own transfer timeout is only 2 s (adap.timeout = 2*HZ), so a
 * 15 s stall means we never even reached it.
 */
static void i2c_step_probe(int bus, int addr)
{
	char path[16];
	int fd, r;
	unsigned char b = 0;
	struct i2c_msg m;
	struct i2c_rdwr_ioctl_data d;

	m.addr = (unsigned short)addr;
	m.flags = 0;
	m.len = 0;
	m.buf = &b;
	d.msgs = &m;
	d.nmsgs = 1;

	printf("\n=== i2c bus %d, addr 0x%02x, step by step ===\n", bus, addr);
	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);

	printf("  open            ... "); fflush(stdout);
	fd = open(path, O_RDWR);
	printf("fd=%d\n", fd); fflush(stdout);
	if (fd < 0)
		return;

	printf("  ioctl I2C_SLAVE ... "); fflush(stdout);
	r = ioctl(fd, I2C_SLAVE, addr);
	printf("ret=%d\n", r); fflush(stdout);

	printf("  0-len write     ... "); fflush(stdout);
	r = ioctl(fd, I2C_RDWR, &d);
	printf("ret=%d errno=%d (%s)\n", r, errno, strerror(errno)); fflush(stdout);

	printf("  1-byte read     ... "); fflush(stdout);
	r = read(fd, &b, 1);
	printf("ret=%d errno=%d (%s) val=0x%02x\n",
	       r, errno, strerror(errno), b); fflush(stdout);

	close(fd);
}

static void step_probe_touch(int a, int b)
{
	i2c_step_probe(a, b);
}

static void scan_fn(int bus, int unused)
{
	(void)unused;
	i2c_scan(bus);
}

/*
 * /dev/mem mmap() refuses the 0x1000_xxxx block (GPIO / IOCFG) with EINVAL,
 * and pread/pwrite are rejected too, so the only way to look at the pin
 * controller is through the kernel driver (see pinctrl_debugfs below).
 * The 0x1100_xxxx peripherals are still reachable through /dev/mem.
 */
static int mem_fd = -1;

/*
 * Print the lines of @path that contain @needle (used for pinmux-pins,
 * whose lines start with "pin <n>", not with the zero-padded pin number).
 */
static void grep_file_sub(const char *path, const char *needle)
{
	FILE *f = fopen(path, "r");
	char line[256];

	if (!f) {
		printf("(open %s failed: errno %d)\n", path, errno);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, needle))
			printf("%s", line);
	}
	fclose(f);
}

/*
 * The kernel's own pin controller is the only way to see the 0x1000_xxxx
 * bank: /dev/mem refuses those pages, but mt6755-pinctrl maps them.
 *
 * DO NOT read ".../pins": pinctrl_pins_show() walks all 190 pins and the
 * mt6755 pin table has holes (no PUPD / PULLEN range for dozens of pins),
 * which makes mtk_hw_pin_field_lookup() dereference an unset field and
 * oops. That killed init and panicked the kernel once already.
 *
 * "pinmux-pins" only reads the core's software bookkeeping, so it is safe.
 * It tells us whether i2c0's pinctrl-0 state was really applied:
 * a claimed pin means mtk_pctrl_set_mux() ran and returned 0.
 */
static void pinctrl_debugfs(void)
{
	DIR *d;
	struct dirent *de;
	int found = 0;

	printf("\n===== pinctrl (kernel view of 0x1000_xxxx) =====\n");
	d = opendir("/sys/kernel/debug/pinctrl");
	if (!d) {
		printf("(no pinctrl debugfs: errno %d %s)\n", errno, strerror(errno));
		return;
	}
	while ((de = readdir(d))) {
		char p[320];

		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p),
			 "/sys/kernel/debug/pinctrl/%s/pinmux-pins", de->d_name);
		if (access(p, R_OK) != 0)
			continue;
		found = 1;
		printf("--- %s ---\n", de->d_name);
		grep_file_sub(p, "pin 91 ");
		grep_file_sub(p, "pin 92 ");
		grep_file_sub(p, "pin 93 ");
		grep_file_sub(p, "pin 94 ");
	}
	closedir(d);
	if (!found)
		printf("(no pin controller registered)\n");
}

/*
 * Read the actual pad level of GPIO92/93 through gpiolib. That works even
 * while the pins are muxed to the I2C0 function, because the GPIO input
 * data register is fed by the pad and stays live.
 *
 * Idle I2C needs both lines high (released, pulled up). A stuck-low line
 * means either no pull-up on the bus or a device holding it down.
 */
static int gpio_find_chip(const char *name, char *out, size_t outlen)
{
	char path[32];
	int i;

	(void)name;
	printf("gpiochips:\n");
	for (i = 0; i < 8; i++) {
		struct gpiochip_info ci;
		struct gpioline_info li;
		int fd, r;

		snprintf(path, sizeof(path), "/dev/gpiochip%d", i);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(&ci, 0, sizeof(ci));
		r = ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &ci);
		if (r == 0)
			printf("  %s: name=%s label=%s lines=%u\n",
			       path, ci.name, ci.label, ci.lines);
		else
			printf("  %s: chipinfo errno %d\n", path, errno);

		memset(&li, 0, sizeof(li));
		li.line_offset = 92;
		r = ioctl(fd, GPIO_GET_LINEINFO_IOCTL, &li);

		/* paris leaves line names empty; match on the chip label */
		if (r == 0 && strstr(ci.label, "paris")) {
			snprintf(out, outlen, "%s", path);
			close(fd);
			return 0;
		}
		close(fd);
	}
	return -1;
}

static int gpio_line_read(const char *chip, unsigned int off)
{
	struct gpiohandle_request req;
	struct gpiohandle_data data;
	int fd, ret;

	fd = open(chip, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	memset(&req, 0, sizeof(req));
	req.lineoffsets[0] = off;
	req.flags = GPIOHANDLE_REQUEST_INPUT;
	req.lines = 1;
	snprintf(req.consumer_label, sizeof(req.consumer_label), "bringup");
	if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
		close(fd);
		return -1;
	}
	memset(&data, 0, sizeof(data));
	ret = ioctl(req.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
	close(req.fd);
	close(fd);
	if (ret < 0)
		return -1;
	return data.values[0];
}

/*
 * Release the touch controller from reset.
 *
 * The DCT table in the LK tree claims GPIO10, but Android's own pin dump
 * shows pin 10 held LOW at runtime while pin 21 is switched from its boot
 * function (MODE 4, DAP_SIB1_SWD) to plain GPIO and driven HIGH. That is the
 * reset line of the AXS controller (0x5d).
 *
 *   MODE3 @ 0x10005320, pins 20..24, 3 bits each -> pin 21 = bits 3..5
 *   DIR   @ 0x10005000, bit 21
 *   DOUT  @ 0x10005100, bit 21
 */
static void ctp_reset_release(void)
{
	const unsigned int gpio = 0x10005000;
	int fd;
	void *m;
	volatile unsigned int *p;
	unsigned int mode;

	printf("\n===== CTP reset release (GPIO21) =====\n");
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("  /dev/mem errno %d (%s)\n", errno, strerror(errno));
		return;
	}
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, gpio);
	close(fd);
	if (m == MAP_FAILED) {
		printf("  mmap errno %d (%s)\n", errno, strerror(errno));
		return;
	}
	p = (volatile unsigned int *)m;

	mode = p[0x320 / 4];
	printf("  MODE3 before : 0x%08x (pin21=%u)\n", mode, (mode >> 3) & 7);
	p[0x320 / 4] = mode & ~(7u << 3);		/* pin 21 -> function 0 (GPIO) */
	p[0x000 / 4] |= (1u << 21);			/* DIR: output */
	p[0x100 / 4] |= (1u << 21);			/* DOUT: high */
	printf("  MODE3 after  : 0x%08x (pin21=%u)  DIR=0x%08x DOUT=0x%08x\n",
	       p[0x320 / 4], (p[0x320 / 4] >> 3) & 7,
	       p[0x000 / 4], p[0x100 / 4]);

	munmap(m, 0x1000);
	usleep(30000);	/* let the controller boot before anyone talks to it */
}

static void gpio_level_probe(void)
{
	/*
	 * Do not go through the GPIO character device here: i2c-gpio already
	 * owns pins 92/93, so a second request fails with EBUSY and tells us
	 * nothing about the actual pad level. Read the pad input register
	 * instead -- DIN2 @ 0x10005220 covers GPIO64..95, so bit 28 is
	 * GPIO92 (SDA) and bit 29 is GPIO93 (SCL).
	 */
	unsigned int din, din2;
	int fd;
	void *m;
	volatile unsigned int *p;
	int sda, scl;

	printf("\n===== pad level (DIN2 @ 0x10005220) =====\n");
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("  /dev/mem errno %d (%s)\n", errno, strerror(errno));
		return;
	}
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x10005000);
	close(fd);
	if (m == MAP_FAILED) {
		printf("  mmap errno %d (%s)\n", errno, strerror(errno));
		return;
	}
	p = (volatile unsigned int *)m;

	din = p[0x220 / 4];
	sda = (din >> 28) & 1;
	scl = (din >> 29) & 1;
	printf("  DIN2 = 0x%08x   GPIO92(SDA0)=%d  GPIO93(SCL0)=%d\n",
	       din, sda, scl);
	usleep(200000);
	din2 = p[0x220 / 4];
	printf("  after 200ms:    0x%08x   SDA=%d SCL=%d\n", din2,
	       (int)((din2 >> 28) & 1), (int)((din2 >> 29) & 1));
	printf("  %s\n", (sda == 1 && scl == 1)
		? "both HIGH -> pull-ups powered, bus idle state is correct"
		: "a line is LOW -> pull-up rail off, or a device clamps the bus");
	munmap(m, 0x1000);
}

/*
 * Named register dump of one controller, so we can look at the block *while*
 * a transfer is in flight. If the state machine is alive, START clears and
 * INTR_STAT shows the pending/complete bits; if the function clock is gated
 * the registers just sit there unchanged for the whole 2 s timeout.
 */
static void dump_one_i2c(unsigned int base, const char *tag)
{
	static const struct { int off; const char *name; } regs[] = {
		{ 0x00, "DATA_PORT  " }, { 0x04, "SLAVE_ADDR " },
		{ 0x08, "INTR_MASK  " }, { 0x0c, "INTR_STAT  " },
		{ 0x10, "CONTROL    " }, { 0x14, "TRANSFER_LN" },
		{ 0x18, "TRANSAC_LN " }, { 0x1c, "DELAY_LEN  " },
		{ 0x20, "TIMING     " }, { 0x24, "START      " },
		{ 0x28, "EXT_CONF   " }, { 0x2c, "FIFO_STAT  " },
		{ 0x30, "FIFO_THRESH" }, { 0x34, "DEBUGSTAT  " },
	};
	unsigned int i;
	int fd;
	void *m;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return;
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	close(fd);
	if (m == MAP_FAILED) {
		printf("  mmap 0x%08x failed\n", base);
		return;
	}
	printf("  [%s]\n", tag);
	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
		printf("    %s = 0x%08x\n", regs[i].name,
		       ((volatile unsigned int *)m)[regs[i].off / 4]);
	munmap(m, 0x1000);
}

/*
 * Turn on the INFRA_I2C0 clock gate.
 *
 * Source: the vendor clock driver drivers/clk/mediatek/clk-mt6755.c.
 *   infra0_cg_regs: set = +0x80, clr = +0x84, sta = +0x90
 *   GATE(INFRA_I2C0, ..., infra0_cg_regs, 11, 0)
 *   GATE(INFRA_I2C1, ..., infra0_cg_regs, 12, 0)
 * i2c1 works because the preloader switched its gate on; i2c0 was never
 * needed before the kernel, so its gate is still closed -- which is exactly
 * what the frozen START bit and the missing interrupt showed.
 *
 * The status register doubles as a sanity check: bit 12 must already read 1
 * for the known-good i2c1. If it does not, the base address is wrong.
 */
static void i2c_clk_gate_fixup(void)
{
	const unsigned int base = 0x10001000;
	unsigned int sta_before, sta_after;
	int fd;
	void *m;
	volatile unsigned int *p;

	printf("\n===== infrasys clock gate, INFRA_I2C0 = bit 11 @ 0x%08x =====\n",
	       base + 0x80);

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("  /dev/mem: errno %d (%s)\n", errno, strerror(errno));
		return;
	}
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	close(fd);
	if (m == MAP_FAILED) {
		printf("  mmap failed: errno %d (%s)\n", errno, strerror(errno));
		return;
	}
	p = (volatile unsigned int *)m;

	sta_before = p[0x90 / 4];
	printf("  STA before : 0x%08x  i2c0=%u i2c1=%u\n", sta_before,
	       (sta_before >> 11) & 1, (sta_before >> 12) & 1);
	if (!((sta_before >> 12) & 1))
		printf("  WARNING: i2c1 gate reads 0 too -- wrong base address?\n");

	if (!((sta_before >> 11) & 1)) {
		p[0x80 / 4] = 1u << 11;		/* SET bit 11 */
		usleep(1000);
	}

	sta_after = p[0x90 / 4];
	printf("  STA after  : 0x%08x  i2c0=%u i2c1=%u\n", sta_after,
	       (sta_after >> 11) & 1, (sta_after >> 12) & 1);
	printf("  %s\n", ((sta_after >> 11) & 1)
		? "INFRA_I2C0 gate is now OPEN"
		: "gate still closed -- the write did not land");

	munmap(m, 0x1000);
}

static int probe_addr(int bus, int addr)
{
	char path[16];
	int fd, r;
	unsigned char b = 0;

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1000 - errno;
	ioctl(fd, I2C_SLAVE, addr);
	r = read(fd, &b, 1);
	if (r < 0)
		r = -errno;
	else
		r = b;
	close(fd);
	return r;
}

/*
 * Try to open INFRA_I2C0 (bit 11 of the infra0 gate group) and see whether
 * that alone makes 0x5d answer.
 *
 * There are two candidate bases for the "infrasys" node: INFRA (0x10000000)
 * and INFRA_AO (0x10001000). The discriminator is the status register: the
 * block that really holds the I2C gates must show i2c1's bit (12) set,
 * because i2c1 works. The first attempt (0x10001000) read STA = 0 for every
 * bit, so that was the wrong block.
 */
static void i2c_gate_try(void)
{
	static const unsigned int bases[] = { 0x10000000, 0x10001000 };
	unsigned int i;

	printf("\n===== opening INFRA_I2C0 gate, candidate bases =====\n");

	for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
		const unsigned int base = bases[i];
		int fd, ret, j;
		void *m;
		volatile unsigned int *p;

		fd = open("/dev/mem", O_RDWR | O_SYNC);
		if (fd < 0) {
			printf("base 0x%08x: /dev/mem errno %d\n", base, errno);
			continue;
		}
		m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
		close(fd);
		if (m == MAP_FAILED) {
			printf("base 0x%08x: mmap errno %d (%s)\n",
			       base, errno, strerror(errno));
			continue;
		}
		p = (volatile unsigned int *)m;

		printf("base 0x%08x:\n", base);
		printf("  gate windows:");
		for (j = 0x80; j <= 0xac; j += 4)
			printf(" %02x=%08x", j, p[j / 4]);
		printf("\n");
		printf("  STA0(+0x90)=0x%08x i2c1bit12=%u  STA1(+0x94)=0x%08x\n",
		       p[0x90 / 4], (p[0x90 / 4] >> 12) & 1, p[0x94 / 4]);
		printf("  STA2(+0xac)=0x%08x\n", p[0xac / 4]);

		if (!((p[0x90 / 4] >> 12) & 1)) {
			printf("  bit12 (i2c1) is 0 here -> not the I2C gate block, skip\n");
			munmap(m, 0x1000);
			continue;
		}

		printf("  bit12 is set: this looks like the right block\n");
		printf("  SET bit11 ... ");
		p[0x80 / 4] = 1u << 11;
		usleep(2000);
		printf("STA0 now 0x%08x (bit11=%u)\n",
		       p[0x90 / 4], (p[0x90 / 4] >> 11) & 1);

		ret = probe_addr(0, 0x5d);
		printf("  probe 0x5d -> %d %s\n", ret,
		       ret >= 0 ? "ACK!" : strerror(-ret));
		if (ret >= 0) {
			printf("  *** I2C0 IS ALIVE ***\n");
			munmap(m, 0x1000);
			return;
		}
		munmap(m, 0x1000);
	}
}

/*
 * Brute-force the I2C0 clock gate.
 *
 * The vendor clock driver is the only documentation we have, and my reading
 * of its bit numbers did not match reality (STA showed i2c1's bit as 0 even
 * though i2c1 works). Rather than keep guessing, walk every gate bit of the
 * three infra groups: setting a gate bit can only power something up, never
 * cut it, so this is safe to do live. After each bit, poke the touch address
 * and see whether the controller wakes up.
 *
 * infrasys base 0x10001000 (vendor dts: infrasys@10001000, reg 0x1000);
 * gate groups set/clr/sta = +0x80/+0x84/+0x90, +0x88/+0x8c/+0x94,
 * +0xa4/+0xa8/+0xac.
 */
static void i2c_gate_bruteforce(void)
{
	static const unsigned int set_regs[] = { 0x80, 0x88, 0xa4 };
	static const char * const names[] = { "infra0", "infra1", "infra2" };
	unsigned int g, bit;
	int fd, ret;
	void *m;
	volatile unsigned int *p;

	printf("\n===== brute-force the I2C0 gate (safe: only ever SET) =====\n");
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("/dev/mem errno %d\n", errno);
		return;
	}
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x10001000);
	close(fd);
	if (m == MAP_FAILED) {
		printf("mmap errno %d\n", errno);
		return;
	}
	p = (volatile unsigned int *)m;

	for (g = 0; g < 3; g++) {
		printf("%s (set 0x%03x):\n  ", names[g], set_regs[g]);
		for (bit = 0; bit < 32; bit++) {
			p[set_regs[g] / 4] = 1u << bit;
			ret = probe_addr(0, 0x5d);
			if (ret >= 0) {
				printf("\n  *** bit %u wakes i2c0: 0x5d -> 0x%02x ***\n",
				       bit, ret);
				printf("  gate = 0x%03x bit %u\n", set_regs[g], bit);
				munmap(m, 0x1000);
				return;
			}
			/* '.'=still frozen  'N'=controller ran but NACK  '?'=other */
			putchar(ret == -ETIMEDOUT ? '.' : (ret == -ENXIO ? 'N' : '?'));
			fflush(stdout);
		}
		printf("\n");
	}
	printf("no gate bit made 0x5d answer\n");
	munmap(m, 0x1000);
}

static void i2c_live_probe(int bus, int addr)
{
	pid_t p;
	unsigned char b = 0;

	printf("\n===== live capture during a transfer: bus %d addr 0x%02x =====\n",
	       bus, addr);

	p = fork();
	if (p == 0) {
		char path[16];
		int fd;

		snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
		fd = open(path, O_RDWR);
		if (fd >= 0) {
			ioctl(fd, I2C_SLAVE, addr);
			read(fd, &b, 1);
			close(fd);
		}
		_exit(0);
	}

	usleep(300000);
	dump_one_i2c(0x11007000, "t=300ms, transfer in flight");
	usleep(900000);
	dump_one_i2c(0x11007000, "t=1.2s, still in flight?");
	waitpid(p, NULL, 0);
	dump_one_i2c(0x11007000, "after the driver gave up");
}

/*
 * Dump the register file of both I2C controllers side by side. i2c1 is
 * known-good, so if i2c0 reads back all-zero or all-ones its clock gate is
 * still off and no amount of pin muxing will help.
 */
static void i2c_regs_dump(void)
{
	static const int offs[] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14,
				    0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c };
	int fd, i;
	void *m0, *m1;
	volatile unsigned int *p0, *p1;

	printf("\n===== i2c controller registers =====\n");
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("/dev/mem open failed\n");
		return;
	}
	m0 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x11007000);
	m1 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x11008000);
	close(fd);
	if (m0 == MAP_FAILED || m1 == MAP_FAILED) {
		printf("mmap failed\n");
		return;
	}
	p0 = (volatile unsigned int *)m0;
	p1 = (volatile unsigned int *)m1;

	printf("off      i2c0@11007000   i2c1@11008000\n");
	for (i = 0; i < 12; i++)
		printf("0x%02x     0x%08x      0x%08x\n",
		       offs[i], p0[offs[i] / 4], p1[offs[i] / 4]);

	munmap(m0, 0x1000);
	munmap(m1, 0x1000);
}

/*
 * Decisive test for "is the I2C0 block actually clocked?".
 *
 * Registers are cheap to read from a gated APB block -- they come back as
 * whatever the bus floats to. Writes are not: if the peripheral gate is
 * closed the write never lands. SLAVE_ADDR (0x04) is unused in master mode,
 * so scribble on it, read it back, then restore. i2c1 is the control group.
 */
static void i2c_write_test(void)
{
	int fd;
	void *m0, *m1;
	volatile unsigned int *p0, *p1;
	unsigned int s0, s1, r0, r1;

	printf("\n===== i2c register write test (SLAVE_ADDR @ 0x04) =====\n");
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("/dev/mem open failed\n");
		return;
	}
	m0 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x11007000);
	m1 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x11008000);
	close(fd);
	if (m0 == MAP_FAILED || m1 == MAP_FAILED) {
		printf("mmap failed\n");
		return;
	}
	p0 = (volatile unsigned int *)m0;
	p1 = (volatile unsigned int *)m1;

	s0 = p0[1];
	s1 = p1[1];
	p0[1] = 0x5a;
	p1[1] = 0x5a;
	r0 = p0[1];
	r1 = p1[1];

	printf("bus0 (i2c0): wr 0x5a -> rd 0x%08x   %s\n", r0,
	       r0 == 0x5a ? "WRITABLE -> block is clocked"
			  : "write LOST -> block is gated or held in reset");
	printf("bus1 (i2c1): wr 0x5a -> rd 0x%08x   %s\n", r1,
	       r1 == 0x5a ? "WRITABLE (control)" : "write LOST (unexpected)");

	p0[1] = s0;
	p1[1] = s1;
	munmap(m0, 0x1000);
	munmap(m1, 0x1000);
}

/* Write @val (NUL-terminated) into @path. Returns 0 on success. */
static int wf(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	int ret;

	if (fd < 0)
		return -1;
	ret = write(fd, val, strlen(val));
	close(fd);
	return ret < 0 ? -1 : 0;
}

/*
 * Write a *regular* file (scripts, /etc/profile, resolv.conf).
 *
 * wf() below is for sysfs/configfs nodes, which must already exist; using it
 * on a new path fails silently with ENOENT, which is why /bin/alpine and
 * /sbin/reboot never appeared on the first try.
 */
static int wfile(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	int ret;

	if (fd < 0)
		return -1;
	ret = write(fd, val, strlen(val));
	close(fd);
	return ret < 0 ? -1 : 0;
}

/*
 * Open the two clock gates the SSUSB block needs.
 *
 * The vendor clock driver (clk-mt6755.c) has:
 *   GATE(INFRA_SSUSB_TOP, ..., infra2_cg_regs, 1, 0)
 *   GATE(PERI_USB0,       ..., peri0_cg_regs, 10, 0)
 * with infrasys @ 0x10001000 (set 0xa4 / clr 0xa8 / sta 0xac) and
 * perisys @ 0x10003000 (set 0x08 / clr 0x10 / sta 0x18).
 *
 * There is no MT6755 clock driver yet, so mtu3 cannot ask CCF for these --
 * poke them here before it retries. The kernel's first probe at boot happens
 * before we get here, which is why we unbind/bind afterwards.
 */
static void usb_gate_open(void)
{
	int fd;
	void *m;
	volatile unsigned int *p;

	printf("\n===== USB clock gates =====\n");
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("  /dev/mem errno %d (%s)\n", errno, strerror(errno));
		return;
	}
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x10001000);
	if (m == MAP_FAILED) {
		printf("  mmap infrasys errno %d (%s)\n", errno, strerror(errno));
		close(fd);
		return;
	}
	p = (volatile unsigned int *)m;
	printf("  INFRA_SSUSB_TOP sta before: 0x%08x (bit1=%u)\n",
	       p[0xac / 4], (p[0xac / 4] >> 1) & 1);
	p[0xa4 / 4] = 1u << 1;
	printf("  INFRA_SSUSB_TOP sta after : 0x%08x (bit1=%u)\n",
	       p[0xac / 4], (p[0xac / 4] >> 1) & 1);
	munmap(m, 0x1000);
	close(fd);

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return;
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x10003000);
	if (m == MAP_FAILED) {
		printf("  mmap perisys errno %d (%s)\n", errno, strerror(errno));
		close(fd);
		return;
	}
	p = (volatile unsigned int *)m;
	printf("  PERI_USB0       sta before: 0x%08x (bit10=%u)\n",
	       p[0x18 / 4], (p[0x18 / 4] >> 10) & 1);
	p[0x08 / 4] = 1u << 10;
	printf("  PERI_USB0       sta after : 0x%08x (bit10=%u)\n",
	       p[0x18 / 4], (p[0x18 / 4] >> 10) & 1);
	munmap(m, 0x1000);
	close(fd);
}

/*
 * Bring up a CDC-ACM gadget on the first UDC that shows up.
 *
 * The whole point of USB here is a second console without the CH340 wires,
 * so build the gadget automatically once mtu3 has registered a UDC. This is
 * done purely through configfs -- exactly what a userland gadget manager
 * would do, no kernel changes involved.
 */
/* Name of the UDC we bound to, so the state file can be polled by name. */
static char g_udc[64];

/* Return 1 and fill @udc as soon as a UDC is registered, 0 on timeout. */
static int udc_find(char *udc, size_t len, int tenths)
{
	DIR *d;
	struct dirent *de;
	int i;

	udc[0] = 0;
	for (i = 0; i < tenths && !udc[0]; i++) {
		d = opendir("/sys/class/udc");
		if (d) {
			while ((de = readdir(d))) {
				if (de->d_name[0] == '.')
					continue;
				snprintf(udc, len, "%s", de->d_name);
				break;
			}
			closedir(d);
		}
		if (!udc[0])
			usleep(100000);
	}
	return udc[0] != 0;
}

/*
 * Read /sys/class/udc/<udc>/state. The kernel reports one of
 * "not attached", "powered", "default", "addressed", "configured".
 * "not attached" is the one that matters here: it means there is no host on
 * the other end of the cable at all (unplugged, or plugged into a charger).
 */
static int udc_state(char *buf, size_t len)
{
	char path[128];
	FILE *f;

	buf[0] = 0;
	if (!g_udc[0])
		return -1;
	snprintf(path, sizeof(path), "/sys/class/udc/%s/state", g_udc);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, len, f))
		buf[0] = 0;
	fclose(f);

	{
		size_t n = strlen(buf);
		while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			buf[--n] = 0;
	}
	return 0;
}

/*
 * Wait until the state file contains @need.
 *
 * The whole point of gating on this: udhcpc against an empty bus just burns
 * its retries and then we would fall back to being a DHCP *server* on a link
 * with no peer -- which is what left us with 192.168.42.1 while the PC sat on
 * 192.168.137.x and nothing could reach anything. Waiting for "configured"
 * means the host has enumerated us, so whichever addressing model it wants
 * (ICS DHCP or manual static) is actually usable.
 */
static int usb_wait_host(const char *need, int tenths, int verbose)
{
	char st[64];
	int i;

	for (i = 0; i < tenths; i++) {
		if (udc_state(st, sizeof(st)) == 0 && strstr(st, need))
			return 1;
		if (verbose && (i % 50) == 49) {
			printf("  waiting for the USB host ... state=%s (%ds)\n",
			       st[0] ? st : "?", (i + 1) / 10);
			fflush(stdout);
		}
		usleep(100000);
	}
	return 0;
}

/*
 * Like usb_wait_host(), but "anything except not attached" counts.
 *
 * The background watcher uses this because the exact state string depends on
 * how far the host got: a Windows box that has not loaded the NCM driver yet
 * parks us at "powered"/"addressed", and waiting for "configured" there would
 * miss the moment the cable was plugged in.
 */
static int usb_wait_link(int tenths)
{
	char st[64];
	int i;

	for (i = 0; i < tenths; i++) {
		if (udc_state(st, sizeof(st)) == 0 && st[0] &&
		    !strstr(st, "not attached"))
			return 1;
		usleep(100000);
	}
	return 0;
}

/*
 * Replay the U2 PHY bring-up that the vendor driver performs in
 * phy_init_soc() (mu3phy/mt6755/mtk-phy-asic.c), by hand.
 *
 * Mainline's phy-mtk-tphy is a descendant of the same driver but runs the
 * MT8173 variant of the sequence, and one of the differences is visible in
 * the results: the vendor forces VBUSVALID / AVALID / SESSEND all to 1 at
 * the end, while mainline clears SESSEND. On this PHY that leaves the
 * transceiver dead: the MAC happily sets SOFT_CONN, the PHY registers all
 * read back sane, and nothing ever appears on D+.
 *
 * Register block: U2 PHY COM at 0x11290800 (port base), offsets below are
 * taken from mtk-phy-asic.h so they are the vendor's own numbers.
 */
#define U2PHY_BASE	0x11290000
#define U2PHY_COM	0x800

static void usb_phy_vendor_seq(void)
{
	int fd;
	void *m;
	volatile unsigned int *p;
	unsigned int v;

	printf("\n===== USB PHY: replaying the vendor U2 sequence =====\n");
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("  /dev/mem errno %d (%s)\n", errno, strerror(errno));
		return;
	}
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, U2PHY_BASE);
	close(fd);
	if (m == MAP_FAILED) {
		printf("  mmap errno %d (%s)\n", errno, strerror(errno));
		return;
	}
	p = (volatile unsigned int *)m;

	/* USBPHYACR6 @0x18: iso off, BC1.1 off, OTG vbus comp on, SQTH=2 */
	v = p[(U2PHY_COM + 0x18) / 4];
	v &= ~(1u << 31);
	v &= ~(1u << 23);
	v |= (1u << 20);
	v = (v & ~0xfu) | 0x2u;
	p[(U2PHY_COM + 0x18) / 4] = v;

	/* USBPHYACR5 @0x14: 100uA switch to SSUSB */
	p[(U2PHY_COM + 0x14) / 4] |= (1u << 11);

	/* U2PHYACR4 @0x20: dp_100k mode, enable the 100k paths off, no GPIO */
	v = p[(U2PHY_COM + 0x20) / 4];
	v |= (1u << 18);
	v &= ~((1u << 17) | (1u << 16) | (1u << 9) | (1u << 8));
	p[(U2PHY_COM + 0x20) / 4] = v;

	/* U2PHYDTM0 @0x68: not in UART mode, release force-suspendm */
	v = p[(U2PHY_COM + 0x68) / 4];
	v &= ~((1u << 26) | (1u << 18));
	p[(U2PHY_COM + 0x68) / 4] = v;

	/* U2PHYDTM1 @0x6c: UART off */
	v = p[(U2PHY_COM + 0x6c) / 4];
	v &= ~(1u << 16);
	p[(U2PHY_COM + 0x6c) / 4] = v;

	usleep(1000);	/* vendor waits 800 us here */

	/* U2PHYDTM1: force VBUSVALID(13) | SESSEND(12) | AVALID(10) */
	v = p[(U2PHY_COM + 0x6c) / 4];
	v |= (1u << 13) | (1u << 12) | (1u << 10);
	p[(U2PHY_COM + 0x6c) / 4] = v;

	printf("  ACR6=0x%08x ACR5=0x%08x ACR4=0x%08x\n",
	       p[(U2PHY_COM + 0x18) / 4], p[(U2PHY_COM + 0x14) / 4],
	       p[(U2PHY_COM + 0x20) / 4]);
	printf("  DTM0=0x%08x DTM1=0x%08x\n",
	       p[(U2PHY_COM + 0x68) / 4], p[(U2PHY_COM + 0x6c) / 4]);
	munmap(m, 0x1000);
}

static void usb_rebind(void)
{
	wf("/sys/bus/platform/drivers/mtu3/unbind", "11271000.usb");
	usleep(20000);
	if (wf("/sys/bus/platform/drivers/mtu3/bind", "11271000.usb") == 0)
		printf("  mtu3 re-bound\n");
	else
		printf("  mtu3 rebind failed: errno %d (%s)\n",
		       errno, strerror(errno));
	usleep(50 * 1000);
}

/*
 * Put an IP on the RNDIS link.
 *
 * Static address on our side plus a tiny DHCP server, so Windows configures
 * itself without anyone touching adapter settings. telnetd is started too --
 * no password, so treat the USB link as trusted (it is point to point).
 */
/*
 * busybox udhcpc only prints "lease obtained"; applying it is the job of a
 * script it calls. The initramfs has none, so without this the interface is
 * left with no IPv4 address and no route -- which is exactly what happened
 * the first time ICS handed us 192.168.137.122.
 */
static const char *UDHCPC_SCRIPT =
	"#!/bin/sh\n"
	"case \"$1\" in\n"
	"deconfig)\n"
	"  ifconfig $interface 0.0.0.0 2>/dev/null\n"
	"  ;;\n"
	"bound|renew)\n"
	"  ifconfig $interface $ip netmask ${subnet:-255.255.255.0} up\n"
	"  if [ -n \"$router\" ]; then set -- $router; route add default gw $1 2>/dev/null; fi\n"
	"  if [ -n \"$dns\" ]; then\n"
	"    : > /etc/resolv.conf\n"
	"    for d in $dns; do echo \"nameserver $d\" >> /etc/resolv.conf; done\n"
	"  fi\n"
	"  ;;\n"
	"esac\n"
	"exit 0\n";

static void usb_net_setup(void)
{
	int have_ip = 0, round;

	printf("\n===== USB network (NCM) =====\n");
	if (access("/sys/class/net/usb0", F_OK) != 0) {
		printf("  usb0 missing -- NCM function did not bind\n");
		return;
	}
	system("/bin/busybox ifconfig usb0 up");

	/* busybox udhcpc only prints the lease unless a script applies it */
	mkdir("/usr", 0755);
	mkdir("/usr/share", 0755);
	mkdir("/usr/share/udhcpc", 0755);
	wfile("/usr/share/udhcpc/default.script", UDHCPC_SCRIPT);
	chmod("/usr/share/udhcpc/default.script", 0755);

	/*
	 * Preferred path: whoever is on the other end runs DHCP (Windows ICS).
	 * Retry a few rounds: right after enumeration the host's DHCP service
	 * is not always listening yet, and a single short attempt used to be
	 * enough to send us down the static fallback below -- which is how the
	 * board ended up on 192.168.42.1 while the PC sat on 192.168.137.1.
	 */
	for (round = 0; round < 3 && !have_ip; round++) {
		printf("  dhcp attempt %d/3 ...\n", round + 1);
		fflush(stdout);
		system("/bin/busybox udhcpc -i usb0 -n -q -t 5 -T 2 "
		       "-s /usr/share/udhcpc/default.script >/dev/null 2>&1");
		have_ip = system("/bin/busybox ifconfig usb0 | "
				 "/bin/busybox grep -q 'inet addr'") == 0;
		if (!have_ip) {
			printf("  no lease yet\n");
			usleep(1500 * 1000);
		}
	}

	if (have_ip) {
		/*
		 * Also answer on 192.168.42.1. The PC side is configured by
		 * hand on some setups (`netsh ... static 192.168.42.2`) and by
		 * ICS on others; carrying both addresses means either wiring
		 * works without touching the board again.
		 */
		system("/bin/busybox ip addr add 192.168.42.1/24 dev usb0 "
		       "2>/dev/null");
		printf("  DHCP lease obtained\n");
	} else {
		/*
		 * No DHCP answer. The PC is then either running ICS (it owns
		 * 192.168.137.1 and expects us to ask for an address) or it has
		 * a hand-set 192.168.42.2. Rather than guessing, put 42.1 on
		 * first, add a 137.50 alias, then probe both gateways and keep
		 * whichever answers -- so the board is reachable either way.
		 */
		printf("  no DHCP lease, falling back to static addressing\n");
		system("/bin/busybox ifconfig usb0 192.168.42.1 netmask 255.255.255.0 up");
		system("/bin/busybox ip addr add 192.168.137.50/24 dev usb0 "
		       "2>/dev/null");

		if (system("/bin/busybox ping -c 1 -W 2 192.168.137.1 "
			   ">/dev/null 2>&1") == 0) {
			system("/bin/busybox route add default gw 192.168.137.1 "
			       "2>/dev/null");
			printf("  PC answers on 192.168.137.1 (ICS) -> gateway set\n");
			printf("  use: telnet 192.168.137.50\n");
		} else if (system("/bin/busybox ping -c 1 -W 2 192.168.42.2 "
				  ">/dev/null 2>&1") == 0) {
			printf("  PC answers on 192.168.42.2 -> static pair\n");
			printf("  use: telnet 192.168.42.1\n");
		} else {
			printf("  no PC on either subnet, serving DHCP on 42.1\n");
			wfile("/etc/udhcpd.conf",
			   "start 192.168.42.20\n"
			   "end 192.168.42.40\n"
			   "interface usb0\n"
			   "lease_file /tmp/udhcpd.leases\n"
			   "option subnet 255.255.255.0\n"
			   "option router 192.168.42.1\n"
			   "option dns 192.168.42.1\n");
			system("/bin/busybox udhcpd /etc/udhcpd.conf");
		}
	}

	/* A resolver must exist even when the lease carried no DNS option. */
	if (access("/etc/resolv.conf", F_OK) != 0 ||
	    system("/bin/busybox grep -q nameserver /etc/resolv.conf") != 0)
		wfile("/etc/resolv.conf", "nameserver 223.5.5.5\nnameserver 119.29.29.29\n");

	system("/bin/busybox ifconfig usb0");
	system("/bin/busybox ip route");
	printf("  link ready -- from the PC: telnet <addr above>\n");
	fflush(stdout);
}

/*
 * Mount the Android userdata partition read-write. It holds the Alpine
 * root filesystem; 22 GiB free, and nothing else on this board is writable
 * (vendor/system are mounted read-only on purpose).
 */
static void mount_userdata(void)
{
	mkdir("/mnt", 0755);
	mkdir("/mnt/data", 0755);
	if (access("/mnt/data/alpine/bin/sh", F_OK) == 0) {
		printf("  /mnt/data already mounted, alpine rootfs present\n");
		return;
	}
	if (mount("/dev/mmcblk0p30", "/mnt/data", "ext4",
		  MS_NOATIME, "errors=continue") != 0) {
		printf("  userdata mount failed: errno %d (%s)\n",
		       errno, strerror(errno));
		return;
	}
	printf("  userdata mounted on /mnt/data (alpine: %s)\n",
	       access("/mnt/data/alpine/bin/sh", F_OK) == 0 ? "yes" : "missing");
}

/*
 * /bin/alpine -- one command to get into the real userland.
 *
 * chroot needs the kernel filesystems and a resolver inside the root; doing
 * it by hand every boot is exactly the kind of friction that makes a
 * bring-up board annoying to use.
 */
static void install_alpine_helper(void)
{
	static const char *s =
		"#!/bin/sh\n"
		"R=/mnt/data/alpine\n"
		"if [ ! -x $R/bin/sh ]; then echo \"no alpine rootfs on /mnt/data\"; exit 1; fi\n"
		"mountpoint -q $R/proc || mount -t proc none $R/proc\n"
		"mountpoint -q $R/sys  || mount -t sysfs none $R/sys\n"
		"mountpoint -q $R/dev  || mount --bind /dev $R/dev\n"
		"grep -q nameserver $R/etc/resolv.conf 2>/dev/null ||"
		" echo 'nameserver 223.5.5.5' > $R/etc/resolv.conf\n"
		"exec chroot $R /bin/sh -l\n";

	mkdir("/bin", 0755);
	wfile("/bin/alpine", s);
	chmod("/bin/alpine", 0755);
}

static void usb_gadget_setup(void)
{
	static const char *g = "/sys/kernel/config/usb_gadget/g1";
	char udc[64] = "";

	printf("\n===== USB gadget (CDC-ACM) =====\n");
	mkdir("/sys/kernel/config", 0755);
	if (mount("configfs", "/sys/kernel/config", "configfs", 0, "") != 0 &&
	    errno != EBUSY)
		printf("  configfs mount failed: errno %d (%s)\n",
		       errno, strerror(errno));

	/*
	 * Always replay the vendor U2 PHY sequence: mainline's tphy leaves the
	 * transceiver in a state where every register reads back fine and the
	 * MAC happily sets SOFT_CONN, but no D+ pull-up ever reaches the wire.
	 * Confirmed on hardware: applying this by hand turned
	 * "state = not attached / 0 interrupts" into "configured / 70 irqs".
	 */
	usb_phy_vendor_seq();

	/*
	 * With mediatek,u3p-dis-msk the probe should already have succeeded
	 * during boot, so normally the UDC is simply there. If it is not,
	 * fall back to opening the SSUSB clock gate by hand and rebinding --
	 * that path is known to be flaky (it can clear IP_PW_STS1), so it is
	 * only a last resort.
	 */
	if (!udc_find(udc, sizeof(udc), 5)) {
		printf("  no UDC after the boot probe\n");
		usb_rebind();
		if (!udc_find(udc, sizeof(udc), 20)) {
			printf("  rebind did not help, trying the clock gate\n");
			usb_gate_open();
			usb_rebind();
			if (!udc_find(udc, sizeof(udc), 20)) {
				printf("  still no UDC -> mtu3 did not probe\n");
				return;
			}
		}
	}
	printf("  UDC: %s\n", udc);

	mkdir(g, 0755);
	wf("/sys/kernel/config/usb_gadget/g1/idVendor", "0x1d6b");
	wf("/sys/kernel/config/usb_gadget/g1/idProduct", "0x0112");
	wf("/sys/kernel/config/usb_gadget/g1/bcdUSB", "0x0200");
	wf("/sys/kernel/config/usb_gadget/g1/bcdDevice", "0x0200");
	/*
	 * Composite device with two IAD-using functions. Declaring the
	 * device as "misc / common / interface association" is what makes
	 * Windows bind a driver per IAD instead of treating the whole
	 * device as one function (with the old bDeviceClass=0 it only ever
	 * picked up the CDC-ACM half and ignored RNDIS).
	 */
	wf("/sys/kernel/config/usb_gadget/g1/bDeviceClass", "0xef");
	wf("/sys/kernel/config/usb_gadget/g1/bDeviceSubClass", "0x02");
	wf("/sys/kernel/config/usb_gadget/g1/bDeviceProtocol", "0x01");
	mkdir("/sys/kernel/config/usb_gadget/g1/strings/0x409", 0755);
	wf("/sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber", "0123456789");
	wf("/sys/kernel/config/usb_gadget/g1/strings/0x409/manufacturer", "k50v1");
	wf("/sys/kernel/config/usb_gadget/g1/strings/0x409/product", "k50v1 serial+net");
	mkdir("/sys/kernel/config/usb_gadget/g1/configs/c.1", 0755);
	mkdir("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409", 0755);
	wf("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409/configuration", "acm");
	mkdir("/sys/kernel/config/usb_gadget/g1/functions/acm.GS0", 0755);
	if (symlink("/sys/kernel/config/usb_gadget/g1/functions/acm.GS0",
		    "/sys/kernel/config/usb_gadget/g1/configs/c.1/acm.GS0") != 0 &&
	    errno != EEXIST)
		printf("  symlink failed: errno %d (%s)\n", errno, strerror(errno));

	/*
	 * Second function: CDC-NCM.
	 *
	 * RNDIS was tried first and failed on Windows with code 10 without the
	 * host ever issuing a single RNDIS control message -- i.e. the driver
	 * never got as far as talking to us, which points at the class/IAD
	 * plumbing rather than the protocol. NCM is an open standard with a
	 * plain interface layout and Windows 10 1903+ ships a host driver.
	 */
	mkdir("/sys/kernel/config/usb_gadget/g1/functions/ncm.usb0", 0755);
	wf("/sys/kernel/config/usb_gadget/g1/functions/ncm.usb0/dev_addr", "02:11:22:33:44:55");
	wf("/sys/kernel/config/usb_gadget/g1/functions/ncm.usb0/host_addr", "02:11:22:33:44:66");
	if (symlink("/sys/kernel/config/usb_gadget/g1/functions/ncm.usb0",
		    "/sys/kernel/config/usb_gadget/g1/configs/c.1/ncm.usb0") != 0 &&
	    errno != EEXIST)
		printf("  ncm symlink failed: errno %d (%s)\n",
		       errno, strerror(errno));

	if (wf("/sys/kernel/config/usb_gadget/g1/UDC", udc) != 0) {
		printf("  binding %s failed: errno %d (%s)\n",
		       udc, errno, strerror(errno));
	} else {
		snprintf(g_udc, sizeof(g_udc), "%s", udc);
		printf("  bound to %s -- the PC should now see a serial port\n", udc);
	}

	printf("  /dev/ttyGS0: %s\n", access("/dev/ttyGS0", F_OK) == 0 ? "present" : "missing");

	/*
	 * Do not touch the network until a host is really on the other end.
	 *
	 * Gating on "configured" is what keeps us from choosing an address
	 * model the PC cannot reach: with no cable (or a charger) udhcpc just
	 * burns its retries and we would fall back to serving DHCP on
	 * 192.168.42.1 -- which is exactly how we ended up on .42.x while
	 * Windows sat on .137.x.
	 *
	 * 10 s inline covers the normal case (cable already plugged in). If
	 * nothing shows up, the wait moves to a background watcher so the
	 * console stays usable and a later plug-in still configures itself.
	 */
	if (!g_udc[0]) {
		printf("  no UDC -- skipping network setup\n");
	} else if (usb_wait_host("configured", 100, 1)) {
		/*
		 * Run the addressing in a child: three rounds of DHCP can take
		 * half a minute, and there is no reason to hold the console (or
		 * the rest of the census) hostage for that.
		 */
		if (fork() == 0) {
			usb_net_setup();
			_exit(0);
		}
	} else {
		printf("  no host on the cable yet -- network setup deferred\n");
		printf("  (plug the USB cable into the PC and the link comes up by itself)\n");
		fflush(stdout);
		if (fork() == 0) {
			if (usb_wait_link(72000)) {
				/* give the host a moment to finish configuring */
				usleep(2000 * 1000);
				printf("\n  USB host appeared -- configuring the link\n");
				usb_net_setup();
			}
			_exit(0);
		}
	}

	/*
	 * There is no MT6353 RTC driver yet, so the clock starts at 1970 and
	 * every TLS handshake fails with "certificate not yet valid". Park it
	 * on a sane floor and let NTP correct it once the link is up.
	 */
	{
		struct timeval tv;

		tv.tv_sec = (time_t)BUILD_EPOCH;
		tv.tv_usec = 0;
		if (settimeofday(&tv, NULL) == 0) {
			printf("  time seeded from build stamp: ");
			system("/bin/busybox date -u");
		}
	}
	if (system("/bin/busybox route -n | /bin/busybox grep -q '^0\\.0\\.0\\.0'") == 0)
		system("( /bin/busybox ntpd -q -n -p ntp.aliyun.com >/dev/null 2>&1; "
		       "/bin/busybox date -u ) &");
	else
		printf("  no default route yet -- NTP skipped\n");

	mount_userdata();
	install_alpine_helper();

	{
		char st[64];

		if (udc_state(st, sizeof(st)) == 0)
			printf("  UDC state: %s%s\n", st,
			       strcmp(st, "configured") ? "  (host not enumerating)" : "");
	}

	/*
	 * A UDC name alone is not much use -- put a shell on the CDC-ACM
	 * port so the USB cable really is a second console. The child blocks
	 * in write() until the host opens the port, which is exactly what we
	 * want: nothing is lost before someone is listening.
	 */
	{
		pid_t p = fork();

		if (p == 0) {
			int fd;

			setsid();
			fd = open("/dev/ttyGS0", O_RDWR);
			if (fd < 0)
				_exit(1);
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
			setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
			setenv("HOME", "/", 1);
			setenv("TERM", "vt100", 1);
			setenv("ENV", "/etc/profile", 1);
			execl("/bin/busybox", "busybox", "sh", "-i", NULL);
			_exit(1);
		}
		if (p > 0)
			printf("  shell spawned on /dev/ttyGS0\n");
	}
}

/*
 * Helpers that make the board pleasant to drive from a shell.
 *
 * reboot/poweroff deliberately go through /proc/sysrq-trigger. PID 1 here is
 * a plain busybox sh, and busybox's own reboot applet, when it is *not* PID 1,
 * signals init instead of calling reboot(2) -- which is why the only way to
 * restart this board used to be holding the power button for ten seconds.
 */
static void install_tools(void)
{
	static const char *reboot_sh =
		"#!/bin/sh\n"
		"echo 'rebooting...'\n"
		"sync\n"
		"echo s > /proc/sysrq-trigger 2>/dev/null\n"
		"sleep 1\n"
		"echo b > /proc/sysrq-trigger\n";
	static const char *poweroff_sh =
		"#!/bin/sh\n"
		"echo 'powering off...'\n"
		"sync\n"
		"echo s > /proc/sysrq-trigger 2>/dev/null\n"
		"sleep 1\n"
		"echo o > /proc/sysrq-trigger\n";
	/*
	 * Read by `sh -l` and by $ENV for interactive shells, so aliases and
	 * a sane PATH show up both on the console and over telnet.
	 */
	static const char *profile =
		"export PATH=/sbin:/bin:/usr/sbin:/usr/bin\n"
		"export PS1='\\w # '\n"
		"alias ll='ls -l'\n"
		"alias la='ls -la'\n"
		"alias ..='cd ..'\n";
	/*
	 * distro [alpine|ubuntu] -- chroot into whichever root filesystem is
	 * unpacked under /mnt/data, mounting the kernel filesystems it needs.
	 */
	static const char *distro =
		"#!/bin/sh\n"
		"case \"$1\" in\n"
		"  '') R=alpine ;;\n"
		"  ubuntu|debian) R=ubuntu ;;\n"
		"  *) R=$1 ;;\n"
		"esac\n"
		"D=/mnt/data/$R\n"
		"[ -x $D/bin/sh ] || { echo \"no rootfs at $D\"; exit 1; }\n"
		"mountpoint -q $D/proc || mount -t proc none $D/proc\n"
		"mountpoint -q $D/sys  || mount -t sysfs none $D/sys\n"
		"mountpoint -q $D/dev  || mount --bind /dev $D/dev\n"
		"if [ -d /dev/pts ]; then\n"
		"  mkdir -p $D/dev/pts\n"
		"  mountpoint -q $D/dev/pts || mount --bind /dev/pts $D/dev/pts\n"
		"fi\n"
		"grep -q nameserver $D/etc/resolv.conf 2>/dev/null ||"
		" echo 'nameserver 223.5.5.5' > $D/etc/resolv.conf\n"
		"exec chroot $D /bin/sh -l\n";
	static const char *net =
		"#!/bin/sh\n"
		"echo '--- usb0 ---'\n"
		"ifconfig usb0 2>/dev/null || echo 'usb0 absent'\n"
		"echo '--- routes ---'\n"
		"ip route 2>/dev/null\n"
		"echo '--- telnetd ---'\n"
		"ps 2>/dev/null | grep -c telnetd\n";

	mkdir("/sbin", 0755);
	wfile("/sbin/reboot", reboot_sh);
	chmod("/sbin/reboot", 0755);
	wfile("/sbin/poweroff", poweroff_sh);
	chmod("/sbin/poweroff", 0755);
	wfile("/sbin/halt", poweroff_sh);
	chmod("/sbin/halt", 0755);

	wfile("/etc/profile", profile);
	wfile("/bin/distro", distro);
	chmod("/bin/distro", 0755);
	wfile("/bin/net", net);
	chmod("/bin/net", 0755);

	/* fail loudly instead of silently missing a helper again */
	if (access("/sbin/reboot", X_OK) != 0)
		printf("  WARN: /sbin/reboot was not installed\n");
	if (access("/bin/distro", X_OK) != 0)
		printf("  WARN: /bin/distro was not installed\n");
}

int main(void)
{
	printf("\n\n");
	printf("******************************************************\n");
	printf("**  mainline Linux on MT6750 / k50v1_64_bsp         **\n");
	printf("**  userspace is alive: this is PID 1               **\n");
	printf("******************************************************\n");

	mkdir("/proc", 0555);
	mkdir("/sys", 0555);
	mkdir("/dev", 0755);
	mount("proc", "/proc", "proc", 0, "");
	mount("sysfs", "/sys", "sysfs", 0, "");
	mount("devtmpfs", "/dev", "devtmpfs", 0, "");
	/*
	 * telnetd allocates a pty per connection, so devpts has to exist
	 * before it starts -- without it telnetd exits silently and the port
	 * is closed.
	 */
	mkdir("/dev/pts", 0755);
	if (mount("devpts", "/dev/pts", "devpts", 0, "mode=0620,ptmxmode=0666") != 0)
		printf("devpts mount failed: errno %d (%s)\n", errno, strerror(errno));

	/*
	 * Get the applet symlinks and ssh-style login shell in place before
	 * anything tries to exec /bin/sh: telnetd spawns it per connection,
	 * and an interactive session is much nicer with /etc/profile.
	 */
	system("/bin/busybox --install -s /bin");
	mkdir("/etc", 0755);
	mkdir("/tmp", 0777);
	install_tools();
	/* telnetd is useful even before the USB link is up: it binds 0.0.0.0 */
	system("/bin/busybox telnetd -l /bin/sh -p 23");
	printf("telnetd: listening on port 23 (root shell, no password)\n");
	mkdir("/sys/kernel/debug", 0755);
	if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, "") != 0)
		printf("debugfs mount failed: errno %d (%s)\n", errno, strerror(errno));
	mknod("/dev/mmcblk0", S_IFBLK | 0600, makedev(179, 0));

	/*
	 * MMC probing is asynchronous and finishes well after init starts, so
	 * give the card time to be scanned before looking at sysfs/blkdev.
	 */
	printf("\n(waiting for eMMC to be probed)\n");
	sleep(3);

	cat("/proc/version");
	cat("/proc/cmdline");
	cat("/proc/cpuinfo");
	cat("/proc/meminfo");
	cat("/sys/firmware/devicetree/base/model");
	cat("/proc/interrupts");
	listdir("/sys/class/block");
	cat("/sys/block/mmcblk0/size");

	{
		char name[64], boot[32] = "", expdb[32] = "", vendor[32] = "", sys[32] = "";
		int i;

		printf("\n===== partition table (PARTNAME from sysfs uevent) =====\n");
		for (i = 1; i <= 40; i++) {
			char node[32];

			snprintf(node, sizeof(node), "/dev/mmcblk0p%d", i);
			if (make_node_from_sysfs(i, node, name, sizeof(name)) != 0)
				continue;
			printf("p%-2d %-14s %s\n", i, name, node);
			if (!strcmp(name, "boot"))
				strcpy(boot, node);
			else if (!strcmp(name, "expdb"))
				strcpy(expdb, node);
			else if (!strcmp(name, "vendor"))
				strcpy(vendor, node);
			else if (!strcmp(name, "system"))
				strcpy(sys, node);
		}
		printf("resolved: boot=%s expdb=%s vendor=%s system=%s\n",
		       boot, expdb, vendor, sys);

		/* expdb starts with the PL_LOG_STORE header: proves block reads */
		if (expdb[0])
			hexdump_dev(expdb, 192);
		/* boot should now hold our own mainline image ("ANDROID!") */
		if (boot[0])
			hexdump_dev(boot, 64);

		mkdir("/mnt", 0755);
		if (vendor[0])
			try_mount(vendor, "/mnt/vendor");
		if (sys[0])
			try_mount(sys, "/mnt/system");

		cat("/mnt/vendor/build.prop");
		cat("/mnt/system/system/build.prop");
		cat("/mnt/system/build.prop");
	}

	/*
	 * I2C0 pins are muxed from device tree now (pinctrl-0 on i2c@11007000),
	 * so the kernel -- not LK -- owns that configuration and we can read
	 * back the real hardware state through debugfs.
	 */
	mknod("/dev/mem", S_IFCHR | 0600, makedev(1, 1));
	mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem_fd < 0)
		printf("/dev/mem open failed: errno %d (%s)\n", errno, strerror(errno));
	pinctrl_debugfs();
	ctp_reset_release();
	gpio_level_probe();
	/* did requesting the lines as GPIO disturb the i2c0 mux? */
	printf("\n--- pinmux after the gpio request ---\n");
	pinctrl_debugfs();
	i2c_regs_dump();
	i2c_write_test();
	/*
	 * Bus 0 is the bit-banged pair on the panel pads (92/93): a full scan
	 * costs well under a second there, unlike the hardware controller
	 * where every dead address burns the driver's 2 s timeout.
	 */
	run_guarded(scan_fn, 0, 0, "bus 0 scan (i2c-gpio, panel pads)");
	run_guarded(scan_fn, 1, 0, "bus 1 scan (i2c-gpio-1, i2c1 pads)");
	cat("/proc/interrupts");
	listdir("/sys/bus/i2c/devices");

	usb_gadget_setup();

	listdir("/sys/class");

	/*
	 * Hand the console over to an interactive shell. Everything above is
	 * the boot census; from here on the board is usable.
	 */
	printf("\n*** census done, handing the console to /bin/sh ***\n");
	fflush(stdout);
	/*
	 * Do NOT run `busybox --install` here again: it recreates
	 * /sbin/reboot, /sbin/poweroff and /sbin/halt as busybox symlinks and
	 * silently undoes install_tools(). Busybox's reboot only signals init,
	 * which is why the power button used to be the only way out.
	 */
	{
		int fd = open("/dev/console", O_RDWR);

		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			/*
			 * PID 1 is already a session leader, so claiming the
			 * console as the controlling terminal is enough to get
			 * job control -- without this the shell prints
			 * "can't access tty; job control turned off" and
			 * Ctrl-C does nothing.
			 */
			ioctl(0, TIOCSCTTY, 1);
			if (fd > 2)
				close(fd);
		}
	}
	setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
	setenv("HOME", "/", 1);
	setenv("TERM", "vt100", 1);
	setenv("ENV", "/etc/profile", 1);
	execl("/bin/busybox", "busybox", "sh", "-i", NULL);
	printf("exec /bin/busybox failed: errno %d (%s)\n", errno, strerror(errno));
	printf("*** idling ***\n");
	fflush(stdout);
	for (;;)
		pause();
	return 0;
}
