#!/usr/bin/env python3
"""Check native-tool syscall ABI using synthetic ARM26 guests."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from process import Guest, MEM_SIZE, image


def main():
    runner = sys.argv[1]
    groups = os.getgroups()
    assert all(g <= 65535 for g in groups), 'Host groups exceed the RISCiX gid_t range'
    with tempfile.TemporaryDirectory(prefix='sdk-groups-') as directory:
        root = Path(directory)
        image(root/'lib', bytes(4), magic=0x90b, entry=0x1800000, library=b'')
        cases = [(-1, 0, 22), (0, 0, None), (len(groups), 'groups', None)]
        if groups:
            cases += [(len(groups), MEM_SIZE-1, 14), (len(groups), 0, 14)]
        if len(groups) > 1:
            cases += [(len(groups)-1, 'groups', 22)]
        for size, dest, error in cases:
            g = Guest()
            g.const(0, size)
            g.const(1, dest)
            g.syscall(77)
            if error is None:
                g.success()
                g.equal(0, len(groups))
            else:
                g.error(error)
            g.const(0, 1)
            g.const(1, 'groups')
            g.const(2, len(groups)*2+4)
            g.syscall(4)
            g.exit(0)
            sentinel = b'\xa5'*(len(groups)*2+4)
            g.blob('groups', sentinel)
            image(root/'guest', g.build())
            result = subprocess.run([runner, './guest'], cwd=root,
                                    env=dict(os.environ, RIX_ROOT=directory, RIX_VERBOSE='0'),
                                    capture_output=True, timeout=10)
            expected = (struct.pack('<'+'H'*len(groups), *groups)+b'\xa5'*4
                        if dest == 'groups' and error is None and size else sentinel)
            assert result.returncode == 0 and result.stdout == expected, (
                size, dest, result.returncode, result.stdout, result.stderr)
        g = Guest()
        # Descriptor zero is a valid descriptor; only its access mode is queried.
        g.const(0, 0)
        g.const(1, 1)             # F_GETFD
        g.const(2, 0)
        g.syscall(92)
        g.success()
        g.const(0, 0xffffffff)
        g.const(1, 1)
        g.syscall(92)
        g.error(9)                # EBADF
        g.const(0, 0)
        g.const(1, 99)
        g.syscall(92)
        g.error(22)               # Unsupported command: EINVAL
        for number, mask, previous in ((110, 2, 0), (109, 4, 2),
                                       (109, 0, 6), (110, 0, 6)):
            g.const(0, mask)
            g.syscall(number)
            g.success()
            g.equal(0, previous)
        g.const(0, 'stack')
        g.const(1, 0)
        g.syscall(112)
        g.success()
        g.const(0, 0)
        g.const(1, 'oldstack')
        g.syscall(112)
        g.success()
        g.const(1, 'oldstack')
        g.load(0, 1)
        g.equal(0, 0x12340)
        g.load(0, 1, 4)
        g.equal(0, 1)
        for new, old in ((MEM_SIZE-1, 0), (0, MEM_SIZE-1)):
            g.const(0, new)
            g.const(1, old)
            g.syscall(112)
            g.error(14)
        g.exit(0)
        g.blob('stack', struct.pack('<II', 0x12340, 1))
        g.blob('oldstack', bytes(8))
        image(root/'guest', g.build())
        result = subprocess.run([runner, './guest'], cwd=root,
                                env=dict(os.environ, RIX_ROOT=directory, RIX_VERBOSE='0'),
                                capture_output=True, timeout=10)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    print('PASS: native-tool fcntl, signal state and getgroups ABI/error paths')


if __name__ == '__main__':
    main()
