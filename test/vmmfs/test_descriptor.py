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

if __name__ == "__main__":
    unittest.main(verbosity=2)
