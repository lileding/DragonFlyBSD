#!/usr/bin/env python3
"""Scalar PIO completion must preserve AL/AX and zero-extend EAX."""
import unittest
from test_regress import COMMON, function, run_c

HARNESS = COMMON + r"""
#define NBBY 8
#define VMM_CPUEXIT_IO 1
#define VMM_X64_GPR_RAX 0
#define VMM_X64_GPR_RIP 1
#define VMMFS_PCI_CONFIG_PIO 1
#define VMMFS_PCI_CONFIG_WRITE 1
#define VMMFS_PCI_CONFIG_READ 0
#define VMMFS_PCI_CONFIG_SUCCESS 0
#define VMMFS_PCISLOT_RESOURCE_PIO 1
typedef void *vmm_vcpu_t;
enum vmm_io_width { VMM_IO_WIDTH_8=1, VMM_IO_WIDTH_16=2, VMM_IO_WIDTH_32=4, VMM_IO_WIDTH_64=8 };
struct vmm_cpustate { uint64_t gprs[2]; };
struct vmm_cpuexit {
    unsigned reason;
    union { struct { unsigned operand_size, port; uint64_t npc; bool in, str, rep; } io; } u;
};
struct vmmfs_vcpu_thread { vmm_vcpu_t vcpu; };
struct vmmfs_pci_config_response { unsigned status; uint64_t value; };
struct vmmfs_pcislot_config { int unused; };
struct vmmfs_pcislot_resource { unsigned kind; bool mapped; uint64_t gpa, size; };
struct vmmfs_pcislot_resources { unsigned count; struct vmmfs_pcislot_resource *items; };
static uint64_t read_value = UINT64_C(0xabcdef12fedcba98);
static unsigned side_effects;
static int match_error, response_status;
struct slot { struct vmmfs_pcislot_config config; };
static struct slot slot;
#define vmmfs_pcislot_resources_slot(r) ((void)(r), &slot)
int vmmfs_pcislot_resource_gpa(struct vmmfs_pcislot_resource *r, uint64_t *v) {
    ++side_effects; *v = r->gpa; return 0;
}
int vmmfs_pcislot_config_match(struct vmmfs_pcislot_config *c,
    struct vmmfs_pcislot_resource *r, int space, uint64_t at,
    enum vmm_io_width width, uint64_t *offset) {
    (void)c; (void)r; (void)space; (void)at; (void)width;
    ++side_effects; *offset=0; return match_error;
}
uint64_t vmmfs_pcislot_config_absent_value(enum vmm_io_width w) {
    return w == 8 ? UINT64_MAX : (UINT64_C(1) << (w * 8)) - 1;
}
int vmmfs_pcislot_config_submit(struct vmmfs_pcislot_config *c,
    struct vmmfs_vcpu_thread *t, struct vmmfs_pcislot_resource *r,
    int space, uint64_t offset, enum vmm_io_width w, int direction,
    uint64_t value, struct vmmfs_pci_config_response *response) {
    (void)c; (void)t; (void)r; (void)space; (void)offset;
    (void)w; (void)direction; (void)value; ++side_effects;
    response->value=read_value; response->status=response_status; return 0;
}
int vmmfs_pcislot_resource_object_read(struct vmmfs_pcislot_resource *r,
    uint64_t offset, void *value, size_t size) {
    (void)r; (void)offset; ++side_effects; memcpy(value,&read_value,size); return 0;
}
int vmmfs_pcislot_resource_object_write(struct vmmfs_pcislot_resource *r,
    uint64_t offset, const void *value, size_t size) {
    (void)r; (void)offset; (void)value; (void)size; ++side_effects; return 0;
}
int vmmfs_pcislot_resource_doorbell(struct vmmfs_pcislot_resource *r,
    uint64_t at, enum vmm_io_width width, uint64_t value) {
    (void)r; (void)at; (void)width; (void)value; ++side_effects; return ENOENT;
}
"""

class PioCompletion(unittest.TestCase):
    def check_path(self, kind):
        source = HARNESS
        if kind == "config":
            source += "\nint\n" + function("vmmfs_pcislot_config.c", "vmmfs_pcislot_config_io")
            call = "vmmfs_pcislot_config_io(&slot.config, &thread, &resource, &state, &exit)"
        elif kind == "resource":
            source += r"""
int vmmfs_pcislot_config_io(struct vmmfs_pcislot_config *c,
    struct vmmfs_vcpu_thread *t, struct vmmfs_pcislot_resource *r,
    struct vmm_cpustate *s, const struct vmm_cpuexit *e) {
    (void)c; (void)t; (void)r; (void)s; (void)e; return ENOENT;
}
"""
            source += "\nint\n" + function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resources_io")
            call = "vmmfs_pcislot_resources_io(&resources, &thread, &state, &exit)"
        else:
            source += "\nstatic int\n" + function("vmmfs_vcpu.c", "vmmfs_vcpu_complete_absent_io")
            call = "vmmfs_vcpu_complete_absent_io(&thread, &state, &exit)"
        source += r"""
int main(void) {
    struct vmmfs_vcpu_thread thread = { &slot };
    struct vmmfs_pcislot_resource resource = { VMMFS_PCISLOT_RESOURCE_PIO, true, 0x100, 16 };
    struct vmmfs_pcislot_resources resources = { 1, &resource };
    struct vmm_cpustate state;
    struct vmm_cpuexit exit = { .reason=VMM_CPUEXIT_IO,
        .u.io={ .port=0x100, .npc=0x1234, .in=true } };
    const uint64_t before = UINT64_C(0x1122334455667788);
    (void)resources; (void)match_error; (void)response_status;
    for (unsigned mode=0; mode<3; ++mode) {
        match_error = mode == 1 ? EINVAL : 0;
        response_status = mode == 2 ? 1 : 0;
        for (unsigned width=1; width<=4; width*=2) {
            exit.u.io.operand_size=width;
            state.gprs[0]=before; state.gprs[1]=0;
            uint64_t mask=(UINT64_C(1) << (width*8))-1;
            uint64_t value=ABSENT ? mask : read_value & mask;
            uint64_t expected=width==4 ? value : (before & ~mask) | value;
            assert(CALL == 0);
            assert(state.gprs[0] == expected && state.gprs[1] == exit.u.io.npc);
            exit.u.io.in=false; state.gprs[0]=before; state.gprs[1]=0;
            assert(CALL == 0);
            assert(state.gprs[0]==before && state.gprs[1]==exit.u.io.npc);
            exit.u.io.in=true;
        }
    }
    const unsigned invalid[] = { 0, 3, 8, 16 };
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        exit.u.io.operand_size=invalid[i]; side_effects=0;
        state.gprs[0]=before; state.gprs[1]=0;
        assert(CALL == EOPNOTSUPP);
        assert(state.gprs[0]==before && state.gprs[1]==0 && side_effects==0);
    }
}
""".replace("CALL", call).replace("ABSENT",
            "1" if kind == "absent" else "mode != 0" if kind == "config" else "0")
        run_c(source)

    def test_config_completion(self):
        self.check_path("config")

    def test_resource_completion(self):
        self.check_path("resource")

    def test_absent_completion(self):
        self.check_path("absent")


class PciRootTokens(unittest.TestCase):
    def test_config_fast_paths_use_root_token(self):
        source = COMMON + r"""
#define NBBY 8
#define VMMFS_PCI_CONFIG_ADDRESS 0xcf8U
#define VMMFS_PCI_CONFIG_DATA 0xcfcU
#define VMMFS_PCI_ECAM_GPA UINT64_C(0xe0000000)
#define VMMFS_PCI_ECAM_SIZE UINT64_C(0x10000000)
typedef void *vmm_vcpu_t;
enum vmm_io_width {
    VMM_IO_WIDTH_8=1, VMM_IO_WIDTH_16=2,
    VMM_IO_WIDTH_32=4, VMM_IO_WIDTH_64=8
};
struct token { unsigned held; };
struct vmmfs_node { struct token token;  struct lock lock; bool dead;};
struct vmmfs_machine { struct vmmfs_node node; struct token token; };
struct vmmfs_pcislot { struct { bool powered; } type0; };
struct vmmfs_pciroot {
    struct vmmfs_node node; struct token token;
    struct vmmfs_machine *machine;
    void *runtime_machine;
    uint32_t config_address;
};
struct vmm_io_read { uint64_t address, value; enum vmm_io_width width; };
struct vmm_io_write { uint64_t address, value; enum vmm_io_width width; };
static struct vmmfs_machine machine;
static struct vmmfs_pciroot root;
static struct vmmfs_pcislot slot;
static bool present = true;
static unsigned reads, writes;
#define vmmfs_pciroot_machine(r) ((r)->machine)
static void lwkt_gettoken(struct token *token) {
    assert(token == &root.token);
    assert(token->held == 0);
    ++token->held;
}
static void lwkt_reltoken(struct token *token) {
    assert(token == &root.token && token->held == 1);
    --token->held;
}
static struct vmmfs_pcislot *vmmfs_pciroot_find_locked(
    struct vmmfs_pciroot *r, uint16_t bdf) {
    assert(r == &root && r->token.held);
    assert(bdf == 8);
    return present ? &slot : NULL;
}
static int vmmfs_pciroot_config_read_locked(struct vmmfs_pciroot *r,
    vmm_vcpu_t cpu, uint16_t bdf, uint16_t offset,
    enum vmm_io_width width, uint32_t *value) {
    assert(r == &root && r->token.held);
    assert(cpu == &machine && bdf == 8 && offset == 0x40);
    (void)width;
    ++reads; *value = 0x12345678;
    return 0;
}
static int vmmfs_pcislot_type0_config_write(struct vmmfs_pcislot *s,
    vmm_vcpu_t cpu, uint16_t offset, enum vmm_io_width width,
    uint32_t value) {
    assert(s == &slot && cpu == &machine && offset == 0x40);
    assert(root.token.held == 0 && machine.token.held == 0);
    assert(width == VMM_IO_WIDTH_32 && value == 0x12345678);
    ++writes;
    return 0;
}
"""
        for name, result in (
                ("config_contains", "bool"), ("ecam_contains", "bool"),
                ("absent_value", "uint32_t"),
                ("config_address_read", "int"), ("config_address_write", "int"),
                ("config_data_read", "int"), ("config_data_write", "int"),
                ("ecam_read", "int"), ("ecam_write", "int")):
            source += "\nstatic " + result + "\n" + function(
                "vmmfs_pciroot.c", "vmmfs_pciroot_" + name)
        source += r"""
int main(void) {
    root.machine = &machine; root.runtime_machine = &machine;
    slot.type0.powered = true;
    /* Partial CF8 accesses share the same owner token as the exit path. */
    for (unsigned width = 1; width <= 4; width *= 2) {
        for (unsigned offset = 0; offset + width <= 4; ++offset) {
            struct vmm_io_write w = {
                .address=0xcf8+offset, .value=0x12345678, .width=width
            };
            struct vmm_io_read r = { .address=w.address, .width=width };
            uint32_t mask = UINT32_MAX >> ((4-width)*8);
            root.config_address = 0xaabbccdd;
            uint32_t expected = (root.config_address & ~(mask << (offset*8))) |
                (((uint32_t)w.value & mask) << (offset*8));
            assert(vmmfs_pciroot_config_address_write(&machine, &root, &w) == 0);
            assert(root.config_address == expected);
            assert(vmmfs_pciroot_config_address_read(&machine, &root, &r) == 0);
            assert(r.value == ((expected >> (offset*8)) & mask));
            assert(root.token.held == 0);
        }
    }
    for (unsigned ecam = 0; ecam != 2; ++ecam) {
        struct vmm_io_read r = { .address=ecam ? VMMFS_PCI_ECAM_GPA+0x8040 : 0xcfc,
            .width=VMM_IO_WIDTH_32 };
        struct vmm_io_write w = { .address=r.address, .value=0x12345678,
            .width=VMM_IO_WIDTH_32 };
        root.config_address = 0x80000840;
        int (*read)(vmm_vcpu_t, void *, struct vmm_io_read *) = ecam ?
            vmmfs_pciroot_ecam_read : vmmfs_pciroot_config_data_read;
        int (*write)(vmm_vcpu_t, void *, const struct vmm_io_write *) = ecam ?
            vmmfs_pciroot_ecam_write : vmmfs_pciroot_config_data_write;
        assert(read(&machine, &root, &r) == 0 && r.value == 0x12345678);
        assert(write(&machine, &root, &w) == 0);
        assert(root.token.held == 0);
        unsigned before = writes;
        present = false;
        assert(write(&machine, &root, &w) == 0 && writes == before);
        present = true; slot.type0.powered = false;
        assert(write(&machine, &root, &w) == 0 && writes == before);
        slot.type0.powered = true;
        root.runtime_machine = NULL;
        assert(read(&machine, &root, &r) == 0 && r.value == UINT32_MAX);
        assert(write(&machine, &root, &w) == 0 && writes == before);
        assert(root.token.held == 0);
        root.runtime_machine = &machine;
    }
    assert(reads == 2 && writes == 2);
    struct vmm_io_read r = { .address=0xcf8, .width=VMM_IO_WIDTH_32 };
    struct vmm_io_write w = { .address=0xcf8, .width=VMM_IO_WIDTH_32 };
    root.runtime_machine = NULL;
    assert(vmmfs_pciroot_config_address_read(&machine, &root, &r) == ENOENT);
    assert(vmmfs_pciroot_config_address_write(&machine, &root, &w) == ENOENT);
    assert(root.token.held == 0 && machine.token.held == 0);
}
"""
        run_c(source)

if __name__ == "__main__":
    unittest.main(verbosity=2)
