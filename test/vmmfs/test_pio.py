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

if __name__ == "__main__":
    unittest.main(verbosity=2)
