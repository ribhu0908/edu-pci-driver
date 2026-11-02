/*
 * edu_test.c - end-to-end test for the edu driver.
 *
 * Runs as a normal program (`./edu_test`) on a system where edu.ko is loaded,
 * or as PID 1 inside a QEMU initramfs (`run-qemu.sh`), in which case it also
 * mounts the basics, loads the module, runs the suite, scans the kernel log
 * for KASAN / lockdep / WARN splats and powers the VM off.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../include/uapi/edu_ioctl.h"

static int g_checks, g_fail;

#define CHECK(cond)                                                          \
	do {                                                                 \
		g_checks++;                                                  \
		if (!(cond)) {                                               \
			g_fail++;                                            \
			printf("  FAIL %s:%d: %s (errno=%d %s)\n", __FILE__, \
			       __LINE__, #cond, errno, strerror(errno));     \
		}                                                            \
	} while (0)

#define RUN(fn) do { printf("[ RUN  ] %s\n", #fn); fflush(stdout); fn(); } while (0)

static char g_dev[320];         /* /dev/eduN */
static char g_bdf[256];         /* PCI address, e.g. 0000:00:04.0 */

static int find_device(void)
{
	DIR *d = opendir("/sys/bus/pci/drivers/edu");
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char path[320], buf[256];
		if (strncmp(e->d_name, "0000:", 5) != 0)
			continue;
		snprintf(g_bdf, sizeof g_bdf, "%s", e->d_name);
		snprintf(path, sizeof path, "/sys/bus/pci/drivers/edu/%s/misc", e->d_name);
		DIR *m = opendir(path);
		if (!m)
			continue;
		struct dirent *me;
		while ((me = readdir(m)) != NULL) {
			if (strncmp(me->d_name, "edu", 3) == 0) {
				snprintf(buf, sizeof buf, "%s", me->d_name);
				snprintf(g_dev, sizeof g_dev, "/dev/%s", buf);
			}
		}
		closedir(m);
	}
	closedir(d);
	return g_dev[0] ? 0 : -1;
}

static int open_dev(void)
{
	for (int i = 0; i < 50; i++) {          /* devtmpfs node may lag probe */
		int fd = open(g_dev, O_RDWR);
		if (fd >= 0)
			return fd;
		usleep(20000);
	}
	return -1;
}

static void fill_pattern(uint8_t *b, size_t n, uint32_t seed)
{
	for (size_t i = 0; i < n; i++) {
		seed = seed * 1103515245u + 12345u;
		b[i] = (uint8_t)(seed >> 16);
	}
}

/* ------------------------------------------------------------------ */
static void test_info_and_liveness(void)
{
	int fd = open_dev();
	CHECK(fd >= 0);
	struct edu_info info;
	CHECK(ioctl(fd, EDU_IOC_INFO, &info) == 0);
	CHECK((info.version & 0xff) == 0xed);
	printf("  device %s v%u.%u, irq=%s\n", g_dev, info.version >> 24,
	       (info.version >> 16) & 0xff, info.irq_is_msi ? "MSI" : "INTx");
	CHECK(info.irq_is_msi == 1);

	uint32_t v = 0x12345678;
	CHECK(ioctl(fd, EDU_IOC_LIVENESS, &v) == 0);
	CHECK(v == (uint32_t)~0x12345678u);
	CHECK(ioctl(fd, _IO('E', 99)) == -1 && errno == ENOTTY);
	close(fd);
}

static void test_factorial_via_irq(void)
{
	int fd = open_dev();
	struct edu_info before, after;
	ioctl(fd, EDU_IOC_INFO, &before);

	static const uint32_t expect[] = { 1, 1, 2, 6, 24, 120, 720, 5040, 40320,
					   362880, 3628800, 39916800, 479001600 };
	for (uint32_t n = 0; n <= 12; n++) {
		struct edu_factorial fa = { .n = n };
		CHECK(ioctl(fd, EDU_IOC_FACTORIAL, &fa) == 0);
		CHECK(fa.result == expect[n]);
	}
	ioctl(fd, EDU_IOC_INFO, &after);
	CHECK(after.irq_count - before.irq_count == 13);   /* one IRQ each */
	CHECK(after.timeouts == 0);
	close(fd);
}

static void test_dma_roundtrip_full_buffer(void)
{
	int fd = open_dev();
	uint8_t out[EDU_BUF_SIZE], in[EDU_BUF_SIZE];
	fill_pattern(out, sizeof out, 42);

	CHECK(pwrite(fd, out, sizeof out, 0) == EDU_BUF_SIZE);
	memset(in, 0, sizeof in);
	CHECK(pread(fd, in, sizeof in, 0) == EDU_BUF_SIZE);
	CHECK(memcmp(in, out, sizeof in) == 0);
	close(fd);
}

static void test_dma_offsets_and_bounds(void)
{
	int fd = open_dev();
	uint8_t a[100], b[100];
	fill_pattern(a, sizeof a, 7);

	CHECK(pwrite(fd, a, 100, 1000) == 100);             /* odd offset   */
	CHECK(pread(fd, b, 100, 1000) == 100);
	CHECK(memcmp(a, b, 100) == 0);

	/* Request crossing the end is clipped, never sent to the card as-is
	 * (an out-of-range DMA would abort the whole QEMU machine). */
	CHECK(pwrite(fd, a, 100, EDU_BUF_SIZE - 10) == 10);
	CHECK(pread(fd, b, 100, EDU_BUF_SIZE - 10) == 10);
	CHECK(memcmp(a, b, 10) == 0);

	CHECK(pread(fd, b, 10, EDU_BUF_SIZE) == 0);         /* EOF          */
	errno = 0;
	CHECK(pwrite(fd, a, 10, EDU_BUF_SIZE) == -1 && errno == ENOSPC);
	CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL);
	CHECK(lseek(fd, 0, SEEK_END) == EDU_BUF_SIZE);
	CHECK(read(fd, NULL, 0) == 0);
	volatile uintptr_t bad_addr = 8;               /* unmapped user pointer */
	errno = 0;
	CHECK(pwrite(fd, (const void *)bad_addr, 16, 0) == -1 && errno == EFAULT);
	close(fd);
}

/* 4 processes hammer disjoint 1 KiB windows; the per-device mutex must keep
 * every DMA intact. */
static void test_concurrent_processes(void)
{
	enum { PROCS = 4, ROUNDS = 6, WIN = 1024 };
	pid_t pid[PROCS];

	for (int p = 0; p < PROCS; p++) {
		pid[p] = fork();
		if (pid[p] == 0) {
			int fd = open_dev();
			uint8_t out[WIN], in[WIN];
			for (int r = 0; r < ROUNDS; r++) {
				fill_pattern(out, WIN, (uint32_t)(p * 1000 + r));
				if (pwrite(fd, out, WIN, p * WIN) != WIN) _exit(2);
				if (pread(fd, in, WIN, p * WIN) != WIN) _exit(3);
				if (memcmp(in, out, WIN) != 0) _exit(4);
			}
			_exit(0);
		}
	}
	for (int p = 0; p < PROCS; p++) {
		int st;
		waitpid(pid[p], &st, 0);
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
	int fd = open_dev();
	struct edu_info info;
	ioctl(fd, EDU_IOC_INFO, &info);
	CHECK(info.timeouts == 0);
	printf("  irqs=%llu dma=%llu bytes=%llu spurious=%llu\n",
	       (unsigned long long)info.irq_count, (unsigned long long)info.dma_count,
	       (unsigned long long)info.dma_bytes, (unsigned long long)info.spurious_irqs);
	close(fd);
}

static int write_sysfs(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	ssize_t n = write(fd, val, strlen(val));
	close(fd);
	return n == (ssize_t)strlen(val) ? 0 : -1;
}

/* Hold the device open, unbind the driver underneath it, keep using the fd. */
static void test_hot_unplug_with_open_file(void)
{
	int fd = open_dev();
	CHECK(fd >= 0);
	CHECK(write_sysfs("/sys/bus/pci/drivers/edu/unbind", g_bdf) == 0);

	uint8_t b[16];
	errno = 0;
	CHECK(pread(fd, b, sizeof b, 0) == -1 && errno == ENODEV);
	struct edu_factorial fa = { .n = 5 };
	errno = 0;
	CHECK(ioctl(fd, EDU_IOC_FACTORIAL, &fa) == -1 && errno == ENODEV);
	CHECK(access(g_dev, F_OK) == -1);            /* node is gone */
	close(fd);                                   /* last ref: frees struct */

	CHECK(write_sysfs("/sys/bus/pci/drivers/edu/bind", g_bdf) == 0);
	g_dev[0] = 0;
	CHECK(find_device() == 0);
	fd = open_dev();
	CHECK(fd >= 0);
	fa.n = 5;
	CHECK(ioctl(fd, EDU_IOC_FACTORIAL, &fa) == 0 && fa.result == 120);
	close(fd);
}

/* ---------------------------------------------------- PID 1 plumbing */
static void scan_kernel_log(void)
{
	static char log[1 << 20];
	int n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, log, sizeof log - 1);
	if (n < 0)
		return;
	log[n] = 0;
	static const char *bad[] = { "KASAN", "BUG:", "WARNING:", "possible circular",
				     "inconsistent lock", "DMA-API", "Oops", "general protection" };
	int found = 0;
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
		if (strstr(log, bad[i])) {
			printf("  kernel log contains \"%s\"\n", bad[i]);
			found++;
		}
	g_checks++;
	if (found)
		g_fail++;
	printf("[ LOG  ] kernel log scan: %s\n", found ? "PROBLEMS FOUND" : "clean (no KASAN/lockdep/WARN/DMA-API reports)");
	char *p = log;
	while ((p = strstr(p, " edu ")) != NULL) {               /* show driver lines */
		char *start = p, *end = strchr(p, '\n');
		while (start > log && start[-1] != '\n') start--;
		printf("    %.*s\n", (int)((end ? end : p + strlen(p)) - start), start);
		p = end ? end : p + strlen(p);
	}
}

static int as_init(void)
{
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	setvbuf(stdout, NULL, _IONBF, 0);

	int mfd = open("/edu.ko", O_RDONLY);
	if (mfd < 0 || syscall(SYS_finit_module, mfd, "", 0) != 0) {
		printf("insmod edu.ko failed: %s\n", strerror(errno));
		return 1;
	}
	close(mfd);
	printf("[ INIT ] edu.ko loaded\n");
	return 0;
}

int main(void)
{
	int is_init = getpid() == 1;

	if (is_init && as_init() != 0)
		goto out;
	if (find_device() != 0) {
		printf("no edu device bound to the driver\n");
		g_fail++;
		goto out;
	}

	RUN(test_info_and_liveness);
	RUN(test_factorial_via_irq);
	RUN(test_dma_roundtrip_full_buffer);
	RUN(test_dma_offsets_and_bounds);
	RUN(test_concurrent_processes);
	RUN(test_hot_unplug_with_open_file);

	if (is_init) {
		CHECK(syscall(SYS_delete_module, "edu", O_NONBLOCK) == 0);  /* rmmod */
		scan_kernel_log();
	}
out:
	printf("\n%d checks, %d failed\nRESULT: %s\n", g_checks, g_fail,
	       g_fail ? "FAIL" : "PASS");
	if (is_init) {
		sync();
		reboot(RB_POWER_OFF);
	}
	return g_fail ? 1 : 0;
}
