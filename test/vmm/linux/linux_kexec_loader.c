/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Linux x86_64 kexec-style fd3/fd4 loader for dfvmm tests.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_SIZE_GUEST		4096ULL
#define ONE_MIB			0x100000ULL
#define ONE_GIB			0x40000000ULL

#define PML4_GPA		0x2000ULL
#define PDPT_GPA		0x3000ULL
#define PD_GPA			0x4000ULL
#define GDT_GPA			0x5000ULL
#define TSS_GPA			0x6000ULL
#define ACPI_GPA		0x70000ULL
#define BOOT_PARAMS_GPA		0x90000ULL
#define CMDLINE_GPA		0x98000ULL
#define STACK_TOP_GPA		0x80000ULL
#define KERNEL_LOAD_GPA		0x100000ULL
#define KERNEL_64_ENTRY_DELTA	0x200ULL
#define ACPI_SIZE		PAGE_SIZE_GUEST
#define ACPI_RSDP_GPA		(ACPI_GPA + 0x000ULL)
#define ACPI_XSDT_GPA		(ACPI_GPA + 0x100ULL)
#define ACPI_FADT_GPA		(ACPI_GPA + 0x200ULL)
#define ACPI_MADT_GPA		(ACPI_GPA + 0x400ULL)
#define ACPI_HPET_GPA		(ACPI_GPA + 0x500ULL)
#define ACPI_DSDT_GPA		(ACPI_GPA + 0x600ULL)
#define ACPI_MCFG_GPA		(ACPI_GPA + 0x800ULL)
#define ACPI_HPET_MMIO_GPA	0xfed00000ULL
#define ACPI_IOAPIC_GPA		0xfec00000ULL
#define ACPI_LAPIC_GPA		0xfee00000ULL
#define VMM_PCIE_ECAM_GPA	0xe0000000ULL
#define VMM_PCIE_ECAM_SIZE	0x00100000ULL
#define CMDLINE_CAP		PAGE_SIZE_GUEST

#define LINUX_ACPI_RSDP_ADDR	0x070U
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
#define LINUX_E820_ACPI		3U

#define ACPI_TABLE_HEADER_SIZE	36U
#define ACPI_RSDP_SIZE		36U
#define ACPI_FADT_SIZE		276U
#define ACPI_MCFG_SIZE		60U
#define ACPI_MADT_LOCAL_APIC_SIZE	8U
#define ACPI_MADT_IOAPIC_SIZE		12U
#define ACPI_MADT_INTERRUPT_OVERRIDE_SIZE	10U
#define ACPI_FADT_WBINVD	0x00000001U
#define ACPI_FADT_RESET_REGISTER	0x00000400U
#define ACPI_FADT_HW_REDUCED	0x00100000U
#define ACPI_FADT_NO_VGA	0x0004U
#define ACPI_SPACE_SYSTEM_MEMORY	0U
#define ACPI_SPACE_SYSTEM_IO	1U
#define ACPI_ACCESS_BYTE	1U
#define ACPI_ACCESS_DWORD	3U
#define ACPI_MADT_LOCAL_APIC_ENABLED	0x00000001U
#define ACPI_MADT_POLARITY_ACTIVE_HIGH	0x0001U
#define ACPI_MADT_TRIGGER_EDGE		0x0004U
#define ACPI_ISA_BUS			0U
#define ACPI_COM1_IRQ			4U
#define ACPI_SLEEP_CONTROL_PORT	0x404U
#define ACPI_SLEEP_STATUS_PORT	0x405U
#define ACPI_PM_TIMER_PORT	0x408U
#define ACPI_RESET_PORT		0x40cU
#define ACPI_RESET_VALUE	0x01U

static const uint8_t vmm_linux_dsdt[] = {
	0x44, 0x53, 0x44, 0x54, 0xa5, 0x00, 0x00, 0x00,
	0x02, 0xb5, 0x44, 0x46, 0x56, 0x4d, 0x4d, 0x00,
	0x44, 0x46, 0x56, 0x4d, 0x4d, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x49, 0x4e, 0x54, 0x4c,
	0x12, 0x12, 0x25, 0x20,
	0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x06, 0x02,
	0x0a, 0x05, 0x0a, 0x05,
	0x10, 0x44, 0x07, 0x5f,
	0x53, 0x42, 0x5f, 0x5b, 0x82, 0x35, 0x43, 0x4f,
	0x4d,
	0x31, 0x08, 0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41,
	0xd0, 0x05, 0x01, 0x08, 0x5f, 0x55, 0x49, 0x44,
	0x01, 0x14, 0x09, 0x5f, 0x53, 0x54, 0x41, 0x00,
	0xa4, 0x0a, 0x0f, 0x08, 0x5f, 0x43, 0x52, 0x53,
	0x11, 0x10, 0x0a, 0x0d, 0x47, 0x01, 0xf8, 0x03,
	0xf8, 0x03, 0x01, 0x08, 0x22, 0x10, 0x00, 0x79,
	0x00,
	0x5b, 0x82, 0x35, 0x52, 0x54, 0x43, 0x30, 0x08,
	0x5f, 0x48, 0x49, 0x44, 0x0c, 0x00, 0x0b, 0xd0,
	0x41, 0x08, 0x5f, 0x55, 0x49, 0x44, 0x00, 0x14,
	0x09, 0x5f, 0x53, 0x54, 0x41, 0x00, 0xa4, 0x0a,
	0x0f, 0x08, 0x5f, 0x43, 0x52, 0x53, 0x11, 0x10,
	0x0a, 0x0d, 0x47, 0x01, 0x70, 0x00, 0x70, 0x00,
	0x01, 0x02, 0x22, 0x00, 0x01, 0x79, 0x00,
};

/* PCI0 AML body, compiled and validated with ACPICA iasl/acpiexec. */
static const uint8_t vmm_linux_pci_root_aml[] = {
	0x10, 0x49, 0x04, 0x5f, 0x53, 0x42, 0x5f, 0x5b,
	0x82, 0x41, 0x04, 0x50, 0x43, 0x49, 0x30, 0x08,
	0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x0a,
	0x08, 0x08, 0x5f, 0x43, 0x49, 0x44, 0x0c, 0x41,
	0xd0, 0x0a, 0x03, 0x08, 0x5f, 0x53, 0x45, 0x47,
	0x00, 0x08, 0x5f, 0x42, 0x42, 0x4e, 0x00, 0x08,
	0x5f, 0x43, 0x52, 0x53, 0x11, 0x15, 0x0a, 0x12,
	0x88, 0x0d, 0x00, 0x02, 0x0c, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x79, 0x00,
};

#define VMM_MANIFEST_MAGIC	"VMMLD0\0\0"
#define VMM_MANIFEST_ABI	1
#define VMM_MANIFEST_ARCH_X64	1
#define VMM_REC_X64_VCPU_STATE	1
#define VMM_REC_GPA_RANGE	2
#define VMM_REC_X64_TIME_STATE	3
#define VMM_REC_F_MANDATORY	1

#define VMM_X64_NGPR		18
#define VMM_X64_NCR		6
#define VMM_X64_NMSR		11
#define VMM_X64_NSEG		10
#define VMM_GPA_RANGE_MAX	32

#define VMM_GPA_RANGE_LOAD		1
#define VMM_GPA_RANGE_BOOT_PARAMS	2
#define VMM_GPA_RANGE_CMDLINE		3
#define VMM_GPA_RANGE_INITRAMFS		4
#define VMM_GPA_RANGE_PAGE_TABLE	5
#define VMM_GPA_RANGE_DESC_TABLE	6
#define VMM_GPA_RANGE_STACK		7
#define VMM_GPA_RANGE_BOOT_DATA		8

#define VMM_X64_GPR_RSP		4
#define VMM_X64_GPR_RSI		6
#define VMM_X64_GPR_RIP		16
#define VMM_X64_GPR_RFLAGS	17
#define VMM_X64_CR_CR0		0
#define VMM_X64_CR_CR3		2
#define VMM_X64_CR_CR4		3
#define VMM_X64_CR_XCR0		5
#define VMM_X64_MSR_EFER	0
#define VMM_X64_MSR_PAT		9
#define VMM_X64_SEG_ES		0
#define VMM_X64_SEG_CS		1
#define VMM_X64_SEG_SS		2
#define VMM_X64_SEG_DS		3
#define VMM_X64_SEG_FS		4
#define VMM_X64_SEG_GS		5
#define VMM_X64_SEG_GDT		6
#define VMM_X64_SEG_IDT		7
#define VMM_X64_SEG_LDT		8
#define VMM_X64_SEG_TR		9

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
	uint64_t	tsc_hz;
	int		tsc_hz_set;
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

struct vmm_manifest_header {
	char		magic[8];
	uint16_t	abi_version;
	uint16_t	arch;
	uint32_t	header_size;
	uint32_t	total_size;
	uint32_t	record_count;
	uint64_t	mem_size;
	uint32_t	flags;
	uint32_t	reserved;
} __attribute__((packed));

struct vmm_manifest_record {
	uint16_t	type;
	uint16_t	flags;
	uint32_t	size;
} __attribute__((packed));

struct vmm_x64_seg_state {
	uint16_t	selector;
	uint16_t	attrib;
	uint32_t	limit;
	uint64_t	base;
} __attribute__((packed));

struct vmm_x64_vcpu_state {
	uint32_t	vcpu_id;
	uint32_t	flags;
	uint64_t	runnable;
	uint64_t	gpr[VMM_X64_NGPR];
	uint64_t	cr[VMM_X64_NCR];
	uint64_t	msr[VMM_X64_NMSR];
	struct vmm_x64_seg_state seg[VMM_X64_NSEG];
	uint64_t	intr_flags;
} __attribute__((packed));

struct vmm_gpa_range {
	uint64_t	start;
	uint64_t	size;
	uint32_t	type;
	uint32_t	flags;
} __attribute__((packed));

struct vmm_x64_time_state {
	uint64_t	tsc_hz;
} __attribute__((packed));

static void parse_options(int argc, char **argv, struct loader_options *opts);
static uint8_t *map_fd(int fd, int prot, uint64_t *sizep);
static void map_file_readonly(const char *path, struct mapped_file *file);
static void unmap_file(const struct mapped_file *file);
static void linux_kernel_init(struct linux_kernel *kernel, const char *path);
static void linux_kernel_fini(const struct linux_kernel *kernel);
static void build_linux_guest(uint8_t *mem, uint64_t mem_size,
    const struct loader_options *opts, const struct linux_kernel *kernel,
    struct loaded_initramfs *initramfs, struct vmm_gpa_range *ranges,
    uint32_t *range_count);
static void build_vcpu(struct vmm_x64_vcpu_state *vcpu);
static void build_manifest(uint8_t *manifest, uint64_t manifest_size,
    uint64_t mem_size, const struct vmm_x64_vcpu_state *vcpu,
    const struct vmm_x64_time_state *time,
    const struct vmm_gpa_range *ranges, uint32_t range_count);
static void loaded_initramfs_fini(const struct loaded_initramfs *initramfs);

int
main(int argc, char **argv)
{
	struct loader_options opts;
	struct linux_kernel kernel;
	struct loaded_initramfs initramfs;
	struct vmm_x64_vcpu_state vcpu;
	struct vmm_gpa_range ranges[VMM_GPA_RANGE_MAX];
	struct vmm_x64_time_state time;
	uint8_t *mem;
	uint8_t *manifest;
	uint64_t mem_size;
	uint64_t manifest_size;
	uint32_t range_count;

	parse_options(argc, argv, &opts);
	mem = map_fd(3, PROT_READ | PROT_WRITE, &mem_size);
	manifest = map_fd(4, PROT_READ | PROT_WRITE, &manifest_size);
	linux_kernel_init(&kernel, opts.kernel_path);
	memset(&initramfs, 0, sizeof(initramfs));

	build_linux_guest(mem, mem_size, &opts, &kernel, &initramfs, ranges,
	    &range_count);
	build_vcpu(&vcpu);
	time.tsc_hz = opts.tsc_hz;
	build_manifest(manifest, manifest_size, mem_size, &vcpu, &time, ranges,
	    range_count);

	loaded_initramfs_fini(&initramfs);
	linux_kernel_fini(&kernel);
	free(opts.cmdline);
	return 0;
}

static size_t
align8(size_t value)
{
	return (value + 7U) & ~(size_t)7U;
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
		if (strncmp(argv[i], "tsc_hz=", 7) == 0) {
			char *end;
			uint64_t tsc_hz;

			if (opts->tsc_hz_set)
				errx(1, "duplicate tsc_hz argument");
			if (strcmp(argv[i] + 7, "host") == 0) {
				tsc_hz = 0;
			} else {
				errno = 0;
				tsc_hz = strtoull(argv[i] + 7, &end, 10);
				if (errno != 0 || end == argv[i] + 7 || *end != '\0' ||
				    tsc_hz == 0) {
					errx(1, "invalid tsc_hz argument: %s", argv[i]);
				}
			}
			opts->tsc_hz = tsc_hz;
			opts->tsc_hz_set = 1;
			continue;
		}
		cmdline_cap += strlen(argv[i]) + 1;
	}
	opts->cmdline = calloc(1, cmdline_cap);
	if (opts->cmdline == NULL)
		err(1, "calloc cmdline");
	cmdline_len = 0;
	for (i = 2; i < argc; i++) {
		if (strncmp(argv[i], "initramfs=", 10) == 0 ||
		    strncmp(argv[i], "tsc_hz=", 7) == 0)
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
add_range(struct vmm_gpa_range *ranges, uint32_t *count, uint64_t start,
    uint64_t size, uint32_t type)
{
	if (size == 0)
		return;
	if (*count >= VMM_GPA_RANGE_MAX)
		errx(1, "too many GPA ranges");
	ranges[*count].start = start;
	ranges[*count].size = size;
	ranges[*count].type = type;
	ranges[*count].flags = 0;
	(*count)++;
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

static uint8_t
checksum8(const uint8_t *buf, uint32_t len)
{
	uint8_t sum;
	uint32_t i;

	sum = 0;
	for (i = 0; i < len; i++)
		sum += buf[i];
	return sum;
}

static void
write_acpi_checksum(uint8_t *table, uint32_t len, uint32_t off)
{
	table[off] = 0;
	table[off] = (uint8_t)(0U - checksum8(table, len));
}

static void
write_acpi_header(uint8_t *table, const char signature[4], uint32_t len,
    uint8_t revision)
{
	memset(table, 0, len);
	memcpy(table, signature, 4);
	write32(table, 4, len);
	write8(table, 8, revision);
	memcpy(table + 10, "DFVMM ", 6);
	memcpy(table + 16, "DFVMM   ", 8);
	write32(table, 24, 1);
	memcpy(table + 28, "VMM ", 4);
	write32(table, 32, 1);
}

static void
build_acpi_tables(uint8_t *mem)
{
	uint8_t *rsdp;
	uint8_t *xsdt;
	uint8_t *fadt;
	uint8_t *madt;
	uint8_t *hpet;
	uint8_t *mcfg;
	uint8_t *dsdt;
	uint8_t *lapic;
	uint8_t *ioapic;
	uint8_t *iso;
	uint32_t xsdt_len;
	uint32_t dsdt_len;
	uint32_t madt_len;

	memset(mem + ACPI_GPA, 0, ACPI_SIZE);

	rsdp = mem + ACPI_RSDP_GPA;
	memcpy(rsdp, "RSD PTR ", 8);
	memcpy(rsdp + 9, "DFVMM ", 6);
	write8(rsdp, 15, 2);
	write32(rsdp, 16, 0);
	write32(rsdp, 20, ACPI_RSDP_SIZE);
	write64(rsdp, 24, ACPI_XSDT_GPA);
	write_acpi_checksum(rsdp, 20, 8);
	write_acpi_checksum(rsdp, ACPI_RSDP_SIZE, 32);

	xsdt = mem + ACPI_XSDT_GPA;
	xsdt_len = ACPI_TABLE_HEADER_SIZE + 4 * sizeof(uint64_t);
	write_acpi_header(xsdt, "XSDT", xsdt_len, 1);
	write64(xsdt, ACPI_TABLE_HEADER_SIZE, ACPI_FADT_GPA);
	write64(xsdt, ACPI_TABLE_HEADER_SIZE + sizeof(uint64_t),
	    ACPI_MADT_GPA);
	write64(xsdt, ACPI_TABLE_HEADER_SIZE + 2 * sizeof(uint64_t),
	    ACPI_HPET_GPA);
	write64(xsdt, ACPI_TABLE_HEADER_SIZE + 3 * sizeof(uint64_t),
	    ACPI_MCFG_GPA);
	write_acpi_checksum(xsdt, xsdt_len, 9);

	dsdt = mem + ACPI_DSDT_GPA;
	memcpy(dsdt, vmm_linux_dsdt, sizeof(vmm_linux_dsdt));
	dsdt_len = sizeof(vmm_linux_dsdt) + sizeof(vmm_linux_pci_root_aml);
	memcpy(dsdt + sizeof(vmm_linux_dsdt), vmm_linux_pci_root_aml,
	    sizeof(vmm_linux_pci_root_aml));
	write32(dsdt, 4, dsdt_len);
	write_acpi_checksum(dsdt, dsdt_len, 9);

	fadt = mem + ACPI_FADT_GPA;
	write_acpi_header(fadt, "FACP", ACPI_FADT_SIZE, 6);
	write32(fadt, 40, (uint32_t)ACPI_DSDT_GPA);
	write8(fadt, 45, 7);
	write16(fadt, 46, 0);
	write32(fadt, 76, ACPI_PM_TIMER_PORT);
	write8(fadt, 91, 4);
	write16(fadt, 109, ACPI_FADT_NO_VGA);
	write32(fadt, 112, ACPI_FADT_WBINVD | ACPI_FADT_RESET_REGISTER |
	    ACPI_FADT_HW_REDUCED);
	write8(fadt, 116, ACPI_SPACE_SYSTEM_IO);
	write8(fadt, 117, 8);
	write8(fadt, 118, 0);
	write8(fadt, 119, ACPI_ACCESS_BYTE);
	write64(fadt, 120, ACPI_RESET_PORT);
	write8(fadt, 128, ACPI_RESET_VALUE);
	write8(fadt, 131, 5);
	write64(fadt, 140, ACPI_DSDT_GPA);
	write8(fadt, 208, ACPI_SPACE_SYSTEM_IO);
	write8(fadt, 209, 32);
	write8(fadt, 210, 0);
	write8(fadt, 211, ACPI_ACCESS_DWORD);
	write64(fadt, 212, ACPI_PM_TIMER_PORT);
	write8(fadt, 244, ACPI_SPACE_SYSTEM_IO);
	write8(fadt, 245, 8);
	write8(fadt, 246, 0);
	write8(fadt, 247, ACPI_ACCESS_BYTE);
	write64(fadt, 248, ACPI_SLEEP_CONTROL_PORT);
	write8(fadt, 256, ACPI_SPACE_SYSTEM_IO);
	write8(fadt, 257, 8);
	write8(fadt, 258, 0);
	write8(fadt, 259, ACPI_ACCESS_BYTE);
	write64(fadt, 260, ACPI_SLEEP_STATUS_PORT);
	write_acpi_checksum(fadt, ACPI_FADT_SIZE, 9);

	madt = mem + ACPI_MADT_GPA;
	madt_len = ACPI_TABLE_HEADER_SIZE + 8 + ACPI_MADT_LOCAL_APIC_SIZE +
	    ACPI_MADT_IOAPIC_SIZE + ACPI_MADT_INTERRUPT_OVERRIDE_SIZE;
	write_acpi_header(madt, "APIC", madt_len, 3);
	write32(madt, 36, (uint32_t)ACPI_LAPIC_GPA);
	write32(madt, 40, 0);
	lapic = madt + 44;
	write8(lapic, 0, 0);
	write8(lapic, 1, ACPI_MADT_LOCAL_APIC_SIZE);
	write8(lapic, 2, 0);
	write8(lapic, 3, 0);
	write32(lapic, 4, ACPI_MADT_LOCAL_APIC_ENABLED);
	ioapic = lapic + ACPI_MADT_LOCAL_APIC_SIZE;
	write8(ioapic, 0, 1);
	write8(ioapic, 1, ACPI_MADT_IOAPIC_SIZE);
	write8(ioapic, 2, 1);
	write8(ioapic, 3, 0);
	write32(ioapic, 4, ACPI_IOAPIC_GPA);
	write32(ioapic, 8, 0);
	iso = ioapic + ACPI_MADT_IOAPIC_SIZE;
	write8(iso, 0, 2);
	write8(iso, 1, ACPI_MADT_INTERRUPT_OVERRIDE_SIZE);
	write8(iso, 2, ACPI_ISA_BUS);
	write8(iso, 3, ACPI_COM1_IRQ);
	write32(iso, 4, ACPI_COM1_IRQ);
	write16(iso, 8, ACPI_MADT_POLARITY_ACTIVE_HIGH |
	    ACPI_MADT_TRIGGER_EDGE);
	write_acpi_checksum(madt, madt_len, 9);

	hpet = mem + ACPI_HPET_GPA;
	write_acpi_header(hpet, "HPET", 56, 1);
	write32(hpet, 36, 0x80862201U);
	write8(hpet, 40, ACPI_SPACE_SYSTEM_MEMORY);
	write8(hpet, 41, 64);
	write8(hpet, 42, 0);
	write8(hpet, 43, 4);
	write64(hpet, 44, ACPI_HPET_MMIO_GPA);
	write8(hpet, 52, 0);
	write16(hpet, 53, 0x80);
	write8(hpet, 55, 0);
	write_acpi_checksum(hpet, 56, 9);

	mcfg = mem + ACPI_MCFG_GPA;
	write_acpi_header(mcfg, "MCFG", ACPI_MCFG_SIZE, 1);
	write64(mcfg, 44, VMM_PCIE_ECAM_GPA);
	write16(mcfg, 52, 0);
	write8(mcfg, 54, 0);
	write8(mcfg, 55, 0);
	write32(mcfg, 56, 0);
	write_acpi_checksum(mcfg, ACPI_MCFG_SIZE, 9);
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
	uint64_t ecam_end;
	uint64_t ram_end;
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
	write64(boot_params, LINUX_ACPI_RSDP_ADDR, ACPI_RSDP_GPA);
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

	e820_count = 0;
	write_e820_entry(boot_params, e820_count++, 0, ACPI_GPA,
	    LINUX_E820_RAM);
	write_e820_entry(boot_params, e820_count++, ACPI_GPA, ACPI_SIZE,
	    LINUX_E820_ACPI);
	write_e820_entry(boot_params, e820_count++, ACPI_GPA + ACPI_SIZE,
	    0x9f000 - (ACPI_GPA + ACPI_SIZE), LINUX_E820_RAM);
	write_e820_entry(boot_params, e820_count++, 0x9f000,
	    ONE_MIB - 0x9f000, LINUX_E820_RESERVED);
	if (mem_size > ONE_MIB) {
		ram_end = mem_size < VMM_PCIE_ECAM_GPA ? mem_size :
		    VMM_PCIE_ECAM_GPA;
		if (ram_end > ONE_MIB) {
			write_e820_entry(boot_params, e820_count++, ONE_MIB,
			    ram_end - ONE_MIB, LINUX_E820_RAM);
		}
		if (mem_size > VMM_PCIE_ECAM_GPA) {
			ecam_end = VMM_PCIE_ECAM_GPA + VMM_PCIE_ECAM_SIZE;
			if (ecam_end > mem_size)
				ecam_end = mem_size;
			write_e820_entry(boot_params, e820_count++,
			    VMM_PCIE_ECAM_GPA, ecam_end - VMM_PCIE_ECAM_GPA,
			    LINUX_E820_RESERVED);
			if (mem_size > ecam_end) {
				write_e820_entry(boot_params, e820_count++, ecam_end,
				    mem_size - ecam_end, LINUX_E820_RAM);
			}
		}
	}
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
    struct loaded_initramfs *initramfs, struct vmm_gpa_range *ranges,
    uint32_t *range_count)
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
	check_guest_range(mem_size, ACPI_GPA, ACPI_SIZE, "ACPI tables");

	build_identity_page_tables(mem);
	build_descriptor_tables(mem);
	build_acpi_tables(mem);
	load_kernel_payload(mem, mem_size, kernel);
	load_initramfs(mem, mem_size, opts, kernel, initramfs);
	build_boot_params(mem, mem_size, opts, kernel, initramfs);

	*range_count = 0;
	add_range(ranges, range_count, KERNEL_LOAD_GPA, kernel->init_size,
	    VMM_GPA_RANGE_LOAD);
	add_range(ranges, range_count, BOOT_PARAMS_GPA, PAGE_SIZE_GUEST,
	    VMM_GPA_RANGE_BOOT_PARAMS);
	add_range(ranges, range_count, CMDLINE_GPA, opts->cmdline_len + 1,
	    VMM_GPA_RANGE_CMDLINE);
	if (initramfs->file.data != NULL) {
		add_range(ranges, range_count, initramfs->gpa,
		    initramfs->file.size, VMM_GPA_RANGE_INITRAMFS);
	}
	add_range(ranges, range_count, PML4_GPA, PAGE_SIZE_GUEST * 3,
	    VMM_GPA_RANGE_PAGE_TABLE);
	add_range(ranges, range_count, GDT_GPA, PAGE_SIZE_GUEST * 2,
	    VMM_GPA_RANGE_DESC_TABLE);
	add_range(ranges, range_count, STACK_TOP_GPA - PAGE_SIZE_GUEST,
	    PAGE_SIZE_GUEST, VMM_GPA_RANGE_STACK);
	add_range(ranges, range_count, ACPI_GPA, ACPI_SIZE,
	    VMM_GPA_RANGE_BOOT_DATA);
}

static void
set_segment(struct vmm_x64_seg_state *seg, uint16_t selector,
    uint16_t attrib, uint32_t limit, uint64_t base)
{
	seg->selector = selector;
	seg->attrib = attrib;
	seg->limit = limit;
	seg->base = base;
}

static void
build_vcpu(struct vmm_x64_vcpu_state *vcpu)
{
	memset(vcpu, 0, sizeof(*vcpu));
	vcpu->runnable = 1;
	vcpu->gpr[VMM_X64_GPR_RSP] = STACK_TOP_GPA;
	vcpu->gpr[VMM_X64_GPR_RSI] = BOOT_PARAMS_GPA;
	vcpu->gpr[VMM_X64_GPR_RIP] =
	    KERNEL_LOAD_GPA + KERNEL_64_ENTRY_DELTA;
	vcpu->gpr[VMM_X64_GPR_RFLAGS] = 2;
	vcpu->cr[VMM_X64_CR_CR0] = CR0_PE | CR0_NE | CR0_PG;
	vcpu->cr[VMM_X64_CR_CR3] = PML4_GPA;
	vcpu->cr[VMM_X64_CR_CR4] = CR4_PAE;
	vcpu->cr[VMM_X64_CR_XCR0] = XCR0_X87;
	vcpu->msr[VMM_X64_MSR_EFER] = EFER_LME | EFER_LMA;
	vcpu->msr[VMM_X64_MSR_PAT] = 0x0007040600070406ULL;

	set_segment(&vcpu->seg[VMM_X64_SEG_ES], 0x18,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_CS], 0x10,
	    0xb | SEG_S | SEG_P | SEG_L | SEG_G, 0xffffffffU, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_SS], 0x18,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_DS], 0x18,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_FS], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_GS], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_GDT], 0, 0, 47, GDT_GPA);
	set_segment(&vcpu->seg[VMM_X64_SEG_IDT], 0, 0, 0, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_LDT], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_TR], 0x20, 0x9 | SEG_P, 0x67,
	    TSS_GPA);
}

static uint8_t *
add_record(uint8_t *ptr, uint16_t type, const void *payload, uint32_t size)
{
	struct vmm_manifest_record rec;
	size_t total;

	rec.type = type;
	rec.flags = VMM_REC_F_MANDATORY;
	rec.size = size;
	total = align8(sizeof(rec) + size);
	memcpy(ptr, &rec, sizeof(rec));
	memcpy(ptr + sizeof(rec), payload, size);
	memset(ptr + sizeof(rec) + size, 0, total - sizeof(rec) - size);
	return ptr + total;
}

static void
build_manifest(uint8_t *manifest, uint64_t manifest_size, uint64_t mem_size,
    const struct vmm_x64_vcpu_state *vcpu,
    const struct vmm_x64_time_state *time,
    const struct vmm_gpa_range *ranges, uint32_t range_count)
{
	struct vmm_manifest_header hdr;
	uint8_t *ptr;

	if (manifest_size < PAGE_SIZE_GUEST)
		errx(1, "fd4 is smaller than one page");
	memset(manifest, 0, (size_t)manifest_size);
	ptr = manifest + sizeof(hdr);
	ptr = add_record(ptr, VMM_REC_X64_VCPU_STATE, vcpu, sizeof(*vcpu));
	ptr = add_record(ptr, VMM_REC_X64_TIME_STATE, time, sizeof(*time));
	ptr = add_record(ptr, VMM_REC_GPA_RANGE, ranges,
	    range_count * sizeof(ranges[0]));
	if ((uint64_t)(ptr - manifest) > manifest_size)
		errx(1, "manifest does not fit fd4");

	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, VMM_MANIFEST_MAGIC, sizeof(hdr.magic));
	hdr.abi_version = VMM_MANIFEST_ABI;
	hdr.arch = VMM_MANIFEST_ARCH_X64;
	hdr.header_size = sizeof(hdr);
	hdr.total_size = (uint32_t)(ptr - manifest);
	hdr.record_count = 3;
	hdr.mem_size = mem_size;
	memcpy(manifest, &hdr, sizeof(hdr));
}

static void
loaded_initramfs_fini(const struct loaded_initramfs *initramfs)
{
	unmap_file(&initramfs->file);
}
