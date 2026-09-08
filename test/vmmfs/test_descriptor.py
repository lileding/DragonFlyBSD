#!/usr/bin/env python3
"""Run the production descriptor parser, including malformed record framing."""
import unittest
from test_regress import COMMON, ROOT, SOURCE, function, run_c

def parser_source():
    header = (SOURCE / "vmmfs_pcislot_descriptor.h").read_text()
    data = header[header.index("#define VMMFS_PCISLOT_DESCRIPTOR_MAX"):
                  header.index("struct vmmfs_pcislot_descriptor {")]
    implementation = (SOURCE / "vmmfs_pcislot_descriptor.c").read_text()
    masks = implementation[implementation.index("#define VMMFS_DESCRIPTOR_VERSION"):
                           implementation.index("static int vmmfs_pcislot_descriptor_open")]
    source = COMMON + '#include <limits.h>\n#include <stdlib.h>\n'
    source += '#include "' + str(ROOT / "sys/sys/vmmfs.h") + '"\n'
    source += r"""
#define PAGE_SIZE 4096
/* Match kernel warning policy for unrelated existing range checks. */
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#define bcmp memcmp
#define bcopy(s,d,n) memcpy((d),(s),(n))
#define bzero(p,n) memset((p),0,(n))
#define powerof2(x) ((((x)-1) & (x)) == 0)
struct vmmfs_pcislot { int unused; };
"""
    source += data + masks
    for suffix, result in (
        ("parse_number", "int"), ("parse_boolean", "int"),
        ("parse_bytes", "int"), ("index_key", "int"), ("key", "bool"),
        ("ranges_overlap", "bool"), ("parse", "int"),
    ):
        source += "\nstatic " + result + "\n" + function(
            "vmmfs_pcislot_descriptor.c", "vmmfs_pcislot_descriptor_" + suffix) + "\n"
    return source

class DescriptorParser(unittest.TestCase):
    def test_missing_delimiters_and_malformed_records(self):
        run_c(parser_source() + r"""
int main(void) {
    struct vmmfs_pcislot slot;
    struct vmmfs_pcislot_descriptor_value value;
    const char *bad[] = {
        "bar0.type\n", "cap0.kind\n", "ecap0.id\n", "config0.bar\n",
        "doorbell0.bar\n", "version\n", "\n", "=1\n", "version=\n",
        "version==1\n", "version=1", "version=1\nbar0.type\n"
    };
    for (unsigned i=0; i<sizeof(bad)/sizeof(bad[0]); ++i)
        assert(vmmfs_pcislot_descriptor_parse(&slot, bad[i], strlen(bad[i]), &value) == EINVAL);
}
""")

    def test_minimal_valid_and_unknown_key(self):
        run_c(parser_source() + r"""
int main(void) {
    struct vmmfs_pcislot slot;
    struct vmmfs_pcislot_descriptor_value value;
    const char valid[] =
        "version=1\nvendor_id=0x1234\ndevice_id=0x5678\n"
        "subsystem_vendor_id=0x1234\nsubsystem_device_id=0x5678\n"
        "class=0x010000\nrevision=1\nheader.type=endpoint\nintx.pin=none\n";
    assert(vmmfs_pcislot_descriptor_parse(&slot, valid, sizeof(valid)-1, &value) == 0);
    assert(value.vendor_id==0x1234 && value.device_id==0x5678);
    assert(value.length==sizeof(valid)-1 && strcmp(value.text,valid)==0);
    const char unknown[] = "unknown=1\n";
    assert(vmmfs_pcislot_descriptor_parse(&slot, unknown, sizeof(unknown)-1, &value)==EINVAL);
}
""")

    def test_transaction_commits_or_cleans_up(self):
        run_c(COMMON + r"""
#include <stdlib.h>
struct token { unsigned held; };
struct vmmfs_node { struct token token; bool dead; unsigned size;  struct lock lock;};
struct vmmfs_pcislot_auth { bool valid; unsigned references; };
struct vmmfs_pcislot_descriptor_value { unsigned marker; size_t length; };
struct vmmfs_pcislot_descriptor {
    struct vmmfs_node node; bool updating, committed; uint64_t generation;
    struct vmmfs_pcislot_descriptor_value value; struct vmmfs_pcislot_auth *auth;
};
struct vmmfs_machine { struct vmmfs_node node; struct token token; void *machine;  };
struct vmmfs_pciroot { int unused; };
struct vmmfs_pcislot {
    struct vmmfs_node node; struct token token; struct vmmfs_pcislot_descriptor descriptor;
    int config;
};
static struct vmmfs_machine machine;
static struct vmmfs_pciroot pciroot;
static struct vmmfs_pcislot slot;
static struct vmmfs_pcislot_auth old_auth, new_auth;
static unsigned mode, allocations, revoked, notifications;
#define M_VMMFS 0
#define M_WAITOK 0
#define M_ZERO 0
static struct vmmfs_pcislot *vmmfs_pcislot_descriptor_slot(void *d) { (void)d; return &slot; }
static struct vmmfs_pciroot *vmmfs_pcislot_pciroot(void *s) { (void)s; return &pciroot; }
static struct vmmfs_machine *vmmfs_pciroot_machine(void *p) { (void)p; return &machine; }
#define bzero(p,n) memset((p),0,(n))
#define bcopy(s,d,n) memcpy((d),(s),(n))
static void *kmalloc(size_t n, int tag, int flags) {
    (void)tag; (void)flags; ++allocations; return calloc(1,n);
}
static void kfree(void *p, int tag) {
    (void)tag; assert(p && allocations); --allocations; free(p);
}
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static int vmmfs_pcislot_descriptor_parse(struct vmmfs_pcislot *s,
    const char *text, size_t length, struct vmmfs_pcislot_descriptor_value *v) {
    assert(s==&slot && text && length==1);
    v->marker=2; v->length=length; return mode==4 ? EINVAL : 0;
}
static int vmmfs_pcislot_auth_create(struct vmmfs_pcislot *s,
    uint64_t generation, struct vmmfs_pcislot_auth **p) {
    assert(s==&slot && generation==8 && slot.descriptor.updating);
    if (mode==5) return EMFILE;
    if (mode==6) slot.node.dead=true; /* Parent close must drain admitted child work. */
    new_auth.valid=true; new_auth.references=1; *p=&new_auth; return 0;
}
static void vmmfs_pcislot_auth_revoke(struct vmmfs_pcislot_auth *auth) {
    if (auth==NULL) return;
    assert(auth->references==1); auth->references=0; auth->valid=false;
    if (auth==&old_auth) {
        ++revoked;
        if (mode==3) slot.node.dead=true; /* Concurrent close drains this update. */
    }
}
void wakeup(void *p) {
    assert(p==&slot.descriptor && !slot.descriptor.updating);

}
static void vmmfs_pcislot_config_descriptor_changed(int *config,
    uint64_t generation, bool committed) {
    assert(config==&slot.config && generation==8);
    assert(committed==slot.descriptor.committed); ++notifications;
    assert(slot.descriptor.updating);
}
static void vmmfs_pciroot_invalidate_slot(struct vmmfs_pciroot *r,
    struct vmmfs_pcislot *s) {
    assert(r==&pciroot && s==&slot); ++notifications;
}
static int
""" + function("vmmfs_pcislot_descriptor.c", "vmmfs_pcislot_descriptor_store") + r"""
int main(void) {
    const unsigned modes[] = {0, 3, 4, 5, 6, 7};
    for (unsigned i=0; i<sizeof(modes)/sizeof(modes[0]); ++i) {
        mode=modes[i];
        memset(&machine,0,sizeof(machine)); memset(&slot,0,sizeof(slot));
        old_auth=(struct vmmfs_pcislot_auth){true,1};
        new_auth=(struct vmmfs_pcislot_auth){false,0};
        slot.descriptor.auth=&old_auth; slot.descriptor.committed=true;
        slot.descriptor.generation=7; slot.descriptor.value.marker=1;
        allocations=revoked=notifications=0;
        int error=vmmfs_pcislot_descriptor_store(&slot.descriptor.node,"x",mode==7 ? 0 : 1);
        if (mode==4 || mode==5) {
            assert(error==(mode==4 ? EINVAL : EMFILE));
            assert(slot.descriptor.auth==&old_auth && old_auth.valid && old_auth.references==1);
            assert(slot.descriptor.generation==7 && slot.descriptor.value.marker==1);
            assert(revoked==0 && notifications==0 && !new_auth.valid);
        } else {
            assert(error==0 && slot.descriptor.generation==8 && revoked==1);
            assert(!old_auth.valid && notifications==2);
            assert(machine.machine==NULL);
            if (mode==7) assert(slot.descriptor.auth==NULL && !slot.descriptor.committed);
            else assert(slot.descriptor.auth==&new_auth && new_auth.valid &&
                slot.descriptor.value.marker==2 && slot.descriptor.committed);
        }
        assert(allocations==0 && !slot.descriptor.updating);
        assert(!slot.token.held && !machine.token.held);
    }
}
""")

if __name__ == "__main__":
    unittest.main(verbosity=2)
