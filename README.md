# Rixrun

v0.1 19 Feb 2022


Run Acorn RISCiX binaries from the comfort of your own laptop!
This program is a simple userspace instruction/syscall emulator that
can run RISCiX ZMAGIC and SPZMAGIC binaries, including squeezed images.

It uses ARMulator to run arm26 code, and with some shockingly-simple
SWI syscall emulation it's able to run some basic programs.

My personal motivation for this is to run the RISCiX compilers on Linux,
so programs/drivers can be "cross-developed" without a cross-compiler.  To
that end, rixrun can now run cc and ld:

```
$ export RIX_ROOT=/path/to/riscix/
$ rixrun usr/lib/cc -help
Norcroft Unix ARM C 345/345/345/dbx [Oct 29 1990, 12:46:07]

Usage :       ./cc [-options] file1 file2 ... filen

Main options:

-ansi          Compile ANSI-style C source code
-pcc           Compile BSD UNIX PCC-style C source code
-c             Do not invoke the linker to link the files being compiled
-C             Prevent the preprocessor from removing comments (Use with -E)
-D<symbol>     Define preprocessor <symbol> on entry to the compiler
-E             Preprocess the C source code only, do not compile or link it
-F<options>    Enable a selection of compiler defined features
-g             Generate code that may be used with the debugger
-I<directory>  Include <directory> on the '#include' search path
-list          Generate a compilation listing
-M<options>    Generate a 'makefile' style list of dependencies
-o <file>      Instruct the linker to name the object code produced <file>
-O             Invoke the object code improver
-p<options>    Generate code to produce 'profile' information
-R             Place all compile time strings in a 'Read only' segment
-S             Generate ARM assembly code instead of object code
-U<symbol>     Undefine preprocessor <symbol> on entry to the compiler
-w<options>    Disable all or selected warning and error messages
```

This tool can compile and link a simple helloworld.c into a working binary.
C345's automatic link step runs the guest shell and linker. The installation
under `RIX_ROOT` must contain `usr/lib/cc`, `usr/bin/sh`, `sbin/sh`,
`usr/bin/ld`, headers and shared libraries.

Save this program as `hello.c` in your working directory:

```c
#include <stdio.h>

int main(void)
{
    puts("Hello world!");
    return 0;
}
```

Compile and run it with `rixrun` on your `PATH`:

```sh
rixrun "$RIX_ROOT/usr/lib/cc" hello.c -o hello
rixrun ./hello
```

Expected output:

```text
Hello world!
```

Default headers and libraries come from the installation under `RIX_ROOT`.
Relative source and output filenames use the working directory. The BSD and
X/Open frontend modes also require `usr/bin/symrename`.

## Limitations

Many!  Only a handful of very simple syscalls are implemented so far:

   * No terminal control: unsupported ioctls return `ENOTTY` for valid descriptors
   * No networking
   * Guest signal handlers and job control are not implemented
   * Old executable formats such as OMAGIC, NMAGIC and IMAGIC are unsupported

Experimental process support uses one host process per guest process.
`vfork` provides `fork` semantics: the child has private memory and the parent
is not suspended. A failed `execve` preserves the caller's image; a successful
call preserves the guest PID, child mappings and open file descriptors.

Guest PIDs are positive signed 16-bit IDs, local to a process and its direct
children. They are not globally unique. `waitpid` supports waiting for a
particular child, any child or children in the current host process group,
with `WNOHANG` and `WUNTRACED`. Resource-usage data is reported as zero.

See [the process implementation notes](docs/fork-experimental.md) for ABI
details, limitations and validation results. Run `make check` for the
self-contained process and squeeze checks; these need no RISCiX installation.


## Usage

### Environment variables

`RIX_ROOT` is the host path to a RISCiX installation. When set, it locates
shared libraries and prefixes absolute filenames passed to guest `execve`:
`/usr/bin/ld` resolves to `$RIX_ROOT/usr/bin/ld`. The executable must be
readable and have execute permission. Loading a host executable as a fallback
is unsupported.

Guest file operations also resolve `/usr/lib`, `/usr/include` and their
contents under `RIX_ROOT`. This locates the compiler's default headers,
startup object, libraries and symbol-renaming tables. Other file paths,
relative `execve` filenames and the initial command-line filename use host
paths. These rules do not provide chroot isolation.

`RIX_VERBOSE` can be set to `1` or `2` for increasing debug output:  syscall
trace and instruction execution trace.


### Squeezedness

RISCiX binaries are usually squeezed to save disk blocks. Rixrun decodes
their text and data directly into guest memory at startup and during
`execve`. Input executables remain unchanged; running `unsqueeze` first is
unnecessary.

Rixrun decodes the whole image before execution; RISCiX decodes pages on demand.
The loader validates dictionary and page bounds, stream lengths and checksums,
and rejects invalid images.
See [the squeeze format notes](docs/squeezed-binaries.md) for details.


# Licence

Copyright 2022 Matt Evans (and ARMulator authors, and small portions borrowed from QEMU's linux-user binary loaders).

This program is licenced under GPLv2.
