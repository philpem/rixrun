# Running squeezed binaries

Rixrun loads squeezed ZMAGIC and SPZMAGIC executables directly. Startup and
guest `execve` share the same loader, so a squeezed compiler can invoke the
original squeezed shell and linker. Decoding writes directly to guest memory;
input executables remain unchanged and no temporary unsqueezed files are
created.

Set `RIX_ROOT` to a RISCiX installation. From a working directory containing
`hello.c`, compile and run it with `rixrun` on your `PATH`:

```sh
export RIX_ROOT=/path/to/riscix
rixrun "$RIX_ROOT/usr/lib/cc" hello.c -o hello
rixrun ./hello
```

Guest `/usr/lib` and `/usr/include` paths resolve beneath `RIX_ROOT`;
working-file paths use the host filesystem. See the
[path rules](../README.md#environment-variables) for details. Squeeze support
uses the existing syscall ABI and loads whole images before execution.

## Format

The `MF_SQUEEZED` flag is octal `01000` (`0x200`). Masking it off gives the
base executable or shared-library magic. The 160-byte executable header
contains counts, encoded byte lengths and last-entry checks for two
dictionaries. Their encoded bytes immediately follow the header and must
fit before text at file offset `0x8000`.

Each dictionary holds at most 1,792 entries. The first contains complete
32-bit words. The second contains 24-bit prefixes, used with a separate low
byte. When the second dictionary is present, the first occupies all 1,792
slots. Values are delta-encoded from an initial value of `0xffffffff`, using
unsigned 32-bit arithmetic.

In this table, `code` is the next encoded byte, `next_byte` is the following
unsigned byte, and `next_u16` is the following two bytes read little-endian:

| Code byte | Meaning |
| --- | --- |
| 0 | Delta in the next four little-endian bytes for the word dictionary, or three for the prefix dictionary |
| 1–9 | That many consecutive values, each one greater than its predecessor |
| 10–91 | Delta is `code - 10` |
| 92–173 | Delta is `((code - 92) << 8) + next_byte` |
| 174–255 | Delta is `((code - 174) << 16) + next_u16` |

The text/data region is divided into 32 KiB file slots starting at `0x8000`.
Squeezing retains these offsets and the file length while reducing allocated
disk blocks. Each slot contains one encoded page, beginning with an
eight-byte little-endian header:

| Field | Width | Meaning |
| --- | --- | --- |
| Encoded size | 16 bits | Size in bytes, including the header; zero denotes an all-zero page |
| Expanded word count | 16 bits | Number of decoded 32-bit words |
| Checksum | 32 bits | Low 32 bits of the sum of decoded words |

For a nonzero encoded size, the size must be between eight bytes and 32 KiB,
and the expanded word count must be even and at most 8,192. Unused bytes at
the end of the expanded page are zero-filled.

Read the encoded stream backwards from its declared end. To pop a byte,
decrement the input position and read that byte. Each code byte describes
two words, high nibble first; the decoded words fill the output backwards:

| Nibble | Word |
| --- | --- |
| 0 | Zero |
| 1 | Literal 32-bit word; pop four bytes, least significant first |
| 2–8 | A 24-bit dictionary prefix plus a separate low byte |
| 9–15 | A complete 32-bit dictionary word |

For dictionary codes, first pop `index_byte`. Codes 2–8 use index
`((code + 5) << 8) + index_byte`, then pop `low_byte` to produce
`(table[index] << 8) | low_byte`. Codes 9–15 use index
`((code - 9) << 8) + index_byte` and return `table[index]` directly. Here,
`table` is the concatenation of the word and prefix dictionaries.

The decoder checks both dictionaries, stream endpoints, dictionary indexes,
expanded lengths and page checksums. Malformed or truncated encoded data
fails with `ENOEXEC`. A failed guest `execve` preserves the caller's image and
memory, even if earlier pages of the replacement decoded successfully. Only
the syscall error result and carry flag change.

## References

The format was checked against these files in the RISCiX installation and
kernel kit used for validation:

- `usr/include/sys/exec.h`: executable headers and format flags.
- `usr/share/man/man1/squeeze.1`: file-layout and disk-occupancy behaviour.
- `SHARED/vm_squeeze.o`: the original decoder.

The local source-recovery workspace at `/mnt/zfs/dev/riscix` contains a
disassembly in `work/object-analysis/SHARED--vm_squeeze.o.asm` and a Python
decoder in `work/bootstrap_rix_tools.py`. The [C decoder](../squeeze.c)
follows that algorithm with explicit input and output bounds.

## Validation

Decoded text and data matched the Python reference byte-for-byte for 228
original binaries, covering 561 pages. The original squeezed C345 compiler,
shell and linker also completed automatic builds and ran the resulting
programs. Validation covered multi-file builds, BSD symbol renaming, `-O`,
compiler and linker error statuses, and the 32-child `fork`/`vfork` linker
harness. These runs also passed with AddressSanitizer and
UndefinedBehaviorSanitizer enabled in the host emulator.

Run `make check` for self-contained fixtures covering both dictionary
families, all dictionary banks, literals, zero pages, separate text/data
destinations, malformed metadata and streams, and failed-exec rollback. These
fixtures need no RISCiX installation.

A synthetic squeezed shared-library fixture checks the same loader path.
The original `exec.h` states that the RISCiX kernel did not support squeezed
shared libraries; the libraries used for native C345 validation were
unsqueezed.

The 24 process checks and 20 squeeze checks passed in both the normal and
AddressSanitizer/UndefinedBehaviorSanitizer builds of the host emulator.
