#!/usr/bin/env python3
"""Run synthetic ARM26 guests; no RISCiX installation or assembler required."""

import os
from pathlib import Path
import struct
import signal
import subprocess
import sys
import tempfile


BASE = 0x8004                  # One word of shared-library text precedes us.
MEM_SIZE = 32 * 1024 * 1024


def image(path, text, magic=0x50B, entry=BASE, library=b"lib", bss=0):
    header = struct.pack("<8I", magic, len(text), 0, bss, 0, entry, 0, 0)
    header += bytes(68) + library.ljust(60, b"\0")
    path.write_bytes(header.ljust(0x8000, b"\0") + text)
    path.chmod(0o755)


class Guest:
    def __init__(self):
        self.code = []
        self.labels = {}
        self.literals = []
        self.branches = []
        self.data = []

    def emit(self, word):
        self.code.append(word)

    def label(self, name):
        self.labels[name] = BASE + len(self.code) * 4

    def blob(self, name, value):
        self.data.append((name, value))

    def string(self, name, value):
        self.blob(name, os.fsencode(value) + b"\0")

    def vector(self, name, *values):
        self.blob(name, values + (0,))

    def const(self, reg, value):
        self.literals.append((len(self.code), reg, value))
        self.emit(0)

    def mov(self, dest, source):
        self.emit(0xE1A00000 | dest << 12 | source)

    def load(self, reg, base, offset=0):
        self.emit(0xE5900000 | base << 16 | reg << 12 | offset)

    def store(self, reg, base):
        self.emit(0xE5800000 | base << 16 | reg << 12)

    def branch(self, label, condition=14):
        self.branches.append((len(self.code), condition, label))
        self.emit(0)

    def syscall(self, number):
        self.emit(0xEF100000 | number)

    def equal(self, reg, value):
        self.const(12, value)
        self.emit(0xE150000C | reg << 16)  # cmp reg, r12
        self.branch("fail", 1)           # bne

    def success(self):
        self.branch("fail", 2)           # bcs

    def error(self, number):
        self.branch("fail", 3)           # bcc
        self.equal(0, number)

    def exit(self, status):
        self.const(0, status)
        self.syscall(1)

    def exec(self, filename="path", argv="args", envp="env"):
        self.const(0, filename)
        self.const(1, argv)
        self.const(2, envp)
        self.syscall(59)

    def build(self):
        self.label("fail")
        self.exit(99)
        code = self.code.copy()
        end = BASE + 4 * (len(code) + len(self.literals))
        for name, value in self.data:
            self.labels[name] = end
            end += len(value) * 4 if isinstance(value, tuple) else len(value)
            end = (end + 3) & ~3

        def resolve(value):
            return self.labels[value] if isinstance(value, str) else value & 0xFFFFFFFF

        for index, condition, label in self.branches:
            delta = (self.labels[label] - (BASE + index * 4 + 8)) // 4
            code[index] = condition << 28 | 0x0A000000 | delta & 0xFFFFFF
        for i, (index, reg, value) in enumerate(self.literals):
            delta = (len(self.code) + i - index) * 4 - 8
            assert 0 <= delta <= 4095
            code[index] = 0xE59F0000 | reg << 12 | delta
            code.append(resolve(value))
        result = struct.pack(f"<{len(code)}I", *code)
        for _, value in self.data:
            if isinstance(value, tuple):
                value = struct.pack(f"<{len(value)}I", *(resolve(v) for v in value))
            result += value
            result += bytes(-len(result) % 4)
        return result


def fork_wait(number, wait_number):
    g = Guest()
    g.const(7, 42)
    g.const(5, 0x100000)
    g.store(7, 5)
    g.syscall(number)
    g.success()
    g.emit(0xE3500000)            # cmp r0, #0
    g.branch("child", 0)
    g.equal(1, 0)                 # fork's parent flag must not be stale.
    g.mov(4, 0)
    g.syscall(20)
    g.equal(0, 1)
    g.equal(1, 0)
    g.equal(7, 42)
    if wait_number == 84:
        g.const(0, 0)
        g.const(1, 0)
    else:
        g.mov(0, 4)
        g.const(1, "status")
        g.const(2, 0)
        g.const(3, 0)
    g.syscall(wait_number)
    g.success()
    g.emit(0xE1500004)            # cmp r0, r4
    g.branch("fail", 1)
    g.equal(1, 23 << 8)
    g.equal(7, 42)
    g.const(5, 0x100000)
    g.load(6, 5)
    g.equal(6, 42)                # Child memory changes stayed private.
    if wait_number != 84:
        g.const(5, "status")
        g.load(6, 5)
        g.equal(6, 23 << 8)
    g.mov(0, 4)
    g.const(1, 0)
    g.const(2, 0)
    g.syscall(11)
    g.error(10)                  # Already reaped: ECHILD.
    g.exit(0)
    g.label("child")
    g.equal(1, 1)
    g.const(7, 19)
    g.const(5, 0x100000)
    g.store(7, 5)
    g.syscall(20)
    g.equal(0, 2)
    g.equal(1, 1)
    g.exit(23)
    g.blob("status", bytes(4))
    return g


def exec_failure(path, error, argv="args", envp="env"):
    g = Guest()
    g.const(7, 42)
    g.exec(argv=argv, envp=envp)
    g.error(error)
    g.equal(7, 42)
    g.const(5, "sentinel")
    g.load(6, 5)
    g.equal(6, 0x12345678)
    g.exit(0)
    g.string("path", path)
    g.vector("args", "arg0")
    g.string("arg0", "different argv[0]")
    g.vector("env")
    g.blob("sentinel", struct.pack("<I", 0x12345678))
    return g


def main():
    executable = str(Path(sys.argv[1] if len(sys.argv) > 1 else "./rixrun").resolve())
    with tempfile.TemporaryDirectory(prefix="rixrun-process-") as directory:
        root = Path(directory)
        image(root / "lib", bytes(4), magic=0x90B, entry=0x1800000, library=b"")
        env = {**os.environ, "RIX_ROOT": directory, "RIX_VERBOSE": "0"}
        tests = 0

        def run(name, guest, expected=b"", pass_fds=()):
            nonlocal tests
            image(root / "guest", guest.build())
            p = subprocess.Popen([executable, "./guest"], cwd=root, env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 start_new_session=True, pass_fds=pass_fds)
            try:
                stdout, stderr = p.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                stdout, stderr = p.communicate()
                raise AssertionError((name, "timed out", stdout, stderr))
            assert p.returncode == 0 and stdout == expected, (
                name, p.returncode, stdout, stderr.decode(errors="replace"))
            tests += 1
            print(f"ok: {name}")

        for number in (2, 66):
            for wait_number in (7, 11, 84):
                run(f"fork syscall {number}, wait syscall {wait_number}",
                    fork_wait(number, wait_number))

        # Exec changes registers and memory, preserves argv[0]/envp, and never
        # executes either of the caller's prefetched following instructions.
        target = Guest()
        target.equal(7, 0)
        target.load(4, 13)
        target.equal(4, 1)
        target.load(4, 13, 4)
        target.const(0, 1)
        target.mov(1, 4)
        target.const(2, 5)
        target.syscall(4)
        target.load(4, 13, 12)
        target.const(0, 1)
        target.mov(1, 4)
        target.const(2, 8)
        target.syscall(4)
        target.const(4, 0x100000)
        target.load(5, 4)
        target.equal(5, 0)
        target.exit(0)
        image(root / "target", target.build())

        g = Guest()
        g.const(7, 42)
        g.const(4, 0x100000)
        g.store(7, 4)
        g.exec()
        g.exit(98)
        g.string("path", "/target")
        g.string("arg0", "alias")
        g.string("env0", "CUSTOM=1")
        g.vector("args", "arg0")
        g.vector("env", "env0")
        run("exec replaces image, registers, argv and environment", g, b"aliasCUSTOM=1")

        # Two consecutive execs must restart the loader's text cursor.
        image(root / "chain", g.build())
        h = Guest()
        h.exec(argv=0, envp=0)
        h.exit(98)
        h.string("path", "/chain")
        run("repeated exec", h, b"aliasCUSTOM=1")

        (root / "bad").write_bytes(b"not an executable")
        (root / "bad").chmod(0o755)
        image(root / "truncated", bytes(16))
        (root / "truncated").write_bytes((root / "truncated").read_bytes()[:-4])
        image(root / "missing-lib", bytes(4), library=b"absent")
        image(root / "overflow", bytes(4))
        data = bytearray((root / "overflow").read_bytes())
        struct.pack_into("<I", data, 4, 0xFFFFFFFF)
        (root / "overflow").write_bytes(data)
        for path, error in (("/absent", 2), ("/bad", 8), ("/truncated", 8),
                            ("/missing-lib", 2), ("/overflow", 8)):
            run(f"failed exec preserves caller: {path}", exec_failure(path, error))
        run("exec directory rejected", exec_failure("/", 13))
        os.mkfifo(root / "fifo", 0o700)
        run("exec FIFO rejected without blocking", exec_failure("/fifo", 13))
        (root / "bad").chmod(0o644)
        run("exec permission denied", exec_failure("/bad", 13))
        g = Guest()
        g.const(0, "path")
        g.const(1, 0o755)
        g.syscall(15)
        g.success()
        g.exec(argv=0, envp=0)
        g.error(8)              # chmod succeeded; now the format is rejected.
        g.exit(0)
        g.string("path", "./bad")
        run("chmod enables exec permission", g)
        run("invalid argv pointer", exec_failure("/target", 14, argv=MEM_SIZE - 2))
        run("invalid envp pointer", exec_failure("/target", 14, envp=0xFFFFFFFF))
        g = exec_failure("/target", 7)
        g.data = [(name, b"a" * 24576 + b"\0" if name == "arg0" else value)
                  for name, value in g.data]
        run("oversized arguments", g)

        # The final word has no string terminator before the end of memory.
        setup = Guest()
        setup.const(4, MEM_SIZE - 4)
        setup.const(5, 0xFFFFFFFF)
        setup.store(5, 4)
        setup.exec(argv="bad-vector", envp=0)
        setup.error(14)
        setup.exit(0)
        setup.string("path", "/target")
        setup.vector("bad-vector", MEM_SIZE - 4)
        run("unterminated argument string", setup)

        # A child killed by SIGPIPE must report signal 13, not an exit code.
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            g = Guest()
            g.syscall(2)
            g.success()
            g.emit(0xE3500000)
            g.branch("child", 0)
            g.const(0, 0xFFFFFFFF)
            g.const(1, 0)
            g.const(2, 0)
            g.syscall(11)
            g.success()
            g.equal(1, 13)
            g.exit(0)
            g.label("child")
            g.const(0, write_fd)
            g.const(1, "byte")
            g.const(2, 1)
            g.syscall(4)
            g.exit(98)
            g.blob("byte", b"x")
            run("signal termination status", g, pass_fds=(write_fd,))
        finally:
            os.close(write_fd)

        # Both fork and exec must preserve an inherited descriptor's offset.
        (root / "input").write_bytes(b"ab")
        with (root / "input").open("rb") as stream:
            target = Guest()
            target.syscall(20)
            target.equal(0, 2)
            target.const(0, stream.fileno())
            target.const(1, "byte")
            target.const(2, 1)
            target.syscall(3)
            target.equal(0, 1)
            target.exit(17)
            target.blob("byte", bytes(4))
            image(root / "fd-target", target.build())
            g = Guest()
            g.syscall(66)
            g.success()
            g.emit(0xE3500000)
            g.branch("child", 0)
            g.const(0, 0xFFFFFFFF)
            g.const(1, 0)
            g.const(2, 0)
            g.syscall(11)
            g.success()
            g.equal(1, 17 << 8)
            g.const(0, stream.fileno())
            g.const(1, "byte")
            g.const(2, 1)
            g.syscall(3)
            g.const(0, 1)
            g.const(1, "byte")
            g.const(2, 1)
            g.syscall(4)
            g.exit(0)
            g.label("child")
            g.exec(argv=0, envp=0)
            g.exit(98)
            g.string("path", "/fd-target")
            g.blob("byte", bytes(4))
            run("vfork/exec preserves PID, descriptors and shared offsets", g,
                expected=b"b", pass_fds=(stream.fileno(),))

        # A blocking host pipe keeps the child alive deterministically while
        # its parent checks WNOHANG and error paths. Descriptors and offsets
        # survive exec and are shared after fork.
        read_fd, write_fd = os.pipe()
        try:
            waiter = Guest()
            waiter.syscall(20)
            waiter.equal(0, 1)
            waiter.const(0, write_fd)
            waiter.const(1, "byte")
            waiter.const(2, 1)
            waiter.syscall(4)
            waiter.const(0, 2)
            waiter.const(1, "status")
            waiter.const(2, 0)
            waiter.syscall(11)
            waiter.success()
            waiter.equal(0, 2)
            waiter.equal(1, 0)
            waiter.exit(0)
            waiter.blob("status", bytes(4))
            waiter.blob("byte", b"x")
            image(root / "waiter", waiter.build())

            g = Guest()
            g.syscall(2)
            g.success()
            g.emit(0xE3500000)
            g.branch("child", 0)
            g.mov(4, 0)
            for status, options, error in ((MEM_SIZE - 2, 0, 14), (0, 4, 22)):
                g.mov(0, 4)
                g.const(1, status)
                g.const(2, options)
                g.syscall(11)
                g.error(error)
            g.mov(0, 4)
            g.const(1, "status")
            g.const(2, 1)
            g.syscall(11)
            g.success()
            g.equal(0, 0)
            g.const(5, "status")
            g.load(6, 5)
            g.equal(6, 42)
            g.exec(argv=0, envp=0)
            g.exit(98)
            g.label("child")
            g.const(0, read_fd)
            g.const(1, "byte")
            g.const(2, 1)
            g.syscall(3)
            g.exit(0)
            g.blob("status", struct.pack("<I", 42))
            g.blob("byte", b"x")
            g.string("path", "/waiter")
            run("wait errors/WNOHANG preserve child; exec preserves child table", g,
                pass_fds=(read_fd, write_fd))
        finally:
            os.close(read_fd)
            os.close(write_fd)

        print(f"{tests} process tests passed")


if __name__ == "__main__":
    main()
