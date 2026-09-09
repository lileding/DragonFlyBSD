"""Auth preparation and actual production descriptor transaction failure paths."""
import unittest
from test_regress import COMMON,function,run_c

class AuthCommit(unittest.TestCase):
    def test_reservation_commit_and_rollback(self):
        code=COMMON+r"""
#include <stdlib.h>
struct token { unsigned held; };
struct vmmfs_node { unsigned references, inode, size; };
struct vmmfs_pcislot;
struct vmmfs_pcislot_auth {
    struct vmmfs_pcislot *slot; ino_t slot_inode; uint64_t generation;
    volatile unsigned valid, references;
};
struct file { unsigned references; int f_type, f_flag; void *f_ops; struct vmmfs_pcislot_auth *f_data; };
struct vmmfs_pcislot_descriptor_value { unsigned marker; size_t length; };
struct vmmfs_pcislot_descriptor {
    struct vmmfs_node node; bool updating, committed; uint64_t generation;
    struct vmmfs_pcislot_descriptor_value value; struct vmmfs_pcislot_auth *auth;
};
struct vmmfs_pcislot { struct vmmfs_node node; struct token token;
    struct vmmfs_pcislot_descriptor descriptor; int config; };
struct vmmfs_machine { struct token token; void *machine; };
struct vmmfs_pciroot { int unused; };
struct filedesc { bool reserved; struct file *file; };
struct proc { struct filedesc *p_fd; };
static struct filedesc fdtable;
static struct proc process={&fdtable};
#define curproc (&process)
static struct vmmfs_pcislot slot;
static struct vmmfs_machine machine;
static struct vmmfs_pciroot pciroot;
static struct vmmfs_pcislot_auth old_auth;
static unsigned mode, allocations, files, publications, cancellations, notifications;
static int vmmfs_pcislot_auth_fileops;
#define M_VMMFS 0
#define M_WAITOK 0
#define M_ZERO 0
#define FREAD 1
#define DTYPE_DMABUF 1
#define atomic_add_int(p,n) (*(p)+=(n))
#define atomic_fetchadd_int(p,n) ((*(p)+=(n))-(n))
#define atomic_store_rel_int(p,n) (*(p)=(n))
#define vmmfs_node_hold(n) (++(n)->references)
#define vmmfs_node_put(n) (--(n)->references)
#define bzero(p,n) memset(p,0,n)
#define bcopy(s,d,n) memcpy(d,s,n)
static void *kmalloc(size_t n,int tag,int flags) {
    (void)tag; (void)flags; ++allocations; return calloc(1,n);
}
static void kfree(void *p,int tag) {
    (void)tag; assert(p); if (p!=&old_auth) { assert(allocations); --allocations; free(p); }
}
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static struct vmmfs_pcislot *vmmfs_pcislot_descriptor_slot(void *p) { (void)p; return &slot; }
static struct vmmfs_pciroot *vmmfs_pcislot_pciroot(void *p) { (void)p; return &pciroot; }
static struct vmmfs_machine *vmmfs_pciroot_machine(void *p) { (void)p; return &machine; }
static int vmmfs_pcislot_descriptor_parse(const char *text,size_t n,struct vmmfs_pcislot_descriptor_value *value) {
    assert(text && n==1); value->marker=2; value->length=1; return mode==1?EINVAL:0;
}
static int falloc(void *p,struct file **file,void *fd) {
    (void)p; (void)fd; if(mode==2) return ENFILE;
    *file=calloc(1,sizeof(**file)); (*file)->references=1; ++files; return 0;
}
static void vmmfs_pcislot_auth_put(struct vmmfs_pcislot_auth *);
static void fdrop(struct file *f) {
    assert(f && f->references);
    if (--f->references==0) { vmmfs_pcislot_auth_put(f->f_data); --files; free(f); }
}
static int fdalloc(struct proc *p,int low,int *fd) {
    assert(p==curproc && low==0 && !fdtable.reserved && !fdtable.file);
    if(mode==3) return EMFILE;
    fdtable.reserved=true; *fd=7;
    /* Boot wins after auth creation and fd reservation, before commit. */
    if(mode==4) machine.machine=(void *)1;
    return 0;
}
static void fsetfd(struct filedesc *table,struct file *file,int fd) {
    assert(table==&fdtable && fd==7 && table->reserved && !table->file);
    table->reserved=false;
    if(file) {
        /* A published file must already describe the committed generation. */
        assert(slot.descriptor.auth==file->f_data && slot.descriptor.generation==8);
        ++file->references; table->file=file; ++publications;
    } else ++cancellations;
}
static void wakeup(void *p) { assert(p==&slot.descriptor && !slot.descriptor.updating); }
static void vmmfs_pcislot_config_descriptor_changed(int *c,uint64_t generation,bool committed) {
    assert(c==&slot.config && generation==8 && committed==slot.descriptor.committed);
    ++notifications;
}
static void vmmfs_pciroot_invalidate_slot(void *p,void *s) { assert(p==&pciroot && s==&slot); ++notifications; }
"""
        for name,kind in [('hold','void'),('put','void'),('create','int'),('revoke','void')]:
            code+='\nstatic '+kind+'\n'+function('vmmfs_pcislot_auth.c','vmmfs_pcislot_auth_'+name)+'\n'
        code+='\nstatic int\n'+function('vmmfs_pcislot_descriptor.c','vmmfs_pcislot_descriptor_store')+r"""
int main(void) {
    for (mode=0; mode<6; ++mode) {
        memset(&slot,0,sizeof(slot)); memset(&machine,0,sizeof(machine));
        memset(&fdtable,0,sizeof(fdtable));
        old_auth=(struct vmmfs_pcislot_auth){.slot=&slot,.generation=7,.valid=1,.references=1};
        slot.node.references=2; slot.node.inode=123;
        slot.descriptor.auth=&old_auth; slot.descriptor.generation=7;
        slot.descriptor.committed=true; slot.descriptor.value.marker=1;
        allocations=files=publications=cancellations=notifications=0;
        int error=vmmfs_pcislot_descriptor_store(&slot.descriptor.node,"x",mode==5?0:1);
        if(mode>=1 && mode<=4) {
            int expected[]={0,EINVAL,ENFILE,EMFILE,EBUSY};
            assert(error==expected[mode]);
            assert(slot.descriptor.auth==&old_auth && old_auth.valid && old_auth.references==1);
            assert(slot.descriptor.generation==7 && slot.descriptor.value.marker==1);
            assert(!fdtable.reserved && !fdtable.file && !publications && !notifications);
            assert(cancellations==(mode==4));
            assert(slot.node.references==2 && !allocations && !files);
        } else {
            assert(error==0 && slot.descriptor.generation==8 && !old_auth.valid && notifications==2);
            if(mode==0) {
                assert(publications==1 && fdtable.file && files==1);
                assert(slot.node.references==2 && allocations==1);
                vmmfs_pcislot_auth_revoke(slot.descriptor.auth);
                fdrop(fdtable.file); fdtable.file=NULL;
            } else assert(!publications && !fdtable.file && !slot.descriptor.auth);
            assert(slot.node.references==1 && !allocations && !files);
        }
        assert(!slot.descriptor.updating && !slot.token.held && !machine.token.held);
    }
}
"""
        run_c(code)
