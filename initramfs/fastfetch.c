// SPDX-License-Identifier: GPL-2.0
/*
 * fastfetch for the k50v1 mainline bring-up console.
 *
 * One-shot system summary in the style of neofetch/fastfetch, reading
 * everything from /proc and sysfs -- no external dependencies, so it
 * works from the bare initramfs shell (serial, telnet or framebuffer).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define C_RST	"\033[0m"
#define C_CYAN	"\033[36;1m"
#define C_GRN	"\033[32;1m"
#define C_YEL	"\033[33;1m"
#define C_WHT	"\033[37;1m"

static void file_line(const char *path, const char *prefix, char *out,
		      size_t len)
{
	FILE *f = fopen(path, "r");
	char buf[512];

	out[0] = 0;
	if (!f)
		return;
	while (fgets(buf, sizeof(buf), f)) {
		if (prefix) {
			if (strncmp(buf, prefix, strlen(prefix)) == 0) {
				char *p = buf + strlen(prefix);

				while (*p == ' ' || *p == '\t')
					p++;
				strncpy(out, p, len - 1);
				out[len - 1] = 0;
				fclose(f);
				return;
			}
		} else {
			buf[strcspn(buf, "\n")] = 0;
			strncpy(out, buf, len - 1);
			out[len - 1] = 0;
			fclose(f);
			return;
		}
	}
	fclose(f);
}

static int count_prefix(const char *path, const char *prefix)
{
	FILE *f = fopen(path, "r");
	char buf[512];
	int n = 0;

	if (!f)
		return 0;
	while (fgets(buf, sizeof(buf), f))
		if (strncmp(buf, prefix, strlen(prefix)) == 0)
			n++;
	fclose(f);
	return n;
}

static long file_int(const char *path, const char *prefix)
{
	char buf[128];

	file_line(path, prefix, buf, sizeof(buf));
	return atol(buf);
}

static void human_bytes(long kb, char *out, size_t len)
{
	if (kb >= 1024 * 1024)
		snprintf(out, len, "%.1f GiB", kb / 1048576.0);
	else
		snprintf(out, len, "%.1f MiB", kb / 1024.0);
}

int main(void)
{
	char ver[256], hostname[128], upt[64], memtotal[128], memavail[128];
	char bat_status[128], bat_cap[128], bat_vol[128];
	char total[128], avail[128];
	long up_s, mem_t, mem_a, mem_u;
	int cpus, vb_mV = -1, vb_cap = -1, is_charging = -1;
	struct statvfs st;
	const char *bat_dir = "/sys/class/power_supply/battery";

	/* kernel + host */
	file_line("/proc/version", NULL, ver, sizeof(ver));
	file_line("/proc/sys/kernel/hostname", NULL, hostname, sizeof(hostname));

	/* uptime */
	file_line("/proc/uptime", NULL, upt, sizeof(upt));
	up_s = atol(upt);

	/* memory */
	mem_t = file_int("/proc/meminfo", "MemTotal:");
	mem_a = file_int("/proc/meminfo", "MemAvailable:");
	mem_u = (mem_t > mem_a) ? mem_t - mem_a : 0;
	human_bytes(mem_t, memtotal, sizeof(memtotal));
	human_bytes(mem_u, memavail, sizeof(memavail));

	/* cpu */
	cpus = count_prefix("/proc/cpuinfo", "processor");

	/* battery */
	bat_status[0] = bat_cap[0] = bat_vol[0] = 0;
	{
		char path[256];

		snprintf(path, sizeof(path), "%s/status", bat_dir);
		file_line(path, NULL, bat_status, sizeof(bat_status));
		snprintf(path, sizeof(path), "%s/capacity", bat_dir);
		file_line(path, NULL, bat_cap, sizeof(bat_cap));
		snprintf(path, sizeof(path), "%s/voltage_now", bat_dir);
		file_line(path, NULL, bat_vol, sizeof(bat_vol));
		vb_mV = (int)(atol(bat_vol) / 1000);
		vb_cap = atoi(bat_cap);
		is_charging = (strstr(bat_status, "Charg") != NULL);
	}

	/* storage (whatever is writable right now) */
	total[0] = avail[0] = 0;
	if (statvfs("/", &st) == 0) {
		long t = (long)st.f_blocks * (st.f_bsize / 1024);
		long a = (long)st.f_bavail * (st.f_bsize / 1024);

		human_bytes(t, total, sizeof(total));
		human_bytes(a, avail, sizeof(avail));
	}

	printf("\n");
	/* little SoC logo */
	printf(C_CYAN"      ___\n");
	printf("     /   \\    "C_RST C_WHT"%s"C_RST"\n", hostname);
	printf(C_CYAN"    | MTK|   "C_RST C_YEL"------------------------------------"C_RST"\n");
	printf(C_CYAN"    |    |   "C_RST C_GRN"OS      "C_RST": mainline Linux (aarch64)\n");
	printf(C_CYAN"     \\  /    "C_RST C_GRN"Kernel  "C_RST": %.60s\n", ver);
	printf(C_CYAN"      \\/     "C_RST C_GRN"Cores   "C_RST": %d online\n", cpus);
	printf(C_YEL"             "C_RST C_GRN"Uptime  "C_RST": %ldm %lds\n",
	       up_s / 60, up_s % 60);
	printf(C_GRN"             "C_RST C_GRN"Display "C_RST": 480x800 (AXS panel, LK fb)\n");
	if (vb_mV > 0)
		printf(C_GRN"             "C_RST C_GRN"Battery "C_RST": %d mV [%d%%]%s\n",
		       vb_mV, vb_cap,
		       is_charging > 0 ? " "C_YEL"(charging)"C_RST : "");
	else
		printf(C_GRN"             "C_RST C_GRN"Battery "C_RST": not available\n");
	printf(C_GRN"             "C_RST C_GRN"Memory  "C_RST": %s used / %s\n",
	       memavail, memtotal);
	if (total[0])
		printf(C_GRN"             "C_RST C_GRN"Storage "C_RST": %s free / %s (/)\n",
		       avail, total);
	printf(C_GRN"             "C_RST C_GRN"Shell   "C_RST": consolehub (serial+telnet+fb)\n");
	printf("\n");
	return 0;
}
