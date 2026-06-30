#!/usr/bin/env python3
import gzip
import os
import stat
import sys


DEVICES = {
    "dev/console": (0o20600, 5, 1),
    "dev/ttyS0": (0o20600, 4, 64),
    "dev/null": (0o20666, 1, 3),
    "dev/kmsg": (0o20600, 1, 11),
    "dev/zero": (0o20666, 1, 5),
    "dev/random": (0o20666, 1, 8),
    "dev/urandom": (0o20666, 1, 9),
}


def align4(out):
    pad = (-out.tell()) & 3
    if pad:
        out.write(b"\0" * pad)


def write_entry(out, name, mode, uid, gid, nlink, mtime, data, rdev, ino):
    names = name.encode() + b"\0"
    fields = [
        b"070701",
        f"{ino:08x}".encode(),
        f"{mode:08x}".encode(),
        f"{uid:08x}".encode(),
        f"{gid:08x}".encode(),
        f"{nlink:08x}".encode(),
        f"{int(mtime):08x}".encode(),
        f"{len(data):08x}".encode(),
        b"00000000",
        b"00000000",
        f"{rdev[0]:08x}".encode(),
        f"{rdev[1]:08x}".encode(),
        f"{len(names):08x}".encode(),
        b"00000000",
    ]
    out.write(b"".join(fields))
    out.write(names)
    align4(out)
    out.write(data)
    align4(out)


def collect(root):
    entries = []

    def visit(rel):
        path = os.path.join(root, rel)
        entries.append(rel)
        if rel != "." and not stat.S_ISDIR(os.lstat(path).st_mode):
            return
        for name in sorted(os.listdir(path)):
            child = os.path.join(rel, name) if rel != "." else name
            child_path = os.path.join(root, child)
            child_mode = os.lstat(child_path).st_mode
            if stat.S_ISDIR(child_mode) and not stat.S_ISLNK(child_mode):
                visit(child)
            else:
                entries.append(child)

    visit(".")
    return entries


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} root output.gz")

    root = sys.argv[1]
    output = sys.argv[2]
    ino = 1

    with gzip.open(output, "wb", compresslevel=9) as out:
        for name in collect(root):
            path = os.path.join(root, name)
            st = os.lstat(path)
            mode = stat.S_IMODE(st.st_mode)
            data = b""
            rdev = (0, 0)

            if name in DEVICES:
                mode, rmaj, rmin = DEVICES[name]
                rdev = (rmaj, rmin)
            elif stat.S_ISDIR(st.st_mode):
                mode |= stat.S_IFDIR
            elif stat.S_ISLNK(st.st_mode):
                mode |= stat.S_IFLNK
                data = os.readlink(path).encode()
            elif stat.S_ISREG(st.st_mode):
                mode |= stat.S_IFREG
                with open(path, "rb") as f:
                    data = f.read()
            else:
                continue

            write_entry(out, name, mode, st.st_uid, st.st_gid, st.st_nlink,
                        st.st_mtime, data, rdev, ino)
            ino += 1

        write_entry(out, "TRAILER!!!", 0, 0, 0, 1, 0, b"", (0, 0), ino)


if __name__ == "__main__":
    main()
