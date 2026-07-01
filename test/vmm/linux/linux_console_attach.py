#!/usr/local/bin/python3
import os
import select
import sys
import termios
import tty


def restore_terminal(fd, old_attrs):
    if old_attrs is not None:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_attrs)


def main():
    if len(sys.argv) != 2:
        print("usage: linux_console_attach.py <console>", file=sys.stderr)
        return 2

    console = sys.argv[1]
    tty_fd = sys.stdin.fileno()
    out_fd = sys.stdout.fileno()
    old_attrs = None
    if os.isatty(tty_fd):
        old_attrs = termios.tcgetattr(tty_fd)
        tty.setraw(tty_fd)

    con_fd = os.open(console, os.O_RDWR | os.O_NONBLOCK)
    line_start = True
    pending_tilde = False

    try:
        while True:
            readable, _, _ = select.select([tty_fd, con_fd], [], [])
            if con_fd in readable:
                try:
                    data = os.read(con_fd, 4096)
                except BlockingIOError:
                    data = b""
                if data:
                    os.write(out_fd, data)

            if tty_fd in readable:
                data = os.read(tty_fd, 1024)
                if not data:
                    break
                out = bytearray()
                for ch in data:
                    if ch == 0x1d:  # Ctrl-]
                        os.write(out_fd, b"\r\n")
                        return 0
                    if line_start and not pending_tilde and ch == ord("~"):
                        pending_tilde = True
                        continue
                    if pending_tilde:
                        if ch == ord("."):
                            os.write(out_fd, b"\r\n")
                            return 0
                        out.append(ord("~"))
                        pending_tilde = False
                    out.append(ch)
                    line_start = ch in (10, 13)
                if out:
                    os.write(con_fd, out)
    finally:
        restore_terminal(tty_fd, old_attrs)
        os.close(con_fd)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
