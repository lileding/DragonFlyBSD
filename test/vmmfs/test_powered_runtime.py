#!/usr/bin/env python3
"""Privileged powered ABI check; pass an empty VMMFS mount, no guest runs."""

from pci_descriptor import descriptor
import errno, fcntl, os, pathlib, select, sys
from contextlib import closing
root = pathlib.Path(sys.argv[1]); machine = root/'test'
def store(p, text):
    fd = os.open(p, os.O_WRONLY)
    try: assert os.write(fd, (text.encode() if isinstance(text, str) else text)) == len(text)
    finally: os.close(fd)
def fds():
    result = set()
    for fd in range(256):
        try: fcntl.fcntl(fd, fcntl.F_GETFD); result.add(fd)
        except OSError: pass
    return result
machine.mkdir()
slot = machine/'pci/0000:00:01.0'
handles = []; queues = []; boot = None
try:
    store(machine/'mem', '67108864')
    store(machine/'vcpu', '1')
    slot.mkdir()
    assert set(os.listdir(slot)) == {'descriptor', 'powered'}
    description = (descriptor(bars=((4096, 2, 0),)))
    before = fds(); store(slot/'descriptor', description)
    auth = fds() - before; assert len(auth) == 1; handles += list(auth)
    assert 'bar0.type=mem32\nbar0.size=0x1000\n' in (slot/'descriptor').read_text()
    powered = os.open(slot/'powered', os.O_RDONLY); handles.append(powered)
    inode = os.fstat(powered).st_ino
    for unused in range(2):
        q = select.kqueue(); queues.append(q)
        q.control([select.kevent(powered, filter=select.KQ_FILTER_VNODE,
            flags=select.KQ_EV_ADD|select.KQ_EV_CLEAR,
            fflags=select.KQ_NOTE_WRITE|select.KQ_NOTE_REVOKE)], 0, 0)
        assert q.control(None, 1, 0) == []
    assert os.pread(powered, 2, 0) == b'0\n'
    assert os.pread(powered, 2, 2) == b''
    try: os.open(slot/'powered', os.O_WRONLY)
    except OSError: pass
    else: raise AssertionError('powered writable')
    for cycle in range(2):
        boot = os.open(machine/'boot', os.O_RDWR)
        assert os.pread(powered, 2, 0) == b'1\n'
        assert 'bar0' in os.listdir(slot) and 'dma' in os.listdir(slot)
        for q in queues:
            events = q.control(None, 1, 2)
            assert len(events) == 1 and events[0].fflags & select.KQ_NOTE_WRITE
            assert not events[0].flags & select.KQ_EV_EOF
            assert q.control(None, 1, 0) == []
        # Register after power-on: current value is still discoverable.
        with closing(select.kqueue()) as late:
            late.control([select.kevent(powered, filter=select.KQ_FILTER_VNODE,
                flags=select.KQ_EV_ADD|select.KQ_EV_CLEAR,
                fflags=select.KQ_NOTE_WRITE)], 0, 0)
            assert os.pread(powered, 2, 0) == b'1\n'
        os.close(boot); boot = None
        assert os.pread(powered, 2, 0) == b'0\n'
        assert os.fstat(powered).st_ino == inode
        assert 'bar0' not in os.listdir(slot)
        for q in queues:
            events = q.control(None, 1, 2)
            assert len(events) == 1 and events[0].fflags & select.KQ_NOTE_WRITE
            assert not events[0].flags & select.KQ_EV_EOF
        print('power cycle passed', cycle, flush=True)
    slot.rmdir()
    for q in queues:
        events = q.control(None, 1, 0)
        assert not events or events[0].flags & select.KQ_EV_EOF
    try: os.pread(powered, 2, 0)
    except OSError as error: assert error.errno == errno.EBADF
    else: raise AssertionError('removed slot still readable')
    print('powered revoke passed', flush=True)
finally:
    if boot is not None: os.close(boot)
    for q in queues: q.close()
    for fd in handles:
        try: os.close(fd)
        except OSError as e:
            if e.errno != errno.EBADF: raise
    if slot.exists(): slot.rmdir()
    machine.rmdir()
assert not os.listdir(root)
