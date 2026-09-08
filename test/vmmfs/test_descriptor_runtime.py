#!/usr/bin/env python3
"""Privileged binary descriptor transaction regression; pass an empty mount."""
import errno
import fcntl
import os
from pathlib import Path
import struct
import sys
from pci_descriptor import descriptor

def fds():
    result = set()
    for fd in range(256):
        try: fcntl.fcntl(fd, fcntl.F_GETFD); result.add(fd)
        except OSError as error:
            if error.errno != errno.EBADF: raise
    return result

def store(path, data, offset=0):
    fd = os.open(path, os.O_WRONLY)
    try: return os.pwrite(fd, data, offset)
    finally: os.close(fd)

machine = Path(sys.argv[1])/'binary-descriptor'
machine.mkdir()
slot = machine/'pci/0000:00:01.0'
slot.mkdir()
path = slot/'descriptor'
owned = set()
try:
    packet = descriptor(bars=((16384, 3, 0),),
                        doorbells=((1024, 256, 0, 4, 1),),
                        configs=((0, 0, 4, 1),),
                        caps=((0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0),
                              (4096, 8192, 0, 0, 2, 3, 0, 0, 0, 0, 0),
                              (0, 0, 0, 4, 0, 4, 0, 0, 0, 0, 9)),
                        ecaps=((4, 4, 0xb, 1),), data=bytes(range(8)))
    before = fds()
    assert store(path,packet)==len(packet)
    owned |= fds()-before
    assert len(owned)==1
    text = path.read_bytes()
    assert b'cap2.data=00010203\n' in text and b'ecap0.data=04050607\n' in text
    assert b'config0.space=mmio\n' in text
    invalid = [packet[:n] for n in (1,135,len(packet)-1)]
    invalid += [packet+b'\0', text]
    for pos in (0,15,17,26,27,28,48,49,50):
        changed = bytearray(packet)
        changed[pos]=255
        invalid.append(changed)
    for bad in invalid:
        before = fds()
        try: store(path,bad)
        except OSError as error: assert error.errno==errno.EINVAL, error
        else: raise AssertionError('invalid descriptor committed')
        assert fds()==before and path.read_bytes()==text
        powered=os.open(slot/'powered',os.O_RDONLY)
        assert os.read(powered,2)==b'0\n'
        os.close(powered)
    try: store(path,packet,1)
    except OSError as error: assert error.errno==errno.EINVAL
    else: raise AssertionError('nonzero offset accepted')
    assert path.read_bytes()==text
    before = fds()
    assert store(path,descriptor())==136
    added=fds()-before; owned |= added; assert len(added)==1
    assert b'bar0.' not in path.read_bytes()
    assert store(path,b'')==0 and path.read_bytes()==b''
    try: os.open(slot/'powered',os.O_RDONLY)
    except OSError as error: assert error.errno==errno.EACCES
    else: raise AssertionError('empty descriptor retained auth')
    print('PASS binary full packet, canonical read, malformed rollback, auth rotation and empty removal')
finally:
    for fd in owned:
        try: os.close(fd)
        except OSError as error:
            if error.errno!=errno.EBADF: raise
    machine.rmdir()
