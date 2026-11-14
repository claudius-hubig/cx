#!/usr/bin/env python3
"""Copy N repetitions of CH from a file, open a prompt with KEY and paste; report
how long cx stays busy. usage: paste_prompt.py CX N CH KEY   (e.g. 50000 x '\x06')"""
import os, sys, time, select, fcntl, termios, struct, tempfile
cx=os.path.abspath(sys.argv[1]); n=int(sys.argv[2]); ch=sys.argv[3].encode().decode('unicode_escape').encode('latin-1'); key=sys.argv[4].encode().decode('unicode_escape').encode('latin-1')
d=tempfile.mkdtemp(dir=os.environ.get("TMPDIR")); open(os.path.join(d,"f.txt"),"wb").write(ch*n)
pid,fd=os.forkpty()
if pid==0:
    os.chdir(d); os.execve(cx,[cx,"f.txt"],{"PATH":"/usr/bin:/bin","HOME":d,"TERM":"xterm-256color"})
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH",24,120,0,0))
def cpu():
    p=open(f"/proc/{pid}/stat").read().rsplit(")",1)[1].split(); return (int(p[11])+int(p[12]))/os.sysconf("SC_CLK_TCK")
def drain(t):
    end=time.time()+t; n=0
    while time.time()<end:
        r,_,_=select.select([fd],[],[],max(0,end-time.time()))
        if r:
            try: n+=len(os.read(fd,65536))
            except OSError: break
    return n
drain(0.3); os.write(fd,b"\x00\x1b[F\x1bw"); drain(0.5); c0=cpu(); t0=time.time()
os.write(fd,key+b"\x19")
last=cpu(); idle=0
while time.time()-t0<40 and idle<3:
    out=drain(0.5); c=cpu(); idle = idle+1 if c-last<0.05 else 0; last=c
print(f"{n} x {ch!r}, key {key!r} + paste: cx busy for {cpu()-c0:.1f}s CPU ({'still spinning after 40s' if idle<3 else 'then idle'})")
os.kill(pid,9)
