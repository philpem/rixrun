#!/usr/bin/env python3
"""Exercise squeeze decoding with generated images, including invalid streams."""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from process import Guest, BASE, image, exec_failure

PAGE = 32768


def encode_table(values, width):
    result = bytearray()
    previous, i = -1, 0
    while i < len(values):
        run = 0
        while run < 9 and i + run < len(values) and values[i + run] == previous + run + 1:
            run += 1
        if run:
            result.append(run)
            previous = values[i + run - 1]
            i += run
            continue
        delta = (values[i] - previous) & 0xFFFFFFFF
        if delta <= 0x51:
            result.append(delta + 10)
        elif delta <= 0x51FF:
            result.extend((0x5C + (delta >> 8), delta & 255))
        elif delta <= 0x51FFFF:
            result.append(0xAE + (delta >> 16))
            result.extend((delta & 0xFFFF).to_bytes(2, "little"))
        else:
            result.append(0)
            result.extend(delta.to_bytes(width, "little"))
        previous = values[i]
        i += 1
    return result


def encode_page(raw, four, three):
    raw = raw.ljust((len(raw) + 7) & ~7, b"\0")
    words = list(struct.unpack(f"<{len(raw) // 4}I", raw))
    while words and words[-1] == 0:
        words.pop()
    if not words:
        return bytes(PAGE)
    if len(words) & 1:
        words.append(0)
    full = {word: i for i, word in enumerate(four)}
    upper = {word: i for i, word in enumerate(three)}

    def token(word):
        if word == 0:
            return 0, b""
        if word in full:
            index = full[word]
            return 9 + (index >> 8), bytes([index & 255])
        if word >> 8 in upper:
            index = upper[word >> 8]
            return 2 + (index >> 8), bytes([word & 255, index & 255])
        return 1, struct.pack(">I", word)

    chunks = []
    for i in range(len(words) - 1, 0, -2):
        high, high_bytes = token(words[i])
        low, low_bytes = token(words[i - 1])
        chunks.append(low_bytes + high_bytes + bytes([high << 4 | low]))
    stream = b"".join(reversed(chunks))
    header = struct.pack("<HHI", 8 + len(stream), len(words), sum(words) & 0xFFFFFFFF)
    assert len(header + stream) <= PAGE
    return (header + stream).ljust(PAGE, b"\0")


def binary(path, text, data=b"", magic=0x70B, library=b"lib", entry=BASE,
           four=(), three=()):
    header = struct.pack("<8I", magic, len(text), len(data), 0, 0, entry, 0, 0)
    header += bytes(36)
    table4, table3 = encode_table(four, 4), encode_table(three, 3)
    header += struct.pack("<8I", len(four), len(three), len(table4), len(table3),
                          four[-1] if four else 0, three[-1] if three else 0, 0, 0)
    header += library.ljust(60, b"\0")
    assert len(header + table4 + table3) < PAGE
    payload = text + data
    if magic & 0x200:
        payload = b"".join(encode_page(payload[i:i + PAGE], four, three)
                           for i in range(0, len(payload), PAGE))
    path.write_bytes((header + table4 + table3).ljust(PAGE, b"\0") + payload)
    path.chmod(0o755)


def main():
    executable = str(Path(sys.argv[1] if len(sys.argv) > 1 else "./rixrun").resolve())
    with tempfile.TemporaryDirectory(prefix="rixrun-squeeze-") as directory:
        root = Path(directory)
        env = {**os.environ, "RIX_ROOT": directory, "RIX_VERBOSE": "0"}
        checks = 0

        def run(name, path, output=b"", status=0, expected_stderr=b""):
            nonlocal checks
            p = subprocess.run([executable, str(path)], cwd=root, env=env,
                               capture_output=True, timeout=10)
            assert p.returncode == status, (name, p.returncode, p.stdout, p.stderr)
            if output is not None:
                assert p.stdout == output, (name, p.stdout, p.stderr)
                if expected_stderr is not None:
                    assert p.stderr == expected_stderr, (name, p.stderr)
            checks += 1
            print(f"ok: {name}")

        libtext, libdata = bytes(4), struct.pack("<I", 0x12345678)
        binary(root / "lib", libtext, libdata, magic=0x90B, library=b"", entry=0x1800000)
        g = Guest()
        g.const(0, 1)
        g.const(1, "message")
        g.const(2, 17)
        g.syscall(4)
        g.exit(0)
        g.blob("message", b"literal squeezed\n")
        binary(root / "literal", (bytes(4) + g.build()).ljust(PAGE, b"\0"),
               magic=0x30B, library=b"")
        run("static squeezed literals and zero words", root / "literal", b"literal squeezed\n")

        # Values force run, short, medium, long and escaped dictionary deltas.
        # The output selects all seven banks of both word-code families.
        four = list(range(1788)) + [0x2000, 0x120000, 0x12345678, 0xEF100001]
        three = list(range(1789)) + [0x1800, 0x1C000, 0xFEDCBA]
        indexes = [0, 255, 256, 512, 768, 1024, 1280, 1536, 1791]
        values = [four[i] for i in indexes] + [(three[i] << 8) | 0x55 for i in indexes]
        expected = struct.pack(f"<{len(values)}I", *values)
        g = Guest()
        for address, value in ((0x1800000, 0x12345678),
                               (BASE + PAGE, 0x12345678), (BASE + 2 * PAGE, 0)):
            g.const(4, address)
            g.load(5, 4)
            g.equal(5, value)
        g.const(0, 1)
        g.const(1, "message")
        g.const(2, len(expected))
        g.syscall(4)
        g.exit(0)
        g.blob("message", expected)
        binary(root / "rich", g.build().ljust(PAGE, b"\0"),
               libdata.ljust(2 * PAGE, b"\0"), four=four, three=three)
        run("both dictionaries, all code banks, text/data and zero pages", root / "rich", expected)
        binary(root / "lib", libtext, libdata, magic=0xB0B, library=b"", entry=0x1800000)
        run("squeezed library with a text/data split inside a page", root / "rich", expected)
        g = Guest()
        g.exec(argv=0, envp=0)
        g.exit(98)
        g.string("path", "/rich")
        image(root / "exec", g.build())
        run("uncompressed caller execs a squeezed image and library", root / "exec", expected)

        def invalid(name, source, offset, fmt, value):
            path = root / name
            data = bytearray((root / source).read_bytes())
            struct.pack_into(fmt, data, offset, value)
            path.write_bytes(data)
            path.chmod(0o755)
            run(name, path, output=None, status=1)
            return path

        invalid("dictionary count too large", "rich", 68, "<I", 1793)
        invalid("dictionary size overflow", "rich", 76, "<I", 0xFFFFFFFF)
        invalid("dictionary endpoint mismatch", "rich", 84, "<I", 0)
        invalid("three-byte dictionary endpoint mismatch", "rich", 88, "<I", 0)
        bad = bytearray((root / "literal").read_bytes())
        struct.pack_into("<6I", bad, 68, 1, 0, 1, 0, 0, 0)
        bad[160] = 9
        (root / "bad-run").write_bytes(bad)
        (root / "bad-run").chmod(0o755)
        run("dictionary run exceeds declared count", root / "bad-run", output=None, status=1)
        bad[160] = 0
        (root / "bad-escape").write_bytes(bad)
        (root / "bad-escape").chmod(0o755)
        run("truncated dictionary escape", root / "bad-escape", output=None, status=1)
        invalid("odd expanded word count", "literal", PAGE + 2, "<H", 3)
        invalid("expanded page too large", "literal", PAGE + 2, "<H", 8194)
        invalid("short page header", "literal", PAGE, "<H", 7)
        invalid("encoded page too large", "literal", PAGE, "<H", 65535)
        checksum = struct.unpack_from("<I", (root / "literal").read_bytes(), PAGE + 4)[0]
        invalid("bad-checksum", "literal", PAGE + 4, "<I", checksum ^ 1)
        bad = bytearray((root / "literal").read_bytes())
        bad[PAGE:PAGE + 11] = struct.pack("<HHI", 11, 2, 0) + b"\0\0\x99"
        (root / "bad-index").write_bytes(bad)
        (root / "bad-index").chmod(0o755)
        run("dictionary index outside table", root / "bad-index", output=None, status=1)
        bad[PAGE:PAGE + 9] = struct.pack("<HHI", 9, 2, 0) + b"\x11"
        (root / "bad-literal").write_bytes(bad)
        (root / "bad-literal").chmod(0o755)
        run("truncated literal", root / "bad-literal", output=None, status=1)
        (root / "truncated").write_bytes((root / "literal").read_bytes()[:-1])
        (root / "truncated").chmod(0o755)
        run("truncated squeezed file", root / "truncated", output=None, status=1)

        image(root / "exec", exec_failure("/bad-checksum", 8).build())
        run("bad squeezed exec preserves caller", root / "exec", expected_stderr=None)
        bad = bytearray((root / "lib").read_bytes())
        bad[PAGE + 4] ^= 1
        (root / "badlib").write_bytes(bad)
        image(root / "bad-library-main", bytes(4), library=b"badlib")
        image(root / "exec", exec_failure("/bad-library-main", 8).build())
        run("bad squeezed library preserves caller", root / "exec", expected_stderr=None)
        print(f"{checks} squeeze checks passed")


if __name__ == "__main__":
    main()
