#!/usr/bin/env python3
# SPDX-License-Identifier: 0BSD
"""
This script runs Xwayland in rooted mode, automatically choosing a
display number, and runs a target command within it.
"""
assert __name__ == "__main__"
import sys, os, subprocess, socket, signal
assert sys.argv[1:]
os.makedirs("/tmp/.X11-unix/", exist_ok=True)
for d in range(2**10):
    l = "/tmp/.X{}-lock".format(d)
    s = "/tmp/.X11-unix/X{}".format(d)
    try:
        open(l, "x").write("{:>10}\n".format(os.getpid()))
    except FileExistsError:
        continue
    try:
        f = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        f.bind(s)
    except OSError:
        os.unlink(l)
        continue
    break
try:
    f.listen(1)
    r, w = os.pipe()
    x = ["Xwayland", ":{}".format(d), "-listenfd", str(f.fileno()), "-displayfd", str(w), "-noreset", "-core"]
    p = [f.fileno(), w]
    if "WAYLAND_SOCKET" in os.environ:
        p.append(int(os.environ["WAYLAND_SOCKET"]))
    w = subprocess.Popen(x, pass_fds=p)
    try:
        def g(*a):
            raise TimeoutError()
        signal.signal(signal.SIGALRM, g)
        signal.alarm(5)
        assert int(os.fdopen(r).readline()) == d
        signal.alarm(0)
        e = os.environ.copy()
        e.pop("WAYLAND_DISPLAY", None)
        e.pop("WAYLAND_SOCKET", None)
        e["DISPLAY"] = ":{}".format(d)
        subprocess.run(sys.argv[1:], env=e)
    finally:
        w.terminate()
        w.wait()
finally:
    os.unlink(l)
    os.unlink(s)
