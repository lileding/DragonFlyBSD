#!/usr/bin/env python3
"""Exercise both launch entries with a guest that powers off immediately."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

root=Path(sys.argv[1])
source=Path(__file__).resolve().parents[2]
program=r"""
#define main old_loader_main
#include "LOADER"
#undef main
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    struct stat st;
    struct vmm_cpustate state;
    int fd=3;
    if (argc==2) {
        fd=open(argv[1],O_RDWR);
        if (fd<0) err(1,"open boot");
    }
    if (fstat(fd,&st)) err(1,"stat");
    uint8_t *mem=mmap(NULL,st.st_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if (mem==MAP_FAILED) err(1,"map");
    build_guest(mem,st.st_size,0,0,0,0);
    /* mov dx,0x600; mov al,0x34; out dx,al; hlt */
    const uint8_t code[]={0x66,0xba,0,6,0xb0,0x34,0xee,0xf4};
    memcpy(mem+ENTRY_GPA,code,sizeof(code));
    if (getenv("VMMFS_TEST_HALT")) {
        const uint8_t halt[]={0xf4,0xeb,0xfd};
        memcpy(mem+ENTRY_GPA,halt,sizeof(halt));
    }
    build_cpu_state(&state);
    if (write(fd,&state,sizeof(state))!=sizeof(state)) err(1,"submit");
    if (munmap(mem,st.st_size)) err(1,"unmap");
    if (close(fd) && errno!=EBADF) err(1,"close revoked launch");
    return 0;
}
""".replace("LOADER",str(source/'test/vmm/vmmfs/vmmfs_halt_loader.c'))
def store(path,text):
    fd=os.open(path,os.O_WRONLY)
    try: assert os.write(fd,text.encode())==len(text)
    finally: os.close(fd)
with tempfile.TemporaryDirectory(prefix='vmmfs-launch-order-') as tmp:
    c=Path(tmp)/'loader.c'; exe=Path(tmp)/'loader'
    c.write_text(program)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-o',str(exe),str(c)],check=True)
    for mode,halt in [(mode,False) for mode in ['rm','boot']*3]+[('rm',True),('boot',True)]:
        machine=root/('order-'+mode)
        machine.mkdir()
        store(machine/'vcpu','4')
        store(machine/'mem',str(64*1024*1024))
        held=os.open(machine/'stopped',os.O_RDONLY)
        old=os.fstat(held).st_ino
        env=dict(os.environ)
        if halt: env['VMMFS_TEST_HALT']='1'
        event=os.open(machine/'events',os.O_RDONLY|os.O_NONBLOCK)
        try:
            if mode=='rm':
                store(machine/'loader',('export VMMFS_TEST_HALT=1; ' if halt else '')+'exec '+str(exe))
                subprocess.run(['rm',str(machine/'stopped')],check=True,timeout=30)
            else:
                subprocess.run([str(exe),str(machine/'boot')],check=True,timeout=30,env=env)
            records=bytearray();deadline=time.monotonic()+30
            stop_sent=False
            while b'machine stopped reason=vcpu' not in records:
                try: records.extend(os.read(event,65536))
                except BlockingIOError: pass
                if halt and not stop_sent and b'machine boot completed' in records:
                    assert not (machine/'stopped').exists()
                    assert 'stopped' not in os.listdir(machine)
                    assert os.fstat(held).st_ino==old and os.read(held,1)==b''
                    if mode=='rm':
                        subprocess.run(['touch',str(machine/'stopped')],check=True,timeout=10)
                    else:
                        os.utime(held,None)
                    stop_sent=True
                if time.monotonic()>deadline: raise RuntimeError(records.decode())
                time.sleep(0.01)
            assert records.index(b'machine boot completed') < records.index(b'machine stopped reason=vcpu')
            assert (machine/'stopped').stat().st_ino == old
            assert os.fstat(held).st_ino==old and os.read(held,1)==b''
            print('PASS',mode,'touch stop' if halt else 'guest poweroff','persistent stopped inode/fd',flush=True)
        finally:
            os.close(event)
            os.close(held)
        machine.rmdir()
