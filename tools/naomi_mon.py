#!/usr/bin/env python3
"""Drive NaomiDIAG's serial monitor from the PC.

    naomi_mon.py [-p /dev/ttyUSB0] [-l log] "r l a05f7034 2" "u f" "x" ...

Opens the monitor with '!' (from the menu or the idle screen), sends each
command, prints what comes back up to the next "mon> " prompt, then 'q'.
Everything received is also appended to the log file."""
import argparse, serial, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
ap.add_argument("-b", "--baud", type=int, default=57600)
ap.add_argument("-l", "--log", default="naomi_mon.log")
ap.add_argument("-t", "--timeout", type=float, default=20.0)
ap.add_argument("cmds", nargs="+")
a = ap.parse_args()

s = serial.Serial(a.port, a.baud, timeout=0.1)
log = open(a.log, "ab")

def until(token, limit):
    buf, t = b"", time.time()
    while time.time() - t < limit:
        d = s.read(4096)
        if d:
            buf += d
            log.write(d); log.flush()
            if token in buf:
                break
    return buf.decode("latin-1", "replace").replace("\r", "")

s.reset_input_buffer()
s.write(b"!")
out = until(b"mon> ", a.timeout)
if "mon> " not in out:
    sys.exit("no monitor prompt (menu or idle screen showing?):\n" + out)
for c in a.cmds:
    s.write(c.encode() + b"\r")
    out = until(b"mon> ", a.timeout)
    print(out.rsplit("mon> ", 1)[0].strip())
s.write(b"q\r")
until(b"\n", 1.0)
