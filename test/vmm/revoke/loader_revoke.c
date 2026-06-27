/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Loader-side fd3/fd4 revoke probe.
 *
 * This intentionally writes an invalid manifest.  The test target is the
 * loader epoch contract: after either loader exit or VM stop, fd3/fd4 must no
 * longer accept mmap and old mappings must fault in any process that inherited
 * them.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sigjmp_buf fault_jmp;
static volatile sig_atomic_t fault_armed;

static void
fault_handler(int sig)
{
	if (fault_armed)
		siglongjmp(fault_jmp, 1);
	_exit(128 + sig);
}

static void
install_fault_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fault_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, NULL) != 0 ||
	    sigaction(SIGBUS, &sa, NULL) != 0) {
		perror("sigaction");
		exit(2);
	}
}

static int
expect_fault(volatile uint8_t *addr)
{
	volatile uint8_t value;

	fault_armed = 1;
	if (sigsetjmp(fault_jmp, 1) == 0) {
		value = addr[0];
		addr[0] = value;
		fault_armed = 0;
		return 0;
	}
	fault_armed = 0;
	return 1;
}

static int
mmap_fails(int fd, size_t page_size, int prot, int flags)
{
	void *addr;

	addr = mmap(NULL, page_size, prot, flags, fd, 0);
	if (addr == MAP_FAILED)
		return 1;
	munmap(addr, page_size);
	return 0;
}

static int
new_mmap_fails(int fd, size_t page_size)
{
	return mmap_fails(fd, page_size, PROT_READ | PROT_WRITE, MAP_SHARED);
}

static int
private_mmap_fails(int fd, size_t page_size)
{
	return mmap_fails(fd, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE);
}

static int
exec_mmap_fails(int fd, size_t page_size)
{
	return mmap_fails(fd, page_size, PROT_READ | PROT_EXEC, MAP_SHARED);
}

static int
exec_mprotect_fails(void *addr, size_t page_size)
{
	return mprotect(addr, page_size, PROT_READ | PROT_EXEC) != 0;
}

static void
write_ready(const char *result_path)
{
	char *ready_path;
	FILE *fp;
	size_t len;

	len = strlen(result_path) + sizeof(".ready");
	ready_path = malloc(len);
	if (ready_path == NULL) {
		perror("malloc");
		exit(2);
	}
	snprintf(ready_path, len, "%s.ready", result_path);
	fp = fopen(ready_path, "w");
	if (fp == NULL) {
		perror("fopen ready");
		free(ready_path);
		exit(2);
	}
	fprintf(fp, "ready=1\n");
	fclose(fp);
	free(ready_path);
}

static int
write_result(const char *path, const char *mode, int new3, int new4,
    int old3, int old4, int private3, int private4, int exec3, int exec4,
    int mprotect3, int mprotect4)
{
	FILE *fp;
	int pass;

	/*
	 * Revoke guarantees that new executable mappings fail and that old
	 * mappings fault when touched.  Existing vm_map entries may still
	 * accept mprotect until DragonFly grows object-level mmap revoke.
	 */
	pass = new3 && new4 && old3 && old4 &&
	    private3 && private4 && exec3 && exec4;
	fp = fopen(path, "w");
	if (fp == NULL)
		return -1;
	fprintf(fp, "mode=%s\n", mode);
	fprintf(fp, "pid=%ld\n", (long)getpid());
	fprintf(fp, "new_mmap3_failed=%d\n", new3);
	fprintf(fp, "new_mmap4_failed=%d\n", new4);
	fprintf(fp, "old_mmap3_faulted=%d\n", old3);
	fprintf(fp, "old_mmap4_faulted=%d\n", old4);
	fprintf(fp, "private_mmap3_failed=%d\n", private3);
	fprintf(fp, "private_mmap4_failed=%d\n", private4);
	fprintf(fp, "exec_mmap3_failed=%d\n", exec3);
	fprintf(fp, "exec_mmap4_failed=%d\n", exec4);
	fprintf(fp, "exec_mprotect3_failed=%d\n", mprotect3);
	fprintf(fp, "exec_mprotect4_failed=%d\n", mprotect4);
	fprintf(fp, "pass=%d\n", pass);
	fclose(fp);
	return pass ? 0 : 1;
}

static size_t
page_size(void)
{
	long value;

	value = sysconf(_SC_PAGESIZE);
	if (value <= 0)
		return 4096;
	return (size_t)value;
}

static void *
map_one_page(int fd, size_t page, const char *name)
{
	struct stat st;
	void *addr;

	if (fstat(fd, &st) != 0) {
		perror(name);
		exit(2);
	}
	if (st.st_size < (off_t)page) {
		fprintf(stderr, "%s too small\n", name);
		exit(2);
	}
	addr = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		perror(name);
		exit(2);
	}
	return addr;
}

int
main(int argc, char **argv)
{
	const char *mode;
	const char *result_path;
	uint8_t *mem_map;
	uint8_t *manifest_map;
	size_t page;
	pid_t child;
	int fd3;
	int fd4;
	int delay;
	int new3;
	int new4;
	int old3;
	int old4;
	int private3;
	int private4;
	int exec3;
	int exec4;
	int mprotect3;
	int mprotect4;

	if (argc < 3) {
		fprintf(stderr, "usage: %s exit|hang|hold result-path [delay]\n",
		    argv[0]);
		return 2;
	}
	mode = argv[1];
	result_path = argv[2];
	delay = argc >= 4 ? atoi(argv[3]) : 3;
	if (delay < 1)
		delay = 1;
	if (strcmp(mode, "exit") != 0 && strcmp(mode, "hang") != 0 &&
	    strcmp(mode, "hold") != 0) {
		fprintf(stderr, "invalid mode: %s\n", mode);
		return 2;
	}

	page = page_size();
	mem_map = map_one_page(3, page, "fd3");
	manifest_map = map_one_page(4, page, "fd4");
	mem_map[0] = 0x90;
	manifest_map[0] = 0;
	fd3 = dup(3);
	fd4 = dup(4);
	if (fd3 < 0 || fd4 < 0) {
		perror("dup");
		return 2;
	}
	private3 = private_mmap_fails(3, page);
	private4 = private_mmap_fails(4, page);
	exec3 = exec_mmap_fails(3, page);
	exec4 = exec_mmap_fails(4, page);
	mprotect3 = exec_mprotect_fails(mem_map, page);
	mprotect4 = exec_mprotect_fails(manifest_map, page);

	child = fork();
	if (child < 0) {
		perror("fork");
		return 2;
	}
	if (child != 0) {
		if (strcmp(mode, "exit") == 0 || strcmp(mode, "hold") == 0)
			return 0;
		for (;;)
			pause();
	}

	install_fault_handlers();
	write_ready(result_path);
	sleep((unsigned int)delay);
	new3 = new_mmap_fails(fd3, page);
	new4 = new_mmap_fails(fd4, page);
	old3 = expect_fault(mem_map);
	old4 = expect_fault(manifest_map);
	if (write_result(result_path, mode, new3, new4, old3, old4, private3,
	    private4, exec3, exec4, mprotect3, mprotect4) != 0)
		return 1;
	while (strcmp(mode, "hold") == 0)
		pause();
	return 0;
}
