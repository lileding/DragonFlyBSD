#!/usr/bin/env python3
"""Privileged VMMFS control-plane regressions; never submits a runnable CPU."""
import array
import ctypes
from contextlib import closing
import errno
import fcntl
import mmap
import os
import pathlib
import resource
import select
import signal
import shlex
import socket
import subprocess
import sys
import threading
import tempfile
import time
import unittest

ROOT = pathlib.Path(sys.argv.pop(1)).resolve()


def store(path, text):
    fd = os.open(path, os.O_WRONLY)
    try:
        data = text.encode()
        if os.write(fd, data) != len(data):
            raise AssertionError("short configuration write")
    finally:
        os.close(fd)


class Runtime(unittest.TestCase):
    def test_config_blocked_read_signal_and_revoke(self):
        slot = self.machine / "pci/0000:00:01.0"
        slot.mkdir()
        def open_fds():
            result = set()
            for fd in range(256):
                try:
                    fcntl.fcntl(fd, fcntl.F_GETFD)
                    result.add(fd)
                except OSError as error:
                    if error.errno != errno.EBADF:
                        raise
            return result
        auth = config = None
        child = None
        try:
            before = open_fds()
            store(slot / "descriptor",
                  "version=1\nheader.type=endpoint\nvendor_id=0x1234\n"
                  "device_id=1\nsubsystem_vendor_id=0x1234\nsubsystem_device_id=1\n"
                  "class=0xff0000\nrevision=0\nintx.pin=none\n"
                  "bar0.type=io\nbar0.size=4\nbar0.prefetchable=0\n"
                  "config0.bar=0\nconfig0.offset=0\nconfig0.width=1\nconfig0.space=pio\n")
            added = open_fds() - before
            self.assertEqual(len(added), 1)
            auth = added.pop()
            config = os.open(slot / "config", os.O_RDWR | os.O_NONBLOCK)
            with self.assertRaises(BlockingIOError):
                os.read(config, 40)
            with closing(select.kqueue()) as queue:
                queue.control([select.kevent(config, filter=select.KQ_FILTER_READ,
                                             flags=select.KQ_EV_ADD)], 0, 0)
                self.assertEqual(queue.control(None, 1, 0), [])
                for action in ("signal", "revoke"):
                    child = subprocess.Popen(
                        [sys.executable, "-c", """
import ctypes, errno, fcntl, os, signal, sys
fd = int(sys.argv[1])
action = sys.argv[2]
signal.signal(signal.SIGUSR1, lambda signum, frame: None)
signal.siginterrupt(signal.SIGUSR1, True)
fcntl.fcntl(fd, fcntl.F_SETFL, 0)
libc = ctypes.CDLL(None, use_errno=True)
libc.read.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
libc.read.restype = ctypes.c_ssize_t
buffer = ctypes.create_string_buffer(40)
result = libc.read(fd, buffer, len(buffer))
error = ctypes.get_errno()
allowed = (errno.EINTR,) if action == 'signal' else (errno.ENXIO, errno.EBADF, errno.EIO, errno.ENOENT)
if result != -1 or error not in allowed:
    raise RuntimeError('config read result=%d errno=%d' % (result, error))
print(action, flush=True)
""", str(config), action], pass_fds=(config,), stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE)
                    deadline = time.monotonic() + 10
                    while True:
                        wait = subprocess.check_output(
                            ["ps", "-p", str(child.pid), "-o", "wchan="],
                            text=True).strip()
                        # ps displays only the first eight characters here.
                        if wait == "vmmfspci":
                            break
                        self.assertIsNone(child.poll())
                        self.assertLess(time.monotonic(), deadline,
                                        "config reader wait channel: %r" % wait)
                        time.sleep(0.02)
                    if action == "signal":
                        child.send_signal(signal.SIGUSR1)
                    else:
                        slot.rmdir()
                    output, error = child.communicate(timeout=10)
                    self.assertEqual(child.returncode, 0, error.decode())
                    self.assertEqual(output, (action + "\n").encode())
                    if action == "signal":
                        # Interrupted read does not release the responder or EOF its knote.
                        fcntl.fcntl(config, fcntl.F_SETFL, os.O_NONBLOCK)
                        with self.assertRaises(BlockingIOError):
                            os.read(config, 40)
                        self.assertEqual(queue.control(None, 1, 0), [])
            self.assertFalse(slot.exists())
        finally:
            if child is not None and child.poll() is None:
                # Deactivate wakes even a regression's uninterruptible reader.
                if slot.exists():
                    slot.rmdir()
                child.kill()
                child.communicate(timeout=10)
            for fd in (config, auth):
                if fd is not None:
                    os.close(fd)
            if slot.exists():
                slot.rmdir()

    def setUp(self):
        self.machine = ROOT / ("control-" + self._testMethodName)
        self.machine.mkdir()
        store(self.machine / "mem", "67108864")
        store(self.machine / "vcpu", "1")

    def tearDown(self):
        # A failed test must leave its machine for diagnosis, not hide the error.
        self.machine.rmdir()

    def boot(self):
        return os.open(self.machine / "boot", os.O_RDWR)

    def test_empty_configuration_write(self):
        for name in ("vcpu", "mem", "loader"):
            fd = os.open(self.machine / name, os.O_WRONLY)
            try:
                with self.assertRaises(OSError) as failure:
                    os.write(fd, b"")
                self.assertEqual(failure.exception.errno, errno.EINVAL)
            finally:
                os.close(fd)
        slot = self.machine / "pci/0000:00:01.0"
        slot.mkdir()
        fd = os.open(slot / "descriptor", os.O_WRONLY)
        try:
            self.assertEqual(os.write(fd, b""), 0)
            self.assertEqual(os.fstat(fd).st_size, 0)
        finally:
            os.close(fd)
        slot.rmdir()

    def test_launch_rights_in_transit(self):
        sender, receiver = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        received = -1
        try:
            fd = self.boot()
            try:
                sender.sendmsg([b"L"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                        array.array("i", [fd]))])
            finally:
                os.close(fd)
            with self.assertRaises(OSError) as failure:
                self.boot()
            self.assertEqual(failure.exception.errno, errno.EBUSY)
            data, controls, flags, address = receiver.recvmsg(
                1, socket.CMSG_SPACE(array.array("i").itemsize))
            self.assertEqual((data, flags), (b"L", 0))
            rights = array.array("i")
            for level, kind, payload in controls:
                self.assertEqual((level, kind), (socket.SOL_SOCKET, socket.SCM_RIGHTS))
                rights.frombytes(payload)
            self.assertEqual(len(rights), 1)
            received = rights[0]
            self.assertEqual(os.fstat(received).st_size, 67108864)
            os.close(received)
            received = -1
            self.assertTrue((self.machine / "stopped").exists())
            os.close(self.boot())
        finally:
            if received >= 0:
                os.close(received)
            sender.close()
            receiver.close()

    def test_loader_exit_without_submission(self):
        store(self.machine / "loader", "exec /usr/bin/true")
        result = subprocess.run(["/bin/rm", str(self.machine / "stopped")],
                                capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.machine / "stopped").exists())
        os.close(self.boot())

    def test_loader_inherits_only_stdio_and_launch(self):
        with tempfile.TemporaryFile() as extra:
            inherited = fcntl.fcntl(extra.fileno(), fcntl.F_DUPFD, 64)
            try:
                script = """
import errno, os
assert os.fstat(3).st_size == 67108864
try:
    os.fstat(%d)
except OSError as error:
    assert error.errno == errno.EBADF
else:
    raise AssertionError("loader inherited unrelated fd")
assert os.read(0, 128) == b"loader stdin\\n"
os.write(1, b"VMMFS_LOADER_STDOUT\\n")
os.write(2, b"VMMFS_LOADER_STDERR\\n")
""" % inherited
                store(self.machine / "loader",
                      "exec " + shlex.quote(sys.executable) + " -c " +
                      shlex.quote(script))
                result = subprocess.run(
                    ["/bin/rm", str(self.machine / "stopped")],
                    input=b"loader stdin\n", capture_output=True,
                    pass_fds=(inherited,), timeout=20)
                # Delivery succeeds; closing without state deliberately aborts.
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"VMMFS_LOADER_STDOUT", result.stdout)
                self.assertIn(b"VMMFS_LOADER_STDERR", result.stderr)
                self.assertNotIn(b"AssertionError", result.stderr)
                self.assertEqual(os.fstat(inherited).st_ino,
                                 os.fstat(extra.fileno()).st_ino)
                self.assertTrue((self.machine / "stopped").exists())
                os.close(self.boot())
            finally:
                os.close(inherited)

    def test_close_without_write(self):
        for _ in range(3):
            fd = self.boot()
            self.assertEqual(os.fstat(fd).st_size, 64 * 1024 * 1024)
            os.close(fd)
            self.assertTrue((self.machine / "stopped").exists())

    def test_bad_write_does_not_affect_next_launch(self):
        old = self.boot()
        try:
            with self.assertRaises(OSError) as failure:
                os.write(old, b"x")
            self.assertEqual(failure.exception.errno, errno.EINVAL)
            current = self.boot()
            try:
                os.close(old)
                old = -1
                self.assertEqual(os.fstat(current).st_size, 64 * 1024 * 1024)
            finally:
                os.close(current)
        finally:
            if old >= 0:
                os.close(old)

    def test_concurrent_open_has_one_owner(self):
        start = threading.Barrier(4)
        opened = threading.Barrier(4)
        results = []
        lock = threading.Lock()

        def worker():
            fd = -1
            start.wait()
            try:
                fd = self.boot()
                result = 0
            except OSError as failure:
                result = failure.errno
            with lock:
                results.append(result)
            opened.wait()
            if fd >= 0:
                os.close(fd)

        threads = [threading.Thread(target=worker) for _ in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=20)
            self.assertFalse(thread.is_alive(), "boot open did not finish")
        self.assertEqual(results.count(0), 1)
        self.assertEqual(sorted(results), [0, errno.EBUSY, errno.EBUSY, errno.EBUSY])

    def test_mmap_revoked_on_close(self):
        child = os.fork()
        if child == 0:
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            fd = self.boot()
            libc = ctypes.CDLL(None, use_errno=True)
            libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_int64]
            libc.mmap.restype = ctypes.c_void_p
            address = libc.mmap(None, 4096, mmap.PROT_READ | mmap.PROT_WRITE,
                                mmap.MAP_SHARED, fd, 0)
            if address == ctypes.c_void_p(-1).value:
                os._exit(101)
            memory = (ctypes.c_ubyte * 4096).from_address(address)
            memory[0] = 42
            os.close(fd)
            # This must fault in the child, never remain writable or kill host.
            memory[0] = 43
            os._exit(100)
        _, status = os.waitpid(child, 0)
        self.assertTrue(os.WIFSIGNALED(status), status)
        self.assertIn(os.WTERMSIG(status), (signal.SIGBUS, signal.SIGSEGV))


    def test_mmap_faults_during_last_close(self):
        # A forked mapper owns no file reference while the parent revokes.
        for iteration in range(8):
            fd = self.boot()
            receiver, sender = socket.socketpair()
            child = os.fork()
            if child == 0:
                receiver.close()
                resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
                signal.alarm(15)
                libc = ctypes.CDLL(None, use_errno=True)
                libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                     ctypes.c_int, ctypes.c_int,
                                     ctypes.c_int, ctypes.c_int64]
                libc.mmap.restype = ctypes.c_void_p
                size = 64 * 1024 * 1024
                address = libc.mmap(None, size, mmap.PROT_READ | mmap.PROT_WRITE,
                                    mmap.MAP_SHARED, fd, 0)
                if address == ctypes.c_void_p(-1).value:
                    os._exit(101)
                os.close(fd)
                memory = (ctypes.c_ubyte * size).from_address(address)
                memory[0] = 42
                sender.sendall(b"R")
                while True:
                    for offset in range(0, size, 4096):
                        memory[offset] = iteration
            sender.close()
            try:
                receiver.settimeout(10)
                self.assertEqual(receiver.recv(1), b"R")
            finally:
                receiver.close()
                os.close(fd)
                _, status = os.waitpid(child, 0)
            self.assertTrue(os.WIFSIGNALED(status), status)
            self.assertIn(os.WTERMSIG(status), (signal.SIGBUS, signal.SIGSEGV))
            self.assertTrue((self.machine / "stopped").exists())
        os.close(self.boot())

    def test_rm_signal_aborts(self):
        store(self.machine / "loader", "exec /bin/sleep 2")
        process = subprocess.Popen(["/bin/rm", str(self.machine / "stopped")],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.3)
        self.assertIsNone(process.poll(), "rm did not wait for launch")
        process.send_signal(signal.SIGINT)
        output, error = process.communicate(timeout=20)
        self.assertNotEqual(process.returncode, 0, (output, error))
        fd = self.boot()
        os.close(fd)

    def test_cancel_with_transferred_launch_still_open(self):
        for cancel in ("signal",) * 8:
            with self.subTest(cancel=cancel):
                with tempfile.TemporaryDirectory(prefix="vmmfs-launch-") as directory:
                    address = str(pathlib.Path(directory) / "control")
                    with closing(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)) as server:
                        server.bind(address)
                        server.listen(1)
                        server.settimeout(10)
                        script = """
import array, os, socket, sys
with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
    peer.connect(sys.argv[1])
    peer.sendmsg([b"L"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                          array.array("i", [3]))])
    os.close(3)
    peer.sendall(b"C")
"""
                        store(self.machine / "loader",
                              "exec " + shlex.quote(sys.executable) + " -c " +
                              shlex.quote(script) + " " + shlex.quote(address))
                        process = subprocess.Popen(
                            ["/bin/rm", str(self.machine / "stopped")],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                        old = current = -1
                        try:
                            peer, _ = server.accept()
                            with closing(peer):
                                peer.settimeout(10)
                                data, controls, flags, _ = peer.recvmsg(
                                    1, socket.CMSG_SPACE(array.array("i").itemsize))
                                rights = array.array("i")
                                for level, kind, payload in controls:
                                    self.assertEqual((level, kind),
                                                     (socket.SOL_SOCKET, socket.SCM_RIGHTS))
                                    rights.frombytes(payload)
                                self.assertEqual(len(rights), 1)
                                old = rights[0]
                                self.assertEqual(data, b"L")
                                self.assertEqual(flags & (socket.MSG_TRUNC |
                                                          socket.MSG_CTRUNC), 0)
                                # The loader has dropped fd 3, but this process
                                # still owns the transferred launch file.
                                self.assertEqual(peer.recv(1), b"C")
                            self.assertIsNone(process.poll(), "rm did not await submission")
                            self.assertEqual(os.fstat(old).st_size, 67108864)
                            with self.assertRaises(OSError) as failure:
                                self.boot()
                            self.assertEqual(failure.exception.errno, errno.EBUSY)
                            if cancel == "signal":
                                process.send_signal(signal.SIGINT)
                            else:
                                subprocess.run(
                                    ["touch", str(self.machine / "stopped")],
                                    check=True, timeout=10)
                            output, error = process.communicate(timeout=10)
                            self.assertNotEqual(process.returncode, 0, (output, error))
                            self.assertTrue((self.machine / "stopped").exists())
                            current = self.boot()
                            with self.assertRaises(OSError) as failure:
                                os.write(old, b"x")
                            self.assertIn(failure.exception.errno,
                                          (errno.EBADF, errno.EPIPE, errno.EINVAL))
                            os.close(old)
                            old = -1
                            self.assertEqual(os.fstat(current).st_size, 67108864)
                            with self.assertRaises(OSError) as failure:
                                self.boot()
                            self.assertEqual(failure.exception.errno, errno.EBUSY)
                        finally:
                            for fd in (old, current):
                                if fd >= 0:
                                    os.close(fd)
                            if process.poll() is None:
                                process.kill()
                                process.communicate(timeout=10)
                            process.stdout.close()
                            process.stderr.close()
                        self.assertTrue((self.machine / "stopped").exists())

    def test_dup_keeps_launch_until_last_close(self):
        fd = self.boot()
        duplicate = os.dup(fd)
        os.close(fd)
        try:
            with self.assertRaises(OSError) as failure:
                self.boot()
            self.assertEqual(failure.exception.errno, errno.EBUSY)
        finally:
            os.close(duplicate)
        os.close(self.boot())

    def test_touch_does_not_cancel_loading(self):
        fd = self.boot()
        try:
            subprocess.run(["touch", str(self.machine / "stopped")], check=True,
                           timeout=10)
            self.assertEqual(os.fstat(fd).st_size, 67108864)
            with self.assertRaises(OSError) as failure:
                self.boot()
            self.assertEqual(failure.exception.errno, errno.EBUSY)
        finally:
            os.close(fd)
        os.close(self.boot())

    def test_rmdir_veto_does_not_cancel_launch(self):
        fd = self.boot()
        try:
            with self.assertRaises(OSError) as failure:
                self.machine.rmdir()
            self.assertEqual(failure.exception.errno, errno.EBUSY)
            self.assertEqual(os.fstat(fd).st_size, 67108864)
        finally:
            os.close(fd)

    def test_control_metadata_is_not_silently_changed(self):
        path = self.machine / "vcpu"
        with self.assertRaises(OSError) as failure:
            os.chmod(path, 0o777)
        self.assertEqual(failure.exception.errno, errno.EOPNOTSUPP)
        with self.assertRaises(OSError) as failure:
            os.truncate(path, 7)
        self.assertEqual(failure.exception.errno, errno.EINVAL)
        fd = os.open(path, os.O_WRONLY | os.O_TRUNC)
        try:
            self.assertEqual(os.write(fd, b"2"), 1)
        finally:
            os.close(fd)
        self.assertEqual(path.read_text().strip(), "2")

    def test_serial_controlling_session_retains_tty(self):
        def tty_bytes():
            return len(subprocess.check_output(["sysctl", "-b", "kern.ttys"]))

        # Use a private mount so unmount can reclaim cached, inactive vnodes.
        with tempfile.TemporaryDirectory(prefix="vmmfs-tty-", dir="/var/tmp") as directory:
            mount = pathlib.Path(directory)
            subprocess.run(["mount", "-t", "vmmfs", "vmmfs", str(mount)], check=True)
            mounted = True
            machine = mount / "session"
            child = None
            try:
                machine.mkdir()
                before = tty_bytes()
                port = machine / "serial/com1"
                fd = os.open(port, os.O_CREAT | os.O_RDWR | os.O_NONBLOCK |
                             os.O_NOCTTY, 0o600)
                os.close(fd)
                registered = tty_bytes()
                self.assertGreater(registered, before)
                child = subprocess.Popen(
                    [sys.executable, "-c", """
import fcntl, os, signal, sys, termios
signal.signal(signal.SIGHUP, signal.SIG_IGN)
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
fcntl.ioctl(fd, termios.TIOCSCTTY, 0)
os.close(fd)
print("SESSION_RETAINS_TTY", flush=True)
sys.stdin.readline()
""", str(port)], start_new_session=True,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE)
                ready, _, _ = select.select([child.stdout], [], [], 10)
                self.assertTrue(ready, "control-terminal setup timed out")
                self.assertEqual(child.stdout.readline(), b"SESSION_RETAINS_TTY\n")
                port.unlink()
                self.assertIsNone(child.poll())
                self.assertEqual(tty_bytes(), registered)
                child.stdin.write(b"exit\n")
                child.stdin.flush()
                self.assertEqual(child.wait(timeout=10), 0)
                machine.rmdir()
                subprocess.run(["umount", str(mount)], check=True)
                mounted = False
                deadline = time.monotonic() + 10
                while tty_bytes() != before and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertEqual(tty_bytes(), before)
            finally:
                if child is not None:
                    if child.poll() is None:
                        child.kill()
                        child.wait(timeout=10)
                    child.stdin.close()
                    child.stdout.close()
                    child.stderr.close()
                if mounted:
                    if machine.exists():
                        machine.rmdir()
                    subprocess.run(["umount", str(mount)], check=True)

    def test_serial_blocked_read_is_revoked(self):
        port = self.machine / "serial/com1"
        fd = os.open(port, os.O_CREAT | os.O_RDWR | os.O_NONBLOCK |
                     os.O_NOCTTY, 0o600)
        child = None
        try:
            with self.assertRaises(BlockingIOError):
                os.read(fd, 1)
            with closing(select.kqueue()) as queue:
                queue.control([select.kevent(fd, filter=select.KQ_FILTER_READ,
                                             flags=select.KQ_EV_ADD)], 0, 0)
                child = subprocess.Popen(
                    [sys.executable, "-c", """
import errno, fcntl, os, sys
fd = int(sys.argv[1])
fcntl.fcntl(fd, fcntl.F_SETFL, 0)
try:
    data = os.read(fd, 1)
    raise RuntimeError("revoked read returned data or EOF: %r" % data)
except OSError as error:
    if error.errno not in (errno.ENXIO, errno.EBADF, errno.EIO, errno.ENOENT):
        raise
    print("REVOKED", flush=True)
""", str(fd)], pass_fds=(fd,), stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE)
                deadline = time.monotonic() + 10
                while True:
                    wait = subprocess.check_output(
                        ["ps", "-p", str(child.pid), "-o", "wchan="], text=True).strip()
                    if wait.startswith("ttyin"):
                        break
                    self.assertIsNone(child.poll())
                    self.assertLess(time.monotonic(), deadline,
                                    "reader did not enter native TTY sleep")
                    time.sleep(0.02)
                port.unlink()
                output, error = child.communicate(timeout=10)
                self.assertEqual(child.returncode, 0, error.decode())
                self.assertEqual(output, b"REVOKED\n")
        finally:
            if child is not None and child.poll() is None:
                child.kill()
                child.communicate(timeout=10)
            os.close(fd)


    def test_parent_child_removal_race(self):
        for iteration in range(30):
            machine = ROOT / ("removal-race-" + str(iteration))
            machine.mkdir()
            slot = machine / "pci/0000:00:01.0"
            slot.mkdir()
            port = machine / "serial/com1"
            fd = os.open(port, os.O_CREAT | os.O_RDWR | os.O_NONBLOCK, 0o600)
            os.close(fd)
            barrier = threading.Barrier(3)
            failures = []

            def remove(path, directory):
                try:
                    barrier.wait(timeout=5)
                    if directory:
                        path.rmdir()
                    else:
                        path.unlink()
                except OSError as error:
                    if error.errno not in (errno.EBUSY, errno.ENOENT):
                        failures.append(error)
                except BaseException as error:
                    failures.append(error)

            workers = [threading.Thread(target=remove, args=(path, directory))
                       for path, directory in ((machine, True), (slot, True),
                                               (port, False))]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join(timeout=10)
                self.assertFalse(worker.is_alive(), "namespace deletion blocked")
            self.assertEqual(failures, [])
            if machine.exists():
                machine.rmdir()
            self.assertFalse(machine.exists())


    def test_duplicate_collection_create_has_one_owner(self):
        for parent, name in ((ROOT, "duplicate-machine"),
                             (self.machine / "pci", "0000:00:01.0")):
            path = parent / name
            previous_inode = None
            for iteration in range(10):
                with self.subTest(parent=str(parent), iteration=iteration):
                    barrier = threading.Barrier(4)
                    outcomes = []

                    def create():
                        try:
                            barrier.wait(timeout=5)
                            path.mkdir()
                            outcomes.append(0)
                        except OSError as error:
                            outcomes.append(error.errno)
                        except BaseException as error:
                            outcomes.append(error)

                    workers = [threading.Thread(target=create) for _ in range(4)]
                    for worker in workers:
                        worker.start()
                    for worker in workers:
                        worker.join(timeout=10)
                        self.assertFalse(worker.is_alive(), "duplicate mkdir blocked")
                    self.assertEqual(outcomes.count(0), 1, outcomes)
                    self.assertEqual(outcomes.count(errno.EEXIST), 3, outcomes)
                    # Rejected private candidates must not damage the winner.
                    inode = path.stat().st_ino
                    self.assertNotEqual(inode, previous_inode)
                    self.assertTrue(list(path.iterdir()))
                    path.rmdir()
                    self.assertFalse(path.exists())
                    previous_inode = inode

    def test_create_during_parent_removal(self):
        for iteration in range(30):
            machine = ROOT / ("create-race-" + str(iteration))
            machine.mkdir()
            barrier = threading.Barrier(2)
            failures = []

            def create():
                try:
                    barrier.wait(timeout=5)
                    (machine / "pci/0000:00:01.0").mkdir()
                except OSError as error:
                    if error.errno not in (errno.EBUSY, errno.ENOENT):
                        failures.append(error)
                except BaseException as error:
                    failures.append(error)

            def remove():
                try:
                    barrier.wait(timeout=5)
                    machine.rmdir()
                except OSError as error:
                    if error.errno not in (errno.EBUSY, errno.ENOENT):
                        failures.append(error)
                except BaseException as error:
                    failures.append(error)

            workers = [threading.Thread(target=action) for action in (create, remove)]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join(timeout=10)
                self.assertFalse(worker.is_alive(), "create/deactivate handoff blocked")
            self.assertEqual(failures, [])
            if machine.exists():
                machine.rmdir()
            self.assertFalse(machine.exists())


    def test_prepare_during_machine_removal(self):
        for iteration in range(30):
            machine = ROOT / ("prepare-race-" + str(iteration))
            machine.mkdir()
            store(machine / "mem", "67108864")
            barrier = threading.Barrier(2)
            failures = []

            def launch():
                try:
                    barrier.wait(timeout=5)
                    fd = os.open(machine / "boot", os.O_RDWR)
                    os.close(fd)
                except OSError as error:
                    if error.errno not in (errno.EBUSY, errno.ENOENT,
                                           errno.ECANCELED, errno.EBADF):
                        failures.append(error)
                except BaseException as error:
                    failures.append(error)

            def remove():
                try:
                    barrier.wait(timeout=5)
                    machine.rmdir()
                except OSError as error:
                    if error.errno not in (errno.EBUSY, errno.ENOENT):
                        failures.append(error)
                except BaseException as error:
                    failures.append(error)

            workers = [threading.Thread(target=action) for action in (launch, remove)]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join(timeout=10)
                self.assertFalse(worker.is_alive(), "private PREPARE did not unwind")
            self.assertEqual(failures, [])
            if machine.exists():
                self.assertTrue((machine / "stopped").exists())
                machine.rmdir()

    def test_pci_function_names_cannot_alias_another_slot(self):
        pci = self.machine / "pci"
        slot = pci / "0000:00:01.0"
        slot.mkdir()
        try:
            for function in "89abcdef":
                alias = pci / ("0000:00:00." + function)
                with self.subTest(function=function):
                    with self.assertRaises(OSError) as failure:
                        alias.mkdir()
                    self.assertEqual(failure.exception.errno, errno.EINVAL)
                    with self.assertRaises(OSError) as failure:
                        os.stat(alias / "descriptor")
                    self.assertIn(failure.exception.errno, (errno.EINVAL, errno.ENOENT))
                    with self.assertRaises(OSError) as failure:
                        alias.rmdir()
                    self.assertIn(failure.exception.errno, (errno.EINVAL, errno.ENOENT))
                    self.assertTrue((slot / "descriptor").exists())
            self.assertEqual([path.name for path in pci.iterdir()], [slot.name])
        finally:
            slot.rmdir()

    def test_namespace_children(self):

        slot = self.machine / "pci" / "0000:00:01.0"
        slot.mkdir()
        self.assertTrue((slot / "descriptor").exists())
        slot.rmdir()
        port = self.machine / "serial" / "com1"
        fd = os.open(port, os.O_CREAT | os.O_WRONLY | os.O_NONBLOCK, 0o600)
        os.close(fd)
        port.unlink()



    def test_launch_revoke_covers_inherited_aliases(self):
        self._check_launch_aliases(("primary", "alias", "read", "split", "protected"))

    @unittest.skipUnless(os.environ.get("VMMFS_TEST_MLOCK") == "1",
                         "host MGTDEVICE wiring fault panics before pager callback; "
                         "enable VMMFS_TEST_MLOCK=1 only for kernel regression testing")
    def test_launch_revoke_covers_wired_aliases(self):
        self._check_launch_aliases(("wired",))

    def _check_launch_aliases(self, probes):
        libc = ctypes.CDLL(None, use_errno=True)
        libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, ctypes.c_int64]
        libc.mmap.restype = ctypes.c_void_p
        for name in ("munmap", "mlock"):
            getattr(libc, name).argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            getattr(libc, name).restype = ctypes.c_int
        libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
        libc.mprotect.restype = ctypes.c_int
        page = mmap.PAGESIZE
        size = 3 * page
        protections = mmap.PROT_READ | mmap.PROT_WRITE
        for abort in ("close", "bad-write"):
            for probe in probes:
                with self.subTest(abort=abort, probe=probe):
                    fd = self.boot()
                    print("LAUNCH_ALIAS abort=%s probe=%s" % (abort, probe), flush=True)
                    mappings = []
                    child = None
                    parent_socket, child_socket = socket.socketpair()
                    try:
                        # Raw mmap must not duplicate the fd: final close is under test.
                        for _ in range(2):
                            address = libc.mmap(None, size, protections, mmap.MAP_SHARED, fd, 0)
                            self.assertNotEqual(address, ctypes.c_void_p(-1).value,
                                                ctypes.get_errno())
                            mappings.append(address)
                        ctypes.c_ubyte.from_address(mappings[0] + page).value = 42
                        self.assertEqual(ctypes.c_ubyte.from_address(
                            mappings[1] + page).value, 42)
                        child = os.fork()
                        if child == 0:
                            parent_socket.close()
                            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
                            signal.alarm(15)
                            os.close(fd)
                            address = mappings[0 if probe == "primary" else 1] + page
                            if probe == "wired" and libc.mlock(address, page) != 0:
                                os._exit(101)
                            # Clip the map on both sides of the page being probed.
                            if probe == "split":
                                if libc.munmap(mappings[1], page) != 0:
                                    os._exit(102)
                                if libc.munmap(mappings[1] + 2 * page, page) != 0:
                                    os._exit(103)
                            if probe == "protected" and libc.mprotect(address, page, 0) != 0:
                                os._exit(104)
                            if probe == "read" and libc.mprotect(address, page, mmap.PROT_READ) != 0:
                                os._exit(105)
                            child_socket.sendall(b"R")
                            if child_socket.recv(1) != b"G":
                                os._exit(106)
                            # PROT_NONE cannot be the reason for the expected signal.
                            if probe == "protected" and libc.mprotect(address, page, protections) != 0:
                                os._exit(107)
                            value = ctypes.c_ubyte.from_address(address)
                            if probe == "read":
                                result = value.value
                                os._exit(100 if result == 42 else 108)
                            value.value = 43
                            os._exit(100)
                        child_socket.close()
                        parent_socket.settimeout(10)
                        self.assertEqual(parent_socket.recv(1), b"R")
                        if abort == "bad-write":
                            with self.assertRaises(OSError) as failure:
                                os.write(fd, b"x")
                            self.assertEqual(failure.exception.errno, errno.EINVAL)
                        os.close(fd)
                        fd = -1
                        self.assertTrue((self.machine / "stopped").exists())
                        parent_socket.sendall(b"G")
                        _, status = os.waitpid(child, 0)
                        child = None
                        self.assertTrue(os.WIFSIGNALED(status), status)
                        self.assertIn(os.WTERMSIG(status), (signal.SIGBUS, signal.SIGSEGV))
                        # Parent still retains both mappings from the old launch.
                        os.close(self.boot())
                    finally:
                        parent_socket.close()
                        child_socket.close()
                        if fd >= 0:
                            os.close(fd)
                        if child is not None:
                            os.waitpid(child, 0)  # Child has a bounded alarm.
                        for address in mappings:
                            self.assertEqual(libc.munmap(address, size), 0,
                                             ctypes.get_errno())


if __name__ == "__main__":
    unittest.main(verbosity=2)
