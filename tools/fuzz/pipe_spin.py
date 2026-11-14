#!/usr/bin/env python3
"""Ctrl-P a command that closes stderr and keeps running; report cx's CPU use
while it waits and whether the kill prompt appears. usage: pipe_spin.py CX"""
import os, sys, time, select, fcntl, termios, struct, tempfile, shutil
cx = os.path.abspath(sys.argv[1])
cmd = sys.argv[2] if len(sys.argv) > 2 else "exec sleep 6 2>/dev/null"
d = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
open(os.path.join(d, "f.txt"), "w").write("b\na\n")
pid, fd = os.forkpty()
if pid == 0:
    os.chdir(d)
    os.execve(cx, [cx, "f.txt"], {"PATH": "/usr/bin:/bin", "HOME": d, "TERM": "xterm-256color"})
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
out = bytearray()
def drain(t):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], max(0, end - time.time()))
        if r:
            try: out.extend(os.read(fd, 65536))
            except OSError: return
def cpu():
    p = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return (int(p[11]) + int(p[12])) / os.sysconf("SC_CLK_TCK")
drain(0.3)
os.write(fd, b"\x00\x1b[B\x1b[B\x10" + cmd.encode() + b"\r")   # mark, 2 x down, Ctrl-P, command
drain(0.5)
c0 = cpu(); out.clear(); drain(float(os.environ.get("WAIT", "4.0"))); c1 = cpu()
print(f"cmd: {cmd!r}: cx used {c1 - c0:.2f}s CPU while waiting; kill prompt shown: {b'Kill?' in out}")
os.write(fd, b"y"); out.clear(); drain(1.5); os.write(fd, b"\x1b[A"); drain(0.3); print("after y: alive and redrawing:", len(out) > 0, "status:", [w for w in (b"killed", b"Killed", b"exit") if w in out])
try: os.kill(pid, 9)
except OSError: pass
shutil.rmtree(d, ignore_errors=True)
