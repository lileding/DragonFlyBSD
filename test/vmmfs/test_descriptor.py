#!/usr/bin/env python3
"""Binary descriptor framing, semantic validation and atomic transaction tests."""
import unittest
from test_regress import COMMON, ROOT, SOURCE, function, run_c

def parser_source():
    header = (SOURCE / "vmmfs_pcislot_descriptor.h").read_text()
    data = header[header.index("#define VMMFS_PCISLOT_DESCRIPTOR_MAX"):
                  header.index("struct vmmfs_pcislot_descriptor {")]
    result = COMMON + '#include <limits.h>\n#include <stdlib.h>\n#include <stdarg.h>\n'
    result += '#include "' + str(ROOT / "sys/sys/vmmfs.h") + '"\n'
    result += r"""
#define PAGE_SIZE 4096
#define bcmp memcmp
#define bcopy(s,d,n) memcpy((d),(s),(n))
#define bzero(p,n) memset((p),0,(n))
#define kvsnprintf vsnprintf
#define powerof2(x) ((((x)-1) & (x)) == 0)
struct vmmfs_pcislot { int unused; };
""" + data
    for name, kind in (("ranges_overlap", "bool"), ("validate", "int"),
                       ("print", "int"), ("render", "int"), ("parse", "int")):
        result += "\nstatic " + kind + "\n" + function(
            "vmmfs_pcislot_descriptor.c", "vmmfs_pcislot_descriptor_" + name) + "\n"
    return result

class DescriptorParser(unittest.TestCase):
    def test_binary_contract(self):
        run_c(parser_source() + r"""
static struct vmmfs_pcislot_descriptor_value value;
static int parse(const void *p, size_t size) {
    return vmmfs_pcislot_descriptor_parse(p, size, &value);
}
int main(void) {
    struct vmmfs_pci_descriptor d = {.version=1, .vendor_id=0x1234};
    unsigned char bytes[VMMFS_PCI_DESCRIPTOR_MAX + 1] = {0};
    assert(parse(&d,sizeof(d)) == 0);
    assert(strstr(value.text,"vendor_id=0x1234\n"));
    assert(value.length == strlen(value.text));
    for (size_t i=0; i<sizeof(d); ++i) assert(parse(&d,i)==EINVAL);
    memcpy(bytes,&d,sizeof(d));
    assert(parse(bytes,sizeof(d)+1)==EINVAL);
    assert(parse("version=1\n",10)==EINVAL);
    d.reserved=1; assert(parse(&d,sizeof(d))==EINVAL); d.reserved=0;
    d.version=2; assert(parse(&d,sizeof(d))==EINVAL); d.version=1;
    d.class_code=0x1000000; assert(parse(&d,sizeof(d))==EINVAL); d.class_code=0;
    d.intx_pin=5; assert(parse(&d,sizeof(d))==EINVAL); d.intx_pin=0;
    d.config_count=65; assert(parse(&d,sizeof(d))==EINVAL); d.config_count=0;
    d.bars[0].size=4096; d.bars[0].type=3;
    assert(parse(&d,sizeof(d))==0);
    d.bars[1]=d.bars[0]; assert(parse(&d,sizeof(d))==EINVAL);
    memset(&d.bars[1],0,sizeof(d.bars[1]));
    d.bars[0].reserved[0]=1; assert(parse(&d,sizeof(d))==EINVAL);
    d.bars[0].reserved[0]=0;
    d.bars[0].size=33; assert(parse(&d,sizeof(d))==EINVAL);
    d.bars[0].size=4096; d.bars[0].type=2;
    struct { struct vmmfs_pci_descriptor d; struct vmmfs_pci_register r; } r={.d=d};
    r.d.config_count=1; r.r.width=4; r.r.space=1;
    assert(parse(&r,sizeof(r))==0);
    r.r.width=0; assert(parse(&r,sizeof(r))==EINVAL); r.r.width=4;
    r.r.offset=4096; assert(parse(&r,sizeof(r))==EINVAL); r.r.offset=0;
    r.r.bar=6; assert(parse(&r,sizeof(r))==EINVAL); r.r.bar=0;
    r.r.space=2; assert(parse(&r,sizeof(r))==EINVAL); r.r.space=1;
    r.r.reserved[4]=1; assert(parse(&r,sizeof(r))==EINVAL);
    struct { struct vmmfs_pci_descriptor d; struct vmmfs_pci_doorbell b;
        struct vmmfs_pci_register r; } overlap={.d=d};
    overlap.d.doorbell_count=overlap.d.config_count=1;
    overlap.b.width=overlap.r.width=4; overlap.b.space=overlap.r.space=1;
    overlap.b.size=4;
    assert(parse(&overlap,sizeof(overlap))==EINVAL);
    overlap.r.offset=4; assert(parse(&overlap,sizeof(overlap))==0);
    struct { struct vmmfs_pci_descriptor d; struct vmmfs_pci_capability c; } c={.d=d};
    c.d.cap_count=1; c.c.kind=1;
    assert(parse(&c,sizeof(c))==0);
    c.c.vectors=1; assert(parse(&c,sizeof(c))==EINVAL);
    c.c.kind=2; c.c.address_width=64; assert(parse(&c,sizeof(c))==0);
    c.c.vectors=0; assert(parse(&c,sizeof(c))==EINVAL);
    c.c.vectors=3; assert(parse(&c,sizeof(c))==EINVAL);
    c.c.kind=3; c.c.address_width=0; c.c.vectors=2; c.c.pba_offset=64;
    assert(parse(&c,sizeof(c))==0);
    c.c.pba_offset=1; assert(parse(&c,sizeof(c))==EINVAL);
    c.c.pba_offset=0; assert(parse(&c,sizeof(c))==EINVAL);
    c.c.pba_offset=4096; assert(parse(&c,sizeof(c))==EINVAL);
    memset(&c.c,0,sizeof(c.c)); c.c.kind=4; c.c.id=9;
    c.c.data_length=4; c.d.data_size=4;
    memcpy(bytes,&c,sizeof(c));
    assert(parse(bytes,sizeof(c)+4)==0);
    c.c.data_offset=UINT32_MAX; memcpy(bytes,&c,sizeof(c));
    assert(parse(bytes,sizeof(c)+4)==EINVAL);
    c.c.data_offset=0; c.c.data_length=193; c.d.data_size=193;
    memcpy(bytes,&c,sizeof(c)); assert(parse(bytes,sizeof(c)+193)==E2BIG);
    /* An unaligned caller buffer is legal; the decoder copies each record. */
    memcpy(bytes+1,&d,sizeof(d)); assert(parse(bytes+1,sizeof(d))==0);
}
""")

    def test_maximum_packet_and_extended_limits(self):
        run_c(parser_source() + r"""
int main(void) {
    struct {
        struct vmmfs_pci_descriptor d;
        struct vmmfs_pci_doorbell b[16];
        struct vmmfs_pci_register r[64];
        struct vmmfs_pci_capability c[32];
        struct vmmfs_pci_ext_capability e[32];
        unsigned char data[4096];
    } packet = {.d={.version=1, .doorbell_count=16, .config_count=64,
                   .cap_count=32, .ecap_count=32, .data_size=4096}};
    struct vmmfs_pcislot_descriptor_value value;
    assert(sizeof(packet)==VMMFS_PCI_DESCRIPTOR_MAX);
    packet.d.bars[0].size=8192; packet.d.bars[0].type=2;
    for (unsigned i=0; i<16; ++i) {
        packet.b[i].offset=i*4; packet.b[i].size=4;
        packet.b[i].width=4; packet.b[i].space=1;
    }
    for (unsigned i=0; i<64; ++i) {
        packet.r[i].offset=64+i*4; packet.r[i].width=4; packet.r[i].space=1;
    }
    for (unsigned i=0; i<32; ++i) {
        packet.c[i].kind=4; packet.c[i].id=9; packet.c[i].data_length=1;
        packet.e[i].id=0xb; packet.e[i].version=1; packet.e[i].data_length=116;
    }
#define PARSE() vmmfs_pcislot_descriptor_parse((const char *)&packet,sizeof(packet),&value)
    assert(PARSE()==0);
    assert(strstr(value.text,"doorbell15.space=mmio\n"));
    assert(strstr(value.text,"config63.width=4\n"));
    assert(strstr(value.text,"cap31.data=00\n"));
    assert(strstr(value.text,"ecap31.data="));
    assert(value.length==strlen(value.text));
    packet.e[31].data_length=117; assert(PARSE()==E2BIG);
    packet.e[31].data_length=116;
    packet.e[31].data_offset=UINT32_MAX; assert(PARSE()==EINVAL);
    packet.e[31].data_offset=0;
    packet.e[31].reserved[4]=1; assert(PARSE()==EINVAL); packet.e[31].reserved[4]=0;
    packet.e[31].version=16; assert(PARSE()==EINVAL); packet.e[31].version=1;
    packet.d.doorbell_count=17; assert(PARSE()==EINVAL); packet.d.doorbell_count=16;
    packet.d.cap_count=33; assert(PARSE()==EINVAL); packet.d.cap_count=32;
    packet.d.ecap_count=33; assert(PARSE()==EINVAL); packet.d.ecap_count=32;
    packet.d.data_size=4097; assert(PARSE()==EINVAL);
#undef PARSE
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
static int vmmfs_pcislot_descriptor_parse(const char *text, size_t length, struct vmmfs_pcislot_descriptor_value *v) {
    assert(text && length==1);
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
