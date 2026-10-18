#!/usr/bin/env python3
"""Small PTY regression test for history navigation and mid-line editing."""
import errno
import os
import pty
import select
import sys
import tempfile
import time

binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./alt")
tmp = tempfile.mkdtemp(prefix="alt-shell-pty-")
work = os.path.join(tmp, "work")
config_home = os.path.join(tmp, "config")
data_home = os.path.join(tmp, "data")
os.makedirs(work)
os.makedirs(os.path.join(config_home, "alt-shell"))
with open(os.path.join(config_home, "alt-shell", "config"), "w", encoding="utf-8") as f:
    f.write("container.enabled=false\n")

pid, fd = pty.fork()
if pid == 0:
    os.chdir(work)
    os.environ.update({
        "XDG_CONFIG_HOME": config_home,
        "XDG_DATA_HOME": data_home,
        "TERM": "xterm",
    })
    os.execv(binary, [binary])

def read_for(seconds=0.45):
    out = bytearray()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0, deadline - time.monotonic()))
        if not ready:
            break
        try:
            chunk = os.read(fd, 65536)
        except OSError as exc:
            if exc.errno == errno.EIO:
                break
            raise
        if not chunk:
            break
        out.extend(chunk)
    return bytes(out)

try:
    read_for(0.25)
    os.write(fd, b"printf one\n")
    first = read_for()
    os.write(fd, b"\x1b[A\n")
    recalled = read_for()
    os.write(fd, b"printf abc\x1b[D\x7f\n")
    edited = read_for()
    os.write(fd, b"exit\n")
    read_for(0.15)

    assert b"\r\none" in first, "entered command did not print its output"
    assert b"\r\none" in recalled, "Up arrow did not recall the previous command"
    assert b"\r\nac" in edited, "Left arrow + Backspace did not edit 'abc' to 'ac'"
    print("ALT-Shell PTY line-editor tests passed.")
finally:
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
