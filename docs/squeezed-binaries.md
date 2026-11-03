# Running squeezed binaries

Rixrun loads squeezed ZMAGIC and SPZMAGIC executables directly. Startup and
guest execve share the same loader, so a squeezed compiler can invoke the
original squeezed shell and linker. No temporary unsqueezed files are created,
and input executables remain unchanged.

Set RIX_ROOT to a RISCiX installation and invoke its compiler normally:

```sh
export RIX_ROOT=/path/to/riscix
rixrun "$RIX_ROOT/usr/lib/cc" hello.c -o hello
rixrun ./hello
```

The existing system-path rules still apply: guest /usr/lib and /usr/include
paths resolve beneath RIX_ROOT, and working-file paths use the host filesystem.
Squeeze support does not extend the syscall ABI or implement demand paging.

## Format

The MF_SQUEEZED flag is octal 01000 (0x200). Masking it off gives the ordinary
executable or shared-library magic. The 160-byte executable header contains
counts, encoded byte lengths and last-entry checks for two dictionaries.
Their encoded bytes immediately follow the header and must fit before text
at file offset 0x8000.

Each dictionary holds at most 1,792 entries. The first contains complete
32-bit words. The second contains 24-bit prefixes, used with a separate low
byte; when it is present, the first dictionary occupies all 1,792 slots.
Values are delta-encoded from an initial value of 0xffffffff:

| Code byte | Meaning |
| --- | --- |
| 0 | Delta in the next four or three little-endian bytes |
| 1–9 | That many consecutive values, each one greater than its predecessor |
| 10–91 | Delta equals code minus 10 |
| 92–173 | Delta equals ((code minus 92) << 8) plus the next byte |
| 174–255 | Delta equals ((code minus 174) << 16) plus the next two little-endian bytes |

Text and data retain their original file offsets, in 32 KiB slots. Squeezing
reduces allocated disk blocks, not file length. Each slot begins with an
eight-byte little-endian header: a 16-bit encoded size, a 16-bit expanded word
count, and a 32-bit additive checksum of those words. Encoded size zero denotes
an all-zero page; otherwise the expanded word count must be even and at most
8,192. Unused bytes at the end of the expanded page are zero-filled.

Read the encoded stream backwards from its declared end. Each code byte
describes two words, high nibble first. The decoded words fill the output
backwards too:

| Nibble | Word |
| --- | --- |
| 0 | Zero |
| 1 | Literal, popping low byte first |
| 2–8 | Dictionary index ((code + 5) << 8) plus the popped index byte; shift that entry left eight bits and append a popped low byte |
| 9–15 | Dictionary index ((code minus 9) << 8) plus the popped index byte |

Both dictionaries, stream endpoints, dictionary indexes, expanded lengths
and page checksums are checked. Truncated or invalid streams return ENOEXEC.
A failed guest exec retains the caller's original CPU state and memory, even
if earlier pages of the replacement had already decoded successfully.

## References and validation

The format references are the supplied RISCiX usr/include/sys/exec.h,
usr/share/man/man1/squeeze.1 and the kernel kit's SHARED/vm_squeeze.o. The local
source-recovery workspace under /mnt/zfs/dev/riscix contains the disassembly
in work/object-analysis/SHARED--vm_squeeze.o.asm and a Python decoder in
work/bootstrap_rix_tools.py. The new C implementation follows that algorithm
with explicit input and output bounds.

Decoded text/data matched the Python reference byte-for-byte for 228 original
binaries, covering 561 pages. The original squeezed C345 compiler, shell and
linker also completed automatic builds and ran the resulting programs. This
included multi-file builds, BSD symbol renaming, -O, compiler/linker error
statuses and the 32-child fork/vfork linker harness under AddressSanitizer and
UndefinedBehaviorSanitizer.

Run make check for self-contained fixtures covering both dictionary families,
all dictionary banks, literals, zero pages, separate text/data destinations,
malformed metadata and streams, and failed-exec rollback. These fixtures need
no RISCiX installation. A synthetic squeezed shared-library fixture checks the
same loader path; the original exec.h notes that squeezed shared libraries
were not supported by the RISCiX kernel, and the supplied libraries used for
C345 validation are unsqueezed.

All 24 process checks and 20 squeeze checks passed in both the normal and
AddressSanitizer/UndefinedBehaviorSanitizer builds.
