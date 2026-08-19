/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Linux x86_64 boot-protocol loader for DragonFly vmmfs.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/vmm.h>

#define PAGE_SIZE_GUEST		4096ULL
#define ONE_MIB			0x100000ULL
#define ONE_GIB			0x40000000ULL

#define PML4_GPA		0x2000ULL
#define PDPT_GPA		0x3000ULL
#define PD_GPA			0x4000ULL
#define GDT_GPA			0x5000ULL
#define TSS_GPA			0x6000ULL
#define BOOT_PARAMS_GPA		0x90000ULL
#define CMDLINE_GPA		0x98000ULL
#define STACK_TOP_GPA		0x80000ULL
#define VMMFS_PLATFORM_ACPI_GPA	0x70000ULL
#define VMMFS_PLATFORM_ACPI_SIZE	(64ULL * 1024ULL)
#define KERNEL_LOAD_GPA		0x100000ULL
#define KERNEL_64_ENTRY_DELTA	0x200ULL
#define CMDLINE_CAP		PAGE_SIZE_GUEST

#define LINUX_SETUP_SECTS	0x1f1U
#define LINUX_SETUP_HEADER_COPY	(0x290U - LINUX_SETUP_SECTS)
#define LINUX_HDR_MAGIC		0x202U
#define LINUX_HDR_VERSION	0x206U
#define LINUX_TYPE_OF_LOADER	0x210U
#define LINUX_LOADFLAGS		0x211U
#define LINUX_CODE32_START	0x214U
#define LINUX_RAMDISK_IMAGE	0x218U
#define LINUX_RAMDISK_SIZE	0x21cU
#define LINUX_HEAP_END_PTR	0x224U
#define LINUX_CMD_LINE_PTR	0x228U
#define LINUX_XLOADFLAGS	0x236U
#define LINUX_CMDLINE_SIZE	0x238U
#define LINUX_INIT_SIZE		0x260U
#define LINUX_EXT_RAMDISK_IMAGE	0x0c0U
#define LINUX_EXT_RAMDISK_SIZE	0x0c4U
#define LINUX_EXT_CMD_LINE_PTR	0x0c8U
#define LINUX_ACPI_RSDP_ADDR	0x070U
#define LINUX_E820_ENTRIES	0x1e8U
#define LINUX_E820_TABLE	0x2d0U
#define LINUX_E820_ENTRY_SIZE	20U

#define LINUX_BOOT_PROTOCOL_MIN	0x020cU
#define LINUX_HDRS		0x53726448U
#define LINUX_LOADED_HIGH	0x01U
#define LINUX_CAN_USE_HEAP	0x80U
#define LINUX_LOADER_KEXEC	0xd0U
#define LINUX_XLF_KERNEL_64	0x0001U
#define LINUX_E820_RAM		1U
#define LINUX_E820_RESERVED	2U
#define CR0_PE			0x00000001ULL
#define CR0_NE			0x00000020ULL
#define CR0_PG			0x80000000ULL
#define CR4_PAE			0x00000020ULL
#define EFER_LME		0x00000100ULL
#define EFER_LMA		0x00000400ULL
#define XCR0_X87		0x00000001ULL
#define SEG_S			0x0010U
#define SEG_P			0x0080U
#define SEG_L			0x0200U
#define SEG_DB			0x0400U
#define SEG_G			0x0800U
#define SEG_UNUSABLE		0x1000U

struct loader_options {
	const char	*kernel_path;
	const char	*initramfs_path;
	char		*cmdline;
	size_t		cmdline_len;
};

struct mapped_file {
	const char	*path;
	uint8_t		*data;
	uint64_t	size;
};

struct linux_kernel {
	struct mapped_file image;
	uint16_t	protocol;
	uint8_t		loadflags;
	uint16_t	xloadflags;
	uint32_t	cmdline_size;
	uint64_t	init_size;
	uint64_t	payload_offset;
	uint64_t	payload_size;
};

struct loaded_initramfs {
	struct mapped_file file;
	uint64_t	gpa;
};

static void parse_options(int argc, char **argv, struct loader_options *opts);
static uint8_t *map_fd(int fd, int prot, uint64_t *sizep);
static void map_file_readonly(const char *path, struct mapped_file *file);
static void unmap_file(const struct mapped_file *file);
static void linux_kernel_init(struct linux_kernel *kernel, const char *path);
static void linux_kernel_fini(const struct linux_kernel *kernel);
static void build_linux_guest(uint8_t *mem, uint64_t mem_size,
    const struct loader_options *opts, const struct linux_kernel *kernel,
    struct loaded_initramfs *initramfs);
static void build_vcpu(struct vmm_cpustate *state);
static void write_cpustate(const struct vmm_cpustate *state);
static void loaded_initramfs_fini(const struct loaded_initramfs *initramfs);

int
main(int argc, char **argv)
{
	struct loader_options opts;
	struct linux_kernel kernel;
	struct loaded_initramfs initramfs;
	struct vmm_cpustate state;
	uint8_t *mem;
	uint64_t mem_size;

	parse_options(argc, argv, &opts);
	mem = map_fd(3, PROT_READ | PROT_WRITE, &mem_size);
	linux_kernel_init(&kernel, opts.kernel_path);
	memset(&initramfs, 0, sizeof(initramfs));

	build_linux_guest(mem, mem_size, &opts, &kernel, &initramfs);
	build_vcpu(&state);
	write_cpustate(&state);

	loaded_initramfs_fini(&initramfs);
	linux_kernel_fini(&kernel);
	free(opts.cmdline);
	return 0;
}

static uint64_t
align_up_u64(uint64_t value, uint64_t align)
{
	return (value + align - 1) & ~(align - 1);
}

static uint64_t
align_down_u64(uint64_t value, uint64_t align)
{
	return value & ~(align - 1);
}

static uint8_t
read8(const uint8_t *buf, uint64_t off)
{
	return buf[off];
}

static uint16_t
read16(const uint8_t *buf, uint64_t off)
{
	return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}

static uint32_t
read32(const uint8_t *buf, uint64_t off)
{
	return (uint32_t)buf[off] |
	    ((uint32_t)buf[off + 1] << 8) |
	    ((uint32_t)buf[off + 2] << 16) |
	    ((uint32_t)buf[off + 3] << 24);
}

static void
write8(uint8_t *buf, uint64_t off, uint8_t value)
{
	buf[off] = value;
}

static void
write16(uint8_t *buf, uint64_t off, uint16_t value)
{
	buf[off] = value & 0xffU;
	buf[off + 1] = (value >> 8) & 0xffU;
}

static void
write32(uint8_t *buf, uint64_t off, uint32_t value)
{
	buf[off] = value & 0xffU;
	buf[off + 1] = (value >> 8) & 0xffU;
	buf[off + 2] = (value >> 16) & 0xffU;
	buf[off + 3] = (value >> 24) & 0xffU;
}

static void
write64(uint8_t *buf, uint64_t off, uint64_t value)
{
	memcpy(buf + off, &value, sizeof(value));
}

static void
parse_options(int argc, char **argv, struct loader_options *opts)
{
	size_t cmdline_cap;
	size_t cmdline_len;
	int i;

	if (argc < 2)
		errx(1, "usage: %s kernel-path [key=value ...]", argv[0]);
	memset(opts, 0, sizeof(*opts));
	opts->kernel_path = argv[1];
	cmdline_cap = 1;
	for (i = 2; i < argc; i++) {
		char *eq = strchr(argv[i], '=');

		if (eq == NULL || eq == argv[i])
			errx(1, "argument is not key=value: %s", argv[i]);
		if (strncmp(argv[i], "initramfs=", 10) == 0) {
			if (argv[i][10] == '\0')
				errx(1, "initramfs path is empty");
			if (opts->initramfs_path != NULL)
				errx(1, "duplicate initramfs argument");
			opts->initramfs_path = argv[i] + 10;
			continue;
		}
		cmdline_cap += strlen(argv[i]) + 1;
	}
	opts->cmdline = calloc(1, cmdline_cap);
	if (opts->cmdline == NULL)
		err(1, "calloc cmdline");
	cmdline_len = 0;
	for (i = 2; i < argc; i++) {
		if (strncmp(argv[i], "initramfs=", 10) == 0)
			continue;
		if (cmdline_len != 0)
			opts->cmdline[cmdline_len++] = ' ';
		strcpy(opts->cmdline + cmdline_len, argv[i]);
		cmdline_len += strlen(argv[i]);
	}
	opts->cmdline_len = cmdline_len;
}

static uint8_t *
map_fd(int fd, int prot, uint64_t *sizep)
{
	struct stat st;
	void *addr;

	if (fstat(fd, &st) != 0)
		err(1, "fstat fd%d", fd);
	if (st.st_size <= 0)
		errx(1, "fd%d has invalid size", fd);
	addr = mmap(NULL, (size_t)st.st_size, prot, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED)
		err(1, "mmap fd%d", fd);
	*sizep = (uint64_t)st.st_size;
	return addr;
}

static void
map_file_readonly(const char *path, struct mapped_file *file)
{
	struct stat st;
	void *addr;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		err(1, "open %s", path);
	if (fstat(fd, &st) != 0)
		err(1, "fstat %s", path);
	if (st.st_size <= 0)
		errx(1, "%s is empty", path);
	addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED)
		err(1, "mmap %s", path);
	close(fd);
	file->path = path;
	file->data = addr;
	file->size = (uint64_t)st.st_size;
}

static void
unmap_file(const struct mapped_file *file)
{
	if (file->data != NULL)
		munmap(file->data, (size_t)file->size);
}

static void
linux_kernel_init(struct linux_kernel *kernel, const char *path)
{
	uint8_t setup_sects;
	uint64_t payload_offset;

	memset(kernel, 0, sizeof(*kernel));
	map_file_readonly(path, &kernel->image);
	if (kernel->image.size < 0x290)
		errx(1, "%s is too small to be a Linux bzImage", path);
	if (read32(kernel->image.data, LINUX_HDR_MAGIC) != LINUX_HDRS)
		errx(1, "%s is not a Linux bzImage with HdrS setup header",
		    path);
	kernel->protocol = read16(kernel->image.data, LINUX_HDR_VERSION);
	if (kernel->protocol < LINUX_BOOT_PROTOCOL_MIN) {
		errx(1, "%s boot protocol %#x is older than required %#x",
		    path, kernel->protocol, LINUX_BOOT_PROTOCOL_MIN);
	}
	kernel->loadflags = read8(kernel->image.data, LINUX_LOADFLAGS);
	if ((kernel->loadflags & LINUX_LOADED_HIGH) == 0)
		errx(1, "%s is not a high-loaded bzImage", path);
	kernel->xloadflags = read16(kernel->image.data, LINUX_XLOADFLAGS);
	if ((kernel->xloadflags & LINUX_XLF_KERNEL_64) == 0)
		errx(1, "%s is not an x86_64 boot-protocol kernel", path);
	kernel->cmdline_size = read32(kernel->image.data,
	    LINUX_CMDLINE_SIZE);
	if (kernel->cmdline_size == 0)
		kernel->cmdline_size = 2048;
	kernel->init_size = read32(kernel->image.data, LINUX_INIT_SIZE);
	setup_sects = read8(kernel->image.data, LINUX_SETUP_SECTS);
	if (setup_sects == 0)
		setup_sects = 4;
	payload_offset = ((uint64_t)setup_sects + 1) * 512;
	if (payload_offset >= kernel->image.size)
		errx(1, "%s has no protected-mode kernel payload", path);
	kernel->payload_offset = payload_offset;
	kernel->payload_size = kernel->image.size - payload_offset;
	if (kernel->init_size < kernel->payload_size)
		kernel->init_size = kernel->payload_size;
}

static void
linux_kernel_fini(const struct linux_kernel *kernel)
{
	unmap_file(&kernel->image);
}

static void
check_guest_range(uint64_t mem_size, uint64_t start, uint64_t size,
    const char *name)
{
	if (size == 0 || start >= mem_size || size > mem_size - start)
		errx(1, "%s does not fit guest memory", name);
}

static void
build_identity_page_tables(uint8_t *mem)
{
	unsigned int i;

	memset(mem + PML4_GPA, 0, PAGE_SIZE_GUEST * 3);
	write64(mem, PML4_GPA, PDPT_GPA | 3);
	write64(mem, PDPT_GPA, PD_GPA | 3);
	for (i = 0; i < 512; i++)
		write64(mem, PD_GPA + i * 8, (uint64_t)i * 0x200000ULL |
		    0x83ULL);
}

static void
build_descriptor_tables(uint8_t *mem)
{
	memset(mem + GDT_GPA, 0, PAGE_SIZE_GUEST);
	write64(mem, GDT_GPA + 16, 0x00209a0000000000ULL);
	write64(mem, GDT_GPA + 24, 0x0000920000000000ULL);
	write64(mem, GDT_GPA + 32, 0x0000890060000067ULL);
	memset(mem + TSS_GPA, 0, 0x68);
}

static void
write_e820_entry(uint8_t *boot_params, unsigned int index, uint64_t addr,
    uint64_t size, uint32_t type)
{
	uint64_t off;

	off = LINUX_E820_TABLE + (uint64_t)index * LINUX_E820_ENTRY_SIZE;
	write64(boot_params, off, addr);
	write64(boot_params, off + 8, size);
	write32(boot_params, off + 16, type);
}

static void
build_boot_params(uint8_t *mem, uint64_t mem_size,
    const struct loader_options *opts, const struct linux_kernel *kernel,
    const struct loaded_initramfs *initramfs)
{
	uint8_t *boot_params;
	uint64_t cmdline_addr;
	uint64_t initramfs_addr;
	uint32_t initramfs_size;
	uint8_t loadflags;
	unsigned int e820_count;

	if (opts->cmdline_len + 1 > CMDLINE_CAP)
		errx(1, "command line does not fit one guest page");
	if (opts->cmdline_len + 1 > kernel->cmdline_size)
		errx(1, "command line exceeds Linux cmdline_size");
	boot_params = mem + BOOT_PARAMS_GPA;
	memset(boot_params, 0, PAGE_SIZE_GUEST);
	memcpy(boot_params + LINUX_SETUP_SECTS,
	    kernel->image.data + LINUX_SETUP_SECTS, LINUX_SETUP_HEADER_COPY);

	loadflags = read8(boot_params, LINUX_LOADFLAGS);
	write8(boot_params, LINUX_TYPE_OF_LOADER, LINUX_LOADER_KEXEC);
	write8(boot_params, LINUX_LOADFLAGS, loadflags | LINUX_CAN_USE_HEAP);
	write16(boot_params, LINUX_HEAP_END_PTR, 0xe000 - 0x200);
	write32(boot_params, LINUX_CODE32_START, (uint32_t)KERNEL_LOAD_GPA);

	cmdline_addr = CMDLINE_GPA;
	write32(boot_params, LINUX_CMD_LINE_PTR, (uint32_t)cmdline_addr);
	write32(boot_params, LINUX_EXT_CMD_LINE_PTR,
	    (uint32_t)(cmdline_addr >> 32));
	memset(mem + CMDLINE_GPA, 0, CMDLINE_CAP);
	memcpy(mem + CMDLINE_GPA, opts->cmdline, opts->cmdline_len);

	if (initramfs->file.data != NULL) {
		initramfs_addr = initramfs->gpa;
		initramfs_size = (uint32_t)initramfs->file.size;
		write32(boot_params, LINUX_RAMDISK_IMAGE,
		    (uint32_t)initramfs_addr);
		write32(boot_params, LINUX_RAMDISK_SIZE, initramfs_size);
		write32(boot_params, LINUX_EXT_RAMDISK_IMAGE,
		    (uint32_t)(initramfs_addr >> 32));
		write32(boot_params, LINUX_EXT_RAMDISK_SIZE,
		    (uint32_t)(initramfs->file.size >> 32));
	}

	write64(boot_params, LINUX_ACPI_RSDP_ADDR,
	    VMMFS_PLATFORM_ACPI_GPA);
	e820_count = 0;
	write_e820_entry(boot_params, e820_count++, 0,
	    VMMFS_PLATFORM_ACPI_GPA, LINUX_E820_RAM);
	write_e820_entry(boot_params, e820_count++, VMMFS_PLATFORM_ACPI_GPA,
	    VMMFS_PLATFORM_ACPI_SIZE, LINUX_E820_RESERVED);
	write_e820_entry(boot_params, e820_count++,
	    VMMFS_PLATFORM_ACPI_GPA + VMMFS_PLATFORM_ACPI_SIZE,
	    0x9f000 - (VMMFS_PLATFORM_ACPI_GPA + VMMFS_PLATFORM_ACPI_SIZE),
	    LINUX_E820_RAM);
	write_e820_entry(boot_params, e820_count++, 0x9f000,
	    ONE_MIB - 0x9f000, LINUX_E820_RESERVED);
	if (mem_size > ONE_MIB)
		write_e820_entry(boot_params, e820_count++, ONE_MIB,
		    mem_size - ONE_MIB, LINUX_E820_RAM);
	write8(boot_params, LINUX_E820_ENTRIES, (uint8_t)e820_count);
}

static void
load_kernel_payload(uint8_t *mem, uint64_t mem_size,
    const struct linux_kernel *kernel)
{
	if (KERNEL_LOAD_GPA + kernel->init_size > ONE_GIB)
		errx(1, "Linux kernel init window exceeds identity map limit");
	check_guest_range(mem_size, KERNEL_LOAD_GPA, kernel->init_size,
	    "Linux kernel init window");
	memcpy(mem + KERNEL_LOAD_GPA,
	    kernel->image.data + kernel->payload_offset,
	    (size_t)kernel->payload_size);
	if (kernel->init_size > kernel->payload_size) {
		memset(mem + KERNEL_LOAD_GPA + kernel->payload_size, 0,
		    (size_t)(kernel->init_size - kernel->payload_size));
	}
}

static void
load_initramfs(uint8_t *mem, uint64_t mem_size,
    const struct loader_options *opts, const struct linux_kernel *kernel,
    struct loaded_initramfs *initramfs)
{
	uint64_t high;
	uint64_t low;
	uint64_t start;

	if (opts->initramfs_path == NULL)
		return;
	map_file_readonly(opts->initramfs_path, &initramfs->file);
	if (initramfs->file.size > UINT32_MAX)
		errx(1, "initramfs is larger than the 32-bit boot field");
	high = mem_size < ONE_GIB ? mem_size : ONE_GIB;
	high = align_down_u64(high, PAGE_SIZE_GUEST);
	low = align_up_u64(KERNEL_LOAD_GPA + kernel->init_size,
	    PAGE_SIZE_GUEST);
	if (high <= low || initramfs->file.size > high - low)
		errx(1, "initramfs does not fit below the identity map limit");
	start = align_down_u64(high - initramfs->file.size, PAGE_SIZE_GUEST);
	if (start < low)
		errx(1, "initramfs overlaps Linux kernel init window");
	memcpy(mem + start, initramfs->file.data, (size_t)initramfs->file.size);
	initramfs->gpa = start;
}

static void
build_linux_guest(uint8_t *mem, uint64_t mem_size,
    const struct loader_options *opts, const struct linux_kernel *kernel,
    struct loaded_initramfs *initramfs)
{
	if (mem_size < 16 * ONE_MIB)
		errx(1, "guest memory must be at least 16M");
	check_guest_range(mem_size, PML4_GPA, PAGE_SIZE_GUEST * 3,
	    "page tables");
	check_guest_range(mem_size, GDT_GPA, PAGE_SIZE_GUEST * 2,
	    "descriptor tables");
	check_guest_range(mem_size, STACK_TOP_GPA - PAGE_SIZE_GUEST,
	    PAGE_SIZE_GUEST, "boot stack");
	check_guest_range(mem_size, BOOT_PARAMS_GPA, PAGE_SIZE_GUEST,
	    "boot params");
	check_guest_range(mem_size, CMDLINE_GPA, CMDLINE_CAP, "cmdline");

	build_identity_page_tables(mem);
	build_descriptor_tables(mem);
	load_kernel_payload(mem, mem_size, kernel);
	load_initramfs(mem, mem_size, opts, kernel, initramfs);
	build_boot_params(mem, mem_size, opts, kernel, initramfs);
}

static void
set_segment(struct vmm_segment *seg, uint16_t selector,
    uint16_t attrib, uint32_t limit, uint64_t base)
{
	seg->selector = selector;
	seg->attrib.type = attrib & 0xf;
	seg->attrib.s = (attrib & SEG_S) != 0;
	seg->attrib.dpl = (attrib >> 5) & 0x3;
	seg->attrib.p = (attrib & SEG_P) != 0;
	seg->attrib.avl = (attrib >> 8) & 0x1;
	seg->attrib.l = (attrib & SEG_L) != 0;
	seg->attrib.def = (attrib & SEG_DB) != 0;
	seg->attrib.g = (attrib & SEG_G) != 0;
	seg->attrib.rsvd = 0;
	seg->limit = limit;
	seg->base = base;
}

static void
build_vcpu(struct vmm_cpustate *state)
{
	memset(state, 0, sizeof(*state));
	state->gprs[VMM_X64_GPR_RSP] = STACK_TOP_GPA;
	state->gprs[VMM_X64_GPR_RSI] = BOOT_PARAMS_GPA;
	state->gprs[VMM_X64_GPR_RIP] =
	    KERNEL_LOAD_GPA + KERNEL_64_ENTRY_DELTA;
	state->gprs[VMM_X64_GPR_RFLAGS] = 2;
	state->crs[VMM_X64_CR_CR0] = CR0_PE | CR0_NE | CR0_PG;
	state->crs[VMM_X64_CR_CR3] = PML4_GPA;
	state->crs[VMM_X64_CR_CR4] = CR4_PAE;
	state->crs[VMM_X64_CR_XCR0] = XCR0_X87;
	state->msrs[VMM_X64_MSR_EFER] = EFER_LME | EFER_LMA;
	state->msrs[VMM_X64_MSR_PAT] = 0x0007040600070406ULL;

	set_segment(&state->segs[VMM_X64_SEG_ES], 0x18,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&state->segs[VMM_X64_SEG_CS], 0x10,
	    0xb | SEG_S | SEG_P | SEG_L | SEG_G, 0xffffffffU, 0);
	set_segment(&state->segs[VMM_X64_SEG_SS], 0x18,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&state->segs[VMM_X64_SEG_DS], 0x18,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&state->segs[VMM_X64_SEG_FS], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&state->segs[VMM_X64_SEG_GS], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&state->segs[VMM_X64_SEG_GDT], 0, 0, 47, GDT_GPA);
	set_segment(&state->segs[VMM_X64_SEG_IDT], 0, 0, 0, 0);
	set_segment(&state->segs[VMM_X64_SEG_LDT], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&state->segs[VMM_X64_SEG_TR], 0x20, 0x9 | SEG_P, 0x67,
	    TSS_GPA);
}

static void
write_cpustate(const struct vmm_cpustate *state)
{
	ssize_t written;

	written = write(2, state, sizeof(*state));
	if (written < 0)
		err(1, "write fd2 cpustate");
	if ((size_t)written != sizeof(*state))
		errx(1, "short write to fd2 cpustate");
}

static void
loaded_initramfs_fini(const struct loaded_initramfs *initramfs)
{
	unmap_file(&initramfs->file);
}
