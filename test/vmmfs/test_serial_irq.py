"""Deterministic serial IRQ ordering and runtime drain regressions."""
import subprocess
import tempfile
from pathlib import Path
import unittest
from test_regress import COMMON, function

class SerialIRQ(unittest.TestCase):
    def test_irq_and_stop(self):
        code = COMMON + r"""
#include <pthread.h>
#include <stdatomic.h>
#define kprintf(...) ((void)0)
struct runtime { atomic_bool alive; };
typedef struct runtime *vmm_machine_t;
typedef void *vmm_io_t;
struct vmmfs_serialport {
    pthread_mutex_t token;
    vmm_machine_t machine;
    vmm_io_t read_io, write_io;
    bool stopping, destroying, closed, irq_asserted, irq_busy, pending;
    unsigned gsi;
};
static struct vmmfs_serialport port;
static struct runtime runtime;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_cond_t drained = PTHREAD_COND_INITIALIZER;
static unsigned mode, calls, untraps, active, max_active, errors;
static bool entered, release_irq, stop_waiting;
static atomic_bool stopped;
static bool levels[8];
static void lwkt_gettoken(pthread_mutex_t *t) { assert(!pthread_mutex_lock(t)); }
static void lwkt_reltoken(pthread_mutex_t *t) { assert(!pthread_mutex_unlock(t)); }
static bool vmmfs_serialport_irq_pending_locked(const struct vmmfs_serialport *p) { return p->pending; }
static void vmmfs_serialport_irq_update(struct vmmfs_serialport *);
int tsleep(void *p, int flags, const char *name, int ticks) {
    (void)flags; (void)name; (void)ticks;
    assert(p==&port && port.stopping);
    pthread_mutex_lock(&gate); stop_waiting=true;
    pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    /* Model token release/reacquire across a scheduler sleep. */
    assert(!pthread_cond_wait(&drained,&port.token)); return 0;
}
void wakeup(void *p) { assert(p==&port); pthread_cond_broadcast(&drained); }
static int vmm_machine_untrap(vmm_machine_t m, vmm_io_t io) {
    assert(m==&runtime && atomic_load(&m->alive) && io); ++untraps; return 0;
}
static int vmm_machine_set_irq(vmm_machine_t m, unsigned gsi, bool level) {
    assert(m==&runtime && gsi==4 && atomic_load(&m->alive));
    ++active; if (active>max_active) max_active=active;
    assert(calls<8); levels[calls++]=level;
    if (mode==1 && level) {
        pthread_mutex_lock(&gate); entered=true; pthread_cond_broadcast(&changed);
        while (!release_irq) pthread_cond_wait(&changed,&gate);
        pthread_mutex_unlock(&gate);
        assert(atomic_load(&m->alive));
    }
    if (mode==2 && calls==1) {
        lwkt_gettoken(&port.token); port.pending=false; lwkt_reltoken(&port.token);
        vmmfs_serialport_irq_update(&port);
    }
    --active;
    if (mode==3) { ++errors; return EIO; }
    return 0;
}
static void
""" + function("vmmfs_serialport.c","vmmfs_serialport_irq_update") + "\nstatic int\n" + function("vmmfs_serialport.c","vmmfs_serialport_stop") + r"""
static void *irq_thread(void *p) { vmmfs_serialport_irq_update(p); return NULL; }
static void *stop_thread(void *p) {
    assert(vmmfs_serialport_stop(p)==0);
    atomic_store(&runtime.alive,false); atomic_store(&stopped,true);
    pthread_mutex_lock(&gate); pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    return NULL;
}
static void setup(unsigned test) {
    memset(&port,0,sizeof(port)); assert(!pthread_mutex_init(&port.token,NULL));
    mode=test; port.machine=&runtime; atomic_store(&runtime.alive,true);
    port.gsi=4; port.pending=true; port.read_io=(void *)1; port.write_io=(void *)2;
    calls=untraps=active=max_active=errors=0;
}
int main(void) {
    pthread_t irq, stop;
    setup(1);
    assert(!pthread_create(&irq,NULL,irq_thread,&port));
    pthread_mutex_lock(&gate);
    while (!entered) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
    assert(!pthread_create(&stop,NULL,stop_thread,&port));
    pthread_mutex_lock(&gate);
    while (!stop_waiting && !atomic_load(&stopped)) pthread_cond_wait(&changed,&gate);
    assert(!atomic_load(&stopped) && atomic_load(&runtime.alive));
    release_irq=true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    pthread_join(irq,NULL); pthread_join(stop,NULL);
    assert(calls==2 && levels[0] && !levels[1] && untraps==2);
    assert(!port.irq_busy && !port.stopping && !port.irq_asserted);
    pthread_mutex_destroy(&port.token);
    setup(2); vmmfs_serialport_irq_update(&port);
    assert(calls==2 && max_active==1 && levels[0] && !levels[1]);
    assert(!port.irq_busy && !port.irq_asserted);
    pthread_mutex_destroy(&port.token);
    setup(3); vmmfs_serialport_irq_update(&port);
    assert(calls==1 && errors==1 && !port.irq_busy && !port.irq_asserted);
    pthread_mutex_destroy(&port.token);
}
"""
        with tempfile.TemporaryDirectory(prefix="vmmfs-serial-irq-") as temporary:
            path=Path(temporary); (path/'test.c').write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',
                            str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True,timeout=10)
