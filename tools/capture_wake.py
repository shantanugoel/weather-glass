#!/usr/bin/env python3
"""Capture /dev/ttyACM0 across device deep-sleep cycles.

The ESP32-S3's USB-Serial-JTAG peripheral is powered down in deep sleep, so
the tty disappears between wakes. Poll for the node and read it the moment it
comes back, appending each wake's log to OUT with a timestamp header.
"""
import os
import select
import sys
import termios
import time

DEV = os.environ.get("GLASS_TTY", "/dev/ttyACM0")


def raw_open(path):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
    iflag = 0
    oflag = 0
    lflag = 0
    cflag = termios.CREAD | termios.CLOCAL | termios.CS8
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag,
                                            ispeed, ospeed, cc])
    return fd


def main():
    out_path = sys.argv[1]
    run_sec = int(sys.argv[2]) if len(sys.argv) > 2 else 3600
    deadline = time.time() + run_sec
    out = open(out_path, "ab", buffering=0)
    out.write(("\n### watcher start %s\n" % time.strftime("%F %T")).encode())
    while time.time() < deadline:
        if not os.path.exists(DEV):
            time.sleep(0.05)
            continue
        try:
            fd = raw_open(DEV)
        except OSError as exc:
            out.write(("### open failed %s: %s\n" % (time.strftime("%F %T"), exc)).encode())
            time.sleep(0.2)
            continue
        out.write(("\n===== port up %s =====\n" % time.strftime("%F %T")).encode())
        while time.time() < deadline:
            try:
                ready, _, _ = select.select([fd], [], [], 0.5)
            except OSError:
                break
            if ready:
                try:
                    data = os.read(fd, 8192)
                except OSError as exc:
                    out.write(("### read failed: %s\n" % exc).encode())
                    break
                if not data:
                    break
                out.write(data)
            elif not os.path.exists(DEV):
                break
        out.write(("\n===== port down %s =====\n" % time.strftime("%F %T")).encode())
        try:
            os.close(fd)
        except OSError:
            pass
        time.sleep(0.1)
    out.write(("### watcher stop %s\n" % time.strftime("%F %T")).encode())


if __name__ == "__main__":
    main()
